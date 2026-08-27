// BIP-340 Schnorr signatures over secp256k1, plus the x-only ECDH used by
// NIP-44. Pure Dart on top of pointycastle's curve arithmetic — no native
// code, so it runs identically on Linux, Windows and Android.
//
// napstr's PROTOCOL.md makes signature validation mandatory ("Clients MUST
// reject events with an invalid signature or ID"), so this is not optional
// window dressing: every catalogue and availability event we surface has
// been through [schnorrVerify].

import 'dart:math';
import 'dart:typed_data';

import 'package:pointycastle/api.dart';
import 'package:pointycastle/digests/sha256.dart';
import 'package:pointycastle/ecc/api.dart';
import 'package:pointycastle/ecc/curves/secp256k1.dart';

import 'hex.dart';

final ECDomainParameters _secp256k1 = ECCurve_secp256k1();
final BigInt _n = _secp256k1.n;
final BigInt _p = BigInt.parse(
    'fffffffffffffffffffffffffffffffffffffffffffffffffffffffefffffc2f',
    radix: 16);
final BigInt _seven = BigInt.from(7);
final BigInt _two = BigInt.two;

/// sha256(tag) || sha256(tag) || msg, hashed. BIP-340 §Design.
Uint8List taggedHash(String tag, List<int> msg) {
  final d = SHA256Digest();
  final tagHash = d.process(Uint8List.fromList(tag.codeUnits));
  final buf = Uint8List(tagHash.length * 2 + msg.length)
    ..setAll(0, tagHash)
    ..setAll(tagHash.length, tagHash)
    ..setAll(tagHash.length * 2, msg);
  return SHA256Digest().process(buf);
}

Uint8List sha256Bytes(List<int> data) =>
    SHA256Digest().process(Uint8List.fromList(data));

/// Recovers the even-Y point with x-coordinate [x], or null when no such
/// point is on the curve (BIP-340 `lift_x`).
ECPoint? liftX(BigInt x) {
  if (x <= BigInt.zero || x >= _p) return null;
  final c = (x.modPow(BigInt.from(3), _p) + _seven) % _p;
  final y = c.modPow((_p + BigInt.one) >> 2, _p);
  if (y.modPow(_two, _p) != c) return null;
  final even = y.isEven ? y : _p - y;
  return _secp256k1.curve.createPoint(x, even);
}

/// Derives the 32-byte x-only public key for [privateKey] (32 bytes).
Uint8List xOnlyPublicKey(Uint8List privateKey) {
  final d = bytesToBigInt(privateKey);
  if (d <= BigInt.zero || d >= _n) {
    throw ArgumentError('private key out of range');
  }
  final point = (_secp256k1.G * d)!;
  return bigIntTo32(point.x!.toBigInteger()!);
}

/// True when [privateKey] is a usable secp256k1 scalar.
bool isValidPrivateKey(Uint8List privateKey) {
  if (privateKey.length != 32) return false;
  final d = bytesToBigInt(privateKey);
  return d > BigInt.zero && d < _n;
}

/// BIP-340 sign. [message] is any length — Nostr always passes a 32-byte
/// event id, but the 2022 BIP-340 extension allows arbitrary lengths and
/// the official vectors exercise it. [auxRand] defaults to 32 fresh random
/// bytes.
Uint8List schnorrSign(Uint8List message, Uint8List privateKey,
    {Uint8List? auxRand}) {
  var d = bytesToBigInt(privateKey);
  if (d <= BigInt.zero || d >= _n) {
    throw ArgumentError('private key out of range');
  }
  final pointP = (_secp256k1.G * d)!;
  final px = pointP.x!.toBigInteger()!;
  if (!pointP.y!.toBigInteger()!.isEven) d = _n - d;

  final aux = auxRand ?? randomBytes(32);
  if (aux.length != 32) throw ArgumentError('auxRand must be 32 bytes');
  final t = taggedHash('BIP0340/aux', aux);
  final dBytes = bigIntTo32(d);
  final masked = Uint8List(32);
  for (var i = 0; i < 32; i++) {
    masked[i] = dBytes[i] ^ t[i];
  }

  final rand = taggedHash(
      'BIP0340/nonce', <int>[...masked, ...bigIntTo32(px), ...message]);
  var k = bytesToBigInt(rand) % _n;
  if (k == BigInt.zero) throw StateError('schnorr: nonce is zero');
  final pointR = (_secp256k1.G * k)!;
  final rx = pointR.x!.toBigInteger()!;
  if (!pointR.y!.toBigInteger()!.isEven) k = _n - k;

  final e = bytesToBigInt(taggedHash('BIP0340/challenge',
          <int>[...bigIntTo32(rx), ...bigIntTo32(px), ...message])) %
      _n;

  final sig = Uint8List(64)
    ..setAll(0, bigIntTo32(rx))
    ..setAll(32, bigIntTo32((k + e * d) % _n));
  return sig;
}

/// BIP-340 verify. Never throws — relay-supplied bytes are hostile.
///
/// Callers in this package always pass a 32-byte Nostr event id; the length
/// itself is not constrained here because BIP-340 does not constrain it.
bool schnorrVerify(Uint8List message, Uint8List xOnlyPubkey, Uint8List sig) {
  try {
    if (xOnlyPubkey.length != 32 || sig.length != 64) {
      return false;
    }
    final pointP = liftX(bytesToBigInt(xOnlyPubkey));
    if (pointP == null) return false;

    final r = bytesToBigInt(sig.sublist(0, 32));
    if (r >= _p) return false;
    final s = bytesToBigInt(sig.sublist(32, 64));
    if (s >= _n) return false;

    final e = bytesToBigInt(taggedHash('BIP0340/challenge',
            <int>[...sig.sublist(0, 32), ...xOnlyPubkey, ...message])) %
        _n;

    // R = s*G - e*P
    final sG = (_secp256k1.G * s)!;
    final eP = (pointP * ((_n - e) % _n))!;
    final pointR = (sG + eP);
    if (pointR == null || pointR.isInfinity) return false;
    if (!pointR.y!.toBigInteger()!.isEven) return false;
    return pointR.x!.toBigInteger()! == r;
  } catch (_) {
    return false;
  }
}

/// NIP-44 conversation input: the 32-byte X coordinate of
/// `privateKey * lift_x(peerXOnlyPubkey)`. NIP-44 explicitly uses only the
/// X coordinate, without hashing it into a compressed-point form.
Uint8List ecdhSharedX(Uint8List privateKey, Uint8List peerXOnlyPubkey) {
  final d = bytesToBigInt(privateKey);
  if (d <= BigInt.zero || d >= _n) {
    throw ArgumentError('private key out of range');
  }
  final peer = liftX(bytesToBigInt(peerXOnlyPubkey));
  if (peer == null) throw ArgumentError('peer public key is not on the curve');
  final shared = (peer * d);
  if (shared == null || shared.isInfinity) {
    throw ArgumentError('degenerate ECDH result');
  }
  return bigIntTo32(shared.x!.toBigInteger()!);
}

final SecureRandom _rng = _seededRandom();

SecureRandom _seededRandom() {
  final r = SecureRandom('Fortuna');
  final seed = Uint8List(32);
  final sys = Random.secure();
  for (var i = 0; i < seed.length; i++) {
    seed[i] = sys.nextInt(256);
  }
  r.seed(KeyParameter(seed));
  return r;
}

Uint8List randomBytes(int n) {
  // Mix the platform CSPRNG straight in rather than trusting only the
  // Fortuna instance: capabilities and nonces are single-use secrets.
  final out = _rng.nextBytes(n);
  final sys = Random.secure();
  for (var i = 0; i < n; i++) {
    out[i] ^= sys.nextInt(256);
  }
  return out;
}

/// A fresh, in-range secp256k1 secret key.
Uint8List generatePrivateKey() {
  while (true) {
    final candidate = randomBytes(32);
    if (isValidPrivateKey(candidate)) return candidate;
  }
}
