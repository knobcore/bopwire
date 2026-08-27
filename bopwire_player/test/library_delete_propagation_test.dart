// Deleting a song must PROPAGATE: local library row gone, the DB2
// snapshot republished to the node (so the website / Discover / other
// players stop listing us), and — only for files bopwire itself
// downloaded — the file removed from disk.
//
// The disk-deletion provenance rule is where real damage would happen
// (wrongly deleting a user's own music is unrecoverable), so it is
// tested the hardest here:
//   downloaded file            → deleted
//   scanned user file          → NOT deleted
//   missing provenance flag    → NOT deleted (legacy/ambiguous = user's)
//   outside the download area  → NOT deleted even when flagged
//   directories                → never deleted
//   removing a scanned folder  → deletes nothing from disk
//
// The publish side runs the REAL publisher logic with only the wallet
// signer and the librats wire hop faked (LibraryPublisher.debug*
// overrides): remove() must republish, removing the LAST song must
// publish an EMPTY snapshot (not early-return), the digest gate must not
// suppress either, and publishOffline must publish empty while the local
// library still has songs.

import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'package:bopwire_player/src/services/library_publisher.dart';
import 'package:bopwire_player/src/services/library_service.dart';
import 'package:bopwire_player/src/services/local_library_actions.dart';

String _hash(int i) => i.toRadixString(16).padLeft(64, '0');

LibraryEntry _entry({
  required String hash,
  String canonical = '',
  String path = '',
  String source = '',
}) =>
    LibraryEntry(
      contentHash:     hash,
      fingerprintHash: '',
      canonicalHash:   canonical,
      title:           'title-$hash',
      artist:          'artist',
      album:           '',
      genre:           '',
      durationMs:      1000,
      audioFormat:     'mp3',
      filePath:        path,
      addedAtMs:       0,
      source:          source,
    );

Future<File> _makeFile(String path) async {
  final f = File(path);
  await f.parent.create(recursive: true);
  await f.writeAsBytes(List<int>.filled(64, 0x41));
  return f;
}

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  late Directory tmp;
  late Directory cacheDir; // stands in for the app-private downloads cache
  late Directory userDir;  // stands in for the user's OWN music folder

  setUp(() async {
    SharedPreferences.setMockInitialValues({});
    LibraryService.instance.debugResetForTests();
    LibraryPublisher.debugResetSession();
    LibraryPublisher.debugWalletOverride  = null;
    LibraryPublisher.debugRequestOverride = null;
    tmp = await Directory.systemTemp.createTemp('bopwire_delete_test_');
    cacheDir = await Directory('${tmp.path}/downloads').create();
    userDir  = await Directory('${tmp.path}/my_music').create();
    LocalLibraryActions.debugDownloadsDirOverride = cacheDir.path;
    await LibraryService.instance.ensureLoaded();
  });

  tearDown(() async {
    LocalLibraryActions.debugDownloadsDirOverride = null;
    LibraryPublisher.debugWalletOverride  = null;
    LibraryPublisher.debugRequestOverride = null;
    LibraryService.instance.debugResetForTests();
    try {
      await tmp.delete(recursive: true);
    } catch (_) {}
  });

  // ------------------------------------------------------------------
  // The provenance rule (pure decision function).
  // ------------------------------------------------------------------
  group('shouldDeleteFromDisk provenance rule', () {
    test('download flag + downloads cache location → delete', () {
      final e = _entry(
          hash: _hash(1),
          path: '${cacheDir.path}/song.mp3',
          source: LibraryEntry.kSourceDownload);
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isTrue);
    });

    test('download flag + bopwire-downloads subfolder → delete', () {
      // Soulseek/napstr downloads land in <watched folder>/bopwire-downloads/
      final e = _entry(
          hash: _hash(2),
          path: '${userDir.path}/bopwire-downloads/song.mp3',
          source: LibraryEntry.kSourceDownload);
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isTrue);
    });

    test('scanned user file (no flag) → NEVER delete', () {
      final e = _entry(hash: _hash(3), path: '${userDir.path}/precious.mp3');
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isFalse);
    });

    test('missing provenance flag → NOT deleted even inside the cache', () {
      // Legacy entry that predates the flag: ambiguous = user-owned.
      final e = _entry(hash: _hash(4), path: '${cacheDir.path}/legacy.mp3');
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isFalse);
    });

    test('flagged but OUTSIDE every download area → NOT deleted', () {
      final e = _entry(
          hash: _hash(5),
          path: '${userDir.path}/somehow_flagged.mp3',
          source: LibraryEntry.kSourceDownload);
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isFalse);
    });

    test('empty file path → NOT deleted', () {
      final e =
          _entry(hash: _hash(6), source: LibraryEntry.kSourceDownload);
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isFalse);
    });

    test('unrelated source values are not deletable', () {
      final e = _entry(
          hash: _hash(7),
          path: '${cacheDir.path}/x.mp3',
          source: 'scan'); // anything but the exact download flag
      expect(
          LocalLibraryActions.shouldDeleteFromDisk(
              entry: e, downloadsCacheDir: cacheDir.path),
          isFalse);
    });
  });

  // ------------------------------------------------------------------
  // deleteEntry against real (temp) files.
  // ------------------------------------------------------------------
  group('deleteEntry disk behavior', () {
    test('bopwire-downloaded file is removed from disk and library',
        () async {
      final f = await _makeFile('${cacheDir.path}/dl.mp3');
      final e = _entry(
          hash: _hash(10),
          path: f.path,
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isTrue);
      expect(await f.exists(), isFalse, reason: 'download must be deleted');
      expect(LibraryService.instance.entryByHash(_hash(10)), isNull);
    });

    test('user-scanned file is NEVER removed from disk', () async {
      final f = await _makeFile('${userDir.path}/mine.mp3');
      final e = _entry(hash: _hash(11), path: f.path); // no flag
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isFalse);
      expect(await f.exists(), isTrue,
          reason: 'the user\'s own file must survive a library delete');
      expect(LibraryService.instance.entryByHash(_hash(11)), isNull,
          reason: 'the library row still goes away');
    });

    test('flagged entry outside the download area keeps its file',
        () async {
      final f = await _makeFile('${userDir.path}/outside.mp3');
      final e = _entry(
          hash: _hash(12),
          path: f.path,
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isFalse,
          reason: 'provenance AND location must both agree');
      expect(await f.exists(), isTrue);
      expect(LibraryService.instance.entryByHash(_hash(12)), isNull);
    });

    test('missing file: removal still completes', () async {
      final e = _entry(
          hash: _hash(13),
          path: '${cacheDir.path}/already_gone.mp3',
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isFalse);
      expect(LibraryService.instance.entryByHash(_hash(13)), isNull);
    });

    test('a directory at the entry path is never deleted', () async {
      final d =
          await Directory('${cacheDir.path}/dir_pretending_to_be_a_file.mp3')
              .create();
      await _makeFile('${d.path}/inner.mp3'); // proves nothing recursive runs
      final e = _entry(
          hash: _hash(14),
          path: d.path,
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isFalse);
      expect(await d.exists(), isTrue, reason: 'directories are untouchable');
      expect(await File('${d.path}/inner.mp3').exists(), isTrue);
    });

    test('empty parent inside the download area is cleaned up', () async {
      final f = await _makeFile('${cacheDir.path}/bopwire-downloads/last.mp3');
      final e = _entry(
          hash: _hash(15),
          path: f.path,
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      final r = await LocalLibraryActions.instance.deleteEntry(e);
      expect(r.fileDeleted, isTrue);
      expect(await f.parent.exists(), isFalse,
          reason: 'empty bopwire-downloads dir inside the cache is removed');
      expect(await cacheDir.exists(), isTrue,
          reason: 'the cache root itself stays');
    });

    test('non-empty parent is left alone', () async {
      final f = await _makeFile('${cacheDir.path}/bopwire-downloads/a.mp3');
      final keep =
          await _makeFile('${cacheDir.path}/bopwire-downloads/keep.mp3');
      final e = _entry(
          hash: _hash(16),
          path: f.path,
          source: LibraryEntry.kSourceDownload);
      await LibraryService.instance.upsert(e);

      await LocalLibraryActions.instance.deleteEntry(e);
      expect(await keep.exists(), isTrue);
      expect(await f.parent.exists(), isTrue);
    });
  });

  test('removing a scanned FOLDER never deletes anything from disk',
      () async {
    final f1 = await _makeFile('${userDir.path}/one.mp3');
    final f2 = await _makeFile('${userDir.path}/sub/two.mp3');
    final lib = LibraryService.instance;
    await lib.addFolder(userDir.path);
    await lib.upsert(_entry(hash: _hash(20), path: f1.path));
    await lib.upsert(_entry(hash: _hash(21), path: f2.path));

    final dropped = await lib.purgeFolder(userDir.path);
    expect(dropped.length, 2);
    expect(lib.entryByHash(_hash(20)), isNull);
    expect(lib.entryByHash(_hash(21)), isNull);
    expect(lib.folders, isNot(contains(userDir.path)));
    expect(await f1.exists(), isTrue, reason: 'folder removal is index-only');
    expect(await f2.exists(), isTrue, reason: 'folder removal is index-only');
  });

  // ------------------------------------------------------------------
  // Publish propagation (real publisher logic, faked signer + wire).
  // ------------------------------------------------------------------
  group('removal republishes the DB2 snapshot', () {
    final captured = <Map<String, dynamic>>[];

    void wirePublisher() {
      captured.clear();
      LibraryPublisher.debugWalletOverride = (
        address:   'aa' * 20,
        publicKey: '02${'bb' * 32}',
        sign:      (bytes) => 'cd' * 64,
      );
      LibraryPublisher.debugRequestOverride = (verb, body) async {
        expect(verb, 'library.delta');
        captured.add(Map<String, dynamic>.of(body));
        return {'applied': true, 'unknown': const <String>[]};
      };
    }

    List<String> lastAdd() =>
        (captured.last['add'] as List).cast<String>();

    test('remove() triggers a republish carrying the smaller set',
        () async {
      wirePublisher();
      final lib = LibraryService.instance;
      await lib.upsert(_entry(hash: _hash(30)));
      await lib.upsert(_entry(hash: _hash(31)));
      await LibraryPublisher.publishFull();
      await LibraryPublisher.debugDrain();
      expect(lastAdd().toSet(), {_hash(30), _hash(31)});

      final before = captured.length;
      await lib.remove(_hash(30)); // fires publishFull itself
      await LibraryPublisher.debugDrain();
      expect(captured.length, greaterThan(before),
          reason: 'remove() must republish without any extra call');
      expect(lastAdd(), [_hash(31)],
          reason: 'the republished snapshot no longer lists the deleted song');
    });

    test('removing the LAST song publishes an EMPTY library', () async {
      wirePublisher();
      final lib = LibraryService.instance;
      await lib.upsert(_entry(hash: _hash(32)));
      await LibraryPublisher.publishFull();
      await LibraryPublisher.debugDrain();
      expect(lastAdd(), [_hash(32)]);

      final before = captured.length;
      await lib.remove(_hash(32));
      await LibraryPublisher.debugDrain();
      expect(captured.length, greaterThan(before),
          reason: 'an emptied library must still publish — no early return');
      expect(lastAdd(), isEmpty,
          reason: 'empty snapshot is what makes the node drop everything');
    });

    test('digest gate: skips a true no-change, never a change to/from empty',
        () async {
      wirePublisher();
      final lib = LibraryService.instance;
      await lib.upsert(_entry(hash: _hash(33)));
      await LibraryPublisher.publishFull();
      await LibraryPublisher.debugDrain();
      final afterFirst = captured.length;

      await LibraryPublisher.publishFull(); // identical set → gated out
      await LibraryPublisher.debugDrain();
      expect(captured.length, afterFirst,
          reason: 'unchanged library re-publish is digest-gated');

      await lib.remove(_hash(33)); // change → empty must pass the gate
      await LibraryPublisher.debugDrain();
      expect(captured.length, greaterThan(afterFirst));
      expect(lastAdd(), isEmpty);

      // ...and going back from empty to non-empty passes it too.
      await lib.upsert(_entry(hash: _hash(34)));
      await LibraryPublisher.publishFull();
      await LibraryPublisher.debugDrain();
      expect(lastAdd(), [_hash(34)]);
    });

    test('publishSetFor of an empty library is a valid empty set', () {
      expect(LibraryPublisher.publishSetFor(const []), isEmpty);
      expect(LibraryPublisher.digestGateSkips(const []), isFalse,
          reason: 'nothing published yet — empty must not read as "same"');
    });

    test('publishOffline publishes empty while the library is non-empty, '
        'and the next publishFull restores the real set', () async {
      wirePublisher();
      final lib = LibraryService.instance;
      await lib.upsert(_entry(hash: _hash(35)));
      await lib.upsert(_entry(hash: _hash(36)));

      await LibraryPublisher.publishOffline();
      expect(lastAdd(), isEmpty,
          reason: '"go offline" must tell the node we hold nothing');
      expect(lib.entries.length, 2,
          reason: 'going offline does not touch the local library');

      await LibraryPublisher.publishFull(); // reconnect path
      await LibraryPublisher.debugDrain();
      expect(lastAdd().toSet(), {_hash(35), _hash(36)},
          reason: 'reconnect republish must not be digest-suppressed');

      // Version must be STRICTLY increasing even inside one millisecond,
      // or the node's version gate would reject the newer snapshot.
      final versions =
          captured.map((b) => b['version'] as int).toList();
      for (var i = 1; i < versions.length; i++) {
        expect(versions[i], greaterThan(versions[i - 1]),
            reason: 'same-ms successor deltas must not tie on version');
      }
    });

    test('canonical hash (song identity) is what gets published', () async {
      wirePublisher();
      final lib = LibraryService.instance;
      await lib
          .upsert(_entry(hash: _hash(37), canonical: _hash(38)));
      await LibraryPublisher.publishFull();
      await LibraryPublisher.debugDrain();
      expect(lastAdd(), [_hash(38)]);

      // Removing by the LOCAL content hash still propagates: the snapshot
      // republished afterwards no longer carries the canonical id.
      await lib.remove(_hash(37));
      await LibraryPublisher.debugDrain();
      expect(lastAdd(), isEmpty);
    });
  });

  test('importDownloadedFile survivors: provenance round-trips through JSON',
      () {
    final e = _entry(
        hash: _hash(40),
        path: '${cacheDir.path}/x.mp3',
        source: LibraryEntry.kSourceDownload);
    final decoded = LibraryEntry.fromJson(e.toJson());
    expect(decoded.isDownloadedByBopwire, isTrue);
    // Pre-provenance persisted entries decode to user-owned.
    final legacy = e.toJson()..remove('source');
    expect(LibraryEntry.fromJson(legacy).isDownloadedByBopwire, isFalse);
  });
}
