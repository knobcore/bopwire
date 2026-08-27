// Reproduces the reported seek behaviour: while a file is still
// downloading, seek BACK into a region that has already arrived and
// check that audio bytes actually come back.
//
// mpv implements a seek by opening a NEW request carrying a Range header
// (often abandoning the first connection). So the thing under test is a
// second, ranged request served against a source whose file is still
// growing — not the initial stream.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:bopwire_player/src/services/networks/predownload_cache.dart';

void main() {
  late Directory tmp;
  late GrowingFileServer server;

  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('seekresume');
    server = GrowingFileServer.forTesting();
    await server.ensureStarted();
  });

  tearDown(() async {
    await server.dispose();
    if (tmp.existsSync()) tmp.deleteSync(recursive: true);
  });

  test('seeking back into downloaded bytes returns audio, not silence',
      () async {
    const total = 300 * 1024;
    final file = File('${tmp.path}/track.part');
    await file.writeAsBytes(Uint8List(0));

    var written = 0;
    var complete = false;
    // Deterministic content so we can assert the bytes are the RIGHT ones,
    // not just that something arrived.
    Uint8List blockAt(int offset, int len) =>
        Uint8List.fromList(List.generate(len, (i) => (offset + i) % 251));

    final url = await server.serve(GrowingFileSource(
      resolve: () async => file,
      totalBytes: () => total,
      isComplete: () => complete,
      isAborted: () => false,
    ));

    // Writer: fill the file in 32 KB steps.
    final writer = Timer.periodic(const Duration(milliseconds: 20), (t) async {
      if (written >= total) {
        complete = true;
        t.cancel();
        return;
      }
      final len = (written + 32 * 1024 > total) ? total - written : 32 * 1024;
      await file.writeAsBytes(blockAt(written, len), mode: FileMode.append);
      written += len;
    });
    addTearDown(writer.cancel);

    // Let a chunk of the file land.
    while (written < 128 * 1024) {
      await Future<void>.delayed(const Duration(milliseconds: 10));
    }

    // THE SEEK: a fresh request for a region that has definitely arrived.
    const seekTo = 64 * 1024;
    final client = HttpClient();
    final req = await client.getUrl(Uri.parse(url));
    req.headers.set(HttpHeaders.rangeHeader, 'bytes=$seekTo-');
    final resp = await req.close();

    expect(resp.statusCode, HttpStatus.partialContent,
        reason: 'a seek must be answered with 206, or mpv treats the '
            'stream as non-seekable');

    // Read just enough to prove real audio bytes flow from the seek point.
    final got = BytesBuilder();
    await for (final chunk in resp) {
      got.add(chunk);
      if (got.length >= 32 * 1024) break;
    }
    client.close(force: true);

    expect(got.length, greaterThanOrEqualTo(32 * 1024),
        reason: 'seeking into downloaded bytes returned nothing — this is '
            'the silence the user reported');
    expect(got.toBytes().sublist(0, 64), blockAt(seekTo, 64),
        reason: 'bytes served from the seek offset are the wrong ones');
  }, timeout: const Timeout(Duration(seconds: 60)));
}
