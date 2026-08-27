// importDownloadedFile: a track pulled off an external network must land
// in the LOCAL library even when everything else is missing — no full
// node, no RatsClient, no platform fingerprint decoder (the exact
// situation on desktop Linux, and in this test). Chain registration is a
// separate concern that retries on a later scan; the entry itself must
// never be silently dropped.
//
// What these tests prove: the local-library half of the download→library
// pipeline (extension gate, tag/filename metadata, content hashing,
// upsert, dedup on re-import, persistence to prefs). What they cannot
// prove here: fingerprint.submit / library.delta against a real full
// node — that needs a reachable node and a platform decoder, neither of
// which exists in a unit test.

import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/library_scanner.dart';
import 'package:bopwire_player/src/services/library_service.dart';

/// Minimal but valid RIFF/WAVE container: 0.25 s of 8 kHz mono s16le
/// noise. Enough for the scanner's byte-hash + tag-fallback path; the
/// fingerprint step fails on this host (no Linux decoder) which is part
/// of what we are testing.
Uint8List _tinyWav() {
  const sampleRate = 8000;
  const samples = sampleRate ~/ 4;
  final data = ByteData(44 + samples * 2);
  void putAscii(int off, String s) {
    for (var i = 0; i < s.length; i++) {
      data.setUint8(off + i, s.codeUnitAt(i));
    }
  }

  putAscii(0, 'RIFF');
  data.setUint32(4, 36 + samples * 2, Endian.little);
  putAscii(8, 'WAVE');
  putAscii(12, 'fmt ');
  data.setUint32(16, 16, Endian.little); // fmt chunk size
  data.setUint16(20, 1, Endian.little); // PCM
  data.setUint16(22, 1, Endian.little); // mono
  data.setUint32(24, sampleRate, Endian.little);
  data.setUint32(28, sampleRate * 2, Endian.little); // byte rate
  data.setUint16(32, 2, Endian.little); // block align
  data.setUint16(34, 16, Endian.little); // bits per sample
  putAscii(36, 'data');
  data.setUint32(40, samples * 2, Endian.little);
  for (var i = 0; i < samples; i++) {
    data.setInt16(44 + i * 2, ((i * 2654435761) & 0xFFFF) - 0x8000,
        Endian.little);
  }
  return data.buffer.asUint8List();
}

/// A real FLAC downloaded from napstr in an earlier live test, when one
/// is lying around — exercises the real tag-read path. Falls back to the
/// synthetic WAV so the test is hermetic on other machines.
Future<File> _sourceAudio(Directory tmp) async {
  final liveDirs = Directory('/tmp')
      .listSync()
      .whereType<Directory>()
      .where((d) => d.path.contains('napstr-live'));
  for (final d in liveDirs) {
    for (final f in d.listSync().whereType<File>()) {
      if (f.path.toLowerCase().endsWith('.flac') && f.lengthSync() > 0) {
        return f.copy('${tmp.path}/${f.uri.pathSegments.last}');
      }
    }
  }
  final wav = File('${tmp.path}/Fallback_Artist - Fallback_Song.wav');
  await wav.writeAsBytes(_tinyWav());
  return wav;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();
  SharedPreferences.setMockInitialValues({});

  late Directory tmp;

  setUpAll(() async {
    tmp = await Directory.systemTemp.createTemp('bopwire-import-test');
  });

  tearDownAll(() async {
    try {
      await tmp.delete(recursive: true);
    } catch (_) {}
  });

  test(
      'importDownloadedFile records a library entry with no node, '
      'no RatsClient and no fingerprint decoder', () async {
    final src = await _sourceAudio(tmp);
    final bytes = await src.readAsBytes();
    final wantHash = crypto.sha256.convert(bytes).toString();

    final lib = LibraryService.instance;
    await lib.ensureLoaded();
    final before = lib.entries.length;

    final ok =
        await LibraryScanner.instance.importDownloadedFile(src.path);
    expect(ok, isTrue,
        reason: 'import must succeed even with no node reachable');

    expect(lib.entries.length, before + 1,
        reason: 'exactly one new library entry');
    final entry = lib.entryByPath(src.path);
    expect(entry, isNotNull, reason: 'entry indexed by file path');
    expect(entry!.contentHash, wantHash,
        reason: 'content hash is sha256 of the file bytes');
    expect(entry.title.trim(), isNotEmpty,
        reason: 'title from tags, or the filename fallback');
    expect(entry.isLocal, isTrue);
    // No decoder on this host → the fingerprint must be EMPTY, not a
    // crash; an empty fingerprintHash is what makes a later force-scan
    // retry the fingerprint + chain submit.
    expect(entry.fingerprintHash, isEmpty,
        reason: 'Linux has no PCM decoder; entry must still exist');

    // The entry must be PERSISTED (this is "the library db"), not just
    // sitting in the in-memory maps.
    final prefs = await SharedPreferences.getInstance();
    final raw = prefs.getString('mc_library_entries') ?? '';
    expect(raw, isNotEmpty);
    final listed = (jsonDecode(raw) as List)
        .map((e) => (e as Map)['content_hash'])
        .toList();
    expect(listed, contains(wantHash));
  }, timeout: const Timeout(Duration(minutes: 3)));

  test('re-importing the same file does not duplicate the entry', () async {
    final src = await _sourceAudio(tmp);
    final lib = LibraryService.instance;
    await lib.ensureLoaded();

    await LibraryScanner.instance.importDownloadedFile(src.path);
    final count = lib.entries.length;
    final ok =
        await LibraryScanner.instance.importDownloadedFile(src.path);
    expect(ok, isTrue);
    expect(lib.entries.length, count, reason: 'upsert, not append');
  }, timeout: const Timeout(Duration(minutes: 3)));

  test('a .part file (unfinished transfer) is refused, not imported',
      () async {
    final part = File('${tmp.path}/half done song.flac.part');
    await part.writeAsBytes(List<int>.filled(1024, 0x42));
    final ok =
        await LibraryScanner.instance.importDownloadedFile(part.path);
    expect(ok, isFalse);
    expect(LibraryService.instance.entryByPath(part.path), isNull);
  });

  test('a missing path is refused', () async {
    final ok = await LibraryScanner.instance
        .importDownloadedFile('${tmp.path}/nope.mp3');
    expect(ok, isFalse);
  });
}
