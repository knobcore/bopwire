// This file used to be the stock Flutter counter-app template, asserting
// on a `MyApp` class that has never existed in this project (the app is
// `BopwireApp`) and on counter text that was never rendered. It failed to
// compile, so the suite could never go green. Replaced with real coverage
// of the foreign-network result row, which is plugin-free and therefore
// safe to pump in a unit test.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/widgets/external_result_tile.dart';

Widget _wrap(Widget child) => MaterialApp(home: Scaffold(body: child));

void main() {
  group('ExternalResultTile', () {
    testWidgets('shows artist, title and a human-readable size', (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'Soulseek',
        track: ExternalTrack(
          networkId: 'slsk',
          id: 'a',
          title: 'Bodies',
          artist: 'Drowning Pool',
          sizeBytes: 7340032, // 7 MB
          bitrate: 320,
          extension: 'mp3',
          owner: 'somepeer',
        ),
      )));

      expect(find.text('Drowning Pool — Bodies'), findsOneWidget);

      final subtitle = t.widget<Text>(find.byWidgetPredicate(
        (w) => w is Text && (w.data?.contains('Soulseek') ?? false),
      ));
      expect(subtitle.data, contains('somepeer'));
      expect(subtitle.data, contains('7 MB'));
      expect(subtitle.data, contains('320 kbps'));
      expect(subtitle.data, contains('MP3'));
    });

    testWidgets('falls back to the bare title when artist is absent', (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'napstr',
        track: ExternalTrack(
          networkId: 'napstr',
          id: 'b',
          title: 'untitled.flac',
        ),
      )));
      expect(find.text('untitled.flac'), findsOneWidget);
    });

    testWidgets('renders a folder with its child count and folder icon',
        (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'Soulseek',
        track: ExternalTrack(
          networkId: 'slsk',
          id: 'c',
          title: 'Sinner (2001)',
          isFolder: true,
          childCount: 12,
          // Size/bitrate must NOT be shown for a folder even if set.
          sizeBytes: 999,
          bitrate: 128,
        ),
      )));

      expect(find.byIcon(Icons.folder_outlined), findsOneWidget);
      final subtitle = t.widget<Text>(find.byWidgetPredicate(
        (w) => w is Text && (w.data?.contains('folder') ?? false),
      ));
      expect(subtitle.data, contains('12 files'));
      expect(subtitle.data, isNot(contains('kbps')));
    });

    testWidgets('right-click opens the download menu with a folder entry',
        (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'Soulseek',
        track: ExternalTrack(
          networkId: 'slsk',
          id: 'd',
          title: 'Bodies',
          sizeBytes: 1024,
        ),
      )));

      // The feature request asked specifically for folder download on
      // right-click, so the menu must offer it even for a plain file —
      // the containing folder is usually the album. Long-press is the
      // touch equivalent and is wired to the same handler, so exercising
      // it covers both entry points.
      await t.longPressAt(t.getCenter(find.byType(GestureDetector).first));
      await t.pumpAndSettle();

      expect(find.text('Download whole folder'), findsOneWidget);
      expect(find.text('Download this track'), findsOneWidget);
    });
  });
}
