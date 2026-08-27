// Search results from peers who cannot serve are dropped at the source:
// a peer with no free upload slot, or one behind a queue deeper than
// kMaxAcceptableQueue, would keep our request waiting indefinitely, and
// the user asked for those rows to never appear at all. A SHALLOW queue is
// accepted — live measurement showed a non-empty queue is the normal state
// on Soulseek, and the old strict rule (any queue > 0) hid ~99% of
// results. The UI layer knows nothing about slot state, so the filtering
// must happen here, in the network's search stream.

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_share.dart';
import 'package:bopwire_player/src/services/networks/soulseek/soulseek_network.dart';

/// Fake login server: answers Login, records SetWaitPort and captures the
/// token of the first FileSearch.
class _FakeServer {
  late ServerSocket _server;
  Socket? _client;
  final SlskFrameBuffer _frames = SlskFrameBuffer();
  int? waitPort;
  int? searchToken;

  int get port => _server.port;

  Future<void> start() async {
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server.listen((socket) {
      _client = socket;
      socket.listen((chunk) {
        _frames.add(chunk);
        for (final f in _frames.readFrames()) {
          switch (f.code) {
            case ServerCode.login:
              final w = SlskWriter()
                ..boolean(true)
                ..str('hi')
                ..raw([1, 0, 0, 127]);
              socket.add(frameWithUint32Code(ServerCode.login, w.take()));
            case ServerCode.setWaitPort:
              waitPort = SlskReader(f.payload).uint32();
            case ServerCode.fileSearch:
              searchToken = SlskReader(f.payload).uint32();
            default:
              break;
          }
        }
      });
    });
  }

  Future<void> close() async {
    _client?.destroy();
    await _server.close();
  }
}

Future<void> waitFor(bool Function() check,
    {Duration timeout = const Duration(seconds: 10), String? what}) async {
  final deadline = DateTime.now().add(timeout);
  while (!check()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('condition not met within $timeout${what == null ? '' : ': $what'}');
    }
    await Future<void>.delayed(const Duration(milliseconds: 10));
  }
}

/// Dials the client's listener as [username] and delivers one search
/// response frame.
Future<Socket> deliverResponse(
    int clientPort, FileSearchResponse resp, String username) async {
  final s = await Socket.connect('127.0.0.1', clientPort);
  s.add(framePeerInit(
      PeerInitCode.peerInit, SlskOut.peerInit(username, ConnType.peer)));
  s.add(frameWithUint32Code(
      PeerCode.fileSearchResponse, FileSearchResponse.build(resp)));
  return s;
}

void main() {
  late Directory shareDir;

  setUp(() async {
    shareDir = await Directory.systemTemp.createTemp('slsk-share-test');
    SlskShare.instance.baseDirOverride = shareDir;
  });

  tearDown(() async {
    SlskShare.instance.baseDirOverride = null;
    try {
      await shareDir.delete(recursive: true);
    } catch (_) {}
  });

  test('busy peers are dropped from the search stream, free peers shown',
      () async {
    final server = _FakeServer();
    await server.start();

    final net = SoulseekNetwork(
      serverHost: '127.0.0.1',
      serverPorts: [server.port],
      usernameOverride: 'me',
      passwordOverride: 'pw',
    );

    final tracks = <ExternalTrack>[];
    final sub = net.search('nofx').listen((batch) => tracks.addAll(batch));

    await waitFor(() => server.searchToken != null,
        what: 'FileSearch never reached the server');
    await waitFor(() => server.waitPort != null);
    final token = server.searchToken!;

    SlskFile song(String name) => SlskFile(
        path: 'Music\\NOFX\\$name.mp3', size: 5 << 20, bitrate: 320);

    // No free slot -> must be dropped.
    final busy1 = await deliverResponse(
        server.waitPort!,
        FileSearchResponse(
          username: 'no-slot',
          token: token,
          files: [song('Linoleum')],
          freeUploadSlots: false,
          queueLength: 0,
        ),
        'no-slot');

    // Free slot, shallow queue -> shown (queues are normal on Soulseek).
    final busy2 = await deliverResponse(
        server.waitPort!,
        FileSearchResponse(
          username: 'queued-up',
          token: token,
          files: [song('The Decline')],
          freeUploadSlots: true,
          queueLength: 12,
        ),
        'queued-up');

    // Free slot but a queue deeper than kMaxAcceptableQueue -> dropped.
    final busy3 = await deliverResponse(
        server.waitPort!,
        FileSearchResponse(
          username: 'deep-queue',
          token: token,
          files: [song('Longest Line')],
          freeUploadSlots: true,
          queueLength: kMaxAcceptableQueue + 1,
        ),
        'deep-queue');

    // Free slot, empty queue -> shown.
    final free = await deliverResponse(
        server.waitPort!,
        FileSearchResponse(
          username: 'servable',
          token: token,
          files: [song('Bob'), song('Leave It Alone')],
          freeUploadSlots: true,
          queueLength: 0,
        ),
        'servable');

    await waitFor(() => tracks.isNotEmpty,
        what: 'the servable peer\'s results never arrived');
    // Give the dropped responses every chance to show up wrongly.
    await Future<void>.delayed(const Duration(milliseconds: 300));

    final owners = tracks.map((t) => t.owner).toSet();
    expect(owners, {'servable', 'queued-up'},
        reason: 'peers with a free slot and an acceptable queue may appear; '
            'no-slot and deep-queue peers may not, got: $owners');
    // servable's two files + queued-up's one; the folder row the network
    // synthesises for a multi-file directory is excluded by isFolder.
    expect(tracks.where((t) => !t.isFolder).length, 3);

    await sub.cancel();
    busy1.destroy();
    busy2.destroy();
    busy3.destroy();
    free.destroy();
    await net.dispose();
    await server.close();
  }, timeout: const Timeout(Duration(seconds: 30)));
}
