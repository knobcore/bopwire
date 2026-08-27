// Helpers that bundle "remove from local library" with all the side
// effects: republishing the DB2 snapshot so the mesh (website / Discover /
// other players) stops listing us as a holder, cancelling any in-flight
// upload of the song, and deleting the on-disk file — but ONLY when the
// file is provably one bopwire itself downloaded.
//
// ── Disk-deletion safety contract ────────────────────────────────────
// The library holds two very different kinds of entry:
//   • files bopwire DOWNLOADED (Soulseek / napstr / the bopwire swarm)
//     into its own download area — ours to delete;
//   • files the USER scanned from their own music folders — deleting one
//     of those destroys an unrecoverable personal collection.
// A file is deleted from disk only when BOTH of these agree:
//   1. PROVENANCE: the entry was flagged `source == 'download'` at import
//     time (importDownloadedFile / the swarm download path). An entry
//     without the flag — including every entry that predates the flag —
//     is treated as user-owned and never deleted.
//   2. LOCATION: the file actually resides inside a bopwire download
//     area (the app-private downloads cache, or a `bopwire-downloads/`
//     directory bopwire itself created for network downloads).
// We only ever delete the specific FILE (never a directory tree); an
// empty parent directory is cleaned up only when it is itself inside a
// bopwire download area and not a registered library folder. Every
// ambiguous case resolves to "do not delete".

import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import 'library_scanner.dart';
import 'library_service.dart';

class DeleteResult {
  const DeleteResult({required this.fileDeleted});
  final bool fileDeleted;
}

class LocalLibraryActions {
  LocalLibraryActions._();
  static final LocalLibraryActions instance = LocalLibraryActions._();

  /// Test-only: bypass path_provider (no platform channel in unit tests)
  /// and treat this directory as the app-private downloads cache.
  @visibleForTesting
  static String? debugDownloadsDirOverride;

  Future<Directory> _downloadsDir() async {
    final o = debugDownloadsDirOverride;
    if (o != null) return Directory(o);
    final base = await getApplicationDocumentsDirectory();
    final dir  = Directory('${base.path}/downloads');
    if (!await dir.exists()) await dir.create(recursive: true);
    return dir;
  }

  /// The app-private swarm download cache path (where
  /// NodeClient.downloadToLibrary writes). Exposed so import sites can
  /// combine it with [isBopwireDownloadPath] when flagging provenance.
  Future<String> downloadsCachePath() async => (await _downloadsDir()).path;

  /// True when [path] lies inside one of bopwire's OWN download areas:
  ///   • [downloadsCacheDir] — the app-private swarm download cache
  ///     (NodeClient.downloadToLibrary writes here), or
  ///   • any `bopwire-downloads/` directory — the subfolder
  ///     NetworkDownloadManager creates inside a watched folder (or the
  ///     app documents dir) for Soulseek/napstr downloads.
  /// Location is necessary but NOT sufficient for deletion — the entry's
  /// recorded provenance must agree (see [shouldDeleteFromDisk]).
  static bool isBopwireDownloadPath(String path, String downloadsCacheDir) {
    if (path.isEmpty) return false;
    final p = _normalize(path);
    if (downloadsCacheDir.isNotEmpty) {
      var cache = _normalize(downloadsCacheDir);
      if (!cache.endsWith('/')) cache = '$cache/';
      if (p.startsWith(cache)) return true;
    }
    return p.contains('/bopwire-downloads/');
  }

  /// The single decision point for on-disk deletion. Pure so the rule is
  /// unit-testable in isolation. Provenance AND location must both agree;
  /// everything else — missing flag, legacy entry, user-scanned file,
  /// flagged entry that somehow sits outside a download area — is NO.
  @visibleForTesting
  static bool shouldDeleteFromDisk({
    required LibraryEntry entry,
    required String downloadsCacheDir,
  }) {
    if (!entry.isDownloadedByBopwire) return false;     // provenance gate
    if (entry.filePath.isEmpty) return false;
    return isBopwireDownloadPath(entry.filePath, downloadsCacheDir);
  }

  /// Returns whether the on-disk file was actually deleted (true only for
  /// a file bopwire itself downloaded, per [shouldDeleteFromDisk]). The
  /// library row is always removed regardless of the disk outcome —
  /// LibraryService.remove() then republishes the DB2 snapshot (so the
  /// removal propagates to the node → website/Discover/other players) and
  /// fires onEntryRemoved (so PlayerServer cancels any in-flight upload
  /// of the song). The disk-side outcome is the only thing the caller has
  /// to surface to the user.
  Future<DeleteResult> deleteEntry(LibraryEntry e) async {
    bool fileDeleted = false;
    try {
      final dlDir = await _downloadsDir();
      if (shouldDeleteFromDisk(entry: e, downloadsCacheDir: dlDir.path)) {
        fileDeleted = await _deleteFileBestEffort(e.filePath, dlDir.path);
      }
    } catch (err) {
      // A failed/impossible disk delete must never block the library
      // removal — log and carry on.
      // ignore: avoid_print
      print('[library] disk delete skipped for ${e.filePath}: $err');
    }

    // Remove the row FIRST: this drops the hash from every lookup map (so
    // stream.open / swarm.fetch immediately answer not_held), cancels
    // in-flight uploads via onEntryRemoved, and republishes the snapshot.
    await LibraryService.instance.remove(e.contentHash);

    // Best-effort legacy deannounce for BOTH our local content_hash AND
    // the canonical (when they differ). Redundant with the republish
    // remove() just triggered — publishFull is digest-gated and
    // idempotent — but cheap and self-healing for any legacy rows.
    final hashes = <String>{e.contentHash};
    if (e.canonicalHash.isNotEmpty) hashes.add(e.canonicalHash);
    unawaited(LibraryScanner.instance.deannounce(hashes.toList()));

    return DeleteResult(fileDeleted: fileDeleted);
  }

  /// Delete exactly one FILE (never a directory), then clean up its parent
  /// directory only when the parent is empty, inside a bopwire download
  /// area, and not a registered library folder. Returns true only when
  /// the file itself was removed. Never throws.
  Future<bool> _deleteFileBestEffort(String path, String cacheDir) async {
    var deleted = false;
    try {
      final f = File(path);
      // File.exists() is type-aware: false for a directory at this path,
      // so we can never unlink a directory tree by mistake.
      if (await f.exists()) {
        await f.delete();
        deleted = true;
      } else {
        // ignore: avoid_print
        print('[library] file already missing on delete: $path');
      }
    } catch (err) {
      // ignore: avoid_print
      print('[library] failed to delete file $path: $err');
      return false;
    }
    if (!deleted) return false;

    // Empty-parent cleanup — strictly confined to bopwire's own areas.
    try {
      final parent = File(path).parent;
      final parentPath = _normalize(parent.path);
      final registered = LibraryService.instance.folders
          .map(_normalize)
          .toSet();
      final insideDownloadArea =
          isBopwireDownloadPath('$parentPath/x', cacheDir);
      if (insideDownloadArea &&
          !registered.contains(parentPath) &&
          parentPath != _normalize(cacheDir) &&
          await parent.exists() &&
          await parent.list().isEmpty) {
        await parent.delete(); // non-recursive: only an EMPTY dir can go
      }
    } catch (_) {/* cleanup is cosmetic — never let it fail the delete */}
    return deleted;
  }

  static String _normalize(String p) {
    final s = p.replaceAll('\\', '/');
    if (s.length >= 2 && s[1] == ':') {
      return s[0].toLowerCase() + s.substring(1);
    }
    return s;
  }
}
