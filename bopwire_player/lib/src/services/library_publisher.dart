import 'dart:async';
import 'dart:convert';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter/foundation.dart';

import '../providers/wallet_provider.dart';
import 'library_scanner.dart';
import 'library_service.dart';
import 'node_service.dart';
import 'rats_client.dart';

/// DB2 — publishes the wallet's library to the off-chain, gossip-replicated
/// LibraryStore on the full node as a wallet-SIGNED delta. The node verifies
/// the signature, applies it (version-gated), then floods it to every other
/// node — so the library clones onto the whole mesh as it changes.
///
/// Canonical signed bytes (MUST match the node's `library_canonical` in
/// rats_api.cpp byte-for-byte or the signature won't verify):
///   "mclib1" || wallet(20) || version(8 LE) || ts(8 LE) ||
///   add_count(4 LE) || add_hashes(32·n) || del_count(4 LE) || del_hashes(32·m)
/// The native wallet signer SHA-256s these bytes and ECDSA-signs the digest;
/// the node verifies the same way (verify_data == sha256 + verify_ecdsa).
class LibraryPublisher {
  static bool _inFlight = false;

  /// Set when publishFull is called while a publish is already running.
  /// The in-flight call loops one more time before releasing the guard,
  /// so a library change that lands mid-publish (e.g. a download import
  /// finishing while a scan's publish RPC is in the air) is never
  /// silently dropped. Before this, the second call returned immediately
  /// and the new song sat un-announced (swarmSize 0 → invisible in
  /// Discover) until the next 30-minute scan.
  static bool _rerunRequested = false;

  /// Digest of the hash-set we last SUCCESSFULLY published this session.
  /// In-memory (cleared on app restart) so we still re-publish once per launch
  /// — covering the node ever losing the record — but skip the redundant
  /// ~N-hash re-upload on every reconnect / node-change / scan when nothing
  /// changed. Updated only on applied=true, so a failed publish retries.
  static String? _lastPublishedDigest;

  /// Highest version we have SENT this session. version == wall-clock ms,
  /// but two deltas composed inside the same millisecond (delete → the
  /// removal republish, or publishOffline → reconnect publishFull) would
  /// tie, and the node's version gate rejects `version <= stored` — the
  /// SECOND, newer snapshot would silently lose. Bump past the last sent
  /// value so every delta is strictly newer.
  static int _lastSentVersion = 0;

  // ---- Test seams ---------------------------------------------------------
  // The real publish path needs a native wallet signer plus a live librats
  // transport, neither of which exists in a unit test. When BOTH overrides
  // are set the publish logic (snapshot collection, empty-library handling,
  // digest gate, canonical byte layout, applied bookkeeping) runs for real
  // and only the signer + wire hop are faked.

  /// Test-only wallet: address (40-hex/20 bytes), pubkey, and a sign
  /// function returning the hex signature. When set, replaces
  /// WalletProvider.active.
  @visibleForTesting
  static ({String address, String publicKey, String Function(Uint8List) sign})?
      debugWalletOverride;

  /// Test-only transport: receives the verb + request body, returns the
  /// node's reply. When set, replaces node discovery + RatsClient.
  @visibleForTesting
  static Future<Object?> Function(String verb, Map<String, dynamic> body)?
      debugRequestOverride;

  /// Test-only: clear per-session publish state (in-flight guard + digest).
  @visibleForTesting
  static void debugResetSession() {
    _inFlight            = false;
    _rerunRequested      = false;
    _lastPublishedDigest = null;
    _lastSentVersion     = 0;
  }

  /// Test-only: wait until no publish is in flight, so a test can assert
  /// on the effects of an `unawaited(publishFull())`.
  @visibleForTesting
  static Future<void> debugDrain(
      {Duration timeout = const Duration(seconds: 5)}) async {
    final sw = Stopwatch()..start();
    while (_inFlight) {
      if (sw.elapsed > timeout) {
        throw TimeoutException('LibraryPublisher.debugDrain timed out');
      }
      await Future<void>.delayed(const Duration(milliseconds: 5));
    }
  }

  /// Publish the wallet's whole current library as a full delta (add =
  /// every content hash, del = none). An EMPTY library is a valid — and
  /// important — publish: it is how "I deleted my last song" and "my
  /// library went offline" propagate to the mesh. No-op without a wallet
  /// or a reachable full node. Safe to call repeatedly — the node's
  /// version gate makes it idempotent.
  static Future<void> publishFull() async {
    if (_inFlight) {
      _rerunRequested = true;
      return;
    }
    _inFlight = true;
    try {
      do {
        _rerunRequested = false;
        await _publishOnce();
      } while (_rerunRequested);
    } finally {
      _inFlight = false;
    }
  }

  /// "Go offline": publish an EMPTY snapshot regardless of what the local
  /// library holds, so the node (and via flood the whole mesh, and
  /// therefore the website / Discover / other players) knows nothing is
  /// available from this wallet. Called by LibratsDiscovery.disconnect()
  /// BEFORE connections drop. The next publishFull (reconnect reAnnounce,
  /// scan, import) republishes the real library — its digest differs from
  /// the empty one, so the digest gate cannot suppress it. If a full
  /// publish is racing us, the node's version gate (version == wall-clock
  /// ms) lets the later-composed delta win, which is this one.
  static Future<void> publishOffline() async {
    try {
      await _sendDelta(const <String>[]);
    } catch (e) {
      // ignore: avoid_print
      print('[db2] publishOffline failed: $e');
    }
  }

  static Future<void> _publishOnce() async {
    try {
      final lib = LibraryService.instance;
      await lib.ensureLoaded();
      // Tie the library to the SONG identity — the chain's fingerprint-resolved
      // CANONICAL hash — not the local file's content hash. Two encodings/
      // formats of the same song share one canonicalHash (the chain's fuzzy
      // fingerprint match collapses them), so they stop looking like different
      // songs in discovery. canonicalHash falls back to contentHash for a song
      // this device first-registered (they're equal then). The Set dedups
      // variants that resolve to the same canonical id.
      //
      // NOTE: an empty [hashes] is NOT skipped. The node stores the add set
      // as the authoritative snapshot, so publishing empty is the only way
      // deleting the LAST song (or wiping the library) ever propagates.
      final hashes = publishSetFor(lib.entries);

      // Digest-gate: if the library is identical to our last ACCEPTED publish
      // this session, skip the full upload — presence.hello (~150 B) already
      // keeps us online + bound. Saves re-sending ~32 KB (at 474 songs) on
      // every reAnnounce when nothing changed. The empty set has its own
      // distinct digest (sha256 of ""), so "just emptied" is correctly seen
      // as a CHANGE, never as "unchanged, skip".
      if (digestGateSkips(hashes)) {
        // ignore: avoid_print
        print('[db2] library unchanged (${hashes.length} songs) — '
            'skipping library.delta');
        return;
      }

      await _sendDelta(hashes, resubmitUnknown: true);
    } catch (e) {
      // ignore: avoid_print
      print('[db2] publishFull failed: $e');
    }
  }

  /// The set of song ids a full publish carries. Pure + visible for tests:
  /// an empty library maps to an empty (valid) publish set, never to
  /// "nothing to do".
  @visibleForTesting
  static List<String> publishSetFor(Iterable<LibraryEntry> entries) {
    final ids = <String>{};
    for (final e in entries) {
      if (e.songId.isNotEmpty) ids.add(e.songId);
    }
    return ids.toList();
  }

  /// Whether the session digest gate would skip publishing [hashes].
  /// Only ever true when [hashes] is EXACTLY the set the node last
  /// confirmed applied this session.
  @visibleForTesting
  static bool digestGateSkips(List<String> hashes) =>
      _digestOf(hashes) == _lastPublishedDigest;

  /// Sign + send one full-snapshot library.delta whose add set is
  /// [hashes] (del is always empty — the snapshot itself carries
  /// removals). Records the digest on a confirmed apply and optionally
  /// feeds the node's `unknown[]` back into the chain-resubmit path.
  static Future<void> _sendDelta(List<String> hashes,
      {bool resubmitUnknown = false}) async {
    final String address;
    final String pubkey;
    final String Function(Uint8List) signer;
    final dbgWallet = debugWalletOverride;
    if (dbgWallet != null) {
      address = dbgWallet.address;
      pubkey  = dbgWallet.publicKey;
      signer  = dbgWallet.sign;
    } else {
      final wp = WalletProvider.active;
      final info = wp?.info;
      if (wp == null || info == null) return;
      address = info.address;
      pubkey  = info.publicKey;
      signer  = wp.sign;
    }

    final dbgRequest = debugRequestOverride;
    final homePid = dbgRequest != null
        ? 'debug'
        : await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 8));
    if (homePid.isEmpty) return;

    // version == ts (wall-clock ms): monotonic without a stored counter, and
    // roughly consistent across a user's devices (the later edit wins).
    // Strictly increasing within this session (see _lastSentVersion) so a
    // same-millisecond successor can't be rejected by the node's gate.
    var ts = DateTime.now().millisecondsSinceEpoch;
    if (ts <= _lastSentVersion) ts = _lastSentVersion + 1;
    _lastSentVersion = ts;

    final wallet20 = _hexToBytes(address, 20);
    if (wallet20 == null) return;
    final add32 = <Uint8List>[];
    for (final h in hashes) {
      final b = _hexToBytes(h, 32);
      if (b == null) return; // a malformed hash must not poison the signature
      add32.add(b);
    }

    final canon = BytesBuilder();
    canon.add(ascii.encode('mclib1')); // 6-byte domain tag
    canon.add(wallet20);               // 20
    canon.add(_u64le(ts));             // version (8 LE)
    canon.add(_u64le(ts));             // ts      (8 LE)
    canon.add(_u32le(add32.length));   // add_count (4 LE) — 0 is valid
    for (final b in add32) {
      canon.add(b);                    // 32 each
    }
    canon.add(_u32le(0));              // del_count = 0 (full publish)

    final sig = signer(Uint8List.fromList(canon.toBytes()));

    final body = <String, dynamic>{
      'wallet': address,
      'pubkey': pubkey,
      'version': ts,
      'ts': ts,
      'add': hashes,
      'del': const <String>[],
      'sig': sig,
    };
    final reply = dbgRequest != null
        ? await dbgRequest('library.delta', body)
        : await RatsClient.instance.request(
            homePid,
            'library.delta',
            body,
            timeout: const Duration(seconds: 10),
          );

    final applied = (reply is Map) && (reply['applied'] == true);
    // Remember the digest only on a confirmed apply, so a failed/rejected
    // publish retries on the next reAnnounce instead of being gated out.
    if (applied) _lastPublishedDigest = _digestOf(hashes);
    // The node replies with `unknown[]`: the published content hashes that
    // aren't registered on chain yet (fresh chain, or one wiped between
    // sessions). Re-fire fingerprint.submit for each so the songs actually
    // land in the mempool and get minted — library.delta alone only records
    // off-chain library membership, not the chain. This is the resubmit the
    // old swarm.hello reply path used to drive; it now rides on DB2.
    final unknown = (reply is Map)
        ? ((reply['unknown'] as List?)?.cast<String>() ?? const <String>[])
        : const <String>[];
    // ignore: avoid_print
    print('[db2] library.delta v$ts add=${hashes.length} applied=$applied'
          ' unknown=${unknown.length}');
    if (resubmitUnknown && unknown.isNotEmpty) {
      // Released the in-flight guard in finally below; resubmit is awaited so
      // its _processFile work completes before publishFull returns (matches
      // the old scanOnce ordering under the scanner's _running guard).
      await LibraryScanner.instance.resubmitUnknown(unknown);
    }
  }

  static Uint8List _u64le(int x) {
    final b = Uint8List(8);
    for (int i = 0; i < 8; i++) {
      b[i] = (x >> (8 * i)) & 0xFF;
    }
    return b;
  }

  /// Order-independent digest of the content-hash set (sorted + SHA-256),
  /// used purely to detect whether the library changed since the last publish.
  static String _digestOf(List<String> hashes) {
    final sorted = List<String>.of(hashes)..sort();
    return crypto.sha256.convert(utf8.encode(sorted.join())).toString();
  }

  static Uint8List _u32le(int x) {
    final b = Uint8List(4);
    for (int i = 0; i < 4; i++) {
      b[i] = (x >> (8 * i)) & 0xFF;
    }
    return b;
  }

  static Uint8List? _hexToBytes(String hexIn, int n) {
    var hex = hexIn;
    if (hex.startsWith('0x') || hex.startsWith('0X')) hex = hex.substring(2);
    if (hex.length != n * 2) return null;
    final out = Uint8List(n);
    for (int i = 0; i < n; i++) {
      final v = int.tryParse(hex.substring(2 * i, 2 * i + 2), radix: 16);
      if (v == null) return null;
      out[i] = v;
    }
    return out;
  }
}
