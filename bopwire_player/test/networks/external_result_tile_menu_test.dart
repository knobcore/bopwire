// The tile's gesture contract after the pre-download cache landed.
//
// Tap is now the *cheap* action (stream into the throwaway cache); the
// committing action (download + fingerprint + chain register) moved
// behind the context menu. These tests pin that split, because getting
// it backwards would silently start real library downloads every time
// someone clicks a search result.
//
// Only the menu is exercised here. Actually tapping the row starts a
// cache download against the live PredownloadCache singleton, which
// resolves a real temp directory and arms a real sweep timer — not
// something a widget test should do. The cache's own behaviour is
// covered in predownload_cache_test.dart.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/widgets/external_result_tile.dart';

Widget _wrap(Widget child) => MaterialApp(home: Scaffold(body: child));

void main() {
  group('ExternalResultTile menu', () {
    testWidgets('offers stream and download separately for a file',
        (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'Soulseek',
        track: ExternalTrack(
          networkId: 'slsk',
          id: 'a',
          title: 'Bodies',
          artist: 'Drowning Pool',
          extension: 'mp3',
        ),
      )));

      // Long-press is the touch equivalent of right-click and is wired
      // to the same handler.
      await t.longPressAt(t.getCenter(find.byType(GestureDetector).first));
      await t.pumpAndSettle();

      expect(find.text('Stream now'), findsOneWidget);
      expect(find.text('Download this track'), findsOneWidget);
      expect(find.text('Download whole folder'), findsOneWidget);
    });

    testWidgets('a folder gets no stream entry', (t) async {
      await t.pumpWidget(_wrap(const ExternalResultTile(
        networkLabel: 'Soulseek',
        track: ExternalTrack(
          networkId: 'slsk',
          id: 'b',
          title: 'Sinner (2001)',
          isFolder: true,
          childCount: 12,
        ),
      )));

      // Long-press is the touch equivalent of right-click and is wired
      // to the same handler.
      await t.longPressAt(t.getCenter(find.byType(GestureDetector).first));
      await t.pumpAndSettle();

      expect(find.text('Stream now'), findsNothing);
      expect(find.text('Download this track'), findsNothing);
      expect(find.text('Download whole folder'), findsOneWidget);
    });
  });

  group('ExternalResultTile.extensionOf', () {
    test('prefers the reported extension, falls back to the remote path', () {
      expect(
          ExternalResultTile.extensionOf(const ExternalTrack(
              networkId: 'slsk', id: '1', title: 'x', extension: 'FLAC')),
          'flac');
      expect(
          ExternalResultTile.extensionOf(const ExternalTrack(
              networkId: 'slsk',
              id: '2',
              title: 'x',
              remotePath: r'shared\music\track.mp3')),
          'mp3');
      expect(
          ExternalResultTile.extensionOf(const ExternalTrack(
              networkId: 'slsk', id: '3', title: 'no extension here')),
          isNull);
    });
  });
}
