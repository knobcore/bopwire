// Widget tests for the lyrics panel (lyrics_panel.dart).
//
// The panel takes an injectable loader and playback link, so these tests
// run plugin-free: no real player, no disk cache, no node. Covered: the
// empty / instrumental / plain states, the synced highlight following the
// clock ONLY when the on-screen song is the one playing, tap-to-seek and
// its wrong-song guard, and dropping a lyrics response that arrives after
// the user moved on to another song.

import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/metadata_lookup.dart';
import 'package:bopwire_player/src/widgets/lyrics_panel.dart';

// ---------------------------------------------------------------------------
// Fixtures
// ---------------------------------------------------------------------------

/// Stand-in for PlayerProvider: a position, a playing hash, and a seek log.
class _FakePlayer extends ChangeNotifier {
  int positionMs = 0;
  String playingHash = '';
  final List<int> seeks = [];

  void tick(int ms) {
    positionMs = ms;
    notifyListeners();
  }
}

LyricsPlayback _link(_FakePlayer f) => LyricsPlayback(
      listenable: f,
      positionMs: () => f.positionMs,
      playingHash: () => f.playingHash,
      seek: (ms) async => f.seeks.add(ms),
    );

LyricsLoader _returning(LyricsResult? r) => (_) async => r;

LyricsRequest _req({String key = 'h1'}) => LyricsRequest(
    songKey: key, title: 'Song $key', artist: 'Artist', hashes: {key});

Widget _wrap(Widget child) => MaterialApp(home: Scaffold(body: child));

const _lrc = '[00:05.00] first line\n'
    '[00:10.00] second line\n'
    '[00:15.00] third line';

TextStyle _styleOf(WidgetTester t, String text) =>
    t.widget<Text>(find.text(text)).style!;

void main() {
  group('LyricsPanel states', () {
    testWidgets('no lyrics found shows the empty state', (t) async {
      final f = _FakePlayer();
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(null),
      )));
      await t.pump();
      expect(find.text('No lyrics found for this track.'), findsOneWidget);
    });

    testWidgets('a throwing loader is the same as no lyrics', (t) async {
      final f = _FakePlayer();
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: (_) async => throw Exception('boom'),
      )));
      await t.pump();
      expect(find.text('No lyrics found for this track.'), findsOneWidget);
    });

    testWidgets('an instrumental track says so', (t) async {
      final f = _FakePlayer();
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(const LyricsResult(instrumental: true)),
      )));
      await t.pump();
      expect(find.text('This track is instrumental.'), findsOneWidget);
    });

    testWidgets('malformed LRC with no plain fallback reads as no lyrics',
        (t) async {
      final f = _FakePlayer();
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(
            const LyricsResult(synced: 'not [really] lrc at all')),
      )));
      await t.pump();
      expect(find.text('No lyrics found for this track.'), findsOneWidget);
    });

    testWidgets('plain lyrics render every line', (t) async {
      final f = _FakePlayer();
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader:
            _returning(const LyricsResult(plain: 'alpha\nbeta\ngamma')),
      )));
      await t.pump();
      expect(find.text('alpha'), findsOneWidget);
      expect(find.text('beta'), findsOneWidget);
      expect(find.text('gamma'), findsOneWidget);
    });

    testWidgets('header shows title, artist, and close works', (t) async {
      final f = _FakePlayer();
      var closed = false;
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () => closed = true,
        loader: _returning(null),
      )));
      await t.pump();
      expect(find.text('Song h1'), findsOneWidget);
      expect(find.text('Artist'), findsOneWidget);
      await t.tap(find.byTooltip('Close lyrics'));
      expect(closed, isTrue);
    });
  });

  group('LyricsPanel synced highlight', () {
    testWidgets('follows the clock while THIS song is playing', (t) async {
      final f = _FakePlayer()..playingHash = 'h1';
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(const LyricsResult(synced: _lrc)),
      )));
      await t.pump();

      // Before the first stamp: nothing highlighted.
      final theme = Theme.of(t.element(find.text('first line')));
      expect(_styleOf(t, 'first line').fontWeight, FontWeight.w500);

      f.tick(5000); // exactly on the first stamp
      await t.pump();
      await t.pump(const Duration(milliseconds: 300)); // scroll animation
      expect(_styleOf(t, 'first line').fontWeight, FontWeight.w800);
      expect(_styleOf(t, 'first line').color, theme.colorScheme.primary);
      expect(_styleOf(t, 'second line').fontWeight, FontWeight.w500);

      f.tick(12000); // between the 2nd and 3rd stamps
      await t.pump();
      await t.pump(const Duration(milliseconds: 300));
      expect(_styleOf(t, 'first line').fontWeight, FontWeight.w500);
      expect(_styleOf(t, 'second line').fontWeight, FontWeight.w800);

      f.tick(99999); // after the last stamp: last line stays active
      await t.pump();
      await t.pump(const Duration(milliseconds: 300));
      expect(_styleOf(t, 'third line').fontWeight, FontWeight.w800);
    });

    testWidgets('does NOT follow the clock when another song is playing',
        (t) async {
      final f = _FakePlayer()..playingHash = 'some-other-song';
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(const LyricsResult(synced: _lrc)),
      )));
      await t.pump();

      f.tick(12000);
      await t.pump();
      for (final text in ['first line', 'second line', 'third line']) {
        expect(_styleOf(t, text).fontWeight, FontWeight.w500,
            reason: '$text must not highlight for another song\'s clock');
      }
    });

    testWidgets('tapping a line seeks the player to its timestamp',
        (t) async {
      final f = _FakePlayer()..playingHash = 'h1';
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(const LyricsResult(synced: _lrc)),
      )));
      await t.pump();

      await t.tap(find.text('second line'));
      expect(f.seeks, [10000]);
    });

    testWidgets('tapping a line of a NON-playing song does not seek',
        (t) async {
      final f = _FakePlayer()..playingHash = 'some-other-song';
      await t.pumpWidget(_wrap(LyricsPanel(
        request: _req(),
        playback: _link(f),
        onClose: () {},
        loader: _returning(const LyricsResult(synced: _lrc)),
      )));
      await t.pump();

      await t.tap(find.text('second line'));
      expect(f.seeks, isEmpty);
    });
  });

  group('LyricsPanel stale responses', () {
    testWidgets('a reply that lands after the song changed is dropped',
        (t) async {
      final f = _FakePlayer();
      final completers = <String, Completer<LyricsResult?>>{};
      Future<LyricsResult?> loader(LyricsRequest r) =>
          (completers[r.songKey] ??= Completer()).future;

      Widget panel(LyricsRequest req) => _wrap(LyricsPanel(
            request: req,
            playback: _link(f),
            onClose: () {},
            loader: loader,
          ));

      await t.pumpWidget(panel(_req(key: 'songA')));
      await t.pump();
      // User moves on to song B while A's lyrics are still in flight.
      await t.pumpWidget(panel(_req(key: 'songB')));
      await t.pump();

      completers['songB']!
          .complete(const LyricsResult(plain: 'B B B lyrics'));
      await t.pump();
      expect(find.text('B B B lyrics'), findsOneWidget);

      // Song A's reply arrives late: it must NOT paint over song B.
      completers['songA']!
          .complete(const LyricsResult(plain: 'A A A lyrics'));
      await t.pump();
      expect(find.text('A A A lyrics'), findsNothing);
      expect(find.text('B B B lyrics'), findsOneWidget);
    });
  });
}
