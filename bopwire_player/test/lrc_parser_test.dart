// Tests for the LRC parser + active-line selection (lyrics_panel.dart).
//
// The parser is a port of the web player's parseLRC and the two clients
// must agree on every file, so the wire-format corners are exercised
// hard: multi-timestamp chorus lines, 2- vs 3-digit fractions, metadata
// tags, blank lines, garbage, and the empty string.

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/widgets/lyrics_panel.dart';

void main() {
  group('parseLrc', () {
    test('empty string parses to no lines', () {
      expect(parseLrc(''), isEmpty);
    });

    test('garbage input parses to no lines and does not throw', () {
      expect(parseLrc('this is not lrc\njust some text\n12:34 no brackets'),
          isEmpty);
      expect(parseLrc('[]\n[:]\n[ab:cd]\n[[[['), isEmpty);
    });

    test('basic lines parse with correct times', () {
      final lines = parseLrc('[00:05.00] hello\n[00:10.50] world');
      expect(lines, hasLength(2));
      expect(lines[0].timeMs, 5000);
      expect(lines[0].text, 'hello');
      expect(lines[1].timeMs, 10500);
      expect(lines[1].text, 'world');
    });

    test('a 2-digit fraction is centiseconds', () {
      final lines = parseLrc('[00:01.23] x');
      expect(lines.single.timeMs, 1230);
    });

    test('a 3-digit fraction is milliseconds', () {
      final lines = parseLrc('[00:01.234] x');
      expect(lines.single.timeMs, 1234);
    });

    test('a stamp with no fraction is whole seconds', () {
      final lines = parseLrc('[01:02] x');
      expect(lines.single.timeMs, 62000);
    });

    test('a colon separates the fraction too ([mm:ss:ff])', () {
      final lines = parseLrc('[00:01:50] x');
      expect(lines.single.timeMs, 1500);
    });

    test('one line with several stamps becomes several entries (chorus)',
        () {
      final lines = parseLrc('[00:10.00][01:10.00] la la la');
      expect(lines, hasLength(2));
      expect(lines[0].timeMs, 10000);
      expect(lines[1].timeMs, 70000);
      expect(lines.map((l) => l.text), everyElement('la la la'));
    });

    test('metadata tags never become lyric lines', () {
      final lines = parseLrc(
          '[ar:NOFX]\n[ti:Linoleum]\n[al:Punk in Drublic]\n'
          '[by:someone]\n[offset:+120]\n[00:01.00] real line');
      expect(lines, hasLength(1));
      expect(lines.single.text, 'real line');
    });

    test('blank and stamp-less lines are skipped', () {
      final lines = parseLrc('\n\n[00:01.00] one\n\nno stamp here\n');
      expect(lines, hasLength(1));
    });

    test('output is sorted by time even when the input is not', () {
      final lines = parseLrc('[01:00.00] later\n[00:05.00] earlier');
      expect(lines[0].text, 'earlier');
      expect(lines[1].text, 'later');
    });

    test('all bracketed content is stripped from the text', () {
      final lines = parseLrc('[00:05.00] hello [ooh] world');
      expect(lines.single.text, 'hello  world');
    });

    test('a timed line with no words keeps an empty text', () {
      final lines = parseLrc('[00:05.00]');
      expect(lines.single.text, '');
    });

    test('minutes above 99 parse (up to 3 digits)', () {
      final lines = parseLrc('[100:00] marathon');
      expect(lines.single.timeMs, 6000000);
    });
  });

  group('activeLrcIndex', () {
    final lines = parseLrc('[00:05.00] a\n[00:10.00] b\n[00:15.00] c');

    test('empty list has no active line', () {
      expect(activeLrcIndex(const [], 99999), -1);
    });

    test('before the first stamp nothing is active', () {
      expect(activeLrcIndex(lines, 0), -1);
      expect(activeLrcIndex(lines, 4999), -1);
    });

    test('exactly on a stamp that line is active', () {
      expect(activeLrcIndex(lines, 5000), 0);
      expect(activeLrcIndex(lines, 10000), 1);
      expect(activeLrcIndex(lines, 15000), 2);
    });

    test('between stamps the earlier line stays active', () {
      expect(activeLrcIndex(lines, 7500), 0);
      expect(activeLrcIndex(lines, 14999), 1);
    });

    test('after the last stamp the last line stays active', () {
      expect(activeLrcIndex(lines, 15001), 2);
      expect(activeLrcIndex(lines, 9999999), 2);
    });
  });

  group('LyricsRequest', () {
    test('empty hashes are dropped and never match', () {
      final req = LyricsRequest(
          songKey: 'k', title: 't', artist: 'a', hashes: {'', 'h1'});
      expect(req.hashes, {'h1'});
      expect(req.matchesPlaying(''), isFalse);
      expect(req.matchesPlaying('h1'), isTrue);
      expect(req.matchesPlaying('h2'), isFalse);
    });

    test('a song with no artist or title still builds a request', () {
      final req = LyricsRequest(songKey: 'k', title: '', artist: '');
      expect(req.hashes, isEmpty);
      expect(req.matchesPlaying('anything'), isFalse);
    });
  });
}
