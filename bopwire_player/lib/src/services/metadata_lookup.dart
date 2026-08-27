// Online metadata (ID3 tag) enrichment + lyrics fetch for LibraryScanner.
//
// Three tag-lookup routes, best-first — first usable hit wins:
//   1. AcoustID — maps the chromaprint fingerprint the scanner already
//      computes (Fingerprinter.ofFile → fp.compressed) to a MusicBrainz
//      recording. Exact, no string guessing. Needs a free application API
//      key which is read from Settings; the route is SKIPPED when the key
//      is absent (never hardcoded).
//   2. MusicBrainz text search — artist + title (+ our duration as a
//      tiebreak). Keyless, canonical album/year/track data. Their policy
//      requires a descriptive User-Agent and ~1 request/second; both are
//      enforced below.
//   3. Genius search — fuzzy rescue for messy inputs (filename stems like
//      "01_nofx_-_linoleum_(1994)") when the structured routes miss.
//      Needs a free access token from Settings; skipped when absent.
//      Lyrics-oriented, so we take ONLY artist + title from it (its
//      album/track/year data is not trusted over MusicBrainz), and its
//      fuzzy ranked hits are LOW confidence by default: good enough to
//      fill blanks, allowed to correct populated tags only on a
//      near-exact normalized artist+title match. We use the official API
//      for search metadata only — it does not return lyrics text, and we
//      do NOT scrape the site.
//
// Merge policy (TagMerge.merge — pure and unit-tested, see
// test/metadata_lookup_test.dart):
//   * blank fields (empty string / 0) are ALWAYS filled from an accepted
//     match;
//   * populated fields are corrected ONLY on a high-confidence match
//     (AcoustID score >= 0.9, an unambiguous MusicBrainz top hit with a
//     close duration, or a near-exact Genius hit);
//   * a filename-derived title (scanner fell back to the stem) is WEAK and
//     replaceable like a blank;
//   * originals are never discarded: every corrected value is logged by
//     the caller AND persisted under 'replaced' in the on-disk cache so a
//     bad correction is diagnosable and reversible.
//
// Lyrics come from LRCLIB (lrclib.net) — free, keyless, returns both
// plain and time-synced (LRC) forms. Fetched AFTER tag correction so the
// corrected artist/title are the lookup key. Stored LOCALLY ONLY, one
// JSON file per song under <app-support>/lyrics/<songId>.json — never
// sent to the chain and never added to SongMeta (kilobytes per track
// would bloat metadata for every listener, and lyrics carry a licensing
// question titles and fingerprints don't). They are NOT inlined into the
// LibraryService prefs blob either: that JSON string is rewritten on
// every upsert and lives in a file this feature doesn't own. A lyrics
// failure never affects tag correction or chain registration.
//
// Never blocks or fails a scan: every public entry point catches its own
// errors, logs, and returns null. Results — including misses — are cached
// on disk keyed by fingerprint hash (falling back to content hash), so
// rescanning 5,000 files does not re-query. All network calls run through
// one serial queue with a per-service minimum gap, so a big scan cannot
// hammer any of the services.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:http/http.dart' as http;
import 'package:path_provider/path_provider.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'album_art_cache.dart';
import 'node_client.dart';
import 'node_service.dart';

/// SharedPreferences keys shared with the Settings screen. Both features
/// are strictly opt-in: no pref → disabled.
class MetadataLookupPrefs {
  static const enabled       = 'metadata_lookup_enabled'; // bool, default false
  static const lyricsEnabled = 'lyrics_lookup_enabled';   // bool, default false
  static const acoustIdKey   = 'acoustid_api_key';        // string, default ''
  static const geniusToken   = 'genius_access_token';     // string, default ''
}

void _mlog(String m) {
  // ignore: avoid_print
  print('[metadata] $m');
}

/// What the file currently says (post tag-read, post filename fallback).
class CurrentTags {
  const CurrentTags({
    required this.title,
    required this.artist,
    required this.album,
    required this.year,
    required this.trackNumber,
    this.titleIsFromFilename = false,
  });

  final String title;
  final String artist;
  final String album;
  final int    year;        // 0 = unknown
  final int    trackNumber; // 0 = unknown

  /// True when the container had no title tag and the scanner used the
  /// filename stem. Filename guesses are weak — fair game to replace even
  /// on a non-confident match.
  final bool titleIsFromFilename;
}

/// One accepted lookup result, normalized across the three services.
class TagCandidate {
  const TagCandidate({
    this.title = '',
    this.artist = '',
    this.album = '',
    this.year = 0,
    this.trackNumber = 0,
    this.durationMs = 0,
    this.releaseMbid = '',
    this.releaseGroupMbid = '',
    required this.source,     // 'acoustid' | 'musicbrainz' | 'genius'
    required this.score,      // acoustid 0..1, musicbrainz 0..100, genius 0..1
    required this.confident,  // may CORRECT populated fields, not just fill
  });

  final String title;
  final String artist;
  final String album;
  final int    year;
  final int    trackNumber;
  final int    durationMs;

  /// MusicBrainz release MBID of the chosen release (bestRel['id']), ''
  /// when the source doesn't carry one. Cover Art Archive is keyed on
  /// exactly this: coverartarchive.org/release/<mbid>/front-500.
  final String releaseMbid;

  /// MBID of that release's release-group — the CAA fallback key when the
  /// specific release has no front image.
  final String releaseGroupMbid;

  final String source;
  final double score;
  final bool   confident;

  Map<String, dynamic> toJson() => {
        'title': title, 'artist': artist, 'album': album,
        'year': year, 'track_number': trackNumber,
        'duration_ms': durationMs,
        if (releaseMbid.isNotEmpty)      'release_mbid': releaseMbid,
        if (releaseGroupMbid.isNotEmpty) 'release_group_mbid': releaseGroupMbid,
        'source': source, 'score': score, 'confident': confident,
      };

  static TagCandidate fromJson(Map<dynamic, dynamic> j) => TagCandidate(
        title:       (j['title']  as String?) ?? '',
        artist:      (j['artist'] as String?) ?? '',
        album:       (j['album']  as String?) ?? '',
        year:        (j['year']         as num?)?.toInt() ?? 0,
        trackNumber: (j['track_number'] as num?)?.toInt() ?? 0,
        durationMs:  (j['duration_ms']  as num?)?.toInt() ?? 0,
        releaseMbid:      (j['release_mbid']       as String?) ?? '',
        releaseGroupMbid: (j['release_group_mbid'] as String?) ?? '',
        source:      (j['source'] as String?) ?? '',
        score:       (j['score']  as num?)?.toDouble() ?? 0,
        confident:   j['confident'] == true,
      );
}

/// The merged tags plus a full record of what changed. `corrected` maps a
/// field name to the ORIGINAL value it replaced — never thrown away.
class MergeOutcome {
  const MergeOutcome({
    required this.title,
    required this.artist,
    required this.album,
    required this.year,
    required this.trackNumber,
    required this.source,
    required this.filled,
    required this.corrected,
    this.releaseMbid = '',
    this.releaseGroupMbid = '',
  });

  final String title;
  final String artist;
  final String album;
  final int    year;
  final int    trackNumber;
  final String source;

  /// MBIDs carried through verbatim from the accepted candidate ('' when
  /// the source had none). Not merge data — they exist so a caller can
  /// fetch the Cover Art Archive front image for the matched release.
  final String releaseMbid;
  final String releaseGroupMbid;

  /// Fields that were blank (or filename-derived) and got a value.
  final List<String> filled;

  /// field name -> original value that a confident match replaced.
  final Map<String, Object> corrected;

  bool get changed => filled.isNotEmpty || corrected.isNotEmpty;

  String describe() => [
        if (filled.isNotEmpty) 'filled ${filled.join('/')}',
        if (corrected.isNotEmpty)
          'corrected ${corrected.entries.map((e) => '${e.key} (was "${e.value}")').join(', ')}',
      ].join('; ');
}

/// Lyrics for one song, as stored locally.
class LyricsResult {
  const LyricsResult({
    this.plain = '',
    this.synced = '',
    this.instrumental = false,
    this.source = 'lrclib',
  });

  final String plain;        // plain text lyrics
  final String synced;       // LRC time-synced lyrics ('' when unavailable)
  final bool   instrumental; // LRCLIB says the track has no lyrics
  final String source;

  bool get hasAny => plain.isNotEmpty || synced.isNotEmpty || instrumental;

  Map<String, dynamic> toJson() => {
        'plain': plain, 'synced': synced,
        'instrumental': instrumental, 'source': source,
      };

  static LyricsResult fromJson(Map<dynamic, dynamic> j) => LyricsResult(
        plain:        (j['plain']  as String?) ?? '',
        synced:       (j['synced'] as String?) ?? '',
        instrumental: j['instrumental'] == true,
        source:       (j['source'] as String?) ?? 'lrclib',
      );
}

/// Pure merge policy — the part that can do damage, so it is deliberately
/// side-effect free and unit-tested in isolation.
class TagMerge {
  static bool _same(String a, String b) =>
      a.trim().toLowerCase() == b.trim().toLowerCase();

  static MergeOutcome merge(CurrentTags cur, TagCandidate cand) {
    final filled    = <String>[];
    final corrected = <String, Object>{};

    String mergeStr(String field, String curVal, String candVal,
                    {bool weak = false}) {
      final c = candVal.trim();
      if (c.isEmpty) return curVal;           // never blank an existing value
      final have = curVal.trim();
      if (have.isEmpty) {
        filled.add(field);
        return c;
      }
      if (_same(have, c)) return curVal;
      if (weak || cand.confident) {
        corrected[field] = curVal;            // keep the original on record
        return c;
      }
      return curVal;                          // populated + weak match: keep
    }

    int mergeInt(String field, int curVal, int candVal) {
      if (candVal <= 0) return curVal;
      if (curVal <= 0) {
        filled.add(field);
        return candVal;
      }
      if (cand.confident && candVal != curVal) {
        corrected[field] = curVal;
        return candVal;
      }
      return curVal;
    }

    final title  = mergeStr('title',  cur.title,  cand.title,
                            weak: cur.titleIsFromFilename);
    final artist = mergeStr('artist', cur.artist, cand.artist);
    final album  = mergeStr('album',  cur.album,  cand.album);
    final year   = mergeInt('year',   cur.year,   cand.year);
    final track  = mergeInt('track',  cur.trackNumber, cand.trackNumber);

    return MergeOutcome(
      title: title, artist: artist, album: album,
      year: year, trackNumber: track,
      source: cand.source, filled: filled, corrected: corrected,
      releaseMbid: cand.releaseMbid,
      releaseGroupMbid: cand.releaseGroupMbid,
    );
  }
}

class MetadataLookup {
  MetadataLookup._();
  static final MetadataLookup instance = MetadataLookup._();

  /// MusicBrainz requires an identifying User-Agent; the other services
  /// get the same courtesy.
  static const _userAgent = 'bopwire/0.7 (https://bopwire.com)';

  /// Tests inject a mock HTTP client / cache file / lyrics dir here.
  static http.Client?  debugClient;
  static File?         debugCacheFile;
  static Directory?    debugLyricsDir;

  http.Client? _ownClient;
  http.Client get _http => debugClient ?? (_ownClient ??= http.Client());

  // ---- serialization + per-service throttle -----------------------------
  // One global queue: the scanner processes files 2-at-a-time, and 5,000
  // concurrent lookups must not fan out. Each service additionally keeps a
  // minimum gap between requests (MusicBrainz policy: ~1/s).
  Future<void> _queue = Future.value();
  final Map<String, DateTime> _lastRequest = {};
  static const _minGap = {
    'acoustid':    Duration(milliseconds: 400),
    'musicbrainz': Duration(milliseconds: 1100),
    'genius':      Duration(milliseconds: 600),
    'lrclib':      Duration(milliseconds: 600),
    'coverart':    Duration(milliseconds: 600),
  };

  Future<T> _serialized<T>(String service, Future<T> Function() job) {
    final run = _queue.then((_) async {
      final last = _lastRequest[service];
      if (last != null) {
        final wait = last.add(_minGap[service]!).difference(DateTime.now());
        if (wait > Duration.zero) await Future.delayed(wait);
      }
      _lastRequest[service] = DateTime.now();
      return job();
    });
    _queue = run.then((_) {}, onError: (_) {});
    return run;
  }

  // ---- on-disk lookup cache ---------------------------------------------
  // { cacheKey: {ts, found, cand?, replaced?} }. Misses are cached too so a
  // rescan doesn't re-query files that will never match.
  Map<String, dynamic>? _cache;
  File? _cacheFile;
  Future<void> _saveQueue = Future.value();

  Future<void> _ensureCacheLoaded() async {
    if (_cache != null) return;
    _cache = {};
    try {
      _cacheFile = debugCacheFile ??
          File('${(await getApplicationSupportDirectory()).path}'
               '/metadata_lookup_cache.json');
      if (await _cacheFile!.exists()) {
        final parsed = jsonDecode(await _cacheFile!.readAsString());
        if (parsed is Map) _cache = parsed.cast<String, dynamic>();
      }
    } catch (e) {
      // Unreadable cache = empty cache. Lookup still works, just re-queries.
      _mlog('cache load failed (starting empty): $e');
    }
  }

  Future<void> _saveCache() {
    // Serialized so two concurrent enrich() calls can't interleave writes.
    _saveQueue = _saveQueue.then((_) async {
      final f = _cacheFile;
      if (f == null || _cache == null) return;
      try {
        await f.writeAsString(jsonEncode(_cache));
      } catch (e) {
        _mlog('cache save failed (ignored): $e');
      }
    });
    return _saveQueue;
  }

  /// Test hook: forget in-memory cache/throttle state and any injected
  /// client, so each test starts cold.
  static void debugReset() {
    instance._cache = null;
    instance._cacheFile = null;
    instance._lastRequest.clear();
    instance._queue = Future.value();
    instance._saveQueue = Future.value();
    instance._ownClient = null;
  }

  // ---- public entry points ----------------------------------------------

  /// Look up better tags for one file and merge them with [current].
  /// Returns null when disabled, when nothing usable was found, or on ANY
  /// error — this must never block or fail a scan. [cacheKey] should be the
  /// fingerprint hash when available, else the content hash.
  Future<MergeOutcome?> enrich({
    required CurrentTags current,
    required String fingerprint, // compressed chromaprint ('' when unknown)
    required int durationMs,     // 0 when unknown
    required String cacheKey,
  }) async {
    try {
      final prefs = await SharedPreferences.getInstance();
      if (!(prefs.getBool(MetadataLookupPrefs.enabled) ?? false)) return null;
      if (cacheKey.isEmpty) return null;

      await _ensureCacheLoaded();
      TagCandidate? cand;
      final hit = _cache![cacheKey];
      if (hit is Map) {
        if (hit['found'] == true && hit['cand'] is Map) {
          cand = TagCandidate.fromJson(hit['cand'] as Map);
        } else {
          return null; // cached miss — don't re-query on every rescan
        }
      } else {
        cand = await _lookupOnline(
          fingerprint: fingerprint,
          durationMs: durationMs,
          title: current.title,
          artist: current.artist,
          acoustIdKey:
              (prefs.getString(MetadataLookupPrefs.acoustIdKey) ?? '').trim(),
          geniusToken:
              (prefs.getString(MetadataLookupPrefs.geniusToken) ?? '').trim(),
        );
        _cache![cacheKey] = {
          'ts': DateTime.now().millisecondsSinceEpoch,
          'found': cand != null,
          if (cand != null) 'cand': cand.toJson(),
        };
        await _saveCache();
      }
      if (cand == null) return null;

      final out = TagMerge.merge(current, cand);
      if (out.corrected.isNotEmpty) {
        // Reversibility: persist the pre-correction values alongside the
        // cache entry (the scanner also logs them via _ilog).
        final entry =
            (_cache![cacheKey] as Map?)?.cast<String, dynamic>() ?? {};
        entry['replaced'] = {
          ...?(entry['replaced'] as Map?),
          for (final e in out.corrected.entries) e.key: '${e.value}',
        };
        _cache![cacheKey] = entry;
        await _saveCache();
      }
      return out;
    } catch (e) {
      _mlog('enrich failed (ignored, scan continues): $e');
      return null;
    }
  }

  // ---- preview enrichment + Cover Art Archive ---------------------------

  /// Cache key for a preview lookup: a preview has no file on disk yet,
  /// so no fingerprint/content hash — key on the normalized artist+title
  /// the search result gave us. '' when the title is unusable.
  static String previewCacheKey(
      {required String title, required String artist}) {
    final t = normalizeFuzzy(title);
    if (t.isEmpty) return '';
    return 'preview:${normalizeFuzzy(artist)}|$t';
  }

  /// Look up corrected tags for a foreign-network PREVIEW (Soulseek /
  /// napstr). Same pipeline, opt-in gate, on-disk cache (hits AND
  /// misses), serial queue and merge policy as [enrich]; the preview's
  /// search-result title is treated as filename-derived (they nearly
  /// always are), so a non-confident match may still replace it for
  /// display. Returns null when disabled, on a miss, or on any error.
  /// Display-only: callers must not write library rows or chain state
  /// from this.
  Future<MergeOutcome?> enrichPreview({
    required String title,
    required String artist,
    String album = '',
    int durationMs = 0,
  }) {
    final key = previewCacheKey(title: title, artist: artist);
    if (key.isEmpty) return Future.value(null);
    return enrich(
      current: CurrentTags(
        title: title, artist: artist, album: album,
        year: 0, trackNumber: 0,
        titleIsFromFilename: true,
      ),
      fingerprint: '',
      durationMs: durationMs,
      cacheKey: key,
    );
  }

  /// Fetch the real front cover for a matched release from Cover Art
  /// Archive (keyless, CDN-backed, keyed on the release MBID; the
  /// release-group is the fallback when that release has no front image).
  /// Stored into [AlbumArtCache] under the CORRECTED artist/album so the
  /// display layer and a later real download reuse it, and offered to the
  /// node (gap-fill only) exactly when it was newly fetched — the same
  /// once-per-album rule the import path follows.
  ///
  /// Honours the same opt-in as tag lookups. A CAA 404 is a NORMAL answer
  /// (plenty of releases have no art) — recorded as a miss in the lookup
  /// cache so scrubbing back and forth re-queries nothing. Returns the
  /// cached art file (which may have been there all along), or null.
  /// Never throws.
  Future<File?> fetchCoverArtIfEnabled({
    required String releaseMbid,
    required String releaseGroupMbid,
    required String artist,
    required String album,
  }) async {
    try {
      final prefs = await SharedPreferences.getInstance();
      if (!(prefs.getBool(MetadataLookupPrefs.enabled) ?? false)) return null;
      if (artist.trim().isEmpty) return null; // no usable cache key
      // Already have art for this album (embedded from a local file, or a
      // previous fetch) → zero network.
      final existing = await AlbumArtCache.cachedArt(artist, album);
      if (existing != null) return existing;
      if (releaseMbid.isEmpty && releaseGroupMbid.isEmpty) return null;

      await _ensureCacheLoaded();
      final missKey =
          'art:${releaseMbid.isNotEmpty ? releaseMbid : releaseGroupMbid}';
      final prior = _cache![missKey];
      if (prior is Map && prior['found'] == false) return null; // cached miss

      final bytes = await _coverArtLookup(releaseMbid, releaseGroupMbid);
      if (bytes == null) {
        _cache![missKey] = {
          'ts': DateTime.now().millisecondsSinceEpoch,
          'found': false,
        };
        await _saveCache();
        return null;
      }
      final stored = await AlbumArtCache.storeBytes(bytes, artist, album);
      if (stored == null) return null; // corrupt bytes / disk error
      _cache![missKey] = {
        'ts': DateTime.now().millisecondsSinceEpoch,
        'found': true,
      };
      await _saveCache();
      // NEWLY fetched (the cachedArt check above was a miss) → offer to
      // the node once. Fire-and-forget, same contract as lyrics.
      unawaited(_contributeArt(artist, album, bytes));
      return stored;
    } catch (e) {
      _mlog('cover art fetch failed (ignored): $e');
      return null;
    }
  }

  /// GET the CAA front image, release first then release-group. Follows
  /// redirects (the endpoint 307s to the archive.org item — package:http
  /// follows GET redirects by default). 404 = "this release has no front
  /// image" — an expected answer, not an error. Only JPEG/PNG (by magic
  /// bytes, never the Content-Type header) are accepted: the node's
  /// art.put rejects anything else, and so do we.
  Future<Uint8List?> _coverArtLookup(
      String releaseMbid, String releaseGroupMbid) {
    return _serialized('coverart', () async {
      for (final path in [
        if (releaseMbid.isNotEmpty) 'release/$releaseMbid',
        if (releaseGroupMbid.isNotEmpty) 'release-group/$releaseGroupMbid',
      ]) {
        try {
          final resp = await _http.get(
            Uri.parse('https://coverartarchive.org/$path/front-500'),
            headers: {'User-Agent': _userAgent},
          ).timeout(const Duration(seconds: 15));
          if (resp.statusCode == 404) {
            _mlog('coverart: no front image for $path (normal miss)');
            continue;
          }
          if (resp.statusCode != 200) {
            _mlog('coverart HTTP ${resp.statusCode} for $path — skipping');
            continue;
          }
          final b = resp.bodyBytes;
          final ext = AlbumArtCache.sniffExtension(b);
          if (ext != 'jpg' && ext != 'png') {
            _mlog('coverart: $path returned non-JPEG/PNG bytes '
                '(${b.length} B) — rejected');
            continue;
          }
          _mlog('coverart: fetched ${b.length} B $ext for $path');
          return b;
        } catch (e) {
          _mlog('coverart fetch failed for $path (ignored): $e');
        }
      }
      return null;
    });
  }

  /// Offer freshly-fetched cover art to the node (gap-fill only, same
  /// contract as [_contributeLyrics]): false usually means the node
  /// already had art — a normal outcome, not a failure.
  Future<void> _contributeArt(
      String artist, String album, Uint8List bytes) async {
    try {
      final pid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 5));
      if (pid.isEmpty) return;
      final ok = await NodeClient(ratsPeerId: pid)
          .contributeAlbumArt(artist, album, bytes);
      if (ok) _mlog('cover art for "$artist / $album" contributed to node');
    } catch (e) {
      _mlog('art contribute failed (ignored): $e');
    }
  }

  /// Fetch lyrics from LRCLIB for one song and store them locally (one
  /// JSON file per song under <app-support>/lyrics/). Call AFTER tag
  /// enrichment so the corrected artist/title are the lookup key.
  /// Opt-in via its own Settings toggle; never throws; a miss is recorded
  /// so a rescan doesn't re-query. [songKey] should be the library's
  /// songId (canonical chain hash when known, else content hash).
  Future<void> fetchLyricsIfEnabled({
    required String songKey,
    required String title,
    required String artist,
    required String album,
    required int durationMs,
  }) async {
    try {
      final prefs = await SharedPreferences.getInstance();
      if (!(prefs.getBool(MetadataLookupPrefs.lyricsEnabled) ?? false)) return;
      if (songKey.isEmpty) return;
      // Artist + title are the minimum for a trustworthy lyrics match;
      // a title-only hit would happily return the wrong song's lyrics.
      if (title.trim().isEmpty || artist.trim().isEmpty) return;

      final dir = await _lyricsDir();
      if (dir == null) return;
      final f = File('${dir.path}/$songKey.json');
      if (await f.exists()) return; // cached (hit or recorded miss)

      final res = await _lrclibLookup(
          title: title, artist: artist, album: album, durationMs: durationMs);
      await f.writeAsString(jsonEncode({
        'found': res != null && res.hasAny,
        'ts': DateTime.now().millisecondsSinceEpoch,
        'artist': artist,
        'title': title,
        if (res != null) 'lyrics': res.toJson(),
      }));
      _mlog(res == null || !res.hasAny
          ? 'lyrics: no match for "$artist - $title" (miss recorded)'
          : 'lyrics: stored for "$artist - $title" '
            '(synced=${res.synced.isNotEmpty}, '
            'plain=${res.plain.isNotEmpty})');

      // Hand them to the node so every client gets them without each one
      // querying LRCLIB for the same song — and so the WEBSITE can show
      // them at all, since it can only read from the node.
      //
      // Deliberately not awaited into the caller's critical path and never
      // allowed to throw: contributing is decoration, and an import must
      // finish identically whether the node is present, absent or slow.
      if (res != null && res.hasAny) unawaited(_contributeLyrics(artist, title, res));
    } catch (e) {
      _mlog('lyrics fetch failed (ignored): $e');
    }
  }

  /// Offer freshly-fetched lyrics to the node. The node is gap-fill only, so
  /// a `false` result usually just means it already had them — a normal
  /// outcome that is not logged as a failure.
  Future<void> _contributeLyrics(
      String artist, String title, LyricsResult res) async {
    try {
      final pid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 5));
      if (pid.isEmpty) return;
      final ok = await NodeClient(ratsPeerId: pid).contributeLyrics(
        artist: artist,
        title: title,
        plain: res.plain,
        synced: res.synced,
        instrumental: res.instrumental,
      );
      if (ok) _mlog('lyrics for "$artist - $title" contributed to node');
    } catch (e) {
      _mlog('lyrics contribute failed (ignored): $e');
    }
  }

  /// Read locally stored lyrics for a song, or null when absent / a
  /// recorded miss. For whatever UI eventually displays them.
  Future<LyricsResult?> storedLyrics(String songKey) async {
    try {
      final dir = await _lyricsDir();
      if (dir == null || songKey.isEmpty) return null;
      final f = File('${dir.path}/$songKey.json');
      if (!await f.exists()) return null;
      final j = jsonDecode(await f.readAsString());
      if (j is! Map || j['found'] != true || j['lyrics'] is! Map) return null;
      return LyricsResult.fromJson(j['lyrics'] as Map);
    } catch (_) {
      return null;
    }
  }

  Future<Directory?> _lyricsDir() async {
    try {
      final dir = debugLyricsDir ??
          Directory('${(await getApplicationSupportDirectory()).path}/lyrics');
      if (!await dir.exists()) await dir.create(recursive: true);
      return dir;
    } catch (e) {
      _mlog('lyrics dir unavailable: $e');
      return null;
    }
  }

  Future<TagCandidate?> _lookupOnline({
    required String fingerprint,
    required int durationMs,
    required String title,
    required String artist,
    required String acoustIdKey,
    required String geniusToken,
  }) async {
    // Route 1: AcoustID — exact fingerprint match. Requires a key (from
    // Settings), the chromaprint, and a duration (the API needs it).
    if (acoustIdKey.isNotEmpty && fingerprint.isNotEmpty && durationMs > 0) {
      final c = await _acoustIdLookup(acoustIdKey, fingerprint, durationMs);
      if (c != null) return c;
    }
    if (title.trim().isEmpty) return null;
    // Route 2: MusicBrainz text search. A title-only query (no artist tag)
    // is only trusted when our duration can corroborate the hit.
    if (artist.trim().isNotEmpty || durationMs > 0) {
      final c = await _musicBrainzLookup(
          title: title, artist: artist, durationMs: durationMs);
      if (c != null) return c;
    }
    // Route 3: Genius fuzzy rescue — messy filename stems often match here
    // when the structured MusicBrainz query returns nothing. Token from
    // Settings; skipped when absent.
    if (geniusToken.isNotEmpty) {
      return _geniusLookup(token: geniusToken, title: title, artist: artist);
    }
    return null;
  }

  // ---- AcoustID ----------------------------------------------------------

  Future<TagCandidate?> _acoustIdLookup(
      String key, String fingerprint, int durationMs) {
    return _serialized('acoustid', () async {
      try {
        final resp = await _http.post(
          Uri.parse('https://api.acoustid.org/v2/lookup'),
          headers: {'User-Agent': _userAgent},
          body: {
            'client': key,
            'duration': (durationMs / 1000).round().toString(),
            'fingerprint': fingerprint,
            'meta': 'recordings releasegroups releases tracks',
          },
        ).timeout(const Duration(seconds: 10));
        if (resp.statusCode != 200) {
          _mlog('acoustid HTTP ${resp.statusCode} — skipping');
          return null;
        }
        final cand = parseAcoustId(jsonDecode(resp.body));
        _mlog(cand == null
            ? 'acoustid: no usable match'
            : 'acoustid: "${cand.artist} - ${cand.title}" '
              'score=${cand.score.toStringAsFixed(2)} '
              'confident=${cand.confident}');
        return cand;
      } catch (e) {
        _mlog('acoustid lookup failed (ignored): $e');
        return null;
      }
    });
  }

  /// Parse an AcoustID /v2/lookup response. Static + pure for tests.
  /// Accepts any decoded JSON; garbage → null.
  static TagCandidate? parseAcoustId(Object? body) {
    if (body is! Map || body['status'] != 'ok') return null;
    final results = body['results'];
    if (results is! List) return null;

    Map? best;
    var bestScore = 0.0;
    for (final r in results.whereType<Map>()) {
      final s = (r['score'] is num) ? (r['score'] as num).toDouble() : 0.0;
      final recs = r['recordings'];
      if (recs is! List || recs.isEmpty) continue;
      if (best == null || s > bestScore) {
        best = r;
        bestScore = s;
      }
    }
    // Below 0.5 the fingerprint match itself is dubious — not even good
    // enough to fill blanks with.
    if (best == null || bestScore < 0.5) return null;

    Map? rec;
    for (final r in (best['recordings'] as List).whereType<Map>()) {
      final t = r['title'];
      if (t is String && t.trim().isNotEmpty) {
        rec = r;
        break;
      }
    }
    if (rec == null) return null;

    final title = (rec['title'] as String).trim();
    final artists = rec['artists'];
    final artist = (artists is List)
        ? artists
            .whereType<Map>()
            .map((a) => _str(a['name']))
            .where((n) => n.isNotEmpty)
            .join(', ')
        : '';
    if (artist.isEmpty) return null;

    // Album / year / track: best-effort walk of releasegroups → releases →
    // mediums → tracks. Prefer an Album-type release group.
    String album = '';
    var year = 0;
    var track = 0;
    final rgs = rec['releasegroups'];
    if (rgs is List) {
      Map? rg;
      for (final g in rgs.whereType<Map>()) {
        if (g['type'] == 'Album') {
          rg = g;
          break;
        }
        rg ??= g;
      }
      if (rg != null) {
        album = _str(rg['title']);
        final rels = rg['releases'];
        if (rels is List && rels.isNotEmpty && rels.first is Map) {
          final rel = rels.first as Map;
          final date = rel['date'];
          if (date is Map) year = (date['year'] as num?)?.toInt() ?? 0;
          final meds = rel['mediums'];
          if (meds is List && meds.isNotEmpty && meds.first is Map) {
            final tracks = (meds.first as Map)['tracks'];
            if (tracks is List && tracks.isNotEmpty && tracks.first is Map) {
              track =
                  ((tracks.first as Map)['position'] as num?)?.toInt() ?? 0;
            }
          }
        }
      }
    }

    return TagCandidate(
      title: title, artist: artist, album: album,
      year: year, trackNumber: track,
      durationMs:
          (((rec['duration'] as num?)?.toDouble() ?? 0.0) * 1000).round(),
      source: 'acoustid',
      score: bestScore,
      confident: bestScore >= 0.9,
    );
  }

  // ---- MusicBrainz -------------------------------------------------------

  Future<TagCandidate?> _musicBrainzLookup({
    required String title,
    required String artist,
    required int durationMs,
  }) {
    return _serialized('musicbrainz', () async {
      try {
        String esc(String s) =>
            s.replaceAll(r'\', r'\\').replaceAll('"', r'\"').trim();
        final q = artist.trim().isEmpty
            ? 'recording:"${esc(title)}"'
            : 'recording:"${esc(title)}" AND artist:"${esc(artist)}"';
        // NB: encode spaces as %20, NOT '+'. Uri.https query encoding
        // uses '+', which MusicBrainz's Lucene endpoint rejects with
        // HTTP 400/503 — found by a live smoke test against the real API.
        final uri = Uri.parse('https://musicbrainz.org/ws/2/recording'
            '?query=${Uri.encodeComponent(q)}&fmt=json&limit=5');
        final resp = await _http.get(uri, headers: {
          'User-Agent': _userAgent,
          'Accept': 'application/json',
        }).timeout(const Duration(seconds: 10));
        if (resp.statusCode != 200) {
          _mlog('musicbrainz HTTP ${resp.statusCode} — skipping');
          return null;
        }
        final cand = parseMusicBrainz(jsonDecode(resp.body),
            localDurationMs: durationMs,
            artistWasKnown: artist.trim().isNotEmpty);
        _mlog(cand == null
            ? 'musicbrainz: no usable match for "$artist - $title"'
            : 'musicbrainz: "${cand.artist} - ${cand.title}" '
              '(album "${cand.album}", ${cand.year}) '
              'score=${cand.score.round()} confident=${cand.confident}');
        return cand;
      } catch (e) {
        _mlog('musicbrainz lookup failed (ignored): $e');
        return null;
      }
    });
  }

  static double _num(Object? v) => (v is num) ? v.toDouble() : 0.0;

  /// Safe string extraction from untrusted JSON: anything non-String
  /// (garbage responses put ints and maps where strings belong) is ''.
  static String _str(Object? v) => (v is String) ? v.trim() : '';

  static String _mbArtist(Map rec) {
    final credit = rec['artist-credit'];
    if (credit is! List) return '';
    final b = StringBuffer();
    for (final c in credit.whereType<Map>()) {
      b.write(_str(c['name']));
      final join = c['joinphrase'];
      if (join is String) b.write(join);
    }
    return b.toString().trim();
  }

  /// Parse a MusicBrainz /ws/2/recording search response. Static + pure
  /// for tests. Garbage → null.
  ///
  /// Confidence contract: `confident` (allowed to CORRECT populated tags)
  /// requires top score >= 95, a duration within 3.5 s of ours, and an
  /// unambiguous top hit (single result, clearly lower runner-up, or a
  /// runner-up that is the same song). Anything accepted below that bar
  /// only fills blanks.
  static TagCandidate? parseMusicBrainz(Object? body,
      {required int localDurationMs, required bool artistWasKnown}) {
    if (body is! Map) return null;
    final recs = body['recordings'];
    if (recs is! List || recs.isEmpty) return null;

    final list = recs.whereType<Map>().toList()
      ..sort((a, b) => _num(b['score']).compareTo(_num(a['score'])));
    final top = list.first;
    final topScore = _num(top['score']);
    final title = _str(top['title']);
    final artist = _mbArtist(top);
    if (title.isEmpty || artist.isEmpty) return null;
    if (topScore < 80) return null; // too fuzzy even for blank-filling

    final lenMs = _num(top['length']).toInt();
    final durClose = localDurationMs > 0 &&
        lenMs > 0 &&
        (lenMs - localDurationMs).abs() <= 3500;
    // Title-only searches are wildly ambiguous; without a corroborating
    // duration, reject outright.
    if (!artistWasKnown && !durClose) return null;

    bool unambiguous = list.length == 1;
    if (!unambiguous) {
      final second = list[1];
      final sameSong =
          _str(second['title']).toLowerCase() == title.toLowerCase() &&
          _mbArtist(second).toLowerCase() == artist.toLowerCase();
      unambiguous = _num(second['score']) <= topScore - 10 || sameSong;
    }
    final confident = topScore >= 95 && durClose && unambiguous;

    // Release info: prefer an Official release from a PLAIN Album
    // release-group. Secondary types (Compilation/Live/Soundtrack…)
    // are demoted so a track that appears on both its original album and
    // a Punk-O-Rama-style compilation resolves to the original —
    // verified against the real MB payload for NOFX "Olympia WA".
    String album = '';
    var year = 0;
    var track = 0;
    var releaseMbid = '';
    var releaseGroupMbid = '';
    final rels = top['releases'];
    if (rels is List) {
      Map? bestRel;
      var bestRank = -1;
      for (final r in rels.whereType<Map>()) {
        final rg = r['release-group'];
        final type = _str(rg is Map ? rg['primary-type'] : null);
        final secondary = (rg is Map) ? rg['secondary-types'] : null;
        final plain = secondary is! List || secondary.isEmpty;
        final official = r['status'] == 'Official';
        final rank = (type == 'Album' ? 4 : 0) +
            (plain ? 2 : 0) +
            (official ? 1 : 0);
        if (rank > bestRank) {
          bestRank = rank;
          bestRel = r;
        }
      }
      if (bestRel != null) {
        album = _str(bestRel['title']);
        // The release MBID (and its release-group's) are what Cover Art
        // Archive is keyed on — capture them instead of discarding.
        releaseMbid = _str(bestRel['id']);
        final bestRg = bestRel['release-group'];
        releaseGroupMbid = _str(bestRg is Map ? bestRg['id'] : null);
        final date = _str(bestRel['date']);
        if (date.length >= 4) year = int.tryParse(date.substring(0, 4)) ?? 0;
        final media = bestRel['media'];
        if (media is List && media.isNotEmpty && media.first is Map) {
          final tr = (media.first as Map)['track'];
          if (tr is List && tr.isNotEmpty && tr.first is Map) {
            final t = tr.first as Map;
            track = int.tryParse('${t['number'] ?? ''}') ??
                (t['position'] as num?)?.toInt() ??
                0;
          }
        }
      }
    }

    return TagCandidate(
      title: title, artist: artist, album: album,
      year: year, trackNumber: track, durationMs: lenMs,
      releaseMbid: releaseMbid, releaseGroupMbid: releaseGroupMbid,
      source: 'musicbrainz', score: topScore, confident: confident,
    );
  }

  // ---- Genius (fuzzy rescue) --------------------------------------------

  Future<TagCandidate?> _geniusLookup({
    required String token,
    required String title,
    required String artist,
  }) {
    return _serialized('genius', () async {
      try {
        final q = artist.trim().isEmpty ? title : '$artist $title';
        final uri = Uri.https('api.genius.com', '/search', {'q': q});
        final resp = await _http.get(uri, headers: {
          'User-Agent': _userAgent,
          'Authorization': 'Bearer $token',
        }).timeout(const Duration(seconds: 10));
        if (resp.statusCode != 200) {
          _mlog('genius HTTP ${resp.statusCode} — skipping');
          return null;
        }
        final cand = parseGenius(jsonDecode(resp.body),
            queryTitle: title, queryArtist: artist);
        _mlog(cand == null
            ? 'genius: no usable match for "$q"'
            : 'genius: "${cand.artist} - ${cand.title}" '
              'overlap=${cand.score.toStringAsFixed(2)} '
              'confident=${cand.confident}');
        return cand;
      } catch (e) {
        _mlog('genius lookup failed (ignored): $e');
        return null;
      }
    });
  }

  /// Aggressive normalization for fuzzy comparisons: lowercase, strip
  /// everything non-alphanumeric to single spaces.
  static String normalizeFuzzy(String s) => s
      .toLowerCase()
      .replaceAll(RegExp(r'[^a-z0-9]+'), ' ')
      .trim();

  /// Parse a Genius /search response. Static + pure for tests.
  ///
  /// Genius returns ranked fuzzy hits and will happily return a
  /// plausible-but-wrong song for a bad query, so acceptance requires the
  /// hit's tokens to substantially appear in our query (>= 60%), and
  /// `confident` requires a near-exact normalized artist+title match.
  /// Only artist + title are taken — Genius album/track/year data is not
  /// trusted (that's MusicBrainz's job).
  static TagCandidate? parseGenius(Object? body,
      {required String queryTitle, required String queryArtist}) {
    if (body is! Map) return null;
    final resp = body['response'];
    if (resp is! Map) return null;
    final hits = resp['hits'];
    if (hits is! List) return null;

    final queryTokens =
        normalizeFuzzy('$queryArtist $queryTitle').split(' ').toSet()
          ..remove('');
    if (queryTokens.isEmpty) return null;

    for (final h in hits.take(3).whereType<Map>()) {
      if (h['type'] != null && h['type'] != 'song') continue;
      final r = h['result'];
      if (r is! Map) continue;
      final title = _str(r['title']);
      final pa = r['primary_artist'];
      final artist = _str(pa is Map ? pa['name'] : null);
      if (title.isEmpty || artist.isEmpty) continue;

      final hitTokens = normalizeFuzzy('$artist $title').split(' ').toSet()
        ..remove('');
      if (hitTokens.isEmpty) continue;
      final overlap =
          hitTokens.where(queryTokens.contains).length / hitTokens.length;
      if (overlap < 0.6) continue; // plausible-but-wrong song — reject

      final confident = queryArtist.trim().isNotEmpty &&
          normalizeFuzzy(artist) == normalizeFuzzy(queryArtist) &&
          normalizeFuzzy(title) == normalizeFuzzy(queryTitle);

      return TagCandidate(
        title: title,
        artist: artist,
        // album/year/track deliberately left empty: Genius data for these
        // is inconsistent and must not outrank MusicBrainz.
        source: 'genius',
        score: overlap,
        confident: confident,
      );
    }
    return null;
  }

  // ---- LRCLIB (lyrics) ---------------------------------------------------

  Future<LyricsResult?> _lrclibLookup({
    required String title,
    required String artist,
    required String album,
    required int durationMs,
  }) {
    return _serialized('lrclib', () async {
      try {
        // Exact endpoint first: artist/title (+ album + duration when we
        // have them) — returns one record or 404.
        final getUri = Uri.https('lrclib.net', '/api/get', {
          'artist_name': artist,
          'track_name': title,
          if (album.trim().isNotEmpty) 'album_name': album,
          if (durationMs > 0) 'duration': (durationMs / 1000).round().toString(),
        });
        final resp = await _http.get(getUri,
                headers: {'User-Agent': _userAgent})
            .timeout(const Duration(seconds: 10));
        if (resp.statusCode == 200) {
          final r = parseLrclibRecord(jsonDecode(resp.body));
          if (r != null) return r;
        }
        // Fallback: search endpoint, corroborate with duration when known.
        final searchUri = Uri.https('lrclib.net', '/api/search', {
          'artist_name': artist,
          'track_name': title,
        });
        final sResp = await _http.get(searchUri,
                headers: {'User-Agent': _userAgent})
            .timeout(const Duration(seconds: 10));
        if (sResp.statusCode != 200) return null;
        return parseLrclibSearch(jsonDecode(sResp.body),
            localDurationMs: durationMs);
      } catch (e) {
        _mlog('lrclib lookup failed (ignored): $e');
        return null;
      }
    });
  }

  /// Parse one LRCLIB record. Static + pure for tests; garbage → null.
  static LyricsResult? parseLrclibRecord(Object? body) {
    if (body is! Map) return null;
    final plain  = _str(body['plainLyrics']);
    final synced = _str(body['syncedLyrics']);
    final instrumental = body['instrumental'] == true;
    if (plain.isEmpty && synced.isEmpty && !instrumental) return null;
    return LyricsResult(
        plain: plain, synced: synced, instrumental: instrumental);
  }

  /// Parse an LRCLIB /api/search response: pick the first result whose
  /// duration corroborates ours (within 4 s) when we know it, else the
  /// first with any lyrics.
  static LyricsResult? parseLrclibSearch(Object? body,
      {required int localDurationMs}) {
    if (body is! List) return null;
    for (final e in body.whereType<Map>()) {
      final durS = _num(e['duration']);
      if (localDurationMs > 0 && durS > 0 &&
          (durS * 1000 - localDurationMs).abs() > 4000) {
        continue;
      }
      final r = parseLrclibRecord(e);
      if (r != null) return r;
    }
    return null;
  }
}

/// Coordinates the metadata + cover-art enrichment of ONE playing preview,
/// handling the two hazards previews add over library scans:
///
///   * Users scrub through previews quickly, so every [start] supersedes
///     the previous one: an epoch counter is re-checked after EVERY await
///     and a stale generation exits without invoking a single callback —
///     a late reply can never land on the wrong track.
///   * The debounce in front of the first network touch means rapid
///     preview-switching fires no request at all for the abandoned ones
///     (on top of MetadataLookup's serial queue + per-service rate
///     limits, which throttle whatever does get through).
///
/// Everything is off the playback path: callbacks fire only with results,
/// failures are swallowed inside MetadataLookup, and the opt-in gate
/// (MetadataLookupPrefs.enabled, default OFF) is enforced by the lookup
/// methods themselves.
class PreviewEnricher {
  PreviewEnricher({
    this.debounce = const Duration(milliseconds: 900),
    MetadataLookup? lookup,
  }) : _lookup = lookup ?? MetadataLookup.instance;

  /// Delay before the first network touch. A preview abandoned within
  /// this window costs zero requests.
  final Duration debounce;

  final MetadataLookup _lookup;
  int _epoch = 0;

  /// Abandon whatever lookup is in flight. Call whenever the preview
  /// stops or something else starts playing.
  void cancel() => _epoch++;

  /// Start enrichment for a just-started preview. [onTags] fires when a
  /// corrected candidate lands, [onArt] when a real cover does (possibly
  /// straight from the on-disk cache). Neither fires after a newer
  /// [start] or a [cancel]. Never throws.
  Future<void> start({
    required String title,
    String artist = '',
    String album = '',
    int durationMs = 0,
    required void Function(MergeOutcome outcome) onTags,
    required void Function(Uint8List bytes) onArt,
  }) async {
    final epoch = ++_epoch;
    try {
      if (debounce > Duration.zero) await Future.delayed(debounce);
      if (epoch != _epoch) return; // superseded during debounce → no request
      final out = await _lookup.enrichPreview(
          title: title, artist: artist, album: album, durationMs: durationMs);
      if (epoch != _epoch) return; // superseded mid-flight → drop the reply
      if (out != null) onTags(out);
      // Art is keyed on the CORRECTED artist/album (so a later real
      // download finds it), falling back to what the search result said.
      final f = await _lookup.fetchCoverArtIfEnabled(
        releaseMbid:      out?.releaseMbid ?? '',
        releaseGroupMbid: out?.releaseGroupMbid ?? '',
        artist: out?.artist ?? artist,
        album:  out?.album ?? album,
      );
      if (f == null || epoch != _epoch) return;
      final bytes = await f.readAsBytes();
      if (epoch != _epoch) return;
      onArt(bytes);
    } catch (e) {
      _mlog('preview enrichment failed (ignored): $e');
    }
  }
}
