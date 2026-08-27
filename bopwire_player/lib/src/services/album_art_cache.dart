// Local on-disk cache for album art embedded in audio files.
//
// Files downloaded from Soulseek/napstr very often ship the correct
// cover for their exact release inside the container (ID3 APIC, FLAC
// PICTURE, MP4 covr). The import path extracts it here so the display
// layer can prefer it over the node's scraped cover and the generated
// deterministic art:
//   1. embedded art from the file            (THIS cache)
//   2. node-scraped cover for artist+album   (existing, untouched)
//   3. generated deterministic cover         (existing, untouched)
//
// Keyed by (artist, album) — normalized EXACTLY like the node-cover path
// in LibraryProvider._artKey (trim, lowercase, collapse whitespace,
// empty album → 'singles') — so art found on ONE track serves the whole
// album, and the display side resolves both caches with the same
// inputs. Image bytes are written verbatim (no re-encode / resize) into
// <app-support>/embedded_art/<sha256(key)>.<ext>; nothing is stored in
// the library DB row.
//
// Performance contract: LibraryScanner reads tags WITHOUT the image
// first, and only re-reads the file with getImage:true when this cache
// has no art for the album yet — so a 5,000-file rescan of an existing
// library decodes zero images, and a fresh N-album import decodes about
// N, not 5,000.
//
// Failure contract (same as tags and lyrics): nothing here ever throws
// out of a public entry point. Corrupt/absent art → log and carry on.
//
// Display-layer entry point:  AlbumArtCache.cachedArt(artist, album)
// → File? (null when we have nothing for that album).

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:audio_metadata_reader/audio_metadata_reader.dart';
import 'package:crypto/crypto.dart' as crypto;
import 'package:path_provider/path_provider.dart';

void _alog(String m) {
  // ignore: avoid_print
  print('[albumart] $m');
}

class AlbumArtCache {
  AlbumArtCache._();

  /// Tests point this at a temp directory; production uses
  /// <app-support>/embedded_art.
  static Directory? debugDir;

  /// Extensions we recognise (by magic bytes, not by trust in the tag's
  /// declared mimetype). Lookup checks each.
  static const _extensions = ['jpg', 'png', 'gif', 'webp', 'bmp'];

  /// Same normalization as LibraryProvider._artKey so both art sources
  /// resolve identically for the same (artist, album).
  static String _norm(String s) =>
      s.trim().toLowerCase().replaceAll(RegExp(r'\s+'), ' ');

  static String _key(String artist, String album) {
    final a = _norm(album);
    return '${_norm(artist)}\n${a.isEmpty ? 'singles' : a}';
  }

  static String _baseName(String artist, String album) =>
      crypto.sha256.convert(_key(artist, album).codeUnits).toString();

  static Future<Directory?> _dir({bool create = false}) async {
    try {
      final d = debugDir ??
          Directory(
              '${(await getApplicationSupportDirectory()).path}/embedded_art');
      if (create && !await d.exists()) await d.create(recursive: true);
      return d;
    } catch (e) {
      _alog('art dir unavailable: $e');
      return null;
    }
  }

  /// Sniff the actual image format from magic bytes. The tag's declared
  /// mimetype is untrusted — files in the wild lie. Unknown magic counts
  /// as corrupt and is not cached.
  static String? sniffExtension(Uint8List b) {
    if (b.length < 12) return null;
    if (b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) return 'jpg';
    if (b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47) {
      return 'png';
    }
    if (b[0] == 0x47 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x38) {
      return 'gif';
    }
    if (b[0] == 0x52 && b[1] == 0x49 && b[2] == 0x46 && b[3] == 0x46 &&
        b[8] == 0x57 && b[9] == 0x45 && b[10] == 0x42 && b[11] == 0x50) {
      return 'webp';
    }
    if (b[0] == 0x42 && b[1] == 0x4D) return 'bmp';
    return null;
  }

  /// The display layer's entry point: the cached art file for this
  /// artist+album, or null when none is cached. Never throws.
  static Future<File?> cachedArt(String artist, String album) async {
    try {
      if (artist.trim().isEmpty) return null;
      final d = await _dir();
      if (d == null) return null;
      final base = _baseName(artist, album);
      for (final ext in _extensions) {
        final f = File('${d.path}/$base.$ext');
        if (await f.exists()) return f;
      }
      return null;
    } catch (_) {
      return null;
    }
  }

  /// True when the album already has cached art — the scanner's cheap
  /// pre-check that lets it skip the image read entirely.
  static Future<bool> hasArt(String artist, String album) async =>
      (await cachedArt(artist, album)) != null;

  /// Store raw image bytes for artist+album, exactly as they came out of
  /// the file. Returns the cache file, or null when the bytes don't look
  /// like an image (corrupt art must not poison the cache). Never throws.
  static Future<File?> storeBytes(
      Uint8List bytes, String artist, String album) async {
    try {
      if (artist.trim().isEmpty || bytes.isEmpty) return null;
      final ext = sniffExtension(bytes);
      if (ext == null) {
        _alog('unrecognised image bytes for "$artist / $album" '
            '(${bytes.length} B) — skipping');
        return null;
      }
      final d = await _dir(create: true);
      if (d == null) return null;
      final f = File('${d.path}/${_baseName(artist, album)}.$ext');
      await f.writeAsBytes(bytes, flush: true);
      _alog('cached ${bytes.length} B $ext for "$artist / $album"');
      return f;
    } catch (e) {
      _alog('store failed for "$artist / $album" (ignored): $e');
      return null;
    }
  }

  /// Extract embedded art from [audioFile] for artist+album, unless the
  /// album already has cached art (dedupe: ONE image decode per album,
  /// not per track). Prefers the front cover when the file carries
  /// several pictures. Never throws — a corrupt container or picture
  /// block is logged and skipped.
  ///
  /// Returns the image bytes ONLY when this call newly cached them (null
  /// on dedupe hits, absent art, or corrupt bytes). The scanner uses that
  /// to contribute genuinely-extracted art to the node exactly once per
  /// album — never re-uploading cached albums and never uploading
  /// anything that didn't come out of a file.
  static Future<Uint8List?> extractFromAudioFile(
      File audioFile, String artist, String album) async {
    try {
      if (artist.trim().isEmpty) return null; // no usable key
      if (await hasArt(artist, album)) return null; // dedupe — no image read
      // Second read of the file, WITH the image this time. The scanner's
      // main tag read keeps getImage:false so this cost is only paid for
      // albums with no art yet.
      AudioMetadata? meta;
      try {
        meta = readMetadata(audioFile, getImage: true);
      } catch (e) {
        _alog('image read failed for ${audioFile.path} (ignored): $e');
        return null;
      }
      final pics = meta.pictures;
      if (pics.isEmpty) return null;
      final pic = pics.firstWhere(
        (p) => p.pictureType == PictureType.coverFront,
        orElse: () => pics.first,
      );
      final stored = await storeBytes(pic.bytes, artist, album);
      return stored == null ? null : pic.bytes;
    } catch (e) {
      _alog('extract failed for ${audioFile.path} (ignored): $e');
      return null;
    }
  }
}
