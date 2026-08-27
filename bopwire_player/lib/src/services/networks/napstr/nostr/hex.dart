// Hex + constant-time byte helpers shared by the napstr Nostr layer.

import 'dart:typed_data';

const _hexDigits = '0123456789abcdef';

/// Lowercase hex encoding of [bytes].
String toHex(List<int> bytes) {
  final sb = StringBuffer();
  for (final b in bytes) {
    sb
      ..write(_hexDigits[(b >> 4) & 0x0f])
      ..write(_hexDigits[b & 0x0f]);
  }
  return sb.toString();
}

/// Decodes lowercase/uppercase hex. Returns null when [s] is not valid hex
/// or has an odd length — callers treat relay data as hostile, so this never
/// throws on bad input.
Uint8List? tryFromHex(String s) {
  if (s.length.isOdd) return null;
  final out = Uint8List(s.length ~/ 2);
  for (var i = 0; i < out.length; i++) {
    final hi = _nibble(s.codeUnitAt(i * 2));
    final lo = _nibble(s.codeUnitAt(i * 2 + 1));
    if (hi < 0 || lo < 0) return null;
    out[i] = (hi << 4) | lo;
  }
  return out;
}

/// Like [tryFromHex] but throws — for values we produced ourselves.
Uint8List fromHex(String s) {
  final r = tryFromHex(s);
  if (r == null) throw FormatException('not hex: $s');
  return r;
}

int _nibble(int c) {
  if (c >= 0x30 && c <= 0x39) return c - 0x30; // 0-9
  if (c >= 0x61 && c <= 0x66) return c - 0x61 + 10; // a-f
  if (c >= 0x41 && c <= 0x46) return c - 0x41 + 10; // A-F
  return -1;
}

/// True when [s] is exactly [byteLength] bytes of *lowercase* hex. napstr
/// file IDs and pubkeys are specified as lowercase, so we do not accept
/// uppercase variants that would break `d`-tag equality.
bool isLowerHex(String s, int byteLength) {
  if (s.length != byteLength * 2) return false;
  for (var i = 0; i < s.length; i++) {
    final c = s.codeUnitAt(i);
    final ok = (c >= 0x30 && c <= 0x39) || (c >= 0x61 && c <= 0x66);
    if (!ok) return false;
  }
  return true;
}

/// Constant-time equality for MAC / capability comparison.
bool constantTimeEquals(List<int> a, List<int> b) {
  if (a.length != b.length) return false;
  var diff = 0;
  for (var i = 0; i < a.length; i++) {
    diff |= a[i] ^ b[i];
  }
  return diff == 0;
}

BigInt bytesToBigInt(List<int> bytes) {
  var r = BigInt.zero;
  for (final b in bytes) {
    r = (r << 8) | BigInt.from(b);
  }
  return r;
}

Uint8List bigIntTo32(BigInt v) {
  final out = Uint8List(32);
  var x = v;
  for (var i = 31; i >= 0; i--) {
    out[i] = (x & BigInt.from(0xff)).toInt();
    x = x >> 8;
  }
  return out;
}
