// Serving our one shared file, end to end against a fake peer.
//
// This is the flow slsk_share.dart exists for, and the one that was
// accepted-but-never-delivered before: a peer queues the shared file, we
// answer with our own upload-direction TransferRequest, the peer allows
// it, and we open an 'F' connection and actually push the bytes. If we
// accept and never send, the peer sees a broken uploader — worse than
// sharing nothing.
//
// Everything runs over loopback: a fake login server (answers Login and
// GetPeerAddress) and a fake peer (dials our listener with PeerInit,
// speaks the P-connection messages, then accepts our F connection).

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_share.dart';
import 'package:bopwire_player/src/services/networks/soulseek/soulseek_network.dart';

/// Minimal login server: accepts one client, answers Login with success
/// and GetPeerAddress with a fixed address, records everything else.
class _FakeServer {
  _FakeServer(this.peerAddressPort);

  final int peerAddressPort;
  late ServerSocket _server;
  Socket? _client;
  final SlskFrameBuffer _frames = SlskFrameBuffer();
  final List<SlskFrame> received = [];
  int? waitPort;
  int sharedFolders = -1, sharedFiles = -1;

  int get port => _server.port;

  Future<void> start() async {
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server.listen((socket) {
      _client = socket;
      socket.listen((chunk) {
        _frames.add(chunk);
        for (final f in _frames.readFrames()) {
          received.add(f);
          _handle(socket, f);
        }
      });
    });
  }

  void _handle(Socket socket, SlskFrame f) {
    switch (f.code) {
      case ServerCode.login:
        // success, greeting, our ip (parse order of LoginResponse).
        final w = SlskWriter()
          ..boolean(true)
          ..str('Welcome to the fake server')
          ..raw([1, 0, 0, 127]); // parses as 127.0.0.1
        socket.add(frameWithUint32Code(ServerCode.login, w.take()));
      case ServerCode.setWaitPort:
        waitPort = SlskReader(f.payload).uint32();
      case ServerCode.sharedFoldersFiles:
        final r = SlskReader(f.payload);
        sharedFolders = r.uint32();
        sharedFiles = r.uint32();
      case ServerCode.getPeerAddress:
        final user = SlskReader(f.payload).str();
        final w = SlskWriter()
          ..str(user)
          ..raw([1, 0, 0, 127]) // 127.0.0.1
          ..uint32(peerAddressPort);
        socket.add(frameWithUint32Code(ServerCode.getPeerAddress, w.take()));
      default:
        break;
    }
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

  test('a peer that queues the shared file receives its bytes over F',
      () async {
    // The fake peer's own listener, where it expects our 'F' dial.
    final peerListener =
        await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final server = _FakeServer(peerListener.port);
    await server.start();

    final net = SoulseekNetwork(
      serverHost: '127.0.0.1',
      serverPorts: [server.port],
      usernameOverride: 'me',
      passwordOverride: 'pw',
    );

    await net.connect();
    expect(net.status, NetworkStatus.connected,
        reason: 'login against the fake server failed: ${net.lastError}');

    // The client must have announced a non-empty share.
    await waitFor(() => server.sharedFolders == 1 && server.sharedFiles == 1,
        what: 'SharedFoldersFiles(1, 1) was never announced');
    await waitFor(() => server.waitPort != null,
        what: 'SetWaitPort never arrived');

    // ---- fake peer dials our listener with PeerInit 'P' and queues the
    // shared file ----
    final p = await Socket.connect('127.0.0.1', server.waitPort!);
    p.add(framePeerInit(
        PeerInitCode.peerInit, SlskOut.peerInit('peerA', ConnType.peer)));
    p.add(frameWithUint32Code(
        PeerCode.queueUpload, SlskOut.queueUpload(kSlskSharePath)));

    // Expect the client's upload-direction TransferRequest back on the
    // same P connection.
    final pFrames = <SlskFrame>[];
    final pBuf = SlskFrameBuffer();
    p.listen((chunk) {
      pBuf.add(chunk);
      pFrames.addAll(pBuf.readFrames());
    });

    await waitFor(
        () => pFrames.any((f) => f.code == PeerCode.transferRequest),
        what: 'no TransferRequest arrived after QueueUpload');
    final req = TransferRequestMessage.parse(
        pFrames.firstWhere((f) => f.code == PeerCode.transferRequest).payload);
    expect(req.direction, TransferDirection.upload);
    expect(req.virtualPath, kSlskSharePath);
    expect(req.fileSize, greaterThan(0));

    // ---- peer allows the transfer; client must dial our F listener ----
    final fConnection = peerListener.first;
    p.add(frameWithUint32Code(PeerCode.transferResponse,
        SlskOut.transferResponse(req.token, true)));

    final f = await fConnection.timeout(const Duration(seconds: 10));
    final fBuf = SlskFrameBuffer(codeIsByte: true, maxMessageSize: 16384);
    final bytes = BytesBuilder();
    var handshakeDone = false;
    int? token;
    final closed = Completer<void>();
    f.listen((chunk) {
      if (handshakeDone) {
        bytes.add(chunk);
        return;
      }
      fBuf.add(chunk);
      final frame = fBuf.readFrame();
      if (frame == null) return;
      expect(frame.code, PeerInitCode.peerInit);
      final init = PeerInitRequest.parse(frame.payload);
      expect(init.username, 'me');
      expect(init.connType, ConnType.file);
      handshakeDone = true;
      final rest = fBuf.drain();
      if (rest.isNotEmpty) bytes.add(rest);
    }, onDone: () => closed.complete());

    // After the handshake the client sends the unframed uint32 token; we
    // answer with an unframed uint64 offset of zero and then collect the
    // file bytes until the client closes.
    await waitFor(() => handshakeDone && bytes.length >= 4,
        what: 'F handshake/token never arrived');
    final all = bytes.takeBytes();
    token = ByteData.sublistView(all, 0, 4).getUint32(0, Endian.little);
    expect(token, req.token, reason: 'F connection used a different token');
    if (all.length > 4) bytes.add(Uint8List.sublistView(all, 4));

    f.add(SlskOut.fileOffset(0));

    await closed.future.timeout(const Duration(seconds: 10));
    final served = bytes.takeBytes();
    final expected = await SlskShare.instance.readBytes();
    expect(served.length, expected.length,
        reason: 'served ${served.length} bytes, share is ${expected.length}');
    expect(served, expected);

    f.destroy();
    p.destroy();
    await peerListener.close();
    await net.dispose();
    await server.close();
  }, timeout: const Timeout(Duration(seconds: 30)));

  test('a QueueUpload for anything else is denied with "File not shared."',
      () async {
    final peerListener =
        await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final server = _FakeServer(peerListener.port);
    await server.start();

    final net = SoulseekNetwork(
      serverHost: '127.0.0.1',
      serverPorts: [server.port],
      usernameOverride: 'me',
      passwordOverride: 'pw',
    );
    await net.connect();
    await waitFor(() => server.waitPort != null);

    final p = await Socket.connect('127.0.0.1', server.waitPort!);
    p.add(framePeerInit(
        PeerInitCode.peerInit, SlskOut.peerInit('peerB', ConnType.peer)));
    p.add(frameWithUint32Code(PeerCode.queueUpload,
        SlskOut.queueUpload(r'Music\secret\track.mp3')));

    final pFrames = <SlskFrame>[];
    final pBuf = SlskFrameBuffer();
    p.listen((chunk) {
      pBuf.add(chunk);
      pFrames.addAll(pBuf.readFrames());
    });

    await waitFor(() => pFrames.any((f) => f.code == PeerCode.uploadDenied),
        what: 'no UploadDenied arrived');
    final denied = UploadDenied.parse(
        pFrames.firstWhere((f) => f.code == PeerCode.uploadDenied).payload);
    expect(denied.virtualPath, r'Music\secret\track.mp3');
    expect(denied.reason, TransferRejectReason.fileNotShared);

    p.destroy();
    await peerListener.close();
    await net.dispose();
    await server.close();
  }, timeout: const Timeout(Duration(seconds: 30)));
}
