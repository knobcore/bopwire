// Minimal bech32 (BIP-173) codec, only as much as NIP-19 `nsec1…`/`npub1…`
// need. Users paste keys in either bech32 or raw hex, so we must read both.

import 'dart:typed_data';

const _charset = 'qpzry9x8gf2tvdw0s3jn54khce6mua7l';

int _polymod(List<int> values) {
  const gen = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3];
  var chk = 1;
  for (final v in values) {
    final top = chk >> 25;
    chk = ((chk & 0x1ffffff) << 5) ^ v;
    for (var i = 0; i < 5; i++) {
      if ((top >> i) & 1 == 1) chk ^= gen[i];
    }
  }
  return chk;
}

List<int> _hrpExpand(String hrp) => [
      for (final c in hrp.codeUnits) c >> 5,
      0,
      for (final c in hrp.codeUnits) c & 31,
    ];

/// Regroups [data] from [from]-bit to [to]-bit groups. Returns null when the
/// padding is invalid (which for decoding means the input is malformed).
List<int>? convertBits(List<int> data, int from, int to, {required bool pad}) {
  var acc = 0;
  var bits = 0;
  final out = <int>[];
  final maxv = (1 << to) - 1;
  for (final value in data) {
    if (value < 0 || (value >> from) != 0) return null;
    acc = (acc << from) | value;
    bits += from;
    while (bits >= to) {
      bits -= to;
      out.add((acc >> bits) & maxv);
    }
  }
  if (pad) {
    if (bits > 0) out.add((acc << (to - bits)) & maxv);
  } else if (bits >= from || ((acc << (to - bits)) & maxv) != 0) {
    return null;
  }
  return out;
}

/// Encodes [data] (8-bit bytes) as `<hrp>1<payload><checksum>`.
String bech32Encode(String hrp, List<int> data) {
  final five = convertBits(data, 8, 5, pad: true);
  if (five == null) throw ArgumentError('cannot regroup payload');
  final values = [..._hrpExpand(hrp), ...five];
  final polymod = _polymod([...values, 0, 0, 0, 0, 0, 0]) ^ 1;
  final checksum = [for (var i = 0; i < 6; i++) (polymod >> (5 * (5 - i))) & 31];
  final sb = StringBuffer(hrp)..write('1');
  for (final v in [...five, ...checksum]) {
    sb.write(_charset[v]);
  }
  return sb.toString();
}

class Bech32Result {
  const Bech32Result(this.hrp, this.data);
  final String hrp;
  final Uint8List data;
}

/// Decodes a bech32 string. Returns null on any malformed input rather than
/// throwing — this parses user-pasted and, for npub, relay-supplied text.
Bech32Result? bech32Decode(String input) {
  if (input.length < 8 || input.length > 2000) return null;
  final lower = input.toLowerCase();
  if (lower != input && input.toUpperCase() != input) return null; // mixed case
  final sep = lower.lastIndexOf('1');
  if (sep < 1 || sep + 7 > lower.length) return null;
  final hrp = lower.substring(0, sep);
  for (final c in hrp.codeUnits) {
    if (c < 33 || c > 126) return null;
  }
  final values = <int>[];
  for (final c in lower.substring(sep + 1).codeUnits) {
    final idx = _charset.indexOf(String.fromCharCode(c));
    if (idx < 0) return null;
    values.add(idx);
  }
  if (_polymod([..._hrpExpand(hrp), ...values]) != 1) return null;
  final payload = values.sublist(0, values.length - 6);
  final bytes = convertBits(payload, 5, 8, pad: false);
  if (bytes == null) return null;
  return Bech32Result(hrp, Uint8List.fromList(bytes));
}
