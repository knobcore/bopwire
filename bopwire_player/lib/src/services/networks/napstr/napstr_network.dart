// napstr — Nostr for discovery, Tor for transfer.
//
// WHAT NAPSTR IS (github.com/lnbits/napstr, PROTOCOL.md)
// -----------------------------------------------------
// It is a live, actively developed peer-to-peer audio network with a
// published, versioned wire specification. There is no server, no HTTP API
// and — despite the lnbits organisation — no Lightning, no invoices and no
// sats anywhere in the protocol. Two transports do everything:
//
//   * Nostr relays carry the public catalogue (kind 30421, one addressable
//     event per file), live seeder heartbeats (kind 30422, 10-minute
//     expiry) and the NIP-17 encrypted request/offer handshake.
//   * A temporary Tor v3 onion service carries the file bytes over a
//     bespoke length-prefixed TCP protocol. PROTOCOL.md is explicit that
//     there is no clearnet fallback.
//
// A track is identified by the SHA-256 of its complete bytes, so identical
// files from different publishers collapse into one entry with several
// seeders.
//
// WHAT THIS IMPLEMENTATION DOES AND DOES NOT DO
// ---------------------------------------------
// Search is fully implemented and needs no credentials at all: relays are
// public and reading them requires no key.
//
// Downloading needs two things bopwire cannot supply from pure Dart:
//   1. a Nostr identity, to send the NIP-17 request (generated and stored
//      in the wallet vault, or ephemeral for the session if it is locked);
//   2. a reachable SOCKS5 proxy that can route `.onion`, because a Tor
//      client cannot be embedded in Dart. The user supplies one — system
//      tor on 127.0.0.1:9050, Tor Browser on 127.0.0.1:9150, or Orbot on
//      Android. Without it [download] fails immediately with a clear
//      message rather than pretending.
//
// napstr has no folder, album or directory concept — the catalogue is a
// flat set of content-addressed files — so [listFolder] returns an empty
// list and no result is ever marked `isFolder`.

import 'dart:async';
import 'dart:convert';
import 'dart:math';

import '../external_network.dart';
import '../network_credentials.dart';
import 'catalogue.dart';
import 'nostr/event.dart';
import 'nostr/hex.dart';
import 'nostr/keys.dart';
import 'nostr/nip59.dart';
import 'nostr/relay_pool.dart';
import 'search_tokens.dart';
import 'socks5.dart';
import 'tor_service.dart';
import 'transfer.dart';
import 'verifier.dart';

/// Most results one search will surface. Every one of these costs a
/// ~18 ms signature verification, so the cap is a real budget, not a
/// cosmetic limit.
const int kMaxSearchResults = 120;

/// Relay-side `limit` for catalogue filters, matching the reference client.
const int kRelayResultLimit = 500;

/// Availability heartbeats to pull per search.
const int kAvailabilityQueryLimit = 1000;

/// `#d` batch size when hydrating catalogue entries during a browse.
const int kIdentifierBatchSize = 75;

/// Stage logging for the download path. This exists because "stuck at
/// 0%" is indistinguishable from a dozen different failures without it —
/// relay delivery, seeder silence, Tor, and the transfer itself all look
/// identical from the UI.
void _nlog(String m) {
  // ignore: avoid_print
  print('[napstr] $m');
}

class NapstrNetwork implements ExternalNetwork {
  NapstrNetwork();

  @override
  String get id => 'napstr';

  @override
  String get displayName => 'Napstr';

  // ---------------------------------------------------------------- creds

  /// Field keys, so callers (and tests) do not repeat string literals.
  static const String fieldRelays = 'relays';
  static const String fieldSecretKey = 'nsec';
  static const String fieldTorSocks = 'tor_socks';

  @override
  List<NetworkCredentialField> get credentialFields => const [
        // Not required: reading the public catalogue needs no login at all,
        // which is the honest answer for a relay-based network.
        NetworkCredentialField(
          key: fieldRelays,
          label: 'Nostr relays',
          hint: 'Comma-separated. Leave blank for the napstr defaults '
              '(relay.damus.io, nos.lol, relay.primal.net, …).',
          required_: false,
        ),
        NetworkCredentialField(
          key: fieldTorSocks,
          label: 'Tor SOCKS5 proxy',
          hint: 'host:port — required for downloads only. System tor is '
              '127.0.0.1:9050, Tor Browser 127.0.0.1:9150, Orbot on '
              'Android 127.0.0.1:9050. napstr transfers run over Tor and '
              'have no clearnet fallback.',
          required_: false,
        ),
        NetworkCredentialField(
          key: fieldSecretKey,
          label: 'Nostr private key',
          hint: 'nsec1… or 64-character hex. Optional — one is generated '
              'and stored in your wallet vault if you leave this blank. '
              'Only needed to request downloads, never to search.',
          secret: true,
          required_: false,
        ),
      ];

  @override
  bool get isConfigured =>
      NetworkCredentials.instance.hasRequired(id, credentialFields);

  // --------------------------------------------------------------- state

  bool _enabled = false;

  @override
  bool get enabled => _enabled;

  @override
  set enabled(bool v) => _enabled = v;

  NetworkStatus _status = NetworkStatus.disconnected;
  final StreamController<NetworkStatus> _statusController =
      StreamController<NetworkStatus>.broadcast();

  @override
  NetworkStatus get status => _status;

  @override
  Stream<NetworkStatus> get statusChanges => _statusController.stream;

  void _setStatus(NetworkStatus s) {
    if (_status == s) return;
    _status = s;
    if (!_statusController.isClosed) _statusController.add(s);
  }

  RelayPool? _pool;
  NostrKeyPair? _keys;
  Socks5Endpoint? _proxy;
  Future<void>? _connecting;
  bool _inboxAnnounced = false;

  /// In-flight downloads keyed by napstr file id. These networks serve
  /// ONE transfer per peer: measured live, a second concurrent request
  /// for the same file is not a retry — it starves (A pulled 16 MB while
  /// B, started 3 s later, got nothing for 60 s) and leaves the seeder's
  /// slot tied up so the NEXT request fails too. So a second request for
  /// a file waits for the first to finish rather than racing it.
  final Map<String, Future<void>> _inflightByFile = {};

  /// Cancel handles for every running transfer, so [shutdown] can abort
  /// them gracefully (CANCEL frame, flushed) before the process exits.
  final Set<TransferCancel> _liveCancels = {};

  /// Human-readable reason for the most recent [NetworkStatus.error].
  String? lastError;

  /// True once a Tor SOCKS5 proxy has been configured and answered a
  /// greeting. Downloads are impossible while this is false.
  bool get torAvailable => _proxy != null && _torReachable;
  bool _torReachable = false;

  /// How Tor was resolved, or why it wasn't — shown to the user instead
  /// of a bare "download failed".
  String? get torDetail => _torDetail;
  String? _torDetail;

  /// The identity downloads are requested under, once [connect] has run.
  String? get publicKeyHex => _keys?.publicKeyHex;

  /// True when the identity is session-only because the wallet vault was
  /// locked and the generated key could not be persisted.
  bool get identityIsEphemeral => _identityEphemeral;
  bool _identityEphemeral = false;

  // ------------------------------------------------------------- connect

  @override
  Future<void> connect() {
    final inFlight = _connecting;
    if (inFlight != null) return inFlight;
    if (_status == NetworkStatus.connected && (_pool?.connectedCount ?? 0) > 0) {
      return Future.value();
    }
    final f = _connect();
    _connecting = f;
    return f.whenComplete(() => _connecting = null);
  }

  Future<void> _connect() async {
    _setStatus(NetworkStatus.connecting);
    lastError = null;
    try {
      final creds =
          await NetworkCredentials.instance.load(id, credentialFields);

      // 1. Identity. Optional for search; required to request a download.
      await _loadIdentity(creds[fieldSecretKey]);

      // 2. Tor SOCKS5 proxy. Resolved by TorService, which honours a
      //    configured endpoint first, then looks for a daemon already
      //    running (system tor, Orbot on Android, Tor Browser) before
      //    falling back to one we ship. Previously this only read the
      //    configured field, so a machine with Tor already running still
      //    got nothing.
      final tor = await TorService.instance
          .ensureProxy(configured: creds[fieldTorSocks]);
      _proxy = tor.endpoint;
      _torReachable = tor.ok;
      _torDetail = tor.detail;

      // 3. Relays.
      final pool = RelayPool(parseRelayList(creds[fieldRelays]));
      final up = await pool.connect();
      if (up == 0) {
        await pool.close();
        lastError = 'no napstr relay accepted a connection '
            '(${pool.errors.take(2).join('; ')})';
        _setStatus(NetworkStatus.error);
        return;
      }
      await _pool?.close();
      _pool = pool;
      _inboxAnnounced = false;
      _setStatus(NetworkStatus.connected);
    } catch (e) {
      lastError = e.toString();
      _setStatus(NetworkStatus.error);
    }
  }

  Future<void> _loadIdentity(String? configured) async {
    if (configured != null && configured.isNotEmpty) {
      final parsed = NostrKeyPair.tryParse(configured);
      if (parsed != null) {
        _keys = parsed;
        _identityEphemeral = false;
        return;
      }
      // A stored value we cannot parse is a real misconfiguration; say so
      // rather than silently replacing the user's key.
      throw const FormatException(
          'napstr: the stored Nostr private key is not a valid nsec or '
          '64-character hex key');
    }
    if (_keys != null) return;
    final generated = NostrKeyPair.generate();
    _keys = generated;
    try {
      await NetworkCredentials.instance.set(
        id,
        credentialFields
            .firstWhere((f) => f.key == fieldSecretKey),
        generated.nsec,
      );
      _identityEphemeral = false;
    } catch (_) {
      // The wallet vault is locked, so the key cannot be persisted. Search
      // is unaffected; downloads still work, under a session identity.
      _identityEphemeral = true;
    }
  }

  @override
  Future<void> disconnect() async {
    final pool = _pool;
    _pool = null;
    _inboxAnnounced = false;
    await pool?.close();
    _setStatus(NetworkStatus.disconnected);
  }

  Future<RelayPool?> _readyPool() async {
    if (_status != NetworkStatus.connected || (_pool?.connectedCount ?? 0) == 0) {
      await connect();
    }
    // connectedCount alone is not enough: a relay that dropped us while
    // idle can leave a HALF-OPEN socket that still reports open, so the
    // count looks healthy while nothing actually arrives. ensureLive()
    // reopens anything whose socket has since died — cheap when they are
    // all up, and the difference between a download working and it
    // timing out after 60 seconds with "no seeder answered".
    final pool = _pool;
    if (pool != null) {
      final live = await pool.ensureLive();
      if (live == 0) {
        // Everything is down; a full reconnect also re-announces our
        // inbox, which a bare socket reopen would not.
        await connect();
      }
    }
    return _pool;
  }

  // -------------------------------------------------------------- search

  @override
  Stream<List<ExternalTrack>> search(String query) async* {
    final pool = await _readyPool();
    if (pool == null) return;

    final trimmed = query.trim();
    final availability = await _fetchAvailability(pool);
    if (availability.isEmpty) return;

    final events = trimmed.isEmpty
        ? await _browseEvents(pool, availability)
        : await _searchEvents(pool, trimmed);

    // Cheap pass first: structure, claims, local match and — critically —
    // "is anyone actually seeding this right now". Signature verification
    // is expensive, so only survivors of this pass pay for it.
    final candidates = <NostrEvent>[];
    final withdrawn = <String>{};
    final seenPairs = <String>{};
    for (final e in events) {
      final parsed = parseCatalogueEvent(e, verifySignature: false);
      switch (parsed.outcome) {
        case CatalogueParseOutcome.withdrawn:
          withdrawn.add('${e.pubkey}:${parsed.fileId}');
          continue;
        case CatalogueParseOutcome.invalid:
          continue;
        case CatalogueParseOutcome.record:
          break;
      }
      final record = parsed.record!;
      if (!e.hasValidId) continue;
      if (!availability.isSeeding(record.publisherPubkey, record.fileId)) {
        continue;
      }
      if (trimmed.isNotEmpty &&
          !searchMatches(trimmed, record.searchableFields)) {
        continue;
      }
      if (!seenPairs.add('${record.publisherPubkey}:${record.fileId}')) continue;
      candidates.add(e);
    }

    // Rank by live seeder count, as PROTOCOL.md prescribes, then bound.
    candidates.sort((a, b) {
      final ad = a.tagValue('d') ?? '';
      final bd = b.tagValue('d') ?? '';
      final bySeeders = availability
          .seederCount(bd)
          .compareTo(availability.seederCount(ad));
      if (bySeeders != 0) return bySeeders;
      return b.createdAt.compareTo(a.createdAt);
    });
    final bounded = candidates.take(kMaxSearchResults * 2).toList();

    // Heartbeats first, and only the ones backing a candidate. A seeder is
    // credible only if the signed heartbeat naming it verifies; doing this
    // up front means each batch of verified catalogue events can be turned
    // into results immediately.
    final neededHeartbeats = <String, NostrEvent>{};
    for (final e in bounded) {
      final hb = availability.heartbeatFor(e.pubkey, e.tagValue('d') ?? '');
      if (hb != null) neededHeartbeats[hb.id] = hb;
    }
    final liveSeeders = <String>{};
    for (final ok in await verifyEvents(neededHeartbeats.values.toList())) {
      liveSeeders.add(ok.pubkey);
    }
    if (liveSeeders.isEmpty) return;

    // Verify catalogue signatures off-isolate, aggregating by file id and
    // yielding a growing snapshot as each batch lands. Verification is the
    // slow part of a search, so the UI fills in progressively rather than
    // waiting several seconds for the whole set.
    final aggregate = <String, _Aggregated>{};
    var emittedCount = -1;

    await for (final verified in verifyEventsStreamed(bounded)) {
      for (final e in verified) {
        final record = parseCatalogueEvent(e, verifySignature: false).record;
        if (record == null) continue;
        if (!liveSeeders.contains(record.publisherPubkey)) continue;
        if (withdrawn.contains('${record.publisherPubkey}:${record.fileId}')) {
          continue;
        }
        (aggregate[record.fileId] ??= _Aggregated(record)).add(record);
      }
      if (aggregate.isEmpty || aggregate.length == emittedCount) continue;
      emittedCount = aggregate.length;
      yield _rank(aggregate);
      if (aggregate.length >= kMaxSearchResults) break;
    }
  }

  /// Most-seeded first, then alphabetical, bounded to [kMaxSearchResults].
  List<ExternalTrack> _rank(Map<String, _Aggregated> aggregate) {
    final entries = aggregate.values.toList()
      ..sort((a, b) {
        final bySeeders = b.publishers.length.compareTo(a.publishers.length);
        if (bySeeders != 0) return bySeeders;
        return a.displayTitle
            .toLowerCase()
            .compareTo(b.displayTitle.toLowerCase());
      });
    return [
      for (final agg in entries.take(kMaxSearchResults)) agg.toTrack(id),
    ];
  }

  /// Named search: one bounded NIP-50 filter plus up to four indexed `#t`
  /// hashtag filters, exactly as PROTOCOL.md describes.
  ///
  /// Each filter goes out as its own subscription, which is not a stylistic
  /// choice: relays that do not implement NIP-50 reject the *whole* REQ
  /// with `CLOSED ... unrecognised filter item: search`, so bundling the
  /// hashtag filters alongside the search filter loses them too. nos.lol
  /// and relay.primal.net both behave this way, which silently reduced
  /// every named search to zero results until the filters were split.
  ///
  /// Neither filter is trusted: hashtags are OR filters and NIP-50 ranking
  /// varies per relay, so every result is re-matched locally afterwards.
  Future<List<NostrEvent>> _searchEvents(RelayPool pool, String query) async {
    final filters = <Map<String, Object?>>[
      {
        'kinds': [kCatalogueKind],
        '#t': [kCatalogueHashtag],
        'search': query,
        'limit': kRelayResultLimit,
      },
      for (final token in queryTagTokens(query))
        {
          'kinds': [kCatalogueKind],
          '#t': [token],
          'limit': kRelayResultLimit,
        },
    ];
    final results = await Future.wait([
      for (final filter in filters)
        pool
            .query([filter], timeout: const Duration(seconds: 8))
            .map((e) => e.event)
            .toList(),
    ]);
    final byId = <String, NostrEvent>{};
    for (final batch in results) {
      for (final e in batch) {
        byId[e.id] = e;
      }
    }
    return byId.values.toList();
  }

  /// Empty query. PROTOCOL.md forbids enumerating the catalogue blindly, so
  /// browsing is driven from live availability: rank the currently seeded
  /// file ids, take a bounded window, and hydrate exactly those by `#d`.
  Future<List<NostrEvent>> _browseEvents(
    RelayPool pool,
    _AvailabilityIndex availability,
  ) async {
    final ranked = availability.rankedFileIds(kMaxSearchResults * 2);
    if (ranked.isEmpty) return const [];
    final out = <NostrEvent>[];
    for (var i = 0; i < ranked.length; i += kIdentifierBatchSize) {
      final end = min(i + kIdentifierBatchSize, ranked.length);
      final batch = ranked.sublist(i, end);
      final events = await pool.query([
        {
          'kinds': [kCatalogueKind],
          '#t': [kCatalogueHashtag],
          '#d': batch,
          'limit': batch.length * 8,
        }
      ], timeout: const Duration(seconds: 8)).map((e) => e.event).toList();
      out.addAll(events);
    }
    return out;
  }

  Future<_AvailabilityIndex> _fetchAvailability(RelayPool pool) async {
    final since = DateTime.now().millisecondsSinceEpoch ~/ 1000 - 12 * 60;
    final events = await pool.query([
      {
        'kinds': [kAvailabilityKind],
        '#t': [kAvailabilityHashtag],
        'since': since,
        'limit': kAvailabilityQueryLimit,
      }
    ], timeout: const Duration(seconds: 8)).map((e) => e.event).toList();

    final index = _AvailabilityIndex();
    for (final e in events) {
      if (!e.hasValidId) continue;
      // Structural parse only. Signatures on the heartbeats that actually
      // back an emitted result are verified later, once we know which
      // handful of them matter.
      final hb = parseAvailabilityEvent(e, verifySignature: false);
      if (hb == null) continue;
      index.add(hb, e);
    }
    return index;
  }

  // ---------------------------------------------------------- listFolder

  /// napstr has no folder, directory or album entity. The catalogue is a
  /// flat set of files keyed by content hash; `album` is a free-text
  /// metadata string with no addressable identity and no way to enumerate
  /// its members. Nothing this network returns is ever a folder, so this
  /// is unreachable in practice and honestly returns nothing.
  @override
  Future<List<ExternalTrack>> listFolder(ExternalTrack folder) async => const [];

  // ------------------------------------------------------------ download

  @override
  Stream<DownloadProgress> download(ExternalTrack track, String destDir) {
    final controller = StreamController<DownloadProgress>();
    final cancel = TransferCancel();
    controller.onCancel = () {
      cancel.abort();
    };

    final fileId = NapstrTrackRef.decode(track.id)?.fileId;
    Future<void> run() => _runDownload(track, destDir, controller, cancel);

    if (fileId == null) {
      unawaited(run());
    } else {
      // Chain onto whatever transfer for this file is already running
      // (see _inflightByFile). If we were cancelled while waiting our
      // turn, _runDownload notices immediately and does nothing.
      final previous = _inflightByFile[fileId] ?? Future<void>.value();
      final mine = previous.catchError((Object _) {}).then((_) => run());
      _inflightByFile[fileId] = mine;
      unawaited(mine.whenComplete(() {
        if (identical(_inflightByFile[fileId], mine)) {
          _inflightByFile.remove(fileId);
        }
      }));
    }
    return controller.stream;
  }

  Future<void> _runDownload(
    ExternalTrack track,
    String destDir,
    StreamController<DownloadProgress> out,
    TransferCancel cancel,
  ) async {
    void fail(String message) {
      _nlog('FAIL ${track.title}: $message');
      if (out.isClosed) return;
      out.add(DownloadProgress(
        trackId: track.id,
        receivedBytes: 0,
        error: message,
      ));
    }

    StreamSubscription<RelayEvent>? inbox;
    StreamController<DownloadOffer>? offers;
    _liveCancels.add(cancel);
    try {
      if (cancel.isCancelled) return; // cancelled while queued behind another
      _nlog('download start "${track.title}" -> $destDir');
      final ref = NapstrTrackRef.decode(track.id);
      if (ref == null) {
        fail('napstr: unrecognised track handle');
        return;
      }
      if (ref.seeders.isEmpty) {
        fail('napstr: no seeder was recorded for this track');
        return;
      }

      final pool = await _readyPool();
      if (pool == null) {
        fail('napstr: ${lastError ?? 'not connected to any relay'}');
        return;
      }
      final keys = _keys;
      if (keys == null) {
        fail('napstr: no Nostr identity is available to request a download');
        return;
      }
      final proxy = _proxy;
      if (proxy == null) {
        fail('napstr downloads run over Tor and have no clearnet fallback. '
            'Set the "Tor SOCKS5 proxy" field (system tor: 127.0.0.1:9050, '
            'Tor Browser: 127.0.0.1:9150, Orbot on Android: 127.0.0.1:9050) '
            'and make sure Tor is running.');
        return;
      }
      if (!_torReachable) {
        _torReachable = await socks5ProxyReachable(proxy);
        if (!_torReachable) {
          fail('napstr: no SOCKS5 proxy answered at $proxy. Start Tor (or '
              'Orbot) and try again — transfers cannot bypass it.');
          return;
        }
      }

      // Re-fetch the signed catalogue entry: the filename and byte size we
      // check the WELCOME frame against must come from a verified event,
      // not from whatever the caller handed us.
      _nlog('relays live=${pool.connectedCount}, seeders=${ref.seeders.length}, '
          'proxy=${_proxy?.host}:${_proxy?.port}');
      final record = await _fetchRecord(pool, ref.fileId, ref.seeders);
      if (record == null) {
        fail('napstr: no valid catalogue entry for this file is on the '
            'relays any more');
        return;
      }
      if (cancel.isCancelled) return;

      out.add(DownloadProgress(
        trackId: track.id,
        receivedBytes: 0,
        totalBytes: record.size,
      ));

      await _announceInbox(pool, keys);
      _nlog('inbox announced as ${keys.publicKeyHex.substring(0, 12)}…');

      // PROTOCOL.md: "The downloader selects up to three distinct active
      // seeders and sends each one" a request.
      final requestId = _uuidV4();
      final targets = ref.seeders.take(3).toList();
      offers = StreamController<DownloadOffer>();

      inbox = pool.subscribe([
        {
          'kinds': [kindGiftWrap],
          '#p': [keys.publicKeyHex],
          'limit': 0,
        }
      ]).listen((re) {
        final unwrapped = unwrapGiftWrap(re.event, keys);
        if (unwrapped == null) return;
        if (unwrapped.kind != kindPrivateDirectMessage) return;
        final expiry = unwrapped.expiration;
        // PROTOCOL.md: a signal without a future expiration is rejected.
        if (expiry == null ||
            expiry <= DateTime.now().millisecondsSinceEpoch ~/ 1000) {
          return;
        }
        Object? body;
        try {
          body = jsonDecode(unwrapped.content);
        } catch (_) {
          return;
        }
        final offer = DownloadOffer.validate(
          body,
          expectedRequestId: requestId,
          expectedFileId: ref.fileId,
          senderPubkey: unwrapped.senderPubkey,
          requestedSeeders: targets.toSet(),
        );
        if (offer != null && !offers!.isClosed) offers.add(offer);
      });

      final requestBody = jsonEncode({
        'type': 'DOWNLOAD_REQUEST',
        'protocol': kProtocolVersion,
        'request_id': requestId,
        'file_id': ref.fileId,
      });
      final signalTags = [
        [
          'expiration',
          '${DateTime.now().millisecondsSinceEpoch ~/ 1000 + 20 * 60}'
        ],
        ['client', 'bopwire'],
      ];
      var delivered = 0;
      for (final seeder in targets) {
        try {
          final wrap = giftWrap(
            sender: keys,
            receiverPubkeyHex: seeder,
            content: requestBody,
            extraTags: signalTags,
          );
          final relaysAccepting = await pool.publish(wrap);
          _nlog('request to seeder ${seeder.substring(0, 12)}… '
              'accepted by $relaysAccepting relay(s)');
          if (relaysAccepting > 0) delivered++;
        } catch (_) {
          // A seeder whose pubkey is not a valid curve point, or a relay
          // that rejected the wrap — try the next one.
        }
      }
      if (delivered == 0) {
        fail('napstr: could not deliver a download request to any seeder');
        return;
      }

      _nlog('awaiting seeder offer (60s)…');
      // Race the offer against the user cancelling: without this, an
      // abandoned preview held the inbox subscription (and this whole
      // function) for the full 60 seconds after the user had moved on.
      final offer = await Future.any<DownloadOffer?>([
        offers.stream.first,
        cancel.whenCancelled.then((_) => null),
      ]).timeout(const Duration(seconds: 60), onTimeout: () {
        throw TimeoutException('no seeder answered the download request');
      });
      if (offer == null || cancel.isCancelled) return;

      final destination =
          '$destDir/${sanitiseForFilesystem(record.filename)}';
      final localPath = await downloadFromOffer(
        proxy: proxy,
        offer: offer,
        expectedSize: record.size,
        destinationPath: destination,
        cancel: cancel,
        onProgress: (received, total) {
          if (out.isClosed) return;
          out.add(DownloadProgress(
            trackId: track.id,
            receivedBytes: received,
            totalBytes: total,
          ));
        },
      );

      if (!out.isClosed) {
        out.add(DownloadProgress(
          trackId: track.id,
          receivedBytes: record.size,
          totalBytes: record.size,
          localPath: localPath,
          done: true,
        ));
      }
    } on TimeoutException {
      fail('napstr: no seeder answered within 60 seconds — they may have '
          'gone offline since the search');
    } catch (e) {
      fail('napstr: $e');
    } finally {
      _liveCancels.remove(cancel);
      await inbox?.cancel();
      if (offers != null && !offers.isClosed) await offers.close();
      if (!out.isClosed) await out.close();
    }
  }

  /// Fetches and verifies the kind-30421 entry for [fileId], preferring one
  /// published by a seeder we are about to ask.
  Future<CatalogueRecord?> _fetchRecord(
    RelayPool pool,
    String fileId,
    List<String> seeders,
  ) async {
    final events = await pool.query([
      {
        'kinds': [kCatalogueKind],
        '#t': [kCatalogueHashtag],
        '#d': [fileId],
        'limit': 32,
      }
    ], timeout: const Duration(seconds: 8)).map((e) => e.event).toList();

    final candidates = <NostrEvent>[
      ...events.where((e) => seeders.contains(e.pubkey)),
      ...events.where((e) => !seeders.contains(e.pubkey)),
    ];
    for (final e in await verifyEvents(candidates)) {
      final parsed = parseCatalogueEvent(e, verifySignature: false);
      if (parsed.outcome == CatalogueParseOutcome.record) return parsed.record;
    }
    return null;
  }

  /// Publishes the kind-10050 DM relay list once per session. A seeder uses
  /// it to work out where to send its encrypted offer, so this has to be on
  /// the relays before the request goes out — but it also announces our
  /// pubkey, so it is deliberately deferred until the first download rather
  /// than done at connect time.
  Future<void> _announceInbox(RelayPool pool, NostrKeyPair keys) async {
    if (_inboxAnnounced) return;
    final event = signEvent(
      keys: keys,
      kind: 10050,
      tags: [
        for (final url in pool.urls) ['relay', url],
      ],
      content: '',
    );
    await pool.publish(event);
    _inboxAnnounced = true;
  }

  String _uuidV4() {
    final rnd = Random.secure();
    final b = List<int>.generate(16, (_) => rnd.nextInt(256));
    b[6] = (b[6] & 0x0f) | 0x40;
    b[8] = (b[8] & 0x3f) | 0x80;
    final h = toHex(b);
    return '${h.substring(0, 8)}-${h.substring(8, 12)}-'
        '${h.substring(12, 16)}-${h.substring(16, 20)}-${h.substring(20)}';
  }

  /// Awaitable teardown for the app-exit hook: sends a flushed CANCEL to
  /// every seeder we are mid-transfer with (so their single upload slot
  /// is actually freed — a vanished socket leaves it occupied and the
  /// next request to that seeder starves), then closes the relay
  /// sockets. The network can connect() again afterwards.
  Future<void> shutdown() async {
    final cancels = _liveCancels.toList();
    _liveCancels.clear();
    await Future.wait([for (final c in cancels) c.abortGracefully()]);
    await disconnect();
  }

  /// Releases the relay sockets and the status stream.
  Future<void> dispose() async {
    await shutdown();
    await _statusController.close();
  }
}

/// One file id, plus every publisher offering it.
class _Aggregated {
  _Aggregated(this.first);
  final CatalogueRecord first;
  final List<String> publishers = [];

  void add(CatalogueRecord r) {
    if (!publishers.contains(r.publisherPubkey)) {
      publishers.add(r.publisherPubkey);
    }
  }

  /// What the result row shows: the embedded title when the publisher set
  /// one, otherwise the public filename.
  String get displayTitle =>
      first.title.isNotEmpty ? first.title : first.filename;

  ExternalTrack toTrack(String networkId) {
    final r = first;
    final seeders = publishers;
    final owner = seeders.length == 1
        ? shortKey(seeders.first)
        : '${shortKey(seeders.first)} +${seeders.length - 1}';
    return ExternalTrack(
      networkId: networkId,
      id: NapstrTrackRef(r.fileId, seeders).encode(),
      title: displayTitle,
      artist: r.artist.isNotEmpty ? r.artist : null,
      album: r.album.isNotEmpty ? r.album : null,
      owner: owner,
      remotePath: r.filename,
      sizeBytes: r.size,
      extension: r.extension,
      // napstr publishes no bitrate or duration in its catalogue — the
      // event carries only filename, size, format and MIME — so leaving
      // these null is the accurate answer, not a gap to fill in.
      isFolder: false,
    );
  }
}

/// `fileId -> publisher -> the heartbeat event proving that pair`.
class _AvailabilityIndex {
  final Map<String, Map<String, NostrEvent>> _byFile = {};

  bool get isEmpty => _byFile.isEmpty;

  void add(AvailabilityHeartbeat hb, NostrEvent source) {
    for (final fileId in hb.fileIds) {
      (_byFile[fileId] ??= {})[hb.pubkey] = source;
    }
  }

  bool isSeeding(String pubkey, String fileId) =>
      _byFile[fileId]?.containsKey(pubkey) ?? false;

  NostrEvent? heartbeatFor(String pubkey, String fileId) =>
      _byFile[fileId]?[pubkey];

  int seederCount(String fileId) => _byFile[fileId]?.length ?? 0;

  /// File ids with the most distinct seeders first.
  List<String> rankedFileIds(int limit) {
    final ids = _byFile.keys.toList();
    ids.sort((a, b) => seederCount(b).compareTo(seederCount(a)));
    return ids.length > limit ? ids.sublist(0, limit) : ids;
  }
}
