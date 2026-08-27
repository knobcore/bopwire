// Tests for the online metadata enrichment (metadata_lookup.dart).
//
// The merge policy is where the damage would be — a wrong "correction"
// overwrites a user's deliberate tags — so it is exercised hardest:
// complete tags + confident match, complete tags + weak match, blank
// tags, filename-derived tags, and garbage API responses. The network
// pipeline (enrich / lyrics) runs against a MockClient: no test here
// touches the real internet.

import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:http/http.dart' as http;
import 'package:http/testing.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/metadata_lookup.dart';

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

TagCandidate cand({
  String title = 'Linoleum',
  String artist = 'NOFX',
  String album = 'Punk in Drublic',
  int year = 1994,
  int track = 8,
  bool confident = false,
  String source = 'musicbrainz',
}) =>
    TagCandidate(
      title: title, artist: artist, album: album,
      year: year, trackNumber: track,
      source: source, score: confident ? 100 : 85, confident: confident,
    );

Map<String, dynamic> mbResponse({
  int score = 100,
  int lengthMs = 178600,
  String title = 'Olympia, WA',
  String artist = 'NOFX',
  List<Map<String, dynamic>>? extraRecordings,
}) =>
    {
      'created': '2026-08-27T00:00:00Z',
      'count': 1,
      'recordings': [
        {
          'score': score,
          'title': title,
          'length': lengthMs,
          'artist-credit': [
            {'name': artist, 'artist': {'name': artist}}
          ],
          'releases': [
            {
              'title': 'Punk in Drublic',
              'status': 'Official',
              'date': '1994-07-19',
              'release-group': {'primary-type': 'Album'},
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
        },
        ...?extraRecordings,
      ],
    };

Map<String, dynamic> acoustIdResponse({double score = 0.97}) => {
      'status': 'ok',
      'results': [
        {
          'id': 'abc',
          'score': score,
          'recordings': [
            {
              'id': 'rec-1',
              'title': 'Linoleum',
              'duration': 130.0,
              'artists': [
                {'id': 'a1', 'name': 'NOFX'}
              ],
              'releasegroups': [
                {
                  'type': 'Album',
                  'title': 'Punk in Drublic',
                  'releases': [
                    {
                      'date': {'year': 1994},
                      'mediums': [
                        {
                          'tracks': [
                            {'position': 8, 'title': 'Linoleum'}
                          ]
                        }
                      ],
                    }
                  ],
                }
              ],
            }
          ],
        }
      ],
    };

Map<String, dynamic> geniusResponse(
        {String title = 'Linoleum', String artist = 'NOFX'}) =>
    {
      'meta': {'status': 200},
      'response': {
        'hits': [
          {
            'type': 'song',
            'result': {
              'title': title,
              'primary_artist': {'name': artist},
            },
          }
        ],
      },
    };

// ---------------------------------------------------------------------------
// Merge policy
// ---------------------------------------------------------------------------

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  group('TagMerge.merge', () {
    const complete = CurrentTags(
      title: 'Linoleum',
      artist: 'NOFX',
      album: 'Desktop', // a real-world wrong tag from the sample FLACs
      year: 0,
      trackNumber: 4,
    );

    test('confident match corrects populated fields and keeps originals', () {
      final out = TagMerge.merge(complete, cand(confident: true));
      expect(out.album, 'Punk in Drublic');
      expect(out.trackNumber, 8);
      expect(out.corrected['album'], 'Desktop'); // original never discarded
      expect(out.corrected['track'], 4);
      expect(out.year, 1994);
      expect(out.filled, contains('year')); // blank -> filled, not corrected
      expect(out.corrected.containsKey('year'), isFalse);
      // Unchanged-but-matching fields are neither filled nor corrected.
      expect(out.corrected.containsKey('title'), isFalse);
      expect(out.title, 'Linoleum');
      expect(out.changed, isTrue);
    });

    test('weak match NEVER corrects populated fields, still fills blanks',
        () {
      final out = TagMerge.merge(complete, cand(confident: false));
      expect(out.album, 'Desktop'); // populated + weak match: keep
      expect(out.trackNumber, 4);
      expect(out.corrected, isEmpty);
      expect(out.year, 1994); // blank is fair game even on a weak match
      expect(out.filled, ['year']);
    });

    test('blank tags are filled from any accepted match', () {
      const blank = CurrentTags(
          title: '', artist: '', album: '', year: 0, trackNumber: 0);
      final out = TagMerge.merge(blank, cand(confident: false));
      expect(out.title, 'Linoleum');
      expect(out.artist, 'NOFX');
      expect(out.album, 'Punk in Drublic');
      expect(out.year, 1994);
      expect(out.trackNumber, 8);
      expect(out.corrected, isEmpty);
      expect(out.filled,
          containsAll(['title', 'artist', 'album', 'year', 'track']));
    });

    test('filename-derived title is weak: replaced even on a weak match, '
        'with the original recorded', () {
      const fromFile = CurrentTags(
        title: '01 nofx - linoleum (1994)', // filename stem
        artist: '',
        album: '',
        year: 0,
        trackNumber: 0,
        titleIsFromFilename: true,
      );
      final out = TagMerge.merge(fromFile, cand(confident: false));
      expect(out.title, 'Linoleum');
      expect(out.corrected['title'], '01 nofx - linoleum (1994)');
    });

    test('a real tagged title is NOT weak: survives a weak match', () {
      const tagged = CurrentTags(
          title: 'Linoleum (live)', artist: 'NOFX', album: '',
          year: 0, trackNumber: 0);
      final out = TagMerge.merge(tagged, cand(confident: false));
      expect(out.title, 'Linoleum (live)');
    });

    test('candidate with empty fields never blanks existing values', () {
      final empty = TagCandidate(
          source: 'genius', score: 1, confident: true); // all fields empty/0
      final out = TagMerge.merge(complete, empty);
      expect(out.title, 'Linoleum');
      expect(out.artist, 'NOFX');
      expect(out.album, 'Desktop');
      expect(out.trackNumber, 4);
      expect(out.changed, isFalse);
    });

    test('case-only differences are not treated as corrections', () {
      const cur = CurrentTags(
          title: 'linoleum', artist: 'nofx', album: 'punk in drublic',
          year: 1994, trackNumber: 8);
      final out = TagMerge.merge(cur, cand(confident: true));
      expect(out.changed, isFalse);
      expect(out.title, 'linoleum'); // keep the file's casing
    });
  });

  // -------------------------------------------------------------------------
  // Response parsers (fed realistic and garbage payloads)
  // -------------------------------------------------------------------------

  group('parseMusicBrainz', () {
    test('single high-score hit with close duration is confident and '
        'carries album/year/track', () {
      final c = MetadataLookup.parseMusicBrainz(mbResponse(),
          localDurationMs: 178600, artistWasKnown: true);
      expect(c, isNotNull);
      expect(c!.confident, isTrue);
      expect(c.title, 'Olympia, WA');
      expect(c.artist, 'NOFX');
      expect(c.album, 'Punk in Drublic');
      expect(c.year, 1994);
      expect(c.trackNumber, 9);
    });

    test('duration mismatch demotes to non-confident (fill-only)', () {
      // The truncated 79 s "Antennas" sample vs the real ~169 s recording.
      final c = MetadataLookup.parseMusicBrainz(mbResponse(lengthMs: 169000),
          localDurationMs: 79000, artistWasKnown: true);
      expect(c, isNotNull);
      expect(c!.confident, isFalse);
    });

    test('ambiguous runner-up (different song, near score) is not confident',
        () {
      final body = mbResponse(extraRecordings: [
        {
          'score': 98,
          'title': 'Olympia, WA',
          'length': 178000,
          'artist-credit': [
            {'name': 'Rancid'}
          ],
        }
      ]);
      final c = MetadataLookup.parseMusicBrainz(body,
          localDurationMs: 178600, artistWasKnown: true);
      expect(c, isNotNull);
      expect(c!.confident, isFalse);
    });

    test('compilation release-groups are demoted below the original album',
        () {
      // Mirrors the real MB payload for NOFX "Olympia WA": the recording
      // appears on Punk-O-Rama (Album + Compilation secondary type) AND
      // on its original plain-Album release. The plain album must win.
      final body = mbResponse();
      (body['recordings'] as List).first['releases'] = [
        {
          'title': 'Punk-O-Rama, Vol. 7',
          'status': 'Official',
          'date': '2008-08-05',
          'release-group': {
            'primary-type': 'Album',
            'secondary-types': ['Compilation'],
          },
        },
        {
          'title': 'BYO Split Series, Volume III',
          'status': 'Official',
          'date': '2002-03-05',
          'release-group': {'primary-type': 'Album'},
          'media': [
            {
              'track': [
                {'number': '2'}
              ]
            }
          ],
        },
      ];
      final c = MetadataLookup.parseMusicBrainz(body,
          localDurationMs: 178600, artistWasKnown: true);
      expect(c, isNotNull);
      expect(c!.album, 'BYO Split Series, Volume III');
      expect(c.year, 2002);
      expect(c.trackNumber, 2);
    });

    test('low score is rejected outright', () {
      expect(
          MetadataLookup.parseMusicBrainz(mbResponse(score: 60),
              localDurationMs: 178600, artistWasKnown: true),
          isNull);
    });

    test('title-only search without corroborating duration is rejected', () {
      expect(
          MetadataLookup.parseMusicBrainz(mbResponse(),
              localDurationMs: 0, artistWasKnown: false),
          isNull);
    });

    test('garbage responses return null, never throw', () {
      for (final garbage in [
        null, 42, 'nonsense', [], {},
        {'recordings': 'nope'},
        {'recordings': []},
        {'recordings': [{'score': 100}]}, // no title/artist
        {'recordings': [{'score': 'high', 'title': 7, 'length': 'long'}]},
      ]) {
        expect(
            MetadataLookup.parseMusicBrainz(garbage,
                localDurationMs: 178600, artistWasKnown: true),
            isNull,
            reason: 'garbage: $garbage');
      }
    });
  });

  group('parseAcoustId', () {
    test('high-score result is confident with full fields', () {
      final c = MetadataLookup.parseAcoustId(acoustIdResponse());
      expect(c, isNotNull);
      expect(c!.confident, isTrue);
      expect(c.source, 'acoustid');
      expect(c.title, 'Linoleum');
      expect(c.artist, 'NOFX');
      expect(c.album, 'Punk in Drublic');
      expect(c.year, 1994);
      expect(c.trackNumber, 8);
      expect(c.durationMs, 130000);
    });

    test('mid-score result is accepted but not confident', () {
      final c = MetadataLookup.parseAcoustId(acoustIdResponse(score: 0.7));
      expect(c, isNotNull);
      expect(c!.confident, isFalse);
    });

    test('sub-0.5 score is rejected', () {
      expect(MetadataLookup.parseAcoustId(acoustIdResponse(score: 0.4)),
          isNull);
    });

    test('garbage responses return null, never throw', () {
      for (final garbage in [
        null, 'x', [], {},
        {'status': 'error'},
        {'status': 'ok'},
        {'status': 'ok', 'results': 'nope'},
        {'status': 'ok', 'results': []},
        {
          'status': 'ok',
          'results': [
            {'score': 0.99} // no recordings
          ]
        },
        {
          'status': 'ok',
          'results': [
            {
              'score': 0.99,
              'recordings': [
                {'duration': 100} // no title/artists
              ]
            }
          ]
        },
      ]) {
        expect(MetadataLookup.parseAcoustId(garbage), isNull,
            reason: 'garbage: $garbage');
      }
    });
  });

  group('parseGenius', () {
    test('rescues a messy filename query, but only as low confidence', () {
      final c = MetadataLookup.parseGenius(geniusResponse(),
          queryTitle: '01 nofx - linoleum (1994)', queryArtist: '');
      expect(c, isNotNull);
      expect(c!.title, 'Linoleum');
      expect(c.artist, 'NOFX');
      expect(c.confident, isFalse); // fuzzy hit: fill blanks only
      // Genius album/year/track are never taken.
      expect(c.album, '');
      expect(c.year, 0);
      expect(c.trackNumber, 0);
    });

    test('near-exact normalized artist+title match is confident', () {
      final c = MetadataLookup.parseGenius(geniusResponse(),
          queryTitle: 'linoleum', queryArtist: 'nofx');
      expect(c, isNotNull);
      expect(c!.confident, isTrue);
    });

    test('plausible-but-wrong song (low token overlap) is rejected', () {
      final c = MetadataLookup.parseGenius(
          geniusResponse(title: 'Sultans of Swing', artist: 'Dire Straits'),
          queryTitle: 'linoleum',
          queryArtist: 'nofx');
      expect(c, isNull);
    });

    test('garbage responses return null, never throw', () {
      for (final garbage in [
        null, 'x', [], {},
        {'response': 'nope'},
        {'response': {}},
        {'response': {'hits': 'nope'}},
        {'response': {'hits': []}},
        {
          'response': {
            'hits': [
              {'type': 'song', 'result': 'nope'},
              {'type': 'song'},
            ]
          }
        },
      ]) {
        expect(
            MetadataLookup.parseGenius(garbage,
                queryTitle: 'linoleum', queryArtist: 'nofx'),
            isNull,
            reason: 'garbage: $garbage');
      }
    });
  });

  group('parseLrclib', () {
    test('record with lyrics parses; empty/garbage does not', () {
      final r = MetadataLookup.parseLrclibRecord({
        'plainLyrics': 'One more line...',
        'syncedLyrics': '[00:01.00] One more line...',
        'instrumental': false,
      });
      expect(r, isNotNull);
      expect(r!.plain, isNotEmpty);
      expect(r.synced, startsWith('[00:01'));
      expect(MetadataLookup.parseLrclibRecord({'plainLyrics': ''}), isNull);
      expect(MetadataLookup.parseLrclibRecord(null), isNull);
      expect(MetadataLookup.parseLrclibRecord('x'), isNull);
    });

    test('search picks the duration-corroborated result', () {
      final r = MetadataLookup.parseLrclibSearch([
        {'duration': 300.0, 'plainLyrics': 'wrong song'},
        {'duration': 179.0, 'plainLyrics': 'right song'},
      ], localDurationMs: 178600);
      expect(r, isNotNull);
      expect(r!.plain, 'right song');
      expect(MetadataLookup.parseLrclibSearch('garbage', localDurationMs: 0),
          isNull);
    });
  });

  // -------------------------------------------------------------------------
  // enrich() pipeline — MockClient, no real network
  // -------------------------------------------------------------------------

  group('enrich pipeline', () {
    late Directory tmp;
    var requests = <Uri>[];

    const current = CurrentTags(
      title: 'Olympia, Wa',
      artist: 'NOFX',
      album: 'Desktop',
      year: 0,
      trackNumber: 2,
    );

    setUp(() async {
      MetadataLookup.debugReset();
      tmp = await Directory.systemTemp.createTemp('meta_test');
      MetadataLookup.debugCacheFile = File('${tmp.path}/cache.json');
      MetadataLookup.debugLyricsDir = Directory('${tmp.path}/lyrics');
      requests = [];
      SharedPreferences.setMockInitialValues(
          {MetadataLookupPrefs.enabled: true});
    });

    tearDown(() async {
      MetadataLookup.debugClient = null;
      MetadataLookup.debugCacheFile = null;
      MetadataLookup.debugLyricsDir = null;
      MetadataLookup.debugReset();
      try { await tmp.delete(recursive: true); } catch (_) {}
    });

    http.Client mbOkClient() => MockClient((req) async {
          requests.add(req.url);
          if (req.url.host == 'musicbrainz.org') {
            return http.Response(jsonEncode(mbResponse()), 200,
                headers: {'content-type': 'application/json'});
          }
          return http.Response('not found', 404);
        });

    test('disabled (default OFF) → null and zero network calls', () async {
      SharedPreferences.setMockInitialValues({});
      MetadataLookup.debugClient = mbOkClient();
      final out = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(out, isNull);
      expect(requests, isEmpty);
    });

    test('MusicBrainz hit corrects the wrong album and fills blanks',
        () async {
      MetadataLookup.debugClient = mbOkClient();
      final out = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(out, isNotNull);
      expect(out!.album, 'Punk in Drublic');
      expect(out.corrected['album'], 'Desktop');
      expect(out.year, 1994);
      expect(out.artist, 'NOFX');
      expect(requests.length, 1);
      expect(requests.single.host, 'musicbrainz.org');
      // No AcoustID key configured → the fingerprint route was skipped.
    });

    test('second lookup for the same key is served from cache', () async {
      MetadataLookup.debugClient = mbOkClient();
      final a = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      final b = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(a, isNotNull);
      expect(b, isNotNull);
      expect(b!.album, a!.album);
      expect(requests.length, 1, reason: 'cache must prevent a re-query');
      // The cache survives a restart (new in-memory state, same file).
      MetadataLookup.debugReset();
      MetadataLookup.debugCacheFile = File('${tmp.path}/cache.json');
      final c = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(c, isNotNull);
      expect(requests.length, 1);
    });

    test('garbage HTTP body → null, and the miss is cached', () async {
      MetadataLookup.debugClient = MockClient((req) async {
        requests.add(req.url);
        return http.Response('<html>definitely not json', 200);
      });
      final a = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(a, isNull);
      final b = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(b, isNull);
      expect(requests.length, 1, reason: 'a miss must be cached too');
    });

    test('a throwing HTTP client never propagates', () async {
      MetadataLookup.debugClient = MockClient((req) async {
        throw const SocketException('network is down');
      });
      final out = await MetadataLookup.instance.enrich(
          current: current, fingerprint: '', durationMs: 178600,
          cacheKey: 'k1');
      expect(out, isNull); // logged and swallowed — the scan continues
    });

    test('genius rescue runs only when MusicBrainz misses and a token is set',
        () async {
      SharedPreferences.setMockInitialValues({
        MetadataLookupPrefs.enabled: true,
        MetadataLookupPrefs.geniusToken: 'tok',
      });
      MetadataLookup.debugClient = MockClient((req) async {
        requests.add(req.url);
        if (req.url.host == 'musicbrainz.org') {
          return http.Response(jsonEncode({'recordings': []}), 200);
        }
        if (req.url.host == 'api.genius.com') {
          expect(req.headers['Authorization'], 'Bearer tok');
          return http.Response(jsonEncode(geniusResponse()), 200);
        }
        return http.Response('nope', 404);
      });
      const messy = CurrentTags(
        title: '01 nofx - linoleum (1994)',
        artist: '', album: '', year: 0, trackNumber: 0,
        titleIsFromFilename: true,
      );
      final out = await MetadataLookup.instance.enrich(
          current: messy, fingerprint: '', durationMs: 130000,
          cacheKey: 'k2');
      expect(out, isNotNull);
      expect(out!.source, 'genius');
      expect(out.title, 'Linoleum');
      expect(out.artist, 'NOFX');
      expect(requests.map((u) => u.host),
          ['musicbrainz.org', 'api.genius.com']);
    });

    test('lyrics: fetched once, stored locally, miss-cached, opt-in', () async {
      SharedPreferences.setMockInitialValues({
        MetadataLookupPrefs.lyricsEnabled: true,
      });
      MetadataLookup.debugClient = MockClient((req) async {
        requests.add(req.url);
        if (req.url.host == 'lrclib.net' && req.url.path == '/api/get') {
          return http.Response(
              jsonEncode({
                'plainLyrics': 'la la la',
                'syncedLyrics': '[00:01.00] la la la',
              }),
              200);
        }
        return http.Response('', 404);
      });
      await MetadataLookup.instance.fetchLyricsIfEnabled(
          songKey: 'song1', title: 'Olympia, WA', artist: 'NOFX',
          album: 'Punk in Drublic', durationMs: 178600);
      final stored = await MetadataLookup.instance.storedLyrics('song1');
      expect(stored, isNotNull);
      expect(stored!.plain, 'la la la');
      expect(stored.synced, startsWith('[00:01'));
      expect(requests.length, 1);
      // Second call: served by the per-song file, no new request.
      await MetadataLookup.instance.fetchLyricsIfEnabled(
          songKey: 'song1', title: 'Olympia, WA', artist: 'NOFX',
          album: 'Punk in Drublic', durationMs: 178600);
      expect(requests.length, 1);
      // Opt-out: no request at all.
      SharedPreferences.setMockInitialValues({});
      await MetadataLookup.instance.fetchLyricsIfEnabled(
          songKey: 'song2', title: 'Antennas', artist: 'NOFX',
          album: '', durationMs: 79000);
      expect(requests.length, 1);
      expect(await MetadataLookup.instance.storedLyrics('song2'), isNull);
    });
  });
}
