// Batched BIP-340 verification, off the UI isolate.
//
// PROTOCOL.md makes signature checking mandatory, but a pure-Dart secp256k1
// verify costs roughly 18 ms, and a single napstr search can touch hundreds
// of events. Doing that inline would freeze the UI for seconds, so batches
// are shipped to a short-lived isolate via [Isolate.run] and the caller
// streams results as each batch lands.
//
// The cheap checks (event id = sha256 of the canonical form, catalogue
// field validation, live-seeder pairing) all run first on the calling
// isolate, so only events that would actually be displayed pay for a
// signature check.

import 'dart:isolate';

import 'nostr/event.dart';
import 'nostr/hex.dart';
import 'nostr/schnorr.dart';

/// How many events one isolate hop verifies. ~18 ms each, so a batch of 40
/// is roughly 0.7 s of background work — small enough that results appear
/// steadily, large enough that isolate spawn overhead stays negligible.
const int kVerifyBatchSize = 40;

/// Verifies [events] and returns only those with a valid id and signature,
/// preserving order.
///
/// Callers that have already confirmed [NostrEvent.hasValidId] still get a
/// correct answer: the id is re-derived here, because skipping it would let
/// a forged event borrow another event's valid signature.
Future<List<NostrEvent>> verifyEvents(List<NostrEvent> events) async {
  if (events.isEmpty) return const [];

  // Reject anything malformed or with a mismatched id before paying for
  // elliptic-curve work.
  final candidates = <NostrEvent>[];
  final triples = <List<String>>[];
  for (final e in events) {
    if (!isLowerHex(e.id, 32) ||
        !isLowerHex(e.pubkey, 32) ||
        !isLowerHex(e.sig, 64)) {
      continue;
    }
    if (!e.hasValidId) continue;
    candidates.add(e);
    triples.add([e.id, e.pubkey, e.sig]);
  }
  if (candidates.isEmpty) return const [];

  final passed = await Isolate.run(() => _verifyTriples(triples));
  final out = <NostrEvent>[];
  for (var i = 0; i < candidates.length; i++) {
    if (passed[i]) out.add(candidates[i]);
  }
  return out;
}

/// Verifies [events] in [kVerifyBatchSize] chunks, yielding each verified
/// chunk as it completes so a search can render progressively.
Stream<List<NostrEvent>> verifyEventsStreamed(List<NostrEvent> events) async* {
  for (var i = 0; i < events.length; i += kVerifyBatchSize) {
    final end = i + kVerifyBatchSize;
    final chunk =
        events.sublist(i, end > events.length ? events.length : end);
    final verified = await verifyEvents(chunk);
    if (verified.isNotEmpty) yield verified;
  }
}

/// Runs inside the spawned isolate. Top-level so the closure sent by
/// [Isolate.run] captures nothing but the plain string data.
List<bool> _verifyTriples(List<List<String>> triples) {
  return [
    for (final t in triples)
      () {
        final id = tryFromHex(t[0]);
        final pk = tryFromHex(t[1]);
        final sig = tryFromHex(t[2]);
        if (id == null || pk == null || sig == null) return false;
        return schnorrVerify(id, pk, sig);
      }(),
  ];
}
