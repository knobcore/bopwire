// Downloads from foreign networks, then folds the results into the local
// library exactly as if the user had dropped the files in a watched
// folder themselves.
//
// The "automatically" in the feature request lives here: when a transfer
// finishes, the file is handed to LibraryScanner.importDownloadedFile,
// which runs the same fingerprint + ID3 pipeline scanOnce uses per file
// and republishes the library snapshot. So a track pulled off Soulseek
// or napstr ends up registered on the bopwire chain and served back to
// the swarm without the user doing anything else.

import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import '../library_scanner.dart';
import '../library_service.dart';
import 'external_network.dart';
import 'network_registry.dart';
import 'predownload_cache.dart';

enum JobState { queued, running, importing, done, failed, cancelled }

class DownloadJob {
  DownloadJob({
    required this.track,
    required this.networkId,
  });

  final ExternalTrack track;
  final String networkId;

  JobState state = JobState.queued;
  int received = 0;
  int? total;
  String? localPath;
  String? error;

  /// True once the file is on disk AND the library import ran.
  bool get imported => state == JobState.done;

  double? get fraction {
    final t = total;
    if (t == null || t <= 0) return null;
    return (received / t).clamp(0.0, 1.0);
  }

  StreamSubscription<DownloadProgress>? _sub;
}

class NetworkDownloadManager extends ChangeNotifier {
  NetworkDownloadManager._();
  static final NetworkDownloadManager instance = NetworkDownloadManager._();

  final List<DownloadJob> _jobs = [];
  List<DownloadJob> get jobs => List.unmodifiable(_jobs);

  int get activeCount => _jobs
      .where((j) => j.state == JobState.running || j.state == JobState.queued)
      .length;

  /// Where foreign downloads land.
  ///
  /// Prefers a folder the library already watches, so imports are also
  /// picked up by an ordinary rescan. If the user has no folders yet we
  /// create one under the app documents dir and register it, otherwise
  /// downloaded files would be fingerprinted but live outside every
  /// watched path and vanish from the library on the next full scan.
  Future<Directory> _destinationRoot() async {
    final lib = LibraryService.instance;
    await lib.ensureLoaded();

    for (final f in lib.folders) {
      final d = Directory(f);
      if (await d.exists()) {
        final sub = Directory('${d.path}${Platform.pathSeparator}bopwire-downloads');
        if (!await sub.exists()) await sub.create(recursive: true);
        return sub;
      }
    }

    final docs = await getApplicationDocumentsDirectory();
    final fallback =
        Directory('${docs.path}${Platform.pathSeparator}bopwire-downloads');
    if (!await fallback.exists()) await fallback.create(recursive: true);
    await lib.addFolder(fallback.path);
    return fallback;
  }

  /// Queue one track. Returns the job so a caller can watch it.
  Future<DownloadJob> downloadTrack(ExternalTrack track) async {
    final job = DownloadJob(track: track, networkId: track.networkId);
    _jobs.insert(0, job);
    notifyListeners();
    unawaited(_run(job));
    return job;
  }

  /// Expand a folder result and queue every file inside it.
  ///
  /// Returns the queued jobs. An empty list means the network reported
  /// no downloadable children, which callers should surface rather than
  /// treat as success.
  Future<List<DownloadJob>> downloadFolder(ExternalTrack folder) async {
    final net = NetworkRegistry.instance.byId(folder.networkId);
    if (net == null) return const [];

    List<ExternalTrack> children;
    try {
      children = await net.listFolder(folder);
    } catch (e) {
      final job = DownloadJob(track: folder, networkId: folder.networkId)
        ..state = JobState.failed
        ..error = 'Could not list folder: $e';
      _jobs.insert(0, job);
      notifyListeners();
      return const [];
    }

    final out = <DownloadJob>[];
    for (final child in children) {
      if (child.isFolder) continue; // one level; no recursive expansion
      out.add(await downloadTrack(child));
    }
    return out;
  }

  Future<void> _run(DownloadJob job) async {
    final net = NetworkRegistry.instance.byId(job.networkId);
    if (net == null) {
      job
        ..state = JobState.failed
        ..error = 'Network ${job.networkId} is not registered.';
      notifyListeners();
      return;
    }

    Directory dest;
    try {
      dest = await _destinationRoot();
    } catch (e) {
      job
        ..state = JobState.failed
        ..error = 'No writable download folder: $e';
      notifyListeners();
      return;
    }

    job.state = JobState.running;
    notifyListeners();

    // Pre-download cache promotion.
    //
    // Tapping a search result "streams" the track, which really means it
    // was downloaded into the throwaway cache. If the user then decides
    // to keep it, the bytes are already on this machine — pulling them a
    // second time over Soulseek/napstr would be slower, ruder to the
    // peer, and on Soulseek would queue us behind our own transfer. So a
    // real download of a cached track is a local copy.
    //
    // promoteInto() also rides an in-flight cache transfer rather than
    // racing it, which is why this awaits before touching the network.
    try {
      final promoted = await PredownloadCache.instance.promoteInto(
        job.track,
        dest.path,
        onProgress: (received, total) {
          job
            ..received = received
            ..total = total ?? job.total;
          notifyListeners();
        },
        isCancelled: () => job.state == JobState.cancelled,
      );
      if (job.state == JobState.cancelled) return;
      if (promoted != null) {
        job
          ..localPath = promoted
          ..received = await File(promoted).length()
          ..total = job.total ?? job.received;
        notifyListeners();
        await _import(job);
        return;
      }
      // No promotable cache entry. Rather than issue an INDEPENDENT
      // network request here, run the transfer through the cache and
      // promote the result.
      //
      // This is deliberate, and it comes from an observed asymmetry:
      // previewing a napstr track downloads, buffers and plays, while a
      // library download of the SAME track from the SAME seeder reported
      // "no seeder answered within 60 seconds". Both call
      // ExternalNetwork.download(), so rather than maintain two
      // independently-reachable paths — one of which demonstrably works
      // and one of which demonstrably doesn't — downloads now reuse the
      // proven one. It also means a preview already in flight is joined
      // instead of raced, and the bytes are only fetched once.
      final entry = await PredownloadCache.instance.prefetch(job.track);
      job
        ..received = entry.received
        ..total = entry.total ?? job.total;
      notifyListeners();

      final ticker = Timer.periodic(const Duration(milliseconds: 250), (_) {
        if (job.state == JobState.cancelled) return;
        job
          ..received = entry.received
          ..total = entry.total ?? job.total;
        notifyListeners();
      });
      try {
        await entry.finished;
      } finally {
        ticker.cancel();
      }
      if (job.state == JobState.cancelled) return;

      final viaCache = await PredownloadCache.instance.promoteInto(
        job.track,
        dest.path,
        isCancelled: () => job.state == JobState.cancelled,
      );
      if (viaCache != null) {
        job
          ..localPath = viaCache
          ..received = await File(viaCache).length()
          ..total = job.total ?? job.received;
        notifyListeners();
        await _import(job);
        return;
      }
      // The cache route is the ONLY route for a track it knows about.
      //
      // Falling through to a direct network download here was actively
      // harmful, and it is what produced "no seeder answered": these
      // networks serve one transfer per peer at a time, so a second
      // concurrent request for the same file is not a retry — it is us
      // competing with ourselves. Measured against a real napstr seeder:
      // request A downloaded 16 MB happily while request B, three
      // seconds behind it, sat through the full 60s offer window and got
      // nothing. On Soulseek the same thing queues us behind our own
      // transfer. The cache dedupes by (network, track), so going
      // through it means exactly one transfer exists per track no matter
      // how many times the user taps.
      //
      // A genuine transfer failure is reported as-is; retrying is the
      // user's call, once the peer is actually free.
      job
        ..state = JobState.failed
        ..error = entry.error ??
            'The transfer did not complete. This network serves one '
            'download per peer at a time — if you were previewing this '
            'track, let that finish first.';
      notifyListeners();
      return;
    } catch (e) {
      // A broken cache must never block a real download — fall through
      // to the network path.
      debugPrint('[predownload] promote failed, downloading instead: $e');
    }

    final completer = Completer<void>();
    job._sub = net.download(job.track, dest.path).listen(
      (p) {
        job
          ..received = p.receivedBytes
          ..total = p.totalBytes ?? job.total;

        if (p.error != null) {
          job
            ..state = JobState.failed
            ..error = p.error;
          notifyListeners();
          if (!completer.isCompleted) completer.complete();
          return;
        }
        if (p.done) {
          job.localPath = p.localPath;
          notifyListeners();
          if (!completer.isCompleted) completer.complete();
          return;
        }
        notifyListeners();
      },
      onError: (Object e) {
        job
          ..state = JobState.failed
          ..error = '$e';
        notifyListeners();
        if (!completer.isCompleted) completer.complete();
      },
      onDone: () {
        if (!completer.isCompleted) completer.complete();
      },
      cancelOnError: true,
    );

    await completer.future;
    await job._sub?.cancel();
    job._sub = null;

    if (job.state == JobState.failed || job.state == JobState.cancelled) return;

    final path = job.localPath;
    if (path == null) {
      job
        ..state = JobState.failed
        ..error = 'Transfer ended without producing a file.';
      notifyListeners();
      return;
    }

    await _import(job);
  }

  /// The automatic part: fingerprint + tag-import, same pipeline as a
  /// local scan, so the track is chain-registered and re-servable.
  ///
  /// Shared by the network path and the cache-promotion path — a
  /// promoted file must be indistinguishable from a freshly downloaded
  /// one once it is in the library folder.
  Future<void> _import(DownloadJob job) async {
    final path = job.localPath;
    if (path == null) return;
    job.state = JobState.importing;
    notifyListeners();
    try {
      final ok = await LibraryScanner.instance.importDownloadedFile(path);
      if (ok) {
        job.state = JobState.done;
      } else {
        job
          ..state = JobState.failed
          ..error = 'Downloaded, but could not be imported (file missing '
              'or not a supported audio format). The file is at $path.';
      }
    } catch (e) {
      job
        ..state = JobState.failed
        ..error = 'Downloaded to $path but import failed: $e';
    }
    notifyListeners();
  }

  Future<void> cancel(DownloadJob job) async {
    if (job.state != JobState.running && job.state != JobState.queued) return;
    job.state = JobState.cancelled;
    await job._sub?.cancel();
    job._sub = null;
    notifyListeners();
  }

  void clearFinished() {
    _jobs.removeWhere((j) =>
        j.state == JobState.done ||
        j.state == JobState.failed ||
        j.state == JobState.cancelled);
    notifyListeners();
  }
}
