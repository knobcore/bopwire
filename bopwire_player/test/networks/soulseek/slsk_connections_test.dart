// Socket-level checks over loopback.
//
// These drive the real dart:io code paths — framing across TCP writes, the
// peer-init handshake and the leftover-bytes handoff into a peer connection —
// against a fake counterpart. They do not touch the live Soulseek network.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_connections.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_messages.dart';

/// Polls until [check] passes or the budget runs out. Cheaper to read than
/// hand-rolled completers for every assertion.
Future<void> waitFor(bool Function() check,
    {Duration timeout = const Duration(seconds: 5)}) async {
  final deadline = DateTime.now().add(timeout);
  while (!check()) {
    if (DateTime.now().isAfter(deadline)) {
      fail('condition not met within $timeout');
    }
    await Future<void>.delayed(const Duration(milliseconds: 5));
  }
}

void main() {
  group('SlskServerConnection', () {
    late ServerSocket fakeServer;
    late SlskServerConnection conn;
    late Socket serverSide;
    final received = <SlskFrame>[];

    setUp(() async {
      received.clear();
      fakeServer = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final pending = fakeServer.first;
      conn = SlskServerConnection();
      await conn.connect('127.0.0.1', fakeServer.port);
      serverSide = await pending;
      conn.messages.listen(received.add);
    });

    tearDown(() async {
      await conn.close();
      serverSide.destroy();
      await fakeServer.close();
    });

    test('decodes a login response delivered in one write', () async {
      final payload = (SlskWriter()
            ..boolean(true)
            ..str('Welcome')
            ..raw([1, 0, 0, 127]) // 127.0.0.1
            ..str('checksum')
            ..boolean(false))
          .take();
      serverSide.add(frameWithUint32Code(ServerCode.login, payload));
      await serverSide.flush();

      await waitFor(() => received.isNotEmpty);
      expect(received.single.code, ServerCode.login);

      final resp = LoginResponse.parse(received.single.payload);
      expect(resp.success, isTrue);
      expect(resp.greeting, 'Welcome');
      expect(resp.ipAddress, '127.0.0.1');
    });

    test('reassembles a message split across several TCP writes', () async {
      final payload = Uint8List.fromList(List.generate(5000, (i) => i & 0xFF));
      final framed = frameWithUint32Code(ServerCode.connectToPeer, payload);

      // Three writes, one of which cuts the length prefix in half.
      serverSide.add(framed.sublist(0, 2));
      await serverSide.flush();
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(received, isEmpty);

      serverSide.add(framed.sublist(2, 1000));
      await serverSide.flush();
      await Future<void>.delayed(const Duration(milliseconds: 20));
      expect(received, isEmpty);

      serverSide.add(framed.sublist(1000));
      await serverSide.flush();

      await waitFor(() => received.isNotEmpty);
      expect(received.single.code, ServerCode.connectToPeer);
      expect(received.single.payload, orderedEquals(payload));
    });

    test('delivers several messages coalesced into one read, in order',
        () async {
      serverSide.add([
        ...frameWithUint32Code(ServerCode.parentMinSpeed,
            (SlskWriter()..uint32(1)).take()),
        ...frameWithUint32Code(ServerCode.wishlistInterval,
            (SlskWriter()..uint32(720)).take()),
        ...frameWithUint32Code(
            ServerCode.serverPing, (SlskWriter()).take()),
      ]);
      await serverSide.flush();

      await waitFor(() => received.length == 3);
      expect(
        received.map((f) => f.code),
        orderedEquals([
          ServerCode.parentMinSpeed,
          ServerCode.wishlistInterval,
          ServerCode.serverPing,
        ]),
      );
    });

    test('send() puts a correctly framed message on the wire', () async {
      final wire = <int>[];
      serverSide.listen(wire.addAll);

      conn.send(ServerCode.fileSearch, SlskOut.fileSearch(4242, 'test query'));

      await waitFor(() => wire.length >= 8);
      final buf = SlskFrameBuffer()..add(wire);
      final frame = buf.readFrame()!;
      expect(frame.code, ServerCode.fileSearch);
      final r = SlskReader(frame.payload);
      expect(r.uint32(), 4242);
      expect(r.str(), 'test query');
    });

    test('completes done when the remote end hangs up', () async {
      var closed = false;
      unawaited(conn.done.then((_) => closed = true));
      await serverSide.close();
      serverSide.destroy();
      await waitFor(() => closed);
      expect(conn.isConnected, isFalse);
    });
  });

  group('SlskPeerListener handshake', () {
    late SlskPeerListener listener;
    final handshakes = <IncomingPeerHandshake>[];

    setUp(() async {
      handshakes.clear();
      listener = SlskPeerListener();
      final port = await listener.bind(preferred: const [0]);
      expect(port, isNotNull);
      listener.incoming.listen(handshakes.add);
    });

    tearDown(() async {
      for (final h in handshakes) {
        h.socket.destroy();
      }
      await listener.close();
    });

    test('reads a PeerInit and preserves the bytes that followed it',
        () async {
      final client = await Socket.connect('127.0.0.1', listener.port!);
      addTearDown(client.destroy);

      // A peer answering a search dials us, announces itself, then
      // immediately pushes the result in the same burst.
      final response = FileSearchResponse.build(const FileSearchResponse(
        username: 'seeder',
        token: 555,
        files: [SlskFile(path: r'M\Album\1.mp3', size: 99, bitrate: 320)],
      ));
      client.add([
        ...framePeerInit(PeerInitCode.peerInit,
            SlskOut.peerInit('seeder', ConnType.peer)),
        ...frameWithUint32Code(PeerCode.fileSearchResponse, response),
      ]);
      await client.flush();

      await waitFor(() => handshakes.isNotEmpty);
      final h = handshakes.single;
      expect(h.isPierce, isFalse);
      expect(h.username, 'seeder');
      expect(h.connType, ConnType.peer);
      // Adopting the socket must replay the bytes that shared the handshake's
      // TCP segment as a peer message.
      final peerFrames = <SlskFrame>[];
      final peer =
          SlskPeerConnection.adopt(h.username!, h.socket, data: h.data);
      peer.messages.listen(peerFrames.add);

      await waitFor(() => peerFrames.isNotEmpty);
      expect(peerFrames.single.code, PeerCode.fileSearchResponse);
      final parsed = FileSearchResponse.parse(peerFrames.single.payload);
      expect(parsed.username, 'seeder');
      expect(parsed.token, 555);
      expect(parsed.files.single.size, 99);
      await peer.close();
    });

    test('reads a PierceFireWall token', () async {
      final client = await Socket.connect('127.0.0.1', listener.port!);
      addTearDown(client.destroy);
      client.add(framePeerInit(
          PeerInitCode.pierceFireWall, SlskOut.pierceFireWall(987654)));
      await client.flush();

      await waitFor(() => handshakes.isNotEmpty);
      expect(handshakes.single.isPierce, isTrue);
      expect(handshakes.single.token, 987654);
    });

    test('a handshake arriving one byte at a time still parses', () async {
      final client = await Socket.connect('127.0.0.1', listener.port!);
      addTearDown(client.destroy);
      final bytes = framePeerInit(
          PeerInitCode.peerInit, SlskOut.peerInit('slowpoke', ConnType.file));
      for (final b in bytes) {
        client.add([b]);
        await client.flush();
      }

      await waitFor(() => handshakes.isNotEmpty);
      expect(handshakes.single.username, 'slowpoke');
      expect(handshakes.single.connType, ConnType.file);
    });

    test('an unknown peer-init code is dropped, not surfaced', () async {
      final client = await Socket.connect('127.0.0.1', listener.port!);
      addTearDown(client.destroy);
      client.add(framePeerInit(77, Uint8List.fromList([1, 2, 3])));
      await client.flush();
      await Future<void>.delayed(const Duration(milliseconds: 200));
      expect(handshakes, isEmpty);
    });
  });

  group('SlskPeerConnection dialling', () {
    test('dialDirect opens with a PeerInit frame', () async {
      final target = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(target.close);
      final wire = <int>[];
      final accepted = target.first.then((s) => s.listen(wire.addAll));

      final conn = await SlskPeerConnection.dialDirect(
        ourUsername: 'alice',
        peerUsername: 'bob',
        address: '127.0.0.1',
        port: target.port,
      );
      addTearDown(conn.close);
      await accepted;

      await waitFor(() => wire.length >= 5);
      final frame =
          (SlskFrameBuffer(codeIsByte: true)..add(wire)).readFrame()!;
      expect(frame.code, PeerInitCode.peerInit);
      final init = PeerInitRequest.parse(frame.payload);
      expect(init.username, 'alice');
      expect(init.connType, ConnType.peer);
    });

    test('dialPierce opens with a PierceFireWall frame', () async {
      final target = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(target.close);
      final wire = <int>[];
      final accepted = target.first.then((s) => s.listen(wire.addAll));

      final conn = await SlskPeerConnection.dialPierce(
        peerUsername: 'bob',
        address: '127.0.0.1',
        port: target.port,
        token: 31337,
      );
      addTearDown(conn.close);
      await accepted;

      await waitFor(() => wire.length >= 5);
      final frame =
          (SlskFrameBuffer(codeIsByte: true)..add(wire)).readFrame()!;
      expect(frame.code, PeerInitCode.pierceFireWall);
      expect(SlskReader(frame.payload).uint32(), 31337);
    });

    test('outbound peer messages are framed with a uint32 code', () async {
      final target = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(target.close);
      final wire = <int>[];
      final accepted = target.first.then((s) => s.listen(wire.addAll));

      final conn = await SlskPeerConnection.dialDirect(
        ourUsername: 'alice',
        peerUsername: 'bob',
        address: '127.0.0.1',
        port: target.port,
      );
      addTearDown(conn.close);
      await accepted;

      conn.send(PeerCode.queueUpload, SlskOut.queueUpload(r'share\x.mp3'));

      await waitFor(() => wire.length > 5);
      // Skip the peer-init frame, then read the peer message.
      final initBuf = SlskFrameBuffer(codeIsByte: true)..add(wire);
      initBuf.readFrame();
      final rest = initBuf.drain();
      final msg = (SlskFrameBuffer()..add(rest)).readFrame()!;
      expect(msg.code, PeerCode.queueUpload);
      expect(SlskReader(msg.payload).str(), r'share\x.mp3');
    });
  });
}
