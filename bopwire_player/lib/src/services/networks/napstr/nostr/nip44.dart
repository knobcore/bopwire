// NIP-44 v2 payload encryption: secp256k1 ECDH -> HKDF-SHA256 -> ChaCha20 +
// HMAC-SHA256, with the specified length-prefixed power-of-two padding.
//
// napstr carries every download request, offer and refusal inside NIP-17,
// which is NIP-44 sealed and NIP-59 wrapped, so this is on the critical
// path for downloading anything.

import 'dart:convert';
import 'dart:typed_data';

import 'package:pointycastle/digests/sha256.dart';
import 'package:pointycastle/macs/hmac.dart';
import 'package:pointycastle/api.dart';
import 'package:pointycastle/stream/chacha7539.dart';

import 'hex.dart';
import 'schnorr.dart';

const int _version = 2;
const int _minPlaintext = 1;
const int _maxPlaintext = 65535;

Uint8List _hmacSha256(Uint8List key, List<int> data) {
  final mac = HMac(SHA256Digest(), 64)..init(KeyParameter(key));
  return mac.process(Uint8List.fromList(data));
}

/// HKDF-Extract (RFC 5869).
Uint8List hkdfExtract(Uint8List salt, Uint8List ikm) => _hmacSha256(salt, ikm);

/// HKDF-Expand (RFC 5869).
Uint8List hkdfExpand(Uint8List prk, List<int> info, int length) {
  if (length > 255 * 32) throw ArgumentError('HKDF length too large');
  final out = BytesBuilder();
  var t = Uint8List(0);
  var counter = 1;
  while (out.length < length) {
    t = _hmacSha256(prk, <int>[...t, ...info, counter]);
    out.add(t);
    counter++;
  }
  return Uint8List.sublistView(out.toBytes(), 0, length);
}

/// The long-lived per-peer key: `hkdf_extract(ikm=ecdh_x, salt="nip44-v2")`.
/// Cache this per conversation — it is the expensive part.
Uint8List conversationKey(Uint8List privateKey, Uint8List peerXOnlyPubkey) {
  final shared = ecdhSharedX(privateKey, peerXOnlyPubkey);
  return hkdfExtract(Uint8List.fromList(utf8.encode('nip44-v2')), shared);
}

class _MessageKeys {
  _MessageKeys(this.chachaKey, this.chachaNonce, this.hmacKey);
  final Uint8List chachaKey;
  final Uint8List chachaNonce;
  final Uint8List hmacKey;
}

_MessageKeys _messageKeys(Uint8List convKey, Uint8List nonce) {
  final k = hkdfExpand(convKey, nonce, 76);
  return _MessageKeys(
    Uint8List.sublistView(k, 0, 32),
    Uint8List.sublistView(k, 32, 44),
    Uint8List.sublistView(k, 44, 76),
  );
}

/// NIP-44 padding: pad to the next power-of-two-derived chunk boundary so
/// the ciphertext length leaks only a coarse size bucket.
int calcPaddedLen(int unpadded) {
  if (unpadded <= 0) throw ArgumentError('length must be positive');
  if (unpadded <= 32) return 32;
  final nextPower = 1 << ((unpadded - 1).bitLength);
  final chunk = nextPower <= 256 ? 32 : nextPower ~/ 8;
  return chunk * (((unpadded - 1) ~/ chunk) + 1);
}

Uint8List _pad(String plaintext) {
  final bytes = Uint8List.fromList(utf8.encode(plaintext));
  if (bytes.length < _minPlaintext || bytes.length > _maxPlaintext) {
    throw ArgumentError('plaintext length out of range');
  }
  final padded = Uint8List(2 + calcPaddedLen(bytes.length));
  padded[0] = (bytes.length >> 8) & 0xff;
  padded[1] = bytes.length & 0xff;
  padded.setAll(2, bytes);
  return padded;
}

String? _unpad(Uint8List padded) {
  if (padded.length < 3) return null;
  final len = (padded[0] << 8) | padded[1];
  if (len < _minPlaintext || len > _maxPlaintext) return null;
  if (padded.length != 2 + calcPaddedLen(len)) return null;
  final body = Uint8List.sublistView(padded, 2, 2 + len);
  try {
    return utf8.decode(body);
  } catch (_) {
    return null;
  }
}

Uint8List _chacha20(Uint8List key, Uint8List nonce, Uint8List data) {
  final engine = ChaCha7539Engine()
    ..init(true, ParametersWithIV(KeyParameter(key), nonce));
  return engine.process(data);
}

/// Encrypts [plaintext] to the base64 NIP-44 v2 payload.
String nip44Encrypt(String plaintext, Uint8List convKey, {Uint8List? nonce}) {
  final n = nonce ?? randomBytes(32);
  if (n.length != 32) throw ArgumentError('nonce must be 32 bytes');
  final keys = _messageKeys(convKey, n);
  final ciphertext = _chacha20(keys.chachaKey, keys.chachaNonce, _pad(plaintext));
  final mac = _hmacSha256(keys.hmacKey, <int>[...n, ...ciphertext]);
  final payload = Uint8List(1 + 32 + ciphertext.length + 32)
    ..[0] = _version
    ..setAll(1, n)
    ..setAll(33, ciphertext)
    ..setAll(33 + ciphertext.length, mac);
  return base64.encode(payload);
}

/// Decrypts a NIP-44 v2 payload. Returns null on any failure — wrong key,
/// bad MAC, unsupported version, malformed base64. Callers must not
/// distinguish these.
String? nip44Decrypt(String payload, Uint8List convKey) {
  try {
    if (payload.isEmpty) return null;
    if (payload.codeUnitAt(0) == 0x23) return null; // '#' = unsupported version
    final raw = base64.decode(payload);
    if (raw.length < 99 || raw.length > 65603) return null;
    if (raw[0] != _version) return null;
    final nonce = Uint8List.sublistView(raw, 1, 33);
    final ciphertext = Uint8List.sublistView(raw, 33, raw.length - 32);
    final mac = Uint8List.sublistView(raw, raw.length - 32);
    final keys = _messageKeys(convKey, nonce);
    final expected = _hmacSha256(keys.hmacKey, <int>[...nonce, ...ciphertext]);
    if (!constantTimeEquals(expected, mac)) return null;
    return _unpad(_chacha20(keys.chachaKey, keys.chachaNonce, ciphertext));
  } catch (_) {
    return null;
  }
}
