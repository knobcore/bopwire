// NIP-59 gift wrapping and the NIP-17 kind-14 direct message, as napstr
// uses them for DOWNLOAD_REQUEST / DOWNLOAD_OFFER / DOWNLOAD_REFUSED.
//
// Layering, outermost first:
//   kind 1059 gift wrap  — signed by a throwaway key, p-tagged to receiver,
//                          content = nip44(ephemeral -> receiver, seal)
//   kind 13   seal       — signed by the real sender, no tags,
//                          content = nip44(sender -> receiver, rumor)
//   kind 14   rumor      — unsigned; the actual message
//
// Timestamps on the two outer layers are randomised up to two days into the
// past so relays cannot correlate a wrap with the moment it was sent.

import 'dart:convert';
import 'dart:math';
import 'dart:typed_data';

import 'event.dart';
import 'hex.dart';
import 'keys.dart';
import 'nip44.dart';

const int kindSeal = 13;
const int kindPrivateDirectMessage = 14;
const int kindGiftWrap = 1059;

const int _twoDaysSeconds = 2 * 24 * 60 * 60;

final Random _jitter = Random.secure();

int _randomisedTimestamp() =>
    (DateTime.now().millisecondsSinceEpoch ~/ 1000) -
    _jitter.nextInt(_twoDaysSeconds);

/// Wraps [content] as a NIP-17 message from [sender] to [receiverPubkeyHex].
/// [extraTags] are added to the rumor alongside the mandatory `p` tag —
/// napstr puts its `expiration` and `client` tags there.
NostrEvent giftWrap({
  required NostrKeyPair sender,
  required String receiverPubkeyHex,
  required String content,
  List<List<String>> extraTags = const [],
  int? rumorCreatedAt,
}) {
  final receiver = fromHex(receiverPubkeyHex);

  final rumor = buildRumor(
    pubkeyHex: sender.publicKeyHex,
    kind: kindPrivateDirectMessage,
    tags: [
      ['p', receiverPubkeyHex],
      ...extraTags,
    ],
    content: content,
    createdAt: rumorCreatedAt,
  );

  final senderConv = conversationKey(sender.privateKey, receiver);
  final seal = signEvent(
    keys: sender,
    kind: kindSeal,
    tags: const [],
    content: nip44Encrypt(jsonEncode(rumor), senderConv),
    createdAt: _randomisedTimestamp(),
  );

  final ephemeral = NostrKeyPair.generate();
  final ephemeralConv = conversationKey(ephemeral.privateKey, receiver);
  return signEvent(
    keys: ephemeral,
    kind: kindGiftWrap,
    tags: [
      ['p', receiverPubkeyHex],
    ],
    content: nip44Encrypt(jsonEncode(seal.toJson()), ephemeralConv),
    createdAt: _randomisedTimestamp(),
  );
}

/// A successfully unwrapped NIP-17 message.
class UnwrappedMessage {
  const UnwrappedMessage({
    required this.senderPubkey,
    required this.content,
    required this.tags,
    required this.createdAt,
    required this.kind,
  });

  final String senderPubkey;
  final String content;
  final List<List<String>> tags;
  final int createdAt;
  final int kind;

  String? tagValue(String name) {
    for (final t in tags) {
      if (t.length >= 2 && t[0] == name) return t[1];
    }
    return null;
  }

  int? get expiration {
    final v = tagValue('expiration');
    return v == null ? null : int.tryParse(v);
  }
}

/// Unwraps a kind-1059 event addressed to [receiver].
///
/// Returns null unless every layer checks out. In particular the rumor's
/// claimed author must equal the seal's *signature-verified* author, which
/// is what stops a third party from forging a DOWNLOAD_OFFER that appears
/// to come from a seeder we requested.
UnwrappedMessage? unwrapGiftWrap(NostrEvent wrap, NostrKeyPair receiver) {
  try {
    if (wrap.kind != kindGiftWrap) return null;
    if (!wrap.verify()) return null;

    final wrapPub = tryFromHex(wrap.pubkey);
    if (wrapPub == null) return null;
    final sealJson =
        nip44Decrypt(wrap.content, conversationKey(receiver.privateKey, wrapPub));
    if (sealJson == null) return null;

    final seal = NostrEvent.tryParse(_decode(sealJson));
    if (seal == null || seal.kind != kindSeal) return null;
    if (seal.tags.isNotEmpty) return null; // NIP-59: a seal carries no tags
    if (!seal.verify()) return null;

    final sealPub = tryFromHex(seal.pubkey);
    if (sealPub == null) return null;
    final rumorJson =
        nip44Decrypt(seal.content, conversationKey(receiver.privateKey, sealPub));
    if (rumorJson == null) return null;

    final rumor = _decode(rumorJson);
    if (rumor is! Map) return null;
    // The seal's verified pubkey is the only trustworthy author claim.
    if (rumor['pubkey'] != seal.pubkey) return null;
    final kind = rumor['kind'];
    final content = rumor['content'];
    final createdAt = rumor['created_at'];
    if (kind is! int || content is! String || createdAt is! int) return null;

    final tags = <List<String>>[];
    final rawTags = rumor['tags'];
    if (rawTags is! List) return null;
    for (final t in rawTags) {
      if (t is! List) return null;
      final row = <String>[];
      for (final v in t) {
        if (v is! String) return null;
        row.add(v);
      }
      tags.add(row);
    }

    return UnwrappedMessage(
      senderPubkey: seal.pubkey,
      content: content,
      tags: tags,
      createdAt: createdAt,
      kind: kind,
    );
  } catch (_) {
    return null;
  }
}

Object? _decode(String s) {
  try {
    return jsonDecode(s);
  } catch (_) {
    return null;
  }
}

/// Convenience: the Uint8List type used across this layer.
typedef KeyBytes = Uint8List;
