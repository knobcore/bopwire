// Parsing and validation of napstr's public Nostr layer: kind 30421
// catalogue entries and kind 30422 availability heartbeats.
//
// Everything here treats relay input as hostile. PROTOCOL.md is explicit
// that filenames, titles and tags are attacker-controlled text and that a
// client MUST validate the signature, protocol string, file id, filename,
// size, format, MIME type and tags before displaying an entry.

import 'dart:convert';

import 'nostr/event.dart';
import 'nostr/hex.dart';
import 'search_tokens.dart';

const int kCatalogueKind = 30421;
const int kAvailabilityKind = 30422;
const String kCatalogueHashtag = 'napstr';
const String kAvailabilityHashtag = 'napstr-availability';
const String kProtocolVersion = 'napstr/1';

/// Max file IDs one heartbeat group may claim (PROTOCOL.md: "at most 400").
const int kAvailabilityGroupLimit = 400;

/// The five audio claims a napstr client must support, as
/// (extension, format, mime) triples. Anything else is rejected: an
/// extension or MIME claim on its own is explicitly not enough.
const Map<String, ({String format, String mime})> kSupportedAudio = {
  'mp3': (format: 'MP3', mime: 'audio/mpeg'),
  'flac': (format: 'FLAC', mime: 'audio/flac'),
  'wav': (format: 'WAV', mime: 'audio/wav'),
  'ogg': (format: 'OGG', mime: 'audio/ogg'),
  'opus': (format: 'OPUS', mime: 'audio/ogg'),
};

/// True when filename extension, `format` and `mime` agree on one of the
/// five supported audio types.
bool audioClaimValid(String filename, String format, String mime) {
  final dot = filename.lastIndexOf('.');
  if (dot < 0 || dot == filename.length - 1) return false;
  final ext = filename.substring(dot + 1).toLowerCase();
  final expected = kSupportedAudio[ext];
  if (expected == null) return false;
  return format.toUpperCase() == expected.format && mime == expected.mime;
}

/// True for exactly 64 lowercase hex characters.
bool validFileId(String value) => isLowerHex(value, 32);

/// Normalises the comma-separated user tag field. Returns null when the
/// field violates napstr's tag rules (>256 chars total, >32 per tag, >12
/// tags, or containing control/bidi characters).
String? normaliseTags(String value) {
  if (value.runes.length > 256) return null;
  final seen = <String>{};
  final tags = <String>[];
  for (final part in value.split(',')) {
    final t = part.trim();
    if (t.isEmpty) continue;
    if (t.runes.length > 32) return null;
    for (final r in t.runes) {
      if (isUnsafeTextRune(r)) return null;
    }
    if (seen.add(t.toLowerCase())) tags.add(t);
  }
  if (tags.length > 12) return null;
  return tags.join(', ');
}

/// Strips any path component and rejects a name that cannot be used as a
/// local filename. PROTOCOL.md: "A public filename MUST be a basename,
/// never a path, and a receiving client MUST treat it as untrusted text."
String? safeBasename(String filename) {
  if (filename.isEmpty || filename.runes.length > 256) return null;
  if (filename.contains('/') || filename.contains(r'\')) return null;
  if (filename == '.' || filename == '..') return null;
  for (final r in filename.runes) {
    if (isUnsafeTextRune(r)) return null;
  }
  return filename;
}

/// Rewrites a napstr basename into something safe to create on the local
/// filesystem on every target platform (Windows forbids `<>:"|?*`).
String sanitiseForFilesystem(String basename) {
  final cleaned = basename
      .replaceAll(RegExp(r'[\x00-\x1f<>:"/\\|?*]'), '_')
      .replaceAll(RegExp(r'\s+'), ' ')
      .trim();
  final trimmed = cleaned.replaceAll(RegExp(r'[. ]+$'), '');
  if (trimmed.isEmpty) return 'napstr-download';
  return trimmed.length > 180 ? trimmed.substring(0, 180) : trimmed;
}

/// One validated kind-30421 record from one publisher.
class CatalogueRecord {
  const CatalogueRecord({
    required this.fileId,
    required this.publisherPubkey,
    required this.eventId,
    required this.filename,
    required this.title,
    required this.artist,
    required this.album,
    required this.format,
    required this.mime,
    required this.size,
    required this.tags,
  });

  final String fileId;
  final String publisherPubkey;
  final String eventId;
  final String filename;
  final String title;
  final String artist;
  final String album;
  final String format;
  final String mime;
  final int size;
  final String tags;

  /// Lowercase extension, derived from the (already validated) filename.
  String get extension =>
      filename.substring(filename.lastIndexOf('.') + 1).toLowerCase();

  /// The fields napstr searches over locally.
  List<String> get searchableFields => [filename, title, artist, album, tags];
}

/// Result of trying to read a kind-30421 event.
enum CatalogueParseOutcome { record, withdrawn, invalid }

class CatalogueParseResult {
  const CatalogueParseResult(this.outcome, [this.record, this.fileId]);
  final CatalogueParseOutcome outcome;
  final CatalogueRecord? record;

  /// Set for a withdrawal so callers can drop the previous entry.
  final String? fileId;
}

/// Validates and parses a kind-30421 event.
///
/// [verifySignature] is separated out because BIP-340 verification is by
/// far the most expensive step; callers batch it, but the default is to do
/// it, so no caller accidentally skips it.
CatalogueParseResult parseCatalogueEvent(
  NostrEvent event, {
  bool verifySignature = true,
}) {
  const invalid = CatalogueParseResult(CatalogueParseOutcome.invalid);
  if (event.kind != kCatalogueKind) return invalid;
  if (!event.tagValues('t').contains(kCatalogueHashtag)) return invalid;
  final d = event.tagValue('d');
  if (d == null || !validFileId(d)) return invalid;

  Object? decoded;
  try {
    decoded = jsonDecode(event.content);
  } catch (_) {
    return invalid;
  }
  if (decoded is! Map) return invalid;
  if (decoded['protocol'] != kProtocolVersion) return invalid;

  if (decoded['deleted'] == true) {
    if (verifySignature && !event.verify()) return invalid;
    return CatalogueParseResult(CatalogueParseOutcome.withdrawn, null, d);
  }

  final fileId = decoded['fileId'];
  final filenameRaw = decoded['filename'];
  final size = decoded['size'];
  final format = decoded['format'];
  final mime = decoded['mime'];
  if (fileId is! String ||
      filenameRaw is! String ||
      size is! int ||
      format is! String ||
      mime is! String) {
    return invalid;
  }
  // The addressable `d` tag is what relays index and replace on, so a
  // content fileId that disagrees with it is a mislabelled entry.
  if (fileId != d || !validFileId(fileId)) return invalid;
  if (size <= 0) return invalid;

  final filename = safeBasename(filenameRaw);
  if (filename == null) return invalid;
  if (!audioClaimValid(filename, format, mime)) return invalid;

  final title = _text(decoded['title']);
  final artist = _text(decoded['artist']);
  final album = _text(decoded['album']);
  final rawTags = _text(decoded['tags']);
  for (final field in [filename, title, artist, album, rawTags]) {
    if (!isValidCatalogueText(field)) return invalid;
  }
  final tags = normaliseTags(rawTags);
  if (tags == null) return invalid;

  // The signed `size` tag is what the transfer step checks the WELCOME
  // frame against, so it must agree with the content body.
  final sizeTag = event.tagValue('size');
  if (sizeTag != null && int.tryParse(sizeTag) != size) return invalid;
  final xTag = event.tagValue('x');
  if (xTag != null && xTag != fileId) return invalid;

  if (verifySignature && !event.verify()) return invalid;

  return CatalogueParseResult(
    CatalogueParseOutcome.record,
    CatalogueRecord(
      fileId: fileId,
      publisherPubkey: event.pubkey,
      eventId: event.id,
      filename: filename,
      title: title,
      artist: artist,
      album: album,
      format: format.toUpperCase(),
      mime: mime,
      size: size,
      tags: tags,
    ),
    fileId,
  );
}

String _text(Object? v) => v is String ? v : '';

/// The file IDs one publisher currently claims to be seeding.
class AvailabilityHeartbeat {
  const AvailabilityHeartbeat(this.pubkey, this.fileIds, this.expiration);
  final String pubkey;
  final Set<String> fileIds;
  final int expiration;
}

/// Validates and parses a kind-30422 heartbeat. Returns null when the event
/// is not a well-formed, unexpired napstr availability group.
AvailabilityHeartbeat? parseAvailabilityEvent(
  NostrEvent event, {
  bool verifySignature = true,
  DateTime? now,
}) {
  if (event.kind != kAvailabilityKind) return null;
  if (!event.tagValues('t').contains(kAvailabilityHashtag)) return null;
  final expiration = event.expiration;
  // PROTOCOL.md requires the expiration tag; an event without one is not a
  // heartbeat and must not be treated as live availability.
  if (expiration == null) return null;
  final nowSeconds =
      (now ?? DateTime.now()).millisecondsSinceEpoch ~/ 1000;
  if (expiration <= nowSeconds) return null;

  Object? decoded;
  try {
    decoded = jsonDecode(event.content);
  } catch (_) {
    return null;
  }
  if (decoded is! List) return null;
  if (decoded.length > kAvailabilityGroupLimit) return null;

  final ids = <String>{};
  for (final v in decoded) {
    if (v is! String || !validFileId(v)) continue;
    ids.add(v);
  }
  if (verifySignature && !event.verify()) return null;
  return AvailabilityHeartbeat(event.pubkey, ids, expiration);
}

/// Opaque handle carried in `ExternalTrack.id`, round-tripping the file id
/// and the seeders that were live when the result was produced.
class NapstrTrackRef {
  const NapstrTrackRef(this.fileId, this.seeders);
  final String fileId;
  final List<String> seeders;

  String encode() => 'napstr:$fileId:${seeders.join(',')}';

  static NapstrTrackRef? decode(String id) {
    final parts = id.split(':');
    if (parts.length != 3 || parts[0] != 'napstr') return null;
    if (!validFileId(parts[1])) return null;
    final seeders = <String>[];
    for (final s in parts[2].split(',')) {
      if (s.isEmpty) continue;
      if (!isLowerHex(s, 32)) return null;
      if (!seeders.contains(s)) seeders.add(s);
    }
    return NapstrTrackRef(parts[1], seeders);
  }
}
