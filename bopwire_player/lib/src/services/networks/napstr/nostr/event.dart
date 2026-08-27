// NIP-01 event model: canonical serialisation, id derivation, signing and
// verification.

import 'dart:convert';
import 'dart:typed_data';

import 'hex.dart';
import 'keys.dart';
import 'schnorr.dart';

class NostrEvent {
  const NostrEvent({
    required this.id,
    required this.pubkey,
    required this.createdAt,
    required this.kind,
    required this.tags,
    required this.content,
    required this.sig,
  });

  final String id;
  final String pubkey;
  final int createdAt;
  final int kind;
  final List<List<String>> tags;
  final String content;
  final String sig;

  /// NIP-01 canonical form: `[0,pubkey,created_at,kind,tags,content]`, JSON
  /// encoded with no extra whitespace. sha256 of this is the event id.
  static String canonical({
    required String pubkey,
    required int createdAt,
    required int kind,
    required List<List<String>> tags,
    required String content,
  }) =>
      jsonEncode([0, pubkey, createdAt, kind, tags, content]);

  static String computeId({
    required String pubkey,
    required int createdAt,
    required int kind,
    required List<List<String>> tags,
    required String content,
  }) =>
      toHex(sha256Bytes(utf8.encode(canonical(
        pubkey: pubkey,
        createdAt: createdAt,
        kind: kind,
        tags: tags,
        content: content,
      ))));

  String get derivedId => computeId(
        pubkey: pubkey,
        createdAt: createdAt,
        kind: kind,
        tags: tags,
        content: content,
      );

  /// True when the id matches the canonical hash. Cheap — run this before
  /// the (much more expensive) signature check.
  bool get hasValidId => derivedId == id;

  /// Full NIP-01 validation: id then BIP-340 signature.
  bool verify() {
    if (!isLowerHex(id, 32) || !isLowerHex(pubkey, 32) || !isLowerHex(sig, 64)) {
      return false;
    }
    if (!hasValidId) return false;
    final idBytes = tryFromHex(id);
    final pkBytes = tryFromHex(pubkey);
    final sigBytes = tryFromHex(sig);
    if (idBytes == null || pkBytes == null || sigBytes == null) return false;
    return schnorrVerify(idBytes, pkBytes, sigBytes);
  }

  /// Parses one relay-supplied event object. Returns null when a required
  /// field is missing or has the wrong shape — no exceptions escape.
  static NostrEvent? tryParse(Object? json) {
    if (json is! Map) return null;
    final id = json['id'];
    final pubkey = json['pubkey'];
    final createdAt = json['created_at'];
    final kind = json['kind'];
    final rawTags = json['tags'];
    final content = json['content'];
    final sig = json['sig'];
    if (id is! String ||
        pubkey is! String ||
        createdAt is! int ||
        kind is! int ||
        content is! String ||
        sig is! String ||
        rawTags is! List) {
      return null;
    }
    final tags = <List<String>>[];
    for (final t in rawTags) {
      if (t is! List) return null;
      final row = <String>[];
      for (final v in t) {
        if (v is! String) return null;
        row.add(v);
      }
      tags.add(row);
    }
    return NostrEvent(
      id: id,
      pubkey: pubkey,
      createdAt: createdAt,
      kind: kind,
      tags: tags,
      content: content,
      sig: sig,
    );
  }

  Map<String, Object?> toJson() => {
        'id': id,
        'pubkey': pubkey,
        'created_at': createdAt,
        'kind': kind,
        'tags': tags,
        'content': content,
        'sig': sig,
      };

  /// First value of the first tag named [name], or null.
  String? tagValue(String name) {
    for (final t in tags) {
      if (t.length >= 2 && t[0] == name) return t[1];
    }
    return null;
  }

  Iterable<String> tagValues(String name) sync* {
    for (final t in tags) {
      if (t.length >= 2 && t[0] == name) yield t[1];
    }
  }

  /// NIP-40 expiration as a Unix timestamp, or null when absent/unparseable.
  int? get expiration {
    final v = tagValue('expiration');
    return v == null ? null : int.tryParse(v);
  }

  bool get isExpired {
    final e = expiration;
    if (e == null) return false;
    return e * 1000 <= DateTime.now().millisecondsSinceEpoch;
  }
}

/// Builds and signs an event for [keys].
NostrEvent signEvent({
  required NostrKeyPair keys,
  required int kind,
  required List<List<String>> tags,
  required String content,
  int? createdAt,
}) {
  final pubkey = keys.publicKeyHex;
  final ts = createdAt ?? DateTime.now().millisecondsSinceEpoch ~/ 1000;
  final id = NostrEvent.computeId(
    pubkey: pubkey,
    createdAt: ts,
    kind: kind,
    tags: tags,
    content: content,
  );
  final sig = schnorrSign(fromHex(id), keys.privateKey);
  return NostrEvent(
    id: id,
    pubkey: pubkey,
    createdAt: ts,
    kind: kind,
    tags: tags,
    content: content,
    sig: toHex(sig),
  );
}

/// An *unsigned* NIP-59 rumor: it carries an id but no signature.
Map<String, Object?> buildRumor({
  required String pubkeyHex,
  required int kind,
  required List<List<String>> tags,
  required String content,
  int? createdAt,
}) {
  final ts = createdAt ?? DateTime.now().millisecondsSinceEpoch ~/ 1000;
  final id = NostrEvent.computeId(
    pubkey: pubkeyHex,
    createdAt: ts,
    kind: kind,
    tags: tags,
    content: content,
  );
  return {
    'id': id,
    'pubkey': pubkeyHex,
    'created_at': ts,
    'kind': kind,
    'tags': tags,
    'content': content,
  };
}

/// Uint8List re-export convenience for callers that only import this file.
typedef Bytes = Uint8List;
