// Coverage for the foreign-result filter: the pure ExternalResultFilter
// logic (type derivation, denylist toggles, folders-only, bitrate floor,
// free text) and the chip bar built from what is actually in the results.

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/external_network.dart';
import 'package:bopwire_player/src/widgets/external_result_filters.dart';

ExternalTrack _file(
  String id, {
  String? ext,
  String? path,
  int? bitrate,
  String? artist,
  String title = 'song',
  String? owner,
}) =>
    ExternalTrack(
      networkId: 'slsk',
      id: id,
      title: title,
      artist: artist,
      owner: owner,
      remotePath: path,
      extension: ext,
      bitrate: bitrate,
    );

ExternalTrack _folder(String id, {String title = 'Album (2001)'}) =>
    ExternalTrack(
      networkId: 'slsk',
      id: id,
      title: title,
      isFolder: true,
      childCount: 10,
    );

void main() {
  group('ExternalResultFilter.typeOf', () {
    test('uses the reported extension, lowercased', () {
      expect(ExternalResultFilter.typeOf(_file('a', ext: 'FLAC')), 'flac');
    });

    test('derives from the remote path when extension is null', () {
      expect(
        ExternalResultFilter.typeOf(
            _file('a', path: r'music\artist\01 - track.Mp3')),
        'mp3',
      );
    });

    test('falls back to the title, then to "other"', () {
      expect(ExternalResultFilter.typeOf(_file('a', title: 'x.ogg')), 'ogg');
      expect(ExternalResultFilter.typeOf(_file('a', title: 'no dot here')),
          ExternalResultFilter.unknownType);
    });

    test('folders get the folder pseudo-type', () {
      expect(ExternalResultFilter.typeOf(_folder('f')),
          ExternalResultFilter.folderType);
    });
  });

  group('ExternalResultFilter.apply', () {
    final tracks = [
      _file('1', ext: 'mp3', bitrate: 320, artist: 'Drowning Pool',
          title: 'Bodies'),
      _file('2', ext: 'mp3', bitrate: 128, artist: 'Drowning Pool',
          title: 'Sinner'),
      _file('3', ext: 'flac', title: 'Bodies'), // lossless, no bitrate
      _file('4', ext: 'ogg', bitrate: 192, title: 'Bodies (live)'),
      _file('5', title: 'mystery'), // unknown type, unknown bitrate
      _folder('6'),
    ];

    test('empty filter passes everything and is not active', () {
      expect(ExternalResultFilter.empty.isActive, isFalse);
      expect(ExternalResultFilter.empty.apply(tracks), hasLength(6));
    });

    test('disabling a type hides exactly that type', () {
      final f = ExternalResultFilter.empty.toggleType('mp3');
      final out = f.apply(tracks);
      expect(out.map((t) => t.id), ['3', '4', '5', '6']);
      // Toggling again brings it back.
      expect(f.toggleType('mp3').apply(tracks), hasLength(6));
    });

    test('disabling the folder pseudo-type hides folders', () {
      final f = ExternalResultFilter.empty
          .toggleType(ExternalResultFilter.folderType);
      expect(f.apply(tracks).any((t) => t.isFolder), isFalse);
    });

    test('foldersOnly keeps only folders, and beats a disabled folder chip',
        () {
      final f = ExternalResultFilter.empty
          .toggleType(ExternalResultFilter.folderType)
          .copyWith(foldersOnly: true);
      expect(f.apply(tracks).map((t) => t.id), ['6']);
    });

    test(
        'bitrate floor drops lossy below it and unknown-bitrate lossy, '
        'but never lossless or folders', () {
      const f = ExternalResultFilter(minBitrate: 256);
      final out = f.apply(tracks);
      // 320 mp3 stays; 128 mp3 and 192 ogg go; flac (no bitrate) stays
      // because it is lossless; unknown-type unknown-bitrate goes;
      // folder stays.
      expect(out.map((t) => t.id), ['1', '3', '6']);
    });

    test('free text matches artist, title, path and owner, case-insensitive',
        () {
      expect(
        const ExternalResultFilter(text: 'drowning')
            .apply(tracks)
            .map((t) => t.id),
        ['1', '2'],
      );
      final withPath = [
        _file('p', path: r'shares\Nirvana\smells.mp3'),
        _file('o', owner: 'NirvanaFan99'),
      ];
      expect(
        const ExternalResultFilter(text: 'nirvana')
            .apply(withPath)
            .map((t) => t.id),
        ['p', 'o'],
      );
    });

    test('disabled types that later vanish from results are harmless', () {
      // Progressive arrival: the user turned WAV off while a WAV was
      // visible; a later state with no WAVs must not be affected.
      final f = ExternalResultFilter.empty.toggleType('wav');
      expect(f.apply(tracks), hasLength(6));
    });
  });

  group('ExternalResultFilterBar', () {
    Widget wrap(Widget child) => MaterialApp(
        home: Scaffold(body: SizedBox(width: 800, child: child)));

    testWidgets(
        'builds chips only for types present — no dead WAV chip, '
        'counts shown', (t) async {
      final results = [
        _file('1', ext: 'mp3', bitrate: 320),
        _file('2', ext: 'mp3', bitrate: 128),
        _file('3', ext: 'flac'),
        _folder('4'),
      ];
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: results,
        hiddenCount: 0,
        onChanged: (_) {},
      )));

      expect(find.text('MP3 · 2'), findsOneWidget);
      expect(find.text('FLAC · 1'), findsOneWidget);
      expect(find.text('Folders · 1'), findsOneWidget);
      expect(find.text('Only folders'), findsOneWidget);
      expect(find.textContaining('WAV'), findsNothing);
      // No hidden results, no active filter: no count line, no reset.
      expect(find.textContaining('hidden by filters'), findsNothing);
      expect(find.text('Reset'), findsNothing);
    });

    testWidgets('tapping a type chip toggles it into the denylist',
        (t) async {
      ExternalResultFilter? emitted;
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: [_file('1', ext: 'mp3'), _file('2', ext: 'flac')],
        hiddenCount: 0,
        onChanged: (f) => emitted = f,
      )));

      await t.tap(find.text('MP3 · 1'));
      expect(emitted, isNotNull);
      expect(emitted!.disabledTypes, contains('mp3'));
      expect(emitted!.disabledTypes, isNot(contains('flac')));
    });

    testWidgets('Only folders chip flips foldersOnly', (t) async {
      ExternalResultFilter? emitted;
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: [_file('1', ext: 'mp3'), _folder('2')],
        hiddenCount: 0,
        onChanged: (f) => emitted = f,
      )));

      await t.tap(find.text('Only folders'));
      expect(emitted?.foldersOnly, isTrue);
    });

    testWidgets('bitrate menu appears only when a bitrate is known, '
        'and selecting an option emits the floor', (t) async {
      // No bitrate anywhere: no chip.
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: [_file('1', ext: 'flac')],
        hiddenCount: 0,
        onChanged: (_) {},
      )));
      expect(find.text('Bitrate'), findsNothing);

      ExternalResultFilter? emitted;
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: [_file('1', ext: 'mp3', bitrate: 320)],
        hiddenCount: 0,
        onChanged: (f) => emitted = f,
      )));
      await t.tap(find.text('Bitrate'));
      await t.pumpAndSettle();
      await t.tap(find.text('≥ 320 kbps'));
      await t.pumpAndSettle();
      expect(emitted?.minBitrate, 320);
    });

    testWidgets('typing in the field emits a text filter', (t) async {
      ExternalResultFilter? emitted;
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: ExternalResultFilter.empty,
        results: [_file('1', ext: 'mp3')],
        hiddenCount: 0,
        onChanged: (f) => emitted = f,
      )));

      await t.enterText(find.byType(TextField), '  live  ');
      expect(emitted?.text, 'live');
    });

    testWidgets(
        'active filter shows the hidden count and a Reset chip that '
        'clears everything', (t) async {
      ExternalResultFilter? emitted;
      final active = ExternalResultFilter.empty
          .toggleType('mp3')
          .copyWith(text: 'x', minBitrate: 192);
      await t.pumpWidget(wrap(ExternalResultFilterBar(
        filter: active,
        results: [_file('1', ext: 'mp3', bitrate: 320)],
        hiddenCount: 12,
        onChanged: (f) => emitted = f,
      )));

      expect(find.text('12 hidden by filters'), findsOneWidget);
      await t.tap(find.text('Reset'));
      expect(emitted, isNotNull);
      expect(emitted!.isActive, isFalse);
    });

    testWidgets(
        'a new batch growing the chip set keeps existing selections '
        '(denylist survives rebuild)', (t) async {
      var filter = ExternalResultFilter.empty.toggleType('mp3');
      final firstBatch = [_file('1', ext: 'mp3'), _file('2', ext: 'flac')];
      final secondBatch = [
        ...firstBatch,
        _file('3', ext: 'ogg'), // arrives seconds later
      ];

      Widget build(List<ExternalTrack> results) =>
          wrap(ExternalResultFilterBar(
            filter: filter,
            results: results,
            hiddenCount: filter.apply(results).length == results.length
                ? 0
                : results.length - filter.apply(results).length,
            onChanged: (f) => filter = f,
          ));

      await t.pumpWidget(build(firstBatch));
      await t.pumpWidget(build(secondBatch));

      // The new OGG chip appeared, defaulting to on…
      expect(find.text('OGG · 1'), findsOneWidget);
      // …and MP3 is still deselected.
      final mp3Chip = t.widget<FilterChip>(find.ancestor(
        of: find.text('MP3 · 1'),
        matching: find.byType(FilterChip),
      ));
      expect(mp3Chip.selected, isFalse);
      expect(filter.disabledTypes, contains('mp3'));
      // And the new type is visible through the filter.
      expect(filter.allows(secondBatch[2]), isTrue);
    });
  });
}
