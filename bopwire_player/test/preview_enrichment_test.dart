// Tests for the PREVIEW enrichment path: live web metadata + Cover Art
// Archive covers for foreign-network (Soulseek / napstr) previews.
//
// What matters most here — and is exercised hardest — is the cancel /
// superseded contract: users scrub through previews quickly, and a late
// reply for preview A must NEVER land on preview B's now-playing state.
// Everything runs against a MockClient; no test touches the real internet.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/album_art_cache.dart';
import 'package:bopwire_player/src/services/metadata_lookup.dart';

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

const relMbid = 'a44dbb1c-08f6-4a53-9c86-4b6c73dfeb14';
const rgMbid  = 'b7d3f3f3-9a53-33e3-a8f5-05b0e5ed5335';

/// Realistic MusicBrainz /ws/2/recording search payload for
/// NOFX "Olympia, WA" — trimmed but structurally faithful, INCLUDING the
/// release and release-group MBIDs that Cover Art Archive is keyed on.
Map<String, dynamic> mbResponse({
  int score = 100,
  int lengthMs = 178600,
  String title = 'Olympia, WA',
  String artist = 'NOFX',
}) =>
    {
      'created': '2026-08-26T00:00:00Z',
      'count': 1,
      'recordings': [
        {
          'id': 'c2a8f83c-b70f-4a04-9773-e1e63ac30f47',
          'score': score,
          'title': title,
          'length': lengthMs,
          'artist-credit': [
            {'name': artist, 'artist': {'name': artist}}
          ],
          'releases': [
            {
              'id': relMbid,
              'title': 'Punk in Drublic',
              'status': 'Official',
              'date': '1994-07-19',
              'release-group': {
                'id': rgMbid,
                'primary-type': 'Album',
              },
              'media': [
                {
                  'format': 'CD',
                  'track': [
                    {'number': '9', 'title': title}
                  ],
                }
              ],
            }
          ],
        }
      ],
    };

/// Minimal-but-valid JPEG bytes (magic FF D8 FF + padding past the 12-byte
/// sniff window).
final Uint8List jpegBytes = Uint8List.fromList(
    [0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 0x4A, 0x46, 0x49, 0x46, 0, 1, 1, 2, 3]);

final Uint8List pngBytes = Uint8List.fromList(
    [0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 1, 2]);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late Directory tmp;
  var requests = <Uri>[];

  setUp(() async {
    MetadataLookup.debugReset();
    tmp = await Directory.systemTemp.createTemp('preview_enrich_test');
    MetadataLookup.debugCacheFile = File('${tmp.path}/cache.json');
    MetadataLookup.debugLyricsDir = Directory('${tmp.path}/lyrics');
    AlbumArtCache.debugDir = Directory('${tmp.path}/art');
    requests = [];
    SharedPreferences.setMockInitialValues(
        {MetadataLookupPrefs.enabled: true});
  });

  tearDown(() async {
    MetadataLookup.debugClient = null;
    MetadataLookup.debugCacheFile = null;
    MetadataLookup.debugLyricsDir = null;
    AlbumArtCache.debugDir = null;
    MetadataLookup.debugReset();
    try { await tmp.delete(recursive: true); } catch (_) {}
  });

  /// MockClient: MusicBrainz answers the fixture, Cover Art Archive
  /// answers per [caa] (release path) and [caaGroup] (release-group path).
  http.Client client({
    http.Response Function()? caa,
    http.Response Function()? caaGroup,
  }) =>
      MockClient((req) async {
        requests.add(req.url);
        if (req.url.host == 'musicbrainz.org') {
          return http.Response(jsonEncode(mbResponse()), 200,
              headers: {'content-type': 'application/json'});
        }
        if (req.url.host == 'coverartarchive.org') {
          final isGroup = req.url.path.startsWith('/release-group/');
          final fn = isGroup ? caaGroup : caa;
          if (fn != null) return fn();
          return http.Response('not here', 404);
        }
        return http.Response('not found', 404);
      });

  http.Response jpegOk() =>
      http.Response.bytes(jpegBytes, 200,
          headers: {'content-type': 'image/jpeg'});

  List<Uri> caaRequests() =>
      requests.where((u) => u.host == 'coverartarchive.org').toList();

  // -------------------------------------------------------------------------
  // MBID capture
  // -------------------------------------------------------------------------

  group('MBID capture', () {
    test('parseMusicBrainz captures release + release-group MBIDs', () {
      final cand = MetadataLookup.parseMusicBrainz(mbResponse(),
          localDurationMs: 178600, artistWasKnown: true);
      expect(cand, isNotNull);
      expect(cand!.releaseMbid, relMbid);
      expect(cand.releaseGroupMbid, rgMbid);
      expect(cand.album, 'Punk in Drublic');
    });

    test('MBIDs survive the TagCandidate JSON round-trip (disk cache)', () {
      final cand = MetadataLookup.parseMusicBrainz(mbResponse(),
          localDurationMs: 178600, artistWasKnown: true)!;
      final back = TagCandidate.fromJson(
          jsonDecode(jsonEncode(cand.toJson())) as Map);
      expect(back.releaseMbid, relMbid);
      expect(back.releaseGroupMbid, rgMbid);
    });

    test('payload without release ids → empty MBIDs, not garbage', () {
      final body = mbResponse();
      final rel =
          (((body['recordings'] as List)[0] as Map)['releases'] as List)[0]
              as Map;
      rel.remove('id');
      (rel['release-group'] as Map).remove('id');
      final cand = MetadataLookup.parseMusicBrainz(body,
          localDurationMs: 178600, artistWasKnown: true);
      expect(cand, isNotNull);
      expect(cand!.releaseMbid, '');
      expect(cand.releaseGroupMbid, '');
    });

    test('enrichPreview outcome carries the MBIDs through the merge', () async {
      MetadataLookup.debugClient = client();
      final out = await MetadataLookup.instance.enrichPreview(
          title: '09_nofx_olympia_wa', artist: 'NOFX', durationMs: 178600);
      expect(out, isNotNull);
      expect(out!.releaseMbid, relMbid);
      expect(out.releaseGroupMbid, rgMbid);
      expect(out.title, 'Olympia, WA'); // preview titles are weak → replaced
      expect(out.album, 'Punk in Drublic');
    });
  });

  // -------------------------------------------------------------------------
  // Cover Art Archive fetch
  // -------------------------------------------------------------------------

  group('cover art fetch', () {
    test('fetches the release front image and stores it in AlbumArtCache',
        () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNotNull);
      expect(await f!.readAsBytes(), jpegBytes);
      // Stored under the corrected artist/album → display + later real
      // download find it through the normal cache lookup.
      final cached = await AlbumArtCache.cachedArt('NOFX', 'Punk in Drublic');
      expect(cached, isNotNull);
      expect(caaRequests().single.path, '/release/$relMbid/front-500');
    });

    test('release 404 → falls back to the release-group image', () async {
      MetadataLookup.debugClient = client(
          caa: () => http.Response('no front cover', 404),
          caaGroup: jpegOk);
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNotNull);
      final caa = caaRequests();
      expect(caa, hasLength(2));
      expect(caa[0].path, '/release/$relMbid/front-500');
      expect(caa[1].path, '/release-group/$rgMbid/front-500');
    });

    test('404 on both paths is a NORMAL miss — cached, no re-query', () async {
      MetadataLookup.debugClient = client(); // both CAA paths 404
      final a = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(a, isNull);
      expect(caaRequests(), hasLength(2));
      // Scrubbing back: the miss is cached, zero further requests.
      final b = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(b, isNull);
      expect(caaRequests(), hasLength(2));
    });

    test('non-image bytes (HTML posing as a cover) are rejected, not stored',
        () async {
      MetadataLookup.debugClient = client(
          caa: () => http.Response('<html>surprise!</html>', 200,
              headers: {'content-type': 'image/jpeg'}), // header lies
          caaGroup: () =>
              http.Response('<html>also not art</html>', 200));
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNull);
      expect(await AlbumArtCache.cachedArt('NOFX', 'Punk in Drublic'), isNull);
    });

    test('GIF/WebP from CAA are rejected too (node accepts only JPEG/PNG)',
        () async {
      final gif = Uint8List.fromList(
          [0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0, 0, 0, 0, 0, 0, 0]);
      MetadataLookup.debugClient =
          client(caa: () => http.Response.bytes(gif, 200));
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: '',
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNull);
    });

    test('PNG is accepted', () async {
      MetadataLookup.debugClient =
          client(caa: () => http.Response.bytes(pngBytes, 200));
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: '',
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNotNull);
      expect(f!.path, endsWith('.png'));
    });

    test('existing cached art short-circuits — zero network', () async {
      await AlbumArtCache.storeBytes(jpegBytes, 'NOFX', 'Punk in Drublic');
      MetadataLookup.debugClient = client(caa: jpegOk);
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: relMbid, releaseGroupMbid: rgMbid,
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNotNull);
      expect(requests, isEmpty);
    });

    test('no MBIDs and no cached art → nothing to do, zero network', () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final f = await MetadataLookup.instance.fetchCoverArtIfEnabled(
          releaseMbid: '', releaseGroupMbid: '',
          artist: 'NOFX', album: 'Punk in Drublic');
      expect(f, isNull);
      expect(requests, isEmpty);
    });
  });

  // -------------------------------------------------------------------------
  // PreviewEnricher — cancel / superseded / debounce (the dangerous part)
  // -------------------------------------------------------------------------

  group('PreviewEnricher', () {
    test('preview B started during A\'s debounce: A fires nothing, no request',
        () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final enricher = PreviewEnricher(
          debounce: const Duration(milliseconds: 60));
      final aTags = <MergeOutcome>[];
      final aArt = <Uint8List>[];
      final bTags = <MergeOutcome>[];
      final bArt = <Uint8List>[];

      final a = enricher.start(
          title: 'trackA_totally_different', artist: 'Someone Else',
          onTags: aTags.add, onArt: aArt.add);
      await Future.delayed(const Duration(milliseconds: 10));
      final b = enricher.start(
          title: '09_nofx_olympia_wa', artist: 'NOFX', durationMs: 178600,
          onTags: bTags.add, onArt: bArt.add);
      await Future.wait([a, b]);

      expect(aTags, isEmpty, reason: 'superseded lookup must stay silent');
      expect(aArt, isEmpty);
      expect(bTags, hasLength(1));
      expect(bTags.single.title, 'Olympia, WA');
      expect(bArt, hasLength(1), reason: 'the winner gets its cover');
      // Debounce: A never reached the network — ONE MusicBrainz query.
      expect(requests.where((u) => u.host == 'musicbrainz.org'),
          hasLength(1));
    });

    test('late reply: A\'s response lands AFTER B started → dropped on the '
        'floor, B\'s state untouched by A', () async {
      final aGate = Completer<void>();
      var mbCalls = 0;
      MetadataLookup.debugClient = MockClient((req) async {
        requests.add(req.url);
        if (req.url.host == 'musicbrainz.org') {
          mbCalls++;
          if (mbCalls == 1) await aGate.future; // hold A's reply in flight
          return http.Response(jsonEncode(mbResponse()), 200);
        }
        return http.Response('no art', 404);
      });
      final enricher = PreviewEnricher(
          debounce: const Duration(milliseconds: 1));
      final aTags = <MergeOutcome>[];
      final bTags = <MergeOutcome>[];

      final a = enricher.start(
          title: 'trackA', artist: 'NOFX',
          onTags: aTags.add, onArt: (_) {});
      // Let A get past the debounce and issue its request.
      await Future.delayed(const Duration(milliseconds: 50));
      expect(mbCalls, 1, reason: 'A\'s request should be in flight');
      final b = enricher.start(
          title: 'trackB', artist: 'NOFX',
          onTags: bTags.add, onArt: (_) {});
      aGate.complete(); // A's reply arrives late
      await Future.wait([a, b]);

      expect(aTags, isEmpty,
          reason: 'a late reply must never land on the current track');
      expect(bTags, hasLength(1));
    });

    test('cancel() alone silences an in-flight lookup', () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final enricher = PreviewEnricher(
          debounce: const Duration(milliseconds: 40));
      final tags = <MergeOutcome>[];
      final art = <Uint8List>[];
      final fut = enricher.start(
          title: '09_nofx_olympia_wa', artist: 'NOFX',
          onTags: tags.add, onArt: art.add);
      enricher.cancel(); // e.g. player.stop() before the debounce elapsed
      await fut;
      expect(tags, isEmpty);
      expect(art, isEmpty);
      expect(requests, isEmpty);
    });

    test('rapid scrubbing A→B→C fires exactly one MusicBrainz request',
        () async {
      MetadataLookup.debugClient = client();
      final enricher = PreviewEnricher(
          debounce: const Duration(milliseconds: 60));
      final futures = <Future<void>>[];
      for (final t in ['scrub_a', 'scrub_b', '09_nofx_olympia_wa']) {
        futures.add(enricher.start(
            title: t, artist: 'NOFX', onTags: (_) {}, onArt: (_) {}));
        await Future.delayed(const Duration(milliseconds: 10));
      }
      await Future.wait(futures);
      expect(requests.where((u) => u.host == 'musicbrainz.org'),
          hasLength(1));
    });

    test('scrubbing back to the same track re-queries NOTHING (tags + art '
        'both served from cache)', () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final enricher = PreviewEnricher(debounce: Duration.zero);
      Future<void> once() => enricher.start(
          title: '09_nofx_olympia_wa', artist: 'NOFX', durationMs: 178600,
          onTags: (_) {}, onArt: (_) {});
      await once();
      final after = requests.length; // 1 MB + 1 CAA
      expect(requests.where((u) => u.host == 'musicbrainz.org'), hasLength(1));
      expect(caaRequests(), hasLength(1));
      await once();
      expect(requests.length, after,
          reason: 'second visit must be pure cache');
    });

    test('opt-out (default OFF): no requests, no callbacks, nothing stored',
        () async {
      SharedPreferences.setMockInitialValues({}); // gate closed
      MetadataLookup.debugClient = client(caa: jpegOk);
      final enricher = PreviewEnricher(debounce: Duration.zero);
      final tags = <MergeOutcome>[];
      final art = <Uint8List>[];
      await enricher.start(
          title: '09_nofx_olympia_wa', artist: 'NOFX',
          onTags: tags.add, onArt: art.add);
      expect(tags, isEmpty);
      expect(art, isEmpty);
      expect(requests, isEmpty);
      expect(await AlbumArtCache.cachedArt('NOFX', 'Punk in Drublic'), isNull);
    });

    test('no usable title → nothing at all', () async {
      MetadataLookup.debugClient = client(caa: jpegOk);
      final enricher = PreviewEnricher(debounce: Duration.zero);
      await enricher.start(
          title: '   ', artist: 'NOFX', onTags: (_) {}, onArt: (_) {});
      expect(requests, isEmpty);
    });
  });
}
