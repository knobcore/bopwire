// Tests for AlbumArtCache (album_art_cache.dart): extraction of embedded
// cover art during import, byte-verbatim storage, magic-byte validation
// of corrupt images, and the album-level dedupe that keeps a large
// rescan from decoding an image per track (and the node contribution
// from firing more than once per album).
//
// Fixtures are hand-built ID3v2.3 MP3s (APIC frame + a few MPEG frames)
// so the tests are fully self-contained — no real audio needed.

import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/album_art_cache.dart';

/// 1x1 red PNG — a real, valid image.
final png = Uint8List.fromList([
  0x89,0x50,0x4E,0x47,0x0D,0x0A,0x1A,0x0A,0x00,0x00,0x00,0x0D,0x49,0x48,0x44,0x52,
  0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,
  0xDE,0x00,0x00,0x00,0x0C,0x49,0x44,0x41,0x54,0x08,0xD7,0x63,0xF8,0xCF,0xC0,0x00,
  0x00,0x00,0x03,0x00,0x01,0x5B,0xF8,0x2F,0x1E,0x00,0x00,0x00,0x00,0x49,0x45,0x4E,
  0x44,0xAE,0x42,0x60,0x82,
]);

/// A minimal JPEG header + junk — enough to sniff as jpg.
final jpeg = Uint8List.fromList(
    [0xFF, 0xD8, 0xFF, 0xE0, 0x00, 0x10, ...List.filled(64, 0x11)]);

/// Bytes that are not any image format (a "corrupt" embedded picture).
final corrupt = Uint8List.fromList(List.generate(64, (i) => (i * 7) & 0xFF));

/// Hand-built ID3v2.3 MP3: TIT2 + optional APIC + 4 MPEG frames.
Uint8List mp3With({Uint8List? image}) {
  final frames = BytesBuilder();
  void frame(String id, List<int> body) {
    frames.add(id.codeUnits);
    final n = body.length;
    frames.add([(n >> 24) & 0xFF, (n >> 16) & 0xFF, (n >> 8) & 0xFF, n & 0xFF]);
    frames.add([0, 0]);
    frames.add(body);
  }
  frame('TIT2', [0x00, ...'Test Song'.codeUnits]);
  if (image != null) {
    frame('APIC',
        [0x00, ...'image/png'.codeUnits, 0x00, 0x03, 0x00, ...image]);
  }
  final body = frames.toBytes();
  final n = body.length;
  final out = BytesBuilder();
  out.add('ID3'.codeUnits);
  out.add([0x03, 0x00, 0x00]);
  out.add([(n >> 21) & 0x7F, (n >> 14) & 0x7F, (n >> 7) & 0x7F, n & 0x7F]);
  out.add(body);
  for (var i = 0; i < 4; i++) {
    out.add([0xFF, 0xFB, 0x90, 0x00]);
    out.add(Uint8List(413));
  }
  return out.toBytes();
}

void main() {
  late Directory tmp;

  setUp(() async {
    tmp = await Directory.systemTemp.createTemp('art_test');
    AlbumArtCache.debugDir = Directory('${tmp.path}/art');
  });

  tearDown(() async {
    AlbumArtCache.debugDir = null;
    try { await tmp.delete(recursive: true); } catch (_) {}
  });

  Future<File> audioFile(String name, {Uint8List? image}) async {
    final f = File('${tmp.path}/$name');
    await f.writeAsBytes(mp3With(image: image));
    return f;
  }

  test('sniffExtension recognises real formats and rejects garbage', () {
    expect(AlbumArtCache.sniffExtension(png), 'png');
    expect(AlbumArtCache.sniffExtension(jpeg), 'jpg');
    expect(AlbumArtCache.sniffExtension(corrupt), isNull);
    expect(AlbumArtCache.sniffExtension(Uint8List(0)), isNull);
    expect(AlbumArtCache.sniffExtension(Uint8List.fromList([0xFF, 0xD8])),
        isNull); // too short
  });

  test('file WITH embedded art: extracted, stored verbatim, findable', () async {
    final f = await audioFile('a.mp3', image: png);
    final returned =
        await AlbumArtCache.extractFromAudioFile(f, 'NOFX', 'Punk in Drublic');
    expect(returned, isNotNull,
        reason: 'newly extracted art must be returned for node contribution');
    expect(returned, equals(png));
    final cached = await AlbumArtCache.cachedArt('NOFX', 'Punk in Drublic');
    expect(cached, isNotNull);
    expect(cached!.path, endsWith('.png'));
    // Bytes as they came out of the file — no re-encode, no resize.
    expect(await cached.readAsBytes(), equals(png));
  });

  test('cache key matches the provider normalization: case/whitespace '
      'variants and empty-album=singles resolve to the same art', () async {
    final f = await audioFile('a.mp3', image: png);
    await AlbumArtCache.extractFromAudioFile(f, ' NOFX ', 'Punk  In Drublic');
    expect(await AlbumArtCache.cachedArt('nofx', 'punk in drublic'),
        isNotNull);
    expect(await AlbumArtCache.cachedArt('NOFX', 'punk in  DRUBLIC '),
        isNotNull);
    expect(await AlbumArtCache.cachedArt('NOFX', 'some other album'), isNull);
    // Empty album buckets under 'singles', same as LibraryProvider._artKey.
    final s = await audioFile('single.mp3', image: png);
    await AlbumArtCache.extractFromAudioFile(s, 'NOFX', '');
    expect(await AlbumArtCache.cachedArt('NOFX', ''), isNotNull);
  });

  test('file WITHOUT embedded art: nothing stored, nothing returned',
      () async {
    final f = await audioFile('plain.mp3');
    final r = await AlbumArtCache.extractFromAudioFile(f, 'NOFX', 'Ribbed');
    expect(r, isNull);
    expect(await AlbumArtCache.cachedArt('NOFX', 'Ribbed'), isNull);
  });

  test('corrupt embedded image: rejected by magic sniff, never cached, '
      'never thrown', () async {
    final f = await audioFile('bad.mp3', image: corrupt);
    final r = await AlbumArtCache.extractFromAudioFile(f, 'NOFX', 'Ribbed');
    expect(r, isNull);
    expect(await AlbumArtCache.cachedArt('NOFX', 'Ribbed'), isNull);
  });

  test('a non-audio file never throws', () async {
    final junk = File('${tmp.path}/junk.mp3');
    await junk.writeAsBytes(corrupt);
    final r = await AlbumArtCache.extractFromAudioFile(
        junk, 'NOFX', 'Punk in Drublic');
    expect(r, isNull);
  });

  test('album-level dedupe: second track of the same album is skipped '
      '(first art wins, no re-return for re-upload)', () async {
    final t1 = await audioFile('t1.mp3', image: png);
    final t2 = await audioFile('t2.mp3', image: jpeg); // different art!
    final r1 = await AlbumArtCache.extractFromAudioFile(
        t1, 'NOFX', 'Punk in Drublic');
    expect(r1, isNotNull);
    final r2 = await AlbumArtCache.extractFromAudioFile(
        t2, 'NOFX', 'Punk in Drublic');
    expect(r2, isNull,
        reason: 'dedupe hit must not re-extract or trigger a second '
            'node upload');
    final cached = await AlbumArtCache.cachedArt('NOFX', 'Punk in Drublic');
    expect(await cached!.readAsBytes(), equals(png),
        reason: 'first extracted art is kept');
    // A DIFFERENT album still extracts.
    final r3 = await AlbumArtCache.extractFromAudioFile(
        t2, 'NOFX', 'The War on Errorism');
    expect(r3, isNotNull);
  });

  test('storeBytes refuses empty artist and empty bytes', () async {
    expect(await AlbumArtCache.storeBytes(png, '', 'X'), isNull);
    expect(await AlbumArtCache.storeBytes(Uint8List(0), 'NOFX', 'X'), isNull);
  });
}
