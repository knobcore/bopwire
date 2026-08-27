// Crypto layer: BIP-340 Schnorr, bech32/NIP-19, NIP-44 v2, NIP-59 wrapping.
//
// The Schnorr and NIP-44 cases are the official upstream vectors, so a
// passing run means our pure-Dart implementations interoperate with the
// reference napstr client rather than merely agreeing with themselves.

import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/napstr/nostr/bech32.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/event.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/hex.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/keys.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/nip44.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/nip59.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/schnorr.dart';

import 'vectors.dart';

void main() {
  group('BIP-340 Schnorr (official vectors)', () {
    test('verifies every vector with the expected result', () {
      for (final v in bip340Vectors) {
        final msg = fromHex(v.message.toLowerCase());
        final pk = fromHex(v.publicKey.toLowerCase());
        final sig = fromHex(v.signature.toLowerCase());
        expect(
          schnorrVerify(msg, pk, sig),
          v.valid,
          reason: 'vector ${v.index}: ${v.comment}',
        );
      }
    });

    test('reproduces every signature that has a secret key', () {
      var signed = 0;
      for (final v in bip340Vectors) {
        if (v.secretKey.isEmpty) continue;
        final sig = schnorrSign(
          fromHex(v.message.toLowerCase()),
          fromHex(v.secretKey.toLowerCase()),
          auxRand: fromHex(v.auxRand.toLowerCase()),
        );
        expect(toHex(sig), v.signature.toLowerCase(),
            reason: 'vector ${v.index}');
        signed++;
      }
      expect(signed, greaterThan(3));
    });

    test('derives the x-only public key from the secret key', () {
      for (final v in bip340Vectors) {
        if (v.secretKey.isEmpty) continue;
        expect(toHex(xOnlyPublicKey(fromHex(v.secretKey.toLowerCase()))),
            v.publicKey.toLowerCase());
      }
    });

    test('rejects wrong-length keys and signatures instead of throwing', () {
      expect(schnorrVerify(Uint8List(32), Uint8List(31), Uint8List(64)), isFalse);
      expect(schnorrVerify(Uint8List(32), Uint8List(33), Uint8List(64)), isFalse);
      expect(schnorrVerify(Uint8List(32), Uint8List(32), Uint8List(63)), isFalse);
      expect(schnorrVerify(Uint8List(32), Uint8List(32), Uint8List(65)), isFalse);
    });

    test('a signature does not verify against a different message', () {
      final keys = NostrKeyPair.generate();
      final msg = sha256Bytes(utf8.encode('napstr'));
      final other = sha256Bytes(utf8.encode('napstrr'));
      final sig = schnorrSign(msg, keys.privateKey);
      expect(schnorrVerify(msg, keys.publicKey, sig), isTrue);
      expect(schnorrVerify(other, keys.publicKey, sig), isFalse);
    });
  });

  group('bech32 / NIP-19', () {
    // Vector from NIP-19 itself.
    const npub = 'npub180cvv07tjdrrgpa0j7j7tmnyl2yr6yr7l8j4s3evf6u64th6gkwsyjh6w6';
    const hex =
        '3bf0c63fcb93463407af97a5e5ee64fa883d107ef9e558472c4eb9aaaefa459d';

    test('decodes a known npub to its hex pubkey', () {
      expect(normalisePublicKey(npub), hex);
    });

    test('round-trips hex -> npub -> hex', () {
      expect(bech32Encode('npub', fromHex(hex)), npub);
      expect(normalisePublicKey(hex), hex);
    });

    test('nsec round-trips through a key pair', () {
      final keys = NostrKeyPair.generate();
      final reparsed = NostrKeyPair.tryParse(keys.nsec);
      expect(reparsed, isNotNull);
      expect(reparsed!.publicKeyHex, keys.publicKeyHex);
      expect(NostrKeyPair.tryParse(toHex(keys.privateKey))!.publicKeyHex,
          keys.publicKeyHex);
    });

    test('rejects corrupted, wrong-prefix and non-key input', () {
      // Flip one character of the payload; the checksum must catch it.
      final broken = npub.replaceRange(10, 11, npub[10] == 'q' ? 'p' : 'q');
      expect(bech32Decode(broken), isNull);
      expect(normalisePublicKey('nsec1qqqqq'), isNull);
      expect(NostrKeyPair.tryParse(npub), isNull);
      expect(NostrKeyPair.tryParse(''), isNull);
      expect(NostrKeyPair.tryParse('not a key'), isNull);
      // Zero and n are out of range for a secp256k1 scalar.
      expect(NostrKeyPair.tryParse('00' * 32), isNull);
      expect(
        NostrKeyPair.tryParse(
            'fffffffffffffffffffffffffffffffebaaedce6af48a03bbfd25e8cd0364141'),
        isNull,
      );
    });
  });

  group('NIP-44 v2 (official vectors)', () {
    test('derives every conversation key', () {
      for (final v in nip44ConversationKeys) {
        expect(toHex(conversationKey(fromHex(v[0]), fromHex(v[1]))), v[2]);
      }
    });

    test('rejects invalid conversation-key inputs', () {
      for (final v in nip44InvalidConversationKeys) {
        expect(
          () => conversationKey(fromHex(v[0]), fromHex(v[1])),
          throwsA(isA<Object>()),
          reason: v[2],
        );
      }
    });

    test('matches the padding table', () {
      for (final v in nip44PaddedLengths) {
        expect(calcPaddedLen(v[0]), v[1], reason: 'unpadded ${v[0]}');
      }
    });

    test('encrypts to the exact expected payload', () {
      for (final v in nip44EncryptDecrypt) {
        expect(
          nip44Encrypt(v[2], fromHex(v[0]), nonce: fromHex(v[1])),
          v[3],
        );
      }
    });

    test('decrypts every expected payload', () {
      for (final v in nip44EncryptDecrypt) {
        expect(nip44Decrypt(v[3], fromHex(v[0])), v[2]);
      }
    });

    test('returns null for every invalid payload', () {
      for (final v in nip44InvalidDecrypt) {
        expect(nip44Decrypt(v[1], fromHex(v[0])), isNull, reason: v[2]);
      }
    });

    test('a tampered MAC fails rather than decrypting to garbage', () {
      final key = fromHex(nip44EncryptDecrypt.first[0]);
      final payload = nip44Encrypt('napstr download request', key);
      final raw = base64.decode(payload);
      raw[raw.length - 1] ^= 0x01;
      expect(nip44Decrypt(base64.encode(raw), key), isNull);
    });

    test('the wrong conversation key fails', () {
      final a = NostrKeyPair.generate();
      final b = NostrKeyPair.generate();
      final c = NostrKeyPair.generate();
      final ab = conversationKey(a.privateKey, b.publicKey);
      final ac = conversationKey(a.privateKey, c.publicKey);
      final payload = nip44Encrypt('hello', ab);
      expect(nip44Decrypt(payload, ab), 'hello');
      expect(nip44Decrypt(payload, ac), isNull);
    });

    test('the conversation key is symmetric between the two parties', () {
      final a = NostrKeyPair.generate();
      final b = NostrKeyPair.generate();
      expect(
        toHex(conversationKey(a.privateKey, b.publicKey)),
        toHex(conversationKey(b.privateKey, a.publicKey)),
      );
    });
  });

  group('NIP-01 events', () {
    test('computes the canonical id and a verifying signature', () {
      final keys = NostrKeyPair.generate();
      final event = signEvent(
        keys: keys,
        kind: 30421,
        tags: [
          ['d', 'a' * 64],
          ['t', 'napstr'],
        ],
        content: '{"protocol":"napstr/1"}',
      );
      expect(event.hasValidId, isTrue);
      expect(event.verify(), isTrue);
      expect(event.tagValue('d'), 'a' * 64);
      expect(event.tagValues('t').toList(), ['napstr']);
    });

    test('an altered field breaks the id, and an altered id breaks the sig',
        () {
      final keys = NostrKeyPair.generate();
      final event = signEvent(
          keys: keys, kind: 1, tags: const [], content: 'original');
      final retagged = NostrEvent(
        id: event.id,
        pubkey: event.pubkey,
        createdAt: event.createdAt,
        kind: event.kind,
        tags: event.tags,
        content: 'tampered',
        sig: event.sig,
      );
      expect(retagged.hasValidId, isFalse);
      expect(retagged.verify(), isFalse);

      final recomputed = NostrEvent(
        id: retagged.derivedId,
        pubkey: event.pubkey,
        createdAt: event.createdAt,
        kind: event.kind,
        tags: event.tags,
        content: 'tampered',
        sig: event.sig,
      );
      expect(recomputed.hasValidId, isTrue);
      expect(recomputed.verify(), isFalse); // signature no longer covers it
    });

    test('tryParse rejects malformed relay payloads', () {
      expect(NostrEvent.tryParse(null), isNull);
      expect(NostrEvent.tryParse('not a map'), isNull);
      expect(NostrEvent.tryParse({'id': 1}), isNull);
      expect(
        NostrEvent.tryParse({
          'id': 'a' * 64,
          'pubkey': 'b' * 64,
          'created_at': 1,
          'kind': 1,
          'tags': [
            [1, 2]
          ],
          'content': '',
          'sig': 'c' * 128,
        }),
        isNull,
        reason: 'tag values must be strings',
      );
    });

    test('reads the NIP-40 expiration tag', () {
      final keys = NostrKeyPair.generate();
      final past = signEvent(keys: keys, kind: 30422, content: '[]', tags: [
        ['expiration', '1000'],
      ]);
      expect(past.expiration, 1000);
      expect(past.isExpired, isTrue);
      final future = signEvent(keys: keys, kind: 30422, content: '[]', tags: [
        ['expiration', '4102444800'],
      ]);
      expect(future.isExpired, isFalse);
    });
  });

  group('NIP-59 gift wrap', () {
    test('round-trips a napstr download request', () {
      final sender = NostrKeyPair.generate();
      final receiver = NostrKeyPair.generate();
      const body = '{"type":"DOWNLOAD_REQUEST","protocol":"napstr/1"}';

      final wrap = giftWrap(
        sender: sender,
        receiverPubkeyHex: receiver.publicKeyHex,
        content: body,
        extraTags: [
          ['expiration', '4102444800'],
          ['client', 'bopwire'],
        ],
      );

      expect(wrap.kind, kindGiftWrap);
      expect(wrap.verify(), isTrue);
      // The wrap must be signed by a throwaway key, not the real sender.
      expect(wrap.pubkey, isNot(sender.publicKeyHex));
      expect(wrap.tagValue('p'), receiver.publicKeyHex);
      // Nothing recognisable leaks into the public event.
      expect(wrap.content.contains('DOWNLOAD_REQUEST'), isFalse);

      final unwrapped = unwrapGiftWrap(wrap, receiver);
      expect(unwrapped, isNotNull);
      expect(unwrapped!.senderPubkey, sender.publicKeyHex);
      expect(unwrapped.kind, kindPrivateDirectMessage);
      expect(unwrapped.content, body);
      expect(unwrapped.expiration, 4102444800);
      expect(unwrapped.tagValue('client'), 'bopwire');
    });

    test('a third party cannot unwrap it', () {
      final sender = NostrKeyPair.generate();
      final receiver = NostrKeyPair.generate();
      final eavesdropper = NostrKeyPair.generate();
      final wrap = giftWrap(
        sender: sender,
        receiverPubkeyHex: receiver.publicKeyHex,
        content: 'secret',
      );
      expect(unwrapGiftWrap(wrap, eavesdropper), isNull);
    });

    test('the wrap timestamp is randomised into the past', () {
      final sender = NostrKeyPair.generate();
      final receiver = NostrKeyPair.generate();
      final now = DateTime.now().millisecondsSinceEpoch ~/ 1000;
      final wrap = giftWrap(
        sender: sender,
        receiverPubkeyHex: receiver.publicKeyHex,
        content: 'x',
      );
      expect(wrap.createdAt, lessThanOrEqualTo(now));
      expect(wrap.createdAt, greaterThan(now - 2 * 24 * 60 * 60 - 5));
    });

    test('rejects a non-gift-wrap kind', () {
      final receiver = NostrKeyPair.generate();
      final impostor = signEvent(
        keys: NostrKeyPair.generate(),
        kind: 1,
        tags: const [],
        content: 'hi',
      );
      expect(unwrapGiftWrap(impostor, receiver), isNull);
    });
  });
}
