// Nostr identity: a secp256k1 secret key plus its x-only public key, with
// NIP-19 `nsec1…` / `npub1…` conversion.

import 'dart:typed_data';

import 'bech32.dart';
import 'hex.dart';
import 'schnorr.dart';

class NostrKeyPair {
  NostrKeyPair(this.privateKey)
      : assert(privateKey.length == 32),
        publicKey = xOnlyPublicKey(privateKey);

  final Uint8List privateKey;
  final Uint8List publicKey;

  String get publicKeyHex => toHex(publicKey);
  String get npub => bech32Encode('npub', publicKey);
  String get nsec => bech32Encode('nsec', privateKey);

  static NostrKeyPair generate() => NostrKeyPair(generatePrivateKey());

  /// Accepts a bech32 `nsec1…` or a bare 64-character hex secret key.
  /// Returns null for anything else, including out-of-range scalars.
  static NostrKeyPair? tryParse(String input) {
    final trimmed = input.trim();
    if (trimmed.isEmpty) return null;
    Uint8List? raw;
    if (trimmed.toLowerCase().startsWith('nsec1')) {
      final decoded = bech32Decode(trimmed);
      if (decoded == null || decoded.hrp != 'nsec') return null;
      raw = decoded.data;
    } else {
      raw = tryFromHex(trimmed);
    }
    if (raw == null || raw.length != 32 || !isValidPrivateKey(raw)) return null;
    return NostrKeyPair(raw);
  }
}

/// Accepts `npub1…` or 64-character hex and returns lowercase hex, or null.
String? normalisePublicKey(String input) {
  final trimmed = input.trim();
  if (trimmed.toLowerCase().startsWith('npub1')) {
    final decoded = bech32Decode(trimmed);
    if (decoded == null || decoded.hrp != 'npub' || decoded.data.length != 32) {
      return null;
    }
    return toHex(decoded.data);
  }
  final raw = tryFromHex(trimmed);
  if (raw == null || raw.length != 32) return null;
  return toHex(raw);
}

/// `abcd1234…wxyz` — the short display form the reference client uses for a
/// seeder with no profile name.
String shortKey(String hexKey) {
  if (hexKey.length <= 12) return hexKey;
  return '${hexKey.substring(0, 8)}…${hexKey.substring(hexKey.length - 4)}';
}
