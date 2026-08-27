// napstr protocol layer: catalogue and availability parsing, search
// tokenisation, SOCKS5 framing, transfer frames and offer validation.

import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/napstr/catalogue.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/event.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/keys.dart';
import 'package:bopwire_player/src/services/networks/napstr/nostr/relay_pool.dart';
import 'package:bopwire_player/src/services/networks/napstr/search_tokens.dart';
import 'package:bopwire_player/src/services/networks/napstr/socks5.dart';
import 'package:bopwire_player/src/services/networks/napstr/transfer.dart';
import 'package:bopwire_player/src/services/networks/napstr/verifier.dart';

import 'live_events.dart';

NostrEvent _parse(String json) => NostrEvent.tryParse(jsonDecode(json))!;

/// Signs a catalogue event the way the reference client does, so tests can
/// build variations without hand-computing ids.
NostrEvent _catalogueEvent(
  NostrKeyPair keys, {
  required String fileId,
  required String filename,
  required int size,
  String format = 'MP3',
  String mime = 'audio/mpeg',
  String title = '',
  String artist = '',
  String album = '',
  String tags = '',
  List<List<String>>? overrideTags,
  Map<String, Object?>? overrideContent,
}) {
  final content = overrideContent ??
      {
        'protocol': 'napstr/1',
        'fileId': fileId,
        'filename': filename,
        'title': title,
        'artist': artist,
        'album': album,
        'format': format,
        'mime': mime,
        'size': size,
        'license': 'unspecified',
        'description': '',
        'tags': tags,
      };
  return signEvent(
    keys: keys,
    kind: kCatalogueKind,
    tags: overrideTags ??
        [
          ['d', fileId],
          ['t', 'napstr'],
          ['x', fileId],
          ['name', filename],
          ['size', '$size'],
          ['m', mime],
          ['alt', 'Napstr shared file catalogue entry'],
        ],
    content: jsonEncode(content),
  );
}

void main() {
  const idA = '0dc5b94456d21e207b382810b36c9f262305915fc08272f48c4262cf6f5e580c';
  const idB = '53b89dce8dbbcf0d9e0fb4a9a64fafffded69c63b74002f5353220dfcd562615';

  group('live napstr events (captured from wss://nos.lol)', () {
    test('every captured catalogue event has a valid id and signature', () {
      for (final json in liveCatalogueEventsJson) {
        final e = _parse(json);
        expect(e.kind, kCatalogueKind);
        expect(e.hasValidId, isTrue, reason: 'id mismatch for ${e.id}');
        expect(e.verify(), isTrue, reason: 'signature failed for ${e.id}');
      }
    });

    test('every captured availability heartbeat verifies', () {
      for (final json in liveAvailabilityEventsJson) {
        final e = _parse(json);
        expect(e.kind, kAvailabilityKind);
        expect(e.verify(), isTrue);
      }
    });

    test('parses a real catalogue entry into a record', () {
      final result = parseCatalogueEvent(_parse(liveCatalogueEventsJson.first));
      expect(result.outcome, CatalogueParseOutcome.record);
      final r = result.record!;
      expect(r.fileId, idA);
      expect(r.title, 'Raining Blood');
      expect(r.artist, 'Robyn Adele Anderson');
      expect(r.album, 'Raining Blood');
      expect(r.format, 'OPUS');
      expect(r.mime, 'audio/ogg');
      expect(r.size, 2523544);
      expect(r.extension, 'opus');
      expect(r.filename.endsWith('.opus'), isTrue);
    });

    test('parses a real heartbeat when it has not yet expired', () {
      final e = _parse(liveAvailabilityEventsJson.first);
      expect(e.expiration, isNotNull);

      // Pin the clock either side of the captured 10-minute expiry so the
      // assertion does not depend on when the suite happens to run.
      final justAfter =
          DateTime.fromMillisecondsSinceEpoch((e.expiration! + 1) * 1000);
      expect(parseAvailabilityEvent(e, now: justAfter), isNull,
          reason: 'an expired heartbeat is not live availability');

      final justBefore = DateTime.fromMillisecondsSinceEpoch(
          (e.expiration! - 1) * 1000);
      final hb = parseAvailabilityEvent(e, now: justBefore);
      expect(hb, isNotNull);
      expect(hb!.pubkey, e.pubkey);
      expect(hb.fileIds, isNotEmpty);
      for (final id in hb.fileIds) {
        expect(validFileId(id), isTrue);
      }
    });

    test('the isolate verifier accepts real events and rejects a forgery',
        () async {
      final real = [for (final j in liveCatalogueEventsJson) _parse(j)];
      final verified = await verifyEvents(real);
      expect(verified.length, real.length);

      final good = real.first;
      // Same signature, someone else's pubkey.
      final forged = NostrEvent(
        id: good.id,
        pubkey: _parse(liveCatalogueEventsJson[1]).pubkey == good.pubkey
            ? 'f' * 64
            : _parse(liveCatalogueEventsJson[1]).pubkey,
        createdAt: good.createdAt,
        kind: good.kind,
        tags: good.tags,
        content: good.content,
        sig: good.sig,
      );
      expect((await verifyEvents([forged])), isEmpty);
    });
  });

  group('catalogue validation', () {
    late NostrKeyPair keys;
    setUp(() => keys = NostrKeyPair.generate());

    test('accepts a well-formed entry', () {
      final e = _catalogueEvent(keys,
          fileId: idA, filename: 'song.mp3', size: 1234, title: 'Song');
      expect(parseCatalogueEvent(e).outcome, CatalogueParseOutcome.record);
    });

    test('detects a withdrawal', () {
      final e = signEvent(
        keys: keys,
        kind: kCatalogueKind,
        tags: [
          ['d', idA],
          ['t', 'napstr'],
        ],
        content: '{"protocol":"napstr/1","deleted":true}',
      );
      final r = parseCatalogueEvent(e);
      expect(r.outcome, CatalogueParseOutcome.withdrawn);
      expect(r.fileId, idA);
    });

    test('rejects a mismatched extension / format / MIME triple', () {
      // .mp3 claimed as FLAC.
      expect(
        parseCatalogueEvent(_catalogueEvent(keys,
                fileId: idA,
                filename: 'song.mp3',
                size: 10,
                format: 'FLAC',
                mime: 'audio/flac'))
            .outcome,
        CatalogueParseOutcome.invalid,
      );
      // A video file with an audio MIME claim.
      expect(
        parseCatalogueEvent(_catalogueEvent(keys,
                fileId: idA, filename: 'clip.mp4', size: 10))
            .outcome,
        CatalogueParseOutcome.invalid,
      );
      expect(audioClaimValid('a.opus', 'OPUS', 'audio/ogg'), isTrue);
      expect(audioClaimValid('a.ogg', 'OGG', 'audio/ogg'), isTrue);
      expect(audioClaimValid('a.wav', 'WAV', 'audio/wav'), isTrue);
      expect(audioClaimValid('a.exe', 'MP3', 'audio/mpeg'), isFalse);
      expect(audioClaimValid('noextension', 'MP3', 'audio/mpeg'), isFalse);
    });

    test('rejects a content fileId that disagrees with the d tag', () {
      final e = _catalogueEvent(keys,
          fileId: idA,
          filename: 'song.mp3',
          size: 10,
          overrideContent: {
            'protocol': 'napstr/1',
            'fileId': idB,
            'filename': 'song.mp3',
            'format': 'MP3',
            'mime': 'audio/mpeg',
            'size': 10,
          });
      expect(parseCatalogueEvent(e).outcome, CatalogueParseOutcome.invalid);
    });

    test('rejects a signed size tag that disagrees with the content size',
        () {
      final e = _catalogueEvent(keys,
          fileId: idA,
          filename: 'song.mp3',
          size: 10,
          overrideTags: [
            ['d', idA],
            ['t', 'napstr'],
            ['size', '999999'],
            ['m', 'audio/mpeg'],
          ]);
      expect(parseCatalogueEvent(e).outcome, CatalogueParseOutcome.invalid);
    });

    test('rejects a path-traversing filename', () {
      for (final name in ['../../etc/passwd.mp3', 'sub/dir.mp3', r'C:\x.mp3']) {
        final e = _catalogueEvent(keys, fileId: idA, filename: name, size: 10);
        expect(parseCatalogueEvent(e).outcome, CatalogueParseOutcome.invalid,
            reason: name);
      }
      expect(safeBasename('ok.mp3'), 'ok.mp3');
      expect(safeBasename('..'), isNull);
      expect(safeBasename(''), isNull);
    });

    test('rejects control and bidi characters in displayed metadata', () {
      final e = _catalogueEvent(keys,
          fileId: idA,
          filename: 'song.mp3',
          size: 10,
          // U+202E right-to-left override — the classic filename spoof.
          title: 'inno\u202Ecuous');
      expect(parseCatalogueEvent(e).outcome, CatalogueParseOutcome.invalid);
      expect(isValidCatalogueText('plain text'), isTrue);
      expect(isValidCatalogueText('a\u0000b'), isFalse);
      expect(isValidCatalogueText('x' * 257), isFalse);
    });

    test('rejects a wrong protocol version, missing hashtag and zero size',
        () {
      expect(
        parseCatalogueEvent(_catalogueEvent(keys,
                fileId: idA,
                filename: 'a.mp3',
                size: 1,
                overrideContent: {
                  'protocol': 'napstr/2',
                  'fileId': idA,
                  'filename': 'a.mp3',
                  'format': 'MP3',
                  'mime': 'audio/mpeg',
                  'size': 1,
                }))
            .outcome,
        CatalogueParseOutcome.invalid,
      );
      expect(
        parseCatalogueEvent(_catalogueEvent(keys,
                fileId: idA,
                filename: 'a.mp3',
                size: 1,
                overrideTags: [
              ['d', idA],
            ])).outcome,
        CatalogueParseOutcome.invalid,
      );
      expect(
        parseCatalogueEvent(
                _catalogueEvent(keys, fileId: idA, filename: 'a.mp3', size: 0))
            .outcome,
        CatalogueParseOutcome.invalid,
      );
    });

    test('rejects an uppercase or short file id', () {
      expect(validFileId(idA), isTrue);
      expect(validFileId(idA.toUpperCase()), isFalse);
      expect(validFileId(idA.substring(1)), isFalse);
      expect(validFileId(''), isFalse);
    });

    test('normalises the user tag field and enforces its limits', () {
      expect(normaliseTags(''), '');
      expect(normaliseTags('punk, live ,PUNK'), 'punk, live');
      expect(normaliseTags(List.generate(13, (i) => 't$i').join(',')), isNull);
      expect(normaliseTags(List.generate(12, (i) => 't$i').join(',')),
          isNotNull);
      expect(normaliseTags('y' * 33), isNull);
      expect(normaliseTags('a\u0007b'), isNull);
    });

    test('sanitises a basename for the local filesystem', () {
      expect(sanitiseForFilesystem('song.mp3'), 'song.mp3');
      expect(sanitiseForFilesystem('a<b>c:d"e|f?g*h.mp3'),
          'a_b_c_d_e_f_g_h.mp3');
      expect(sanitiseForFilesystem('   '), 'napstr-download');
      expect(sanitiseForFilesystem('trailing dot.'), 'trailing dot');
      expect(sanitiseForFilesystem('${'n' * 300}.mp3').length, 180);
    });
  });

  group('availability heartbeats', () {
    late NostrKeyPair keys;
    final future = DateTime.now().add(const Duration(minutes: 5));
    setUp(() => keys = NostrKeyPair.generate());

    NostrEvent heartbeat(List<String> ids, {int? expiration, String? hashtag}) =>
        signEvent(
          keys: keys,
          kind: kAvailabilityKind,
          tags: [
            ['d', 'availability-0000'],
            ['t', hashtag ?? 'napstr-availability'],
            if (expiration != null) ['expiration', '$expiration'],
          ],
          content: jsonEncode(ids),
        );

    test('parses a live heartbeat', () {
      final hb = parseAvailabilityEvent(
        heartbeat([idA, idB],
            expiration: future.millisecondsSinceEpoch ~/ 1000),
      );
      expect(hb, isNotNull);
      expect(hb!.fileIds, {idA, idB});
      expect(hb.pubkey, keys.publicKeyHex);
    });

    test('rejects an expired or unexpiring heartbeat', () {
      expect(parseAvailabilityEvent(heartbeat([idA], expiration: 1000)), isNull);
      expect(parseAvailabilityEvent(heartbeat([idA])), isNull,
          reason: 'the expiration tag is mandatory');
    });

    test('rejects the wrong hashtag', () {
      expect(
        parseAvailabilityEvent(heartbeat([idA],
            expiration: future.millisecondsSinceEpoch ~/ 1000,
            hashtag: 'napstr')),
        isNull,
      );
    });

    test('rejects an oversized group and drops malformed ids', () {
      final tooMany = List.filled(kAvailabilityGroupLimit + 1, idA);
      expect(
        parseAvailabilityEvent(heartbeat(tooMany,
            expiration: future.millisecondsSinceEpoch ~/ 1000)),
        isNull,
      );
      final mixed = parseAvailabilityEvent(heartbeat([idA, 'nope', idA.toUpperCase()],
          expiration: future.millisecondsSinceEpoch ~/ 1000));
      expect(mixed!.fileIds, {idA});
    });
  });

  group('track handles', () {
    test('round-trip', () {
      final pk = 'a' * 64;
      final ref = NapstrTrackRef(idA, [pk]);
      expect(ref.encode(), 'napstr:$idA:$pk');
      final back = NapstrTrackRef.decode(ref.encode())!;
      expect(back.fileId, idA);
      expect(back.seeders, [pk]);
    });

    test('rejects malformed handles', () {
      expect(NapstrTrackRef.decode('nope'), isNull);
      expect(NapstrTrackRef.decode('slsk:$idA:${'a' * 64}'), isNull);
      expect(NapstrTrackRef.decode('napstr:short:${'a' * 64}'), isNull);
      expect(NapstrTrackRef.decode('napstr:$idA:notapubkey'), isNull);
    });

    test('tolerates a track with no seeder recorded', () {
      final ref = NapstrTrackRef.decode('napstr:$idA:')!;
      expect(ref.seeders, isEmpty);
    });
  });

  group('search tokenisation', () {
    test('splits on non-alphanumerics and lowercases', () {
      expect(searchTokens('Raining Blood (Slayer) [K60h]'),
          ['raining', 'blood', 'slayer', 'k60h']);
      expect(searchTokens(''), isEmpty);
    });

    test('drops stop words and format words from catalogue tags', () {
      final tokens = catalogueSearchTokens(['The Best of MP3 and Jazz.mp3']);
      expect(tokens, isNot(contains('the')));
      expect(tokens, isNot(contains('mp3')));
      expect(tokens, isNot(contains('and')));
      expect(tokens, contains('best'));
      expect(tokens, contains('jazz'));
    });

    test('interleaves fields so one long field cannot starve the others', () {
      final tokens = catalogueSearchTokens([
        List.generate(30, (i) => 'file$i').join(' '),
        'Metallica',
      ]);
      expect(tokens.length, kCatalogueSearchTokenLimit);
      expect(tokens, contains('metallica'),
          reason: 'the second field must get a slot in the first round');
    });

    test('caps at 20 tokens of at most 32 characters', () {
      final tokens = catalogueSearchTokens(['${'z' * 33} short']);
      expect(tokens, ['short']);
    });

    test('query tag tokens are the four longest, longest first', () {
      expect(queryTagTokens('a bb ccc dddd eeeee ffffff'),
          ['ffffff', 'eeeee', 'dddd', 'ccc']);
      expect(queryTagTokens('the and of'), isEmpty);
    });

    test('matching requires every query token, with substring hits', () {
      expect(searchMatches('rain blood', ['Raining Blood', 'Slayer']), isTrue);
      expect(searchMatches('rain jazz', ['Raining Blood']), isFalse);
      expect(searchMatches('', ['anything']), isTrue);
    });

    test('tolerates a one-character typo in words of five or more', () {
      expect(searchMatches('metalica', ['Metallica']), isTrue);
      // Short words get no fuzzy allowance, so they must match exactly.
      expect(searchMatches('abcd', ['abce']), isFalse);
    });

    test('bounded edit distance', () {
      expect(editDistanceAtMost('abc', 'abc', 1), isTrue);
      expect(editDistanceAtMost('abc', 'abd', 1), isTrue);
      expect(editDistanceAtMost('abc', 'ab', 1), isTrue);
      expect(editDistanceAtMost('abc', 'xyz', 1), isFalse);
      expect(editDistanceAtMost('abc', 'abcde', 1), isFalse);
    });
  });

  group('relay list parsing', () {
    test('falls back to the napstr defaults', () {
      expect(parseRelayList(null), kDefaultNapstrRelays);
      expect(parseRelayList('   '), kDefaultNapstrRelays);
      expect(parseRelayList('http://not-a-relay.example'), kDefaultNapstrRelays);
    });

    test('accepts ws/wss, de-duplicates and strips trailing slashes', () {
      expect(
        parseRelayList('wss://a.example/, wss://a.example  ws://b.example'),
        ['wss://a.example', 'ws://b.example'],
      );
    });
  });

  group('SOCKS5', () {
    test('validates v3 onion hostnames', () {
      final onion = '${'a' * 56}.onion';
      expect(isV3Onion(onion), isTrue);
      expect(isV3Onion('${'a' * 55}.onion'), isFalse, reason: 'too short');
      expect(isV3Onion('${'a' * 57}.onion'), isFalse, reason: 'too long');
      expect(isV3Onion('${'A' * 56}.onion'), isFalse, reason: 'uppercase');
      expect(isV3Onion('${'a' * 55}1.onion'), isFalse, reason: '1 is not base32');
      expect(isV3Onion('${'a' * 55}8.onion'), isFalse, reason: '8 is not base32');
      expect(isV3Onion('${'a' * 55}7.onion'), isTrue);
      expect(isV3Onion('example.com'), isFalse);
      expect(isV3Onion('${'a' * 16}.onion'), isFalse, reason: 'v2 onion');
    });

    test('builds a DOMAINNAME CONNECT request rather than resolving', () {
      final req = buildConnectRequest('host.onion', 80);
      expect(req[0], 0x05); // SOCKS5
      expect(req[1], 0x01); // CONNECT
      expect(req[2], 0x00);
      expect(req[3], 0x03); // ATYP = DOMAINNAME, so Tor does the lookup
      expect(req[4], 'host.onion'.length);
      expect(String.fromCharCodes(req.sublist(5, 5 + 10)), 'host.onion');
      expect(req[15], 0x00);
      expect(req[16], 80);
    });

    test('rejects an unusable hostname length', () {
      expect(() => buildConnectRequest('', 80), throwsA(isA<Socks5Exception>()));
      expect(() => buildConnectRequest('x' * 256, 80),
          throwsA(isA<Socks5Exception>()));
    });

    test('parses proxy endpoints', () {
      expect(Socks5Endpoint.tryParse('127.0.0.1:9050').toString(),
          '127.0.0.1:9050');
      expect(Socks5Endpoint.tryParse('9150').toString(), '127.0.0.1:9150');
      expect(Socks5Endpoint.tryParse('localhost').toString(), 'localhost:9050');
      expect(Socks5Endpoint.tryParse(''), isNull);
      expect(Socks5Endpoint.tryParse(null), isNull);
      expect(Socks5Endpoint.tryParse('host:0'), isNull);
      expect(Socks5Endpoint.tryParse('host:99999'), isNull);
      expect(Socks5Endpoint.tryParse('host:nope'), isNull);
    });
  });

  group('transfer protocol v2', () {
    test('frames are u32 big-endian length plus JSON', () {
      final frame = encodeFrame({'type': 'REQUEST_FILE'});
      final length =
          ByteData.sublistView(frame).getUint32(0, Endian.big);
      expect(length, frame.length - 4);
      final body = decodeFrameBody(frame.sublist(4))!;
      expect(body['type'], 'REQUEST_FILE');
    });

    test('encodes the HELLO frame the seeder expects', () {
      final frame = encodeFrame({
        'type': 'HELLO',
        'version': kTransferProtocolVersion,
        'capability': 'c' * 64,
        'file_id': idA,
      });
      final body = decodeFrameBody(frame.sublist(4))!;
      expect(body['type'], 'HELLO');
      expect(body['version'], 2);
      expect(body['file_id'], idA);
    });

    test('refuses to build an oversized control frame', () {
      expect(
        () => encodeFrame({'type': 'X', 'pad': 'y' * (64 * 1024)}),
        throwsA(isA<TransferException>()),
      );
    });

    test('decodeFrameBody rejects non-objects and typeless frames', () {
      expect(decodeFrameBody(utf8.encode('[]')), isNull);
      expect(decodeFrameBody(utf8.encode('{"no":"type"}')), isNull);
      expect(decodeFrameBody(utf8.encode('{"type":1}')), isNull);
      expect(decodeFrameBody(utf8.encode('not json')), isNull);
    });
  });

  group('download offer validation', () {
    const requestId = '42f9ac7c-fd56-475c-9a6d-adcc35a1f826';
    final seeder = 'b' * 64;
    final onion = '${'a' * 56}.onion';

    Map<String, Object?> offerBody({
      String? reqId,
      String? fileId,
      String? host,
      int port = 80,
      String? capability,
      int? expiresAt,
      String protocol = 'napstr/1',
    }) =>
        {
          'type': 'DOWNLOAD_OFFER',
          'protocol': protocol,
          'offer': {
            'requestId': reqId ?? requestId,
            'fileId': fileId ?? idA,
            'onion': host ?? onion,
            'port': port,
            'capability': capability ?? 'c' * 64,
            'expiresAt': expiresAt ??
                DateTime.now().millisecondsSinceEpoch ~/ 1000 + 900,
          },
        };

    DownloadOffer? validate(Map<String, Object?> body, {String? sender}) =>
        DownloadOffer.validate(
          body,
          expectedRequestId: requestId,
          expectedFileId: idA,
          senderPubkey: sender ?? seeder,
          requestedSeeders: {seeder},
        );

    test('accepts a well-formed offer', () {
      final offer = validate(offerBody());
      expect(offer, isNotNull);
      expect(offer!.onion, onion);
      expect(offer.port, 80);
      expect(offer.seederPubkey, seeder);
      expect(offer.isExpired(), isFalse);
    });

    test('rejects an offer from a seeder we did not ask', () {
      expect(validate(offerBody(), sender: 'd' * 64), isNull);
    });

    test('rejects a mismatched request id or file id', () {
      expect(validate(offerBody(reqId: 'other')), isNull);
      expect(validate(offerBody(fileId: idB)), isNull);
    });

    test('rejects a clearnet host — there is no non-Tor fallback', () {
      expect(validate(offerBody(host: 'evil.example.com')), isNull);
      expect(validate(offerBody(host: '203.0.113.5')), isNull);
      expect(validate(offerBody(host: '${'a' * 16}.onion')), isNull);
    });

    test('rejects an expired offer', () {
      expect(validate(offerBody(expiresAt: 1000)), isNull);
    });

    test('rejects a bad capability, port or protocol', () {
      expect(validate(offerBody(capability: 'short')), isNull);
      expect(validate(offerBody(port: 0)), isNull);
      expect(validate(offerBody(port: 70000)), isNull);
      expect(validate(offerBody(protocol: 'napstr/2')), isNull);
    });

    test('rejects structurally wrong bodies', () {
      expect(validate({'type': 'DOWNLOAD_OFFER'}), isNull);
      expect(
          DownloadOffer.validate('nope',
              expectedRequestId: requestId,
              expectedFileId: idA,
              senderPubkey: seeder,
              requestedSeeders: {seeder}),
          isNull);
      expect(validate({'type': 'DOWNLOAD_REFUSED', 'protocol': 'napstr/1'}),
          isNull);
    });
  });
}
