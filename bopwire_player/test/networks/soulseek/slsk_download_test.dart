// The file-transfer state machine, driven over a loopback socket pair by a
// fake uploader that speaks the real 'F' connection sequence:
//
//   uploader -> unframed uint32 transfer token
//   us       -> unframed uint64 resume offset
//   uploader -> raw file bytes until the advertised size is reached
//
// This is the part with the most ways to go quietly wrong (partial files left
// behind, truncation, completion never firing), so it is exercised end to end.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';
import 'package:bopwire_player/src/services/networks/soulseek/slsk_download.dart';

/// A loopback socket pair plus the fake uploader's side of it.
class _Wire {
  _Wire(this.server, this.ours, this.theirs);
  final ServerSocket server;

  /// The socket our download code reads from and writes the offset to.
  final Socket ours;

  /// The fake uploader's end.
  final Socket theirs;

  static Future<_Wire> create() async {
    final server = await ServerSocket.bind(InternetAddress.loopbackIPv4, 0);
    final pending = server.first;
    final ours = await Socket.connect('127.0.0.1', server.port);
    final theirs = await pending;
    return _Wire(server, ours, theirs);
  }

  Future<void> dispose() async {
    ours.destroy();
    theirs.destroy();
    await server.close();
  }
}

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
  late Directory destDir;

  setUp(() {
    destDir = Directory.systemTemp.createTempSync('slsk_dl_test');
  });

  tearDown(() {
    if (destDir.existsSync()) destDir.deleteSync(recursive: true);
  });

  /// Builds a download plus the progress events it emits.
  (SlskDownload, List<DownloadProgress>, StreamController<DownloadProgress>)
      makeDownload({
    required String virtualPath,
    int? size,
    void Function(SlskDownload)? onFinished,
  }) {
    final events = <DownloadProgress>[];
    final ctl = StreamController<DownloadProgress>();
    ctl.stream.listen(events.add);
    final d = SlskDownload(
      trackId: 'track-1',
      username: 'uploader',
      virtualPath: virtualPath,
      expectedSize: size,
      destDir: destDir.path,
      controller: ctl,
      log: (_) {},
      onFinished: onFinished ?? (_) {},
    );
    return (d, events, ctl);
  }

  group('file name handling', () {
    test('takes the leaf of a Windows-style share path', () {
      expect(
        SlskDownload.sanitizeFileName(r'@@abc\Music\Album\01 - Track.mp3'),
        '01 - Track.mp3',
      );
    });

    test('replaces characters that are illegal in a local filename', () {
      expect(SlskDownload.sanitizeFileName(r'dir\a:b*c?d"e<f>g|h.mp3'),
          'a_b_c_d_e_f_g_h.mp3');
    });

    test('falls back to a placeholder when nothing usable is left', () {
      expect(SlskDownload.sanitizeFileName('dir\\'), 'soulseek-download');
    });
  });

  group('transfer over a socket', () {
    test('answers the token with an offset and writes the file', () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      final payload =
          Uint8List.fromList(List.generate(50000, (i) => (i * 7) & 0xFF));
      var finished = false;
      final (d, events, _) = makeDownload(
        virtualPath: r'@@x\Album\song.mp3',
        size: payload.length,
        onFinished: (_) => finished = true,
      );
      d.token = 4242;

      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 4242 ? d : null,
      );

      // Uploader sends the token, then waits for our 8-byte offset.
      final theirBytes = <int>[];
      wire.theirs.listen(theirBytes.addAll);
      wire.theirs.add((SlskWriter()..uint32(4242)).take());
      await wire.theirs.flush();

      await waitFor(() => theirBytes.length >= 8);
      expect(SlskReader(Uint8List.fromList(theirBytes)).uint64(), 0,
          reason: 'a fresh download resumes from offset zero');

      // Then the file, in a few chunks.
      for (var i = 0; i < payload.length; i += 8192) {
        wire.theirs.add(payload.sublist(
            i, (i + 8192).clamp(0, payload.length)));
        await wire.theirs.flush();
      }

      await waitFor(() => finished);
      final done = events.lastWhere((e) => e.done, orElse: () => events.last);
      expect(done.done, isTrue, reason: 'events were: $events');
      expect(done.error, isNull);
      expect(done.receivedBytes, payload.length);
      expect(done.totalBytes, payload.length);
      expect(done.localPath, isNotNull);

      final written = File(done.localPath!);
      expect(written.existsSync(), isTrue);
      expect(written.readAsBytesSync(), orderedEquals(payload));
      expect(written.path, endsWith('song.mp3'));

      // No .slskpart residue.
      expect(
        destDir.listSync().where((f) => f.path.endsWith('.slskpart')),
        isEmpty,
      );
    });

    test('ignores bytes sent past the advertised size', () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      var finished = false;
      final (d, events, _) = makeDownload(
        virtualPath: r'x\short.mp3',
        size: 10,
        onFinished: (_) => finished = true,
      );
      d.token = 1;

      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 1 ? d : null,
      );

      wire.theirs.add((SlskWriter()..uint32(1)).take());
      // 25 bytes for a 10-byte file — a misbehaving or padding peer.
      wire.theirs.add(List<int>.generate(25, (i) => i));
      await wire.theirs.flush();

      await waitFor(() => finished);
      final done = events.last;
      expect(done.done, isTrue);
      expect(done.receivedBytes, 10);
      expect(File(done.localPath!).lengthSync(), 10);
    });

    test('reports an error and deletes the partial file on an early close',
        () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      var finished = false;
      final (d, events, _) = makeDownload(
        virtualPath: r'x\truncated.mp3',
        size: 1000,
        onFinished: (_) => finished = true,
      );
      d.token = 2;

      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 2 ? d : null,
      );

      wire.theirs.add((SlskWriter()..uint32(2)).take());
      wire.theirs.add(List<int>.filled(100, 1));
      await wire.theirs.flush();
      await Future<void>.delayed(const Duration(milliseconds: 50));
      await wire.theirs.close();

      await waitFor(() => finished);
      final last = events.last;
      expect(last.done, isFalse);
      expect(last.error, isNotNull);
      expect(last.error, contains('100 of 1000'));
      expect(destDir.listSync(), isEmpty,
          reason: 'the partial file must not be left behind');
    });

    test('a token we do not recognise drops the connection', () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      var lookups = 0;
      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) {
          lookups++;
          return null;
        },
      );

      var closed = false;
      wire.theirs.listen((_) {}, onDone: () => closed = true);
      wire.theirs.add((SlskWriter()..uint32(9999)).take());
      await wire.theirs.flush();

      await waitFor(() => lookups == 1);
      await waitFor(() => closed);
      expect(destDir.listSync(), isEmpty);
    });

    test('a token split across two packets is still matched', () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      final (d, events, _) = makeDownload(
        virtualPath: r'x\split.mp3',
        size: 4,
      );
      d.token = 0x01020304;

      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 0x01020304 ? d : null,
      );

      final token = (SlskWriter()..uint32(0x01020304)).take();
      wire.theirs.add(token.sublist(0, 1));
      await wire.theirs.flush();
      await Future<void>.delayed(const Duration(milliseconds: 30));
      wire.theirs.add(token.sublist(1));
      wire.theirs.add([1, 2, 3, 4]);
      await wire.theirs.flush();

      await waitFor(() => events.any((e) => e.done));
      expect(File(events.last.localPath!).readAsBytesSync(),
          orderedEquals([1, 2, 3, 4]));
    });

    test('abort() removes the partial file and stops the transfer', () async {
      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      final (d, events, _) = makeDownload(
        virtualPath: r'x\cancelled.mp3',
        size: 100000,
      );
      d.token = 3;

      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 3 ? d : null,
      );

      wire.theirs.add((SlskWriter()..uint32(3)).take());
      wire.theirs.add(List<int>.filled(2000, 9));
      await wire.theirs.flush();

      await waitFor(() => d.receivedBytes >= 2000);
      await d.abort();

      expect(d.isFinished, isTrue);
      expect(events.any((e) => e.done), isFalse);
      expect(destDir.listSync(), isEmpty);
    });

    test('a second download of the same name does not overwrite the first',
        () async {
      File('${destDir.path}${Platform.pathSeparator}dupe.mp3')
          .writeAsBytesSync([0]);

      final wire = await _Wire.create();
      addTearDown(wire.dispose);

      final (d, events, _) =
          makeDownload(virtualPath: r'x\dupe.mp3', size: 3);
      d.token = 5;
      attachFileConnection(
        socket: wire.ours,
        data: wire.ours,
        lookup: (t) => t == 5 ? d : null,
      );

      wire.theirs.add((SlskWriter()..uint32(5)).take());
      wire.theirs.add([7, 7, 7]);
      await wire.theirs.flush();

      await waitFor(() => events.any((e) => e.done));
      final path = events.last.localPath!;
      expect(path, endsWith('dupe (2).mp3'));
      expect(File(path).readAsBytesSync(), orderedEquals([7, 7, 7]));
      // The pre-existing file is untouched.
      expect(
        File('${destDir.path}${Platform.pathSeparator}dupe.mp3')
            .readAsBytesSync(),
        orderedEquals([0]),
      );
    });

    test('fail() emits an error and cleans up', () async {
      final (d, events, _) = makeDownload(virtualPath: r'x\y.mp3', size: 10);
      d.fail('uploader banned us');
      expect(d.isFinished, isTrue);
      await waitFor(() => events.isNotEmpty);
      expect(events.last.error, 'uploader banned us');
      expect(events.last.done, isFalse);
    });
  });
}
