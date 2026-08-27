// End-to-end exercise of the download path: SOCKS5 handshake, transfer
// protocol v2 framing, SHA-256 verification and cancellation.
//
// A local harness plays both roles — it speaks SOCKS5, then (instead of
// really dialling an onion) turns around and speaks the seeder side of the
// protocol. That covers everything except Tor itself, which cannot be run
// from a test.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/napstr/socks5.dart';
import 'package:bopwire_player/src/services/networks/napstr/transfer.dart';

/// How the fake seeder should misbehave, if at all.
enum SeederBehaviour {
  wellBehaved,
  unauthorized,
  wrongWelcomeSize,
  corruptBytes,
  wrongProtocolVersion,
}

class FakeSeeder {
  FakeSeeder(this.payload, this.filename, {this.behaviour = SeederBehaviour.wellBehaved});

  final Uint8List payload;
  final String filename;
  final SeederBehaviour behaviour;

  ServerSocket? _server;
  String? connectedHost;
  int? connectedPort;

  String get fileId => crypto.sha256.convert(payload).toString();
  Socks5Endpoint get endpoint =>
      Socks5Endpoint('127.0.0.1', _server!.port);

  Future<void> start() async {
    _server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    _server!.listen((socket) => _handle(socket).catchError((_) {}));
  }

  Future<void> stop() async => _server?.close();

  Future<void> _handle(Socket socket) async {
    final reader = ByteReader(socket);
    const t = Duration(seconds: 10);

    // --- SOCKS5 ---
    final greeting = await reader.readExactly(3, timeout: t);
    expect(greeting[0], 0x05);
    socket.add([0x05, 0x00]);
    final head = await reader.readExactly(4, timeout: t);
    expect(head[1], 0x01); // CONNECT
    expect(head[3], 0x03); // DOMAINNAME — never pre-resolved
    final len = (await reader.readExactly(1, timeout: t))[0];
    connectedHost =
        String.fromCharCodes(await reader.readExactly(len, timeout: t));
    final portBytes = await reader.readExactly(2, timeout: t);
    connectedPort = (portBytes[0] << 8) | portBytes[1];
    socket.add([0x05, 0x00, 0x00, 0x01, 0, 0, 0, 0, 0, 0]);
    await socket.flush();

    // --- napstr transfer protocol v2 ---
    final hello = await _frame(reader);
    expect(hello['type'], 'HELLO');
    expect(hello['version'], 2);
    expect(hello['file_id'], fileId);

    if (behaviour == SeederBehaviour.unauthorized) {
      socket.add(encodeFrame({
        'type': 'ERROR',
        'code': 'UNAUTHORIZED',
        'message': 'capability is invalid or expired',
      }));
      await socket.flush();
      await socket.close();
      return;
    }

    socket.add(encodeFrame({
      'type': 'WELCOME',
      'version':
          behaviour == SeederBehaviour.wrongProtocolVersion ? 3 : 2,
      'file_id': fileId,
      'filename': filename,
      'size': behaviour == SeederBehaviour.wrongWelcomeSize
          ? payload.length + 1
          : payload.length,
    }));
    await socket.flush();

    final request = await _frame(reader);
    expect(request['type'], 'REQUEST_FILE');

    socket.add(encodeFrame({
      'type': 'FILE_DATA',
      'size': payload.length,
      'sha256': fileId,
    }));
    final body = Uint8List.fromList(payload);
    if (behaviour == SeederBehaviour.corruptBytes) body[0] ^= 0xff;
    // Deliberately dribbled out, so the reader's chunk loop is exercised.
    for (var i = 0; i < body.length; i += 1000) {
      socket.add(body.sublist(i, i + 1000 > body.length ? body.length : i + 1000));
      await socket.flush();
    }

    final complete = await _frame(reader);
    if (complete['type'] == 'TRANSFER_COMPLETE') {
      socket.add(encodeFrame({'type': 'TRANSFER_COMPLETE'}));
      await socket.flush();
    }
    await socket.close();
  }

  Future<Map<String, Object?>> _frame(ByteReader reader) =>
      readFrame(reader, timeout: const Duration(seconds: 10));
}

DownloadOffer offerFor(String fileId) => DownloadOffer(
      requestId: '42f9ac7c-fd56-475c-9a6d-adcc35a1f826',
      fileId: fileId,
      onion: '${'a' * 56}.onion',
      port: 80,
      capability: 'c' * 64,
      expiresAt: DateTime.now().millisecondsSinceEpoch ~/ 1000 + 900,
      seederPubkey: 'b' * 64,
    );

void main() {
  late Directory dir;
  late Uint8List payload;

  setUp(() async {
    dir = await Directory.systemTemp.createTemp('napstr_transfer_test');
    // ~48 KB so the transfer spans many read chunks.
    payload = Uint8List.fromList(
        List<int>.generate(48 * 1024, (i) => (i * 31 + 7) & 0xff));
  });

  tearDown(() async {
    if (await dir.exists()) await dir.delete(recursive: true);
  });

  test('downloads a file, verifies its digest and writes it to disk',
      () async {
    final seeder = FakeSeeder(payload, 'song.mp3');
    await seeder.start();
    addTearDown(seeder.stop);

    final dest = '${dir.path}/song.mp3';
    final progress = <int>[];
    final path = await downloadFromOffer(
      proxy: seeder.endpoint,
      offer: offerFor(seeder.fileId),
      expectedSize: payload.length,
      destinationPath: dest,
      onProgress: (received, _) => progress.add(received),
    );

    expect(path, dest);
    final written = await File(dest).readAsBytes();
    expect(written, payload);
    expect(crypto.sha256.convert(written).toString(), seeder.fileId);

    // The onion went to the proxy as a hostname, unresolved.
    expect(seeder.connectedHost, '${'a' * 56}.onion');
    expect(seeder.connectedPort, 80);

    expect(progress, isNotEmpty);
    expect(progress.last, payload.length);
    expect(progress, orderedEquals(List.of(progress)..sort()));
    // No .part file is left behind.
    expect(await File('$dest.part').exists(), isFalse);
  });

  test('rejects bytes that do not hash to the catalogue file id', () async {
    final seeder =
        FakeSeeder(payload, 'song.mp3', behaviour: SeederBehaviour.corruptBytes);
    await seeder.start();
    addTearDown(seeder.stop);

    final dest = '${dir.path}/song.mp3';
    await expectLater(
      downloadFromOffer(
        proxy: seeder.endpoint,
        offer: offerFor(seeder.fileId),
        expectedSize: payload.length,
        destinationPath: dest,
      ),
      throwsA(isA<TransferException>().having(
          (e) => e.message, 'message', contains('do not hash'))),
    );
    // Nothing corrupt is left where the import pipeline could pick it up.
    expect(await File(dest).exists(), isFalse);
    expect(await File('$dest.part').exists(), isFalse);
  });

  test('rejects a WELCOME whose size disagrees with the signed catalogue',
      () async {
    final seeder = FakeSeeder(payload, 'song.mp3',
        behaviour: SeederBehaviour.wrongWelcomeSize);
    await seeder.start();
    addTearDown(seeder.stop);

    await expectLater(
      downloadFromOffer(
        proxy: seeder.endpoint,
        offer: offerFor(seeder.fileId),
        expectedSize: payload.length,
        destinationPath: '${dir.path}/song.mp3',
      ),
      throwsA(isA<TransferException>()
          .having((e) => e.message, 'message', contains('disagrees'))),
    );
  });

  test('rejects an unsupported transfer protocol version', () async {
    final seeder = FakeSeeder(payload, 'song.mp3',
        behaviour: SeederBehaviour.wrongProtocolVersion);
    await seeder.start();
    addTearDown(seeder.stop);

    await expectLater(
      downloadFromOffer(
        proxy: seeder.endpoint,
        offer: offerFor(seeder.fileId),
        expectedSize: payload.length,
        destinationPath: '${dir.path}/song.mp3',
      ),
      throwsA(isA<TransferException>()
          .having((e) => e.message, 'message', contains('version'))),
    );
  });

  test('surfaces an ERROR frame by code, not by its remote-supplied text',
      () async {
    final seeder = FakeSeeder(payload, 'song.mp3',
        behaviour: SeederBehaviour.unauthorized);
    await seeder.start();
    addTearDown(seeder.stop);

    await expectLater(
      downloadFromOffer(
        proxy: seeder.endpoint,
        offer: offerFor(seeder.fileId),
        expectedSize: payload.length,
        destinationPath: '${dir.path}/song.mp3',
      ),
      throwsA(isA<TransferException>()
          .having((e) => e.message, 'message', contains('UNAUTHORIZED'))),
    );
  });

  test('an aborted transfer stops and cleans up', () async {
    // Large enough to span several 64 KiB read chunks, so there is a
    // partial transfer left to actually abort.
    final big = Uint8List.fromList(
        List<int>.generate(512 * 1024, (i) => (i * 17 + 3) & 0xff));
    final seeder = FakeSeeder(big, 'song.mp3');
    await seeder.start();
    addTearDown(seeder.stop);

    final dest = '${dir.path}/song.mp3';
    final cancel = TransferCancel();
    final future = downloadFromOffer(
      proxy: seeder.endpoint,
      offer: offerFor(seeder.fileId),
      expectedSize: big.length,
      destinationPath: dest,
      cancel: cancel,
      onProgress: (received, _) {
        if (received > 0) cancel.abort();
      },
    );
    await expectLater(future, throwsA(isA<Object>()));
    expect(await File(dest).exists(), isFalse);
    expect(await File('$dest.part').exists(), isFalse);
  });

  test('refuses to dial anything that is not a v3 onion', () async {
    final seeder = FakeSeeder(payload, 'song.mp3');
    await seeder.start();
    addTearDown(seeder.stop);

    final clearnet = DownloadOffer(
      requestId: 'r',
      fileId: seeder.fileId,
      onion: 'evil.example.com',
      port: 80,
      capability: 'c' * 64,
      expiresAt: DateTime.now().millisecondsSinceEpoch ~/ 1000 + 900,
      seederPubkey: 'b' * 64,
    );
    await expectLater(
      downloadFromOffer(
        proxy: seeder.endpoint,
        offer: clearnet,
        expectedSize: payload.length,
        destinationPath: '${dir.path}/song.mp3',
      ),
      throwsA(isA<TransferException>()
          .having((e) => e.message, 'message', contains('v3 onion'))),
    );
    expect(seeder.connectedHost, isNull, reason: 'no connection attempted');
  });

  group('SOCKS5 failures', () {
    test('reports a refused CONNECT rather than hanging', () async {
      final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(server.close);
      server.listen((socket) async {
        final reader = ByteReader(socket);
        await reader.readExactly(3, timeout: const Duration(seconds: 5));
        socket.add([0x05, 0x00]);
        await socket.flush();
        await reader.readExactly(4 + 1, timeout: const Duration(seconds: 5));
        // 0x04 = host unreachable, what Tor returns for a dead onion.
        socket.add([0x05, 0x04, 0x00, 0x01, 0, 0, 0, 0, 0, 0]);
        await socket.flush();
        await socket.close();
      });

      await expectLater(
        socks5Connect(Socks5Endpoint('127.0.0.1', server.port),
            '${'a' * 56}.onion', 80),
        throwsA(isA<Socks5Exception>()
            .having((e) => e.message, 'message', contains('unreachable'))),
      );
    });

    test('rejects a proxy that demands authentication', () async {
      final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(server.close);
      server.listen((socket) async {
        final reader = ByteReader(socket);
        await reader.readExactly(3, timeout: const Duration(seconds: 5));
        socket.add([0x05, 0x02]); // username/password required
        await socket.flush();
        await socket.close();
      });

      await expectLater(
        socks5Connect(Socks5Endpoint('127.0.0.1', server.port),
            '${'a' * 56}.onion', 80),
        throwsA(isA<Socks5Exception>()
            .having((e) => e.message, 'message', contains('authentication'))),
      );
    });

    test('the reachability probe answers honestly', () async {
      final good = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      addTearDown(good.close);
      good.listen((socket) async {
        final reader = ByteReader(socket);
        await reader.readExactly(3, timeout: const Duration(seconds: 5));
        socket.add([0x05, 0x00]);
        await socket.flush();
      });
      expect(
          await socks5ProxyReachable(Socks5Endpoint('127.0.0.1', good.port)),
          isTrue);

      // A port with nothing on it.
      final dead = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
      final deadPort = dead.port;
      await dead.close();
      expect(
        await socks5ProxyReachable(Socks5Endpoint('127.0.0.1', deadPort),
            timeout: const Duration(seconds: 2)),
        isFalse,
      );
    });
  });

  test('ByteReader delivers exactly the requested byte counts', () async {
    final source = StreamController<List<int>>();
    final reader = ByteReader(source.stream);
    source
      ..add([1, 2, 3])
      ..add([4, 5])
      ..add(List<int>.filled(10, 9));
    expect(await reader.readExactly(4), [1, 2, 3, 4]);
    expect(await reader.readExactly(1), [5]);
    expect((await reader.readExactly(10)).length, 10);
    unawaited(source.close());
    await expectLater(reader.readExactly(1), throwsA(isA<Socks5Exception>()));
  });

  test('readFrame rejects an oversized declared length', () async {
    final source = StreamController<List<int>>();
    final reader = ByteReader(source.stream);
    final header = Uint8List(4);
    ByteData.sublistView(header).setUint32(0, 1 << 20, Endian.big);
    source.add(header);
    await expectLater(
      readFrame(reader, timeout: const Duration(seconds: 5)),
      throwsA(isA<TransferException>()
          .having((e) => e.message, 'message', contains('frame size'))),
    );
    unawaited(source.close());
  });

  test('a control frame survives a JSON round trip through the socket',
      () async {
    final source = StreamController<List<int>>();
    final reader = ByteReader(source.stream);
    source.add(encodeFrame({'type': 'WELCOME', 'version': 2, 'size': 42}));
    final frame = await readFrame(reader, timeout: const Duration(seconds: 5));
    expect(frame['type'], 'WELCOME');
    expect(frame['size'], 42);
    expect(utf8.encode(jsonEncode(frame)).length, lessThan(kMaxControlFrame));
    unawaited(source.close());
  });
}
