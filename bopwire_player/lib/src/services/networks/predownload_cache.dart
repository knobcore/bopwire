// Pre-download cache: "stream" a Soulseek / napstr result by starting a
// throwaway download and playing the file while it is still growing.
//
// Why this exists
// ---------------
// Neither Soulseek nor napstr is a streaming protocol. Both hand you a
// file, byte 0 to byte N, over a single connection. The only way to
// "stream" from them is to start the transfer, point the audio player at
// the partial file, and keep feeding it as bytes land. That is exactly
// what this does:
//
//   1. Tap a result  -> a download starts into a *cache* directory that
//      is deliberately outside every watched library folder, so the
//      LibraryScanner never sees a half-written file.
//   2. As soon as a small prefix exists, a loopback HTTP URL backed by
//      the growing file is handed to media_kit.
//   3. Entries expire one hour after their last use and are swept.
//   4. If the user later does a REAL download of the same track,
//      NetworkDownloadManager asks us to promote the cached bytes: a
//      local copy into the library folder instead of a second transfer.
//
// Why an HTTP loopback instead of just handing libmpv the file path
// -----------------------------------------------------------------
// libmpv treats a local file as a fixed-size seekable stream: it sizes
// the file at open() and stops at that EOF. Give it a half-written file
// and it plays the prefix and calls the track finished; it does not
// follow a growing file the way `tail -f` does. An HTTP response, on the
// other hand, is *not* finished until the server closes it — so the
// handler below simply blocks when it reaches the current end of the
// partial file and resumes when more bytes arrive. That is the standard
// progressive-download shape every browser uses, and it is the only
// mechanism here that makes partial playback actually work.
//
// Honest limits, stated where they bite
// -------------------------------------
//  * Formats whose decoder needs data from the END of the file before it
//    can decode the beginning — MP4/M4A/AAC-in-MP4 with the moov atom
//    written last, which is the common case for files produced by
//    non-streaming muxers — cannot be played partially. mpv will request
//    the tail, the handler will block until the download reaches it, and
//    playback effectively starts when the transfer finishes. We do NOT
//    pretend otherwise: [isProgressiveFriendly] reports which extensions
//    can genuinely start early (mp3/flac/ogg/opus/wav/aiff/wv and raw
//    ADTS .aac), and for everything else we wait for completion rather
//    than opening a source that would stutter or fail to probe.
//  * MP3/AAC without a Xing/Info header have no reliable duration until
//    the file is complete, so the seek bar may show a wrong or unknown
//    length while streaming. That is cosmetic.
//  * The underlying network implementations write to a temporary
//    `.part`/`.slskpart` file and rename it into place at the end. The
//    reader below therefore re-resolves the file by *directory listing*
//    on every read and holds no long-lived handle, so the rename cannot
//    be blocked for more than one read's duration. On POSIX this is
//    airtight (rename keeps the inode). On Windows a rename can still
//    lose a race with an open read handle; the window is one read call
//    wide and would surface as a download error, not corruption.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:crypto/crypto.dart' as crypto;
import 'package:flutter/foundation.dart';
import 'package:path_provider/path_provider.dart';

import '../library_service.dart';
import 'external_network.dart';
import 'network_registry.dart';

/// Lifecycle of one cached track.
enum CacheState {
  /// Transfer running; bytes are landing in [CacheEntry.dir].
  downloading,

  /// The file is complete and [CacheEntry.path] is final.
  ready,

  /// Transfer failed; [CacheEntry.error] says why.
  failed,

  /// Preview moved on to another track. The partial bytes are KEPT and
  /// this entry is still a valid cache hit — re-previewing resumes it,
  /// and a real download can still promote what's there. Distinct from
  /// [cancelled], which means the user threw the bytes away.
  paused,

  /// Cancelled by the user (or superseded).
  cancelled,

  /// Swept / deleted. Terminal.
  evicted,
}

/// One track held in the pre-download cache.
class CacheEntry {
  CacheEntry({
    required this.key,
    required this.track,
    required this.dir,
    required this.createdAtMs,
  }) : lastUsedAtMs = createdAtMs;

  /// `<networkId>\0<track.id>` — stable across searches.
  final String key;
  final ExternalTrack track;

  /// Private directory holding exactly this track's file (plus whatever
  /// `.part` name the network implementation chose). Owning a whole
  /// directory is what lets us find the growing file without knowing
  /// each protocol's temp-file naming.
  final Directory dir;

  final int createdAtMs;

  /// Bumped on every play / promote so an entry the user is actually
  /// using is not swept out from under them at the 60-minute mark.
  int lastUsedAtMs;

  CacheState state = CacheState.downloading;
  int received = 0;
  int? total;
  String? error;

  /// Final path, set when the transfer completes.
  String? path;

  /// True while this entry is being copied into the library. The sweeper
  /// must not delete a file mid-copy.
  bool promoting = false;

  StreamSubscription<DownloadProgress>? _sub;

  /// Completes when the transfer reaches a terminal state.
  final Completer<void> _finished = Completer<void>();

  /// Resolves once at least [PredownloadCache.minPlayableBytes] are on
  /// disk (or the transfer ended first).
  final Completer<void> _hasPrefix = Completer<void>();

  Future<void> get finished => _finished.future;

  bool get isTerminal =>
      state == CacheState.ready ||
      state == CacheState.failed ||
      state == CacheState.cancelled ||
      state == CacheState.evicted;

  double? get fraction {
    final t = total;
    if (t == null || t <= 0) return null;
    return (received / t).clamp(0.0, 1.0);
  }

  void _complete() {
    if (!_finished.isCompleted) _finished.complete();
    if (!_hasPrefix.isCompleted) _hasPrefix.complete();
  }
}

/// Extensions whose first bytes are enough to start decoding, so playback
/// can begin long before the transfer finishes.
///
/// Deliberately conservative. Everything not listed here falls back to
/// "wait for the download to finish, then play" — see the file header.
const Set<String> _progressiveExtensions = {
  'mp3', 'flac', 'ogg', 'oga', 'opus', 'wav', 'aiff', 'aif', 'wv', 'aac',
  'mpc', 'ape',
};

/// Extensions we know are usually NOT progressively playable because the
/// index lives at the end of the container. Listed so the reason can be
/// reported to the user instead of silently behaving differently.
const Set<String> _tailIndexedExtensions = {'m4a', 'mp4', 'm4b', 'alac'};

/// True when a file with [extension] (bare, no dot) can start decoding
/// from a leading prefix.
bool isProgressiveFriendly(String? extension) {
  if (extension == null || extension.isEmpty) return false;
  return _progressiveExtensions.contains(extension.toLowerCase());
}

/// True when the container's seek index is normally written last, so a
/// partial file is undecodable and playback must wait for completion.
bool isTailIndexed(String? extension) {
  if (extension == null || extension.isEmpty) return false;
  return _tailIndexedExtensions.contains(extension.toLowerCase());
}

// ---------------------------------------------------------------------
// Growing-file HTTP source
// ---------------------------------------------------------------------

/// Everything the HTTP handler needs to serve one growing file.
class GrowingFileSource {
  GrowingFileSource({
    required this.resolve,
    required this.totalBytes,
    required this.isComplete,
    required this.isAborted,
  });

  /// Returns the file currently holding the bytes, or null if it does
  /// not exist yet. Called before every read, so a `.part` -> final
  /// rename is picked up transparently.
  final Future<File?> Function() resolve;

  /// Final size when the protocol advertised one, else null (the
  /// response then uses chunked encoding and mpv has no duration until
  /// it has decoded far enough to guess).
  final int? Function() totalBytes;

  /// True once no further bytes will ever be appended.
  final bool Function() isComplete;

  /// True when the transfer died — the handler closes the response
  /// early rather than blocking forever.
  final bool Function() isAborted;
}

/// Loopback HTTP server that serves a file *while it is being written*.
///
/// Reaching the current end of the file is not EOF: the handler waits
/// for the writer to append, which is what keeps libmpv from declaring
/// the track finished at whatever byte count happened to exist when it
/// opened the URL.
class GrowingFileServer {
  GrowingFileServer._();
  static final GrowingFileServer instance = GrowingFileServer._();

  /// Visible for tests: an isolated server that does not touch the
  /// singleton's state.
  @visibleForTesting
  GrowingFileServer.forTesting();

  HttpServer? _server;
  int _port = 0;
  int _nextTicket = 1;
  final Map<String, GrowingFileSource> _sources = {};

  /// How long the handler will sit at the end of the file with no new
  /// bytes before deciding the writer is dead. Generous: Soulseek
  /// queues routinely stall for a minute.
  static const Duration stallTimeout = Duration(seconds: 90);

  /// Poll interval while waiting for the writer to append.
  static const Duration pollInterval = Duration(milliseconds: 60);

  static const int _chunkBytes = 64 * 1024;

  int get port => _port;

  Future<void> ensureStarted() async {
    if (_server != null) return;
    final s =
        await HttpServer.bind(InternetAddress.loopbackIPv4, 0, shared: false);
    _port = s.port;
    _server = s;
    s.listen(_handle, cancelOnError: false);
  }

  /// Register [source] and return the URL to hand the player. Unlike
  /// AudioStreamProxy's single-use tickets this one is reusable: libmpv
  /// re-opens the URL with a Range header when it seeks, and a 404 on
  /// the second GET would break seeking.
  Future<String> serve(GrowingFileSource source) async {
    await ensureStarted();
    final id = '${DateTime.now().microsecondsSinceEpoch}-${_nextTicket++}';
    _sources[id] = source;
    return 'http://127.0.0.1:$_port/$id';
  }

  /// Stop serving [url] (returned by [serve]).
  void release(String url) {
    final id = Uri.parse(url).pathSegments.isEmpty
        ? ''
        : Uri.parse(url).pathSegments.first;
    _sources.remove(id);
  }

  Future<void> dispose() async {
    final s = _server;
    _server = null;
    _sources.clear();
    _port = 0;
    if (s != null) await s.close(force: true);
  }

  /// `bytes=a-b` / `bytes=a-` / `bytes=-n`. Returns (start, endInclusive)
  /// with a null end meaning "to the end of the file". Null result = no
  /// or unparseable range, serve the whole thing.
  @visibleForTesting
  static ({int start, int? end})? parseRange(String? header, int? total) {
    if (header == null) return null;
    final m = RegExp(r'^bytes=(\d*)-(\d*)$').firstMatch(header.trim());
    if (m == null) return null;
    final a = m.group(1)!;
    final b = m.group(2)!;
    if (a.isEmpty && b.isEmpty) return null;
    if (a.isEmpty) {
      // Suffix range: the last N bytes. Only answerable when the total
      // size is known; mpv uses this to find a tail-written index.
      if (total == null) return null;
      final n = int.parse(b);
      final start = max(0, total - n);
      return (start: start, end: total - 1);
    }
    final start = int.parse(a);
    final end = b.isEmpty ? null : int.parse(b);
    return (start: start, end: end);
  }

  Future<void> _handle(HttpRequest req) async {
    final id = req.uri.pathSegments.isEmpty ? '' : req.uri.pathSegments.first;
    final source = _sources[id];
    final resp = req.response;
    if (source == null) {
      resp.statusCode = HttpStatus.notFound;
      await resp.close();
      return;
    }

    final total = source.totalBytes();
    final range = parseRange(req.headers.value(HttpHeaders.rangeHeader), total);
    final start = range?.start ?? 0;
    final endInclusive = range?.end;

    resp.headers.set(HttpHeaders.acceptRangesHeader, 'bytes');
    resp.headers.set(HttpHeaders.cacheControlHeader, 'no-store');
    // Left generic on purpose: libmpv sniffs the container from the
    // bytes, and guessing wrong from a filename is worse than silence.
    resp.headers.contentType = ContentType('audio', 'mpeg');

    if (range != null && total != null) {
      final last = endInclusive == null ? total - 1 : min(endInclusive, total - 1);
      if (start >= total || last < start) {
        resp.statusCode = HttpStatus.requestedRangeNotSatisfiable;
        resp.headers.set(HttpHeaders.contentRangeHeader, 'bytes */$total');
        await resp.close();
        return;
      }
      resp.statusCode = HttpStatus.partialContent;
      resp.headers.set(
          HttpHeaders.contentRangeHeader, 'bytes $start-$last/$total');
      resp.headers.contentLength = last - start + 1;
    } else if (range != null) {
      // Range against an unknown total: answer 200 from the offset and
      // let the client re-sync. Advertising a bogus Content-Range would
      // be worse than not honouring the range at all.
      resp.statusCode = HttpStatus.ok;
      resp.headers.contentLength = -1;
    } else if (total != null) {
      resp.statusCode = HttpStatus.ok;
      resp.headers.contentLength = total;
    } else {
      resp.statusCode = HttpStatus.ok;
      resp.headers.contentLength = -1; // chunked
    }

    // HEAD probes: headers are the whole answer.
    if (req.method == 'HEAD') {
      await resp.close();
      return;
    }

    var clientGone = false;
    resp.done.then<void>((_) {}, onError: (Object _) {
      clientGone = true;
    });

    final hardEnd = (total != null)
        ? (endInclusive == null ? total - 1 : min(endInclusive, total - 1))
        : endInclusive;

    var offset = start;
    var lastProgress = DateTime.now();

    try {
      while (!clientGone) {
        if (hardEnd != null && offset > hardEnd) break;

        final file = await source.resolve();
        var chunk = const <int>[];
        if (file != null) {
          RandomAccessFile? raf;
          try {
            // Opened and closed around every read so the writer's
            // `.part` -> final rename never contends with a long-lived
            // handle (see the file header's Windows note).
            raf = await file.open();
            final len = await raf.length();
            if (len > offset) {
              var want = _chunkBytes;
              if (hardEnd != null) {
                want = min(want, hardEnd - offset + 1);
              }
              want = min(want, len - offset);
              if (want > 0) {
                await raf.setPosition(offset);
                chunk = await raf.read(want);
              }
            }
          } on FileSystemException {
            // Mid-rename, or the sweeper won a race. Retry below.
            chunk = const [];
          } finally {
            await raf?.close();
          }
        }

        if (chunk.isNotEmpty) {
          offset += chunk.length;
          lastProgress = DateTime.now();
          resp.add(chunk);
          // flush() applies backpressure: without it the whole file is
          // buffered in the socket sink and "streaming" turns into
          // "read the file into RAM as fast as the disk allows".
          await resp.flush();
          continue;
        }

        // At the current end of the file.
        if (source.isAborted()) break;
        if (source.isComplete()) {
          // Complete and nothing more to read: genuinely EOF. One more
          // resolve pass already happened above, so a rename that just
          // landed has been seen.
          break;
        }
        if (DateTime.now().difference(lastProgress) > stallTimeout) break;
        await Future<void>.delayed(pollInterval);
      }
    } catch (_) {
      // Client hung up mid-write; nothing to salvage.
    }

    try {
      await resp.close();
    } catch (_) {/* already gone */}
  }
}

// ---------------------------------------------------------------------
// The cache
// ---------------------------------------------------------------------

/// Injection seam so tests can drive the cache without media_kit or
/// path_provider.
typedef PlayerOpener = Future<void> Function(String url);

class PredownloadCache extends ChangeNotifier {
  PredownloadCache._()
      : _lookupOverride = null,
        _openOverride = null,
        _stopOverride = null,
        _serverOverride = null;
  static final PredownloadCache instance = PredownloadCache._();

  /// Test constructor: everything the production singleton resolves from
  /// plugins is supplied explicitly.
  @visibleForTesting
  PredownloadCache.forTesting({
    required Directory cacheDir,
    ExternalNetwork? Function(String id)? lookupNetwork,
    PlayerOpener? openPlayer,
    Future<void> Function()? stopPlayer,
    GrowingFileServer? server,
    Duration? ttl,
  })  : _cacheDir = cacheDir,
        _lookupOverride = lookupNetwork,
        _openOverride = openPlayer,
        _stopOverride = stopPlayer,
        _serverOverride = server {
    if (ttl != null) this.ttl = ttl;
    _initialised = true;
  }

  /// How long an entry survives after its last use. The feature request
  /// says "clears after every 1 hour"; the clock restarts when the entry
  /// is played or promoted, so a track you are still listening to is not
  /// yanked at the 60-minute mark.
  Duration ttl = const Duration(hours: 1);

  /// How often the sweeper runs.
  static const Duration sweepInterval = Duration(minutes: 5);

  /// Bytes that must exist before we hand the URL to the player. Small
  /// enough to feel instant, large enough that libmpv's container probe
  /// has something to chew on.
  static const int minPlayableBytes = 96 * 1024;

  /// How long to wait for that prefix before giving up with an error
  /// instead of leaving the user staring at a dead play button.
  static const Duration prefixTimeout = Duration(seconds: 45);

  final Map<String, CacheEntry> _entries = {};
  final Map<String, String> _servedUrls = {}; // key -> loopback URL

  Directory? _cacheDir;
  Timer? _sweepTimer;
  bool _initialised = false;
  Future<void>? _initFuture;

  final ExternalNetwork? Function(String id)? _lookupOverride;
  /// Installed once at startup (main.dart) to hand preview playback to
  /// the app's single PlayerProvider. Static because the cache is a
  /// singleton and the wiring is process-wide.
  static Future<void> Function(String url, ExternalTrack? track)? previewOpener;
  static Future<void> Function()? previewStopper;

  /// Reports 0..1 of the previewed file that has downloaded, or null when
  /// the size isn't known yet. Installed in main.dart.
  static void Function(double? fraction)? previewProgress;

  /// The track whose URL was most recently handed to the player, so the
  /// transport bar can show its title/artist instead of a bare URL.
  ExternalTrack? _previewTrack;

  final PlayerOpener? _openOverride;
  final Future<void> Function()? _stopOverride;
  final GrowingFileServer? _serverOverride;


  /// Key of the entry currently feeding the preview player. Never swept.
  String? _playingKey;

  /// The track the preview player is on, for UI.
  ExternalTrack? get playingTrack => _entryFor(_playingKey)?.track;

  List<CacheEntry> get entries => List.unmodifiable(_entries.values);

  CacheEntry? entryFor(ExternalTrack t) => _entries[_keyOf(t)];

  CacheEntry? _entryFor(String? key) => key == null ? null : _entries[key];

  GrowingFileServer get _server => _serverOverride ?? GrowingFileServer.instance;

  static String _keyOf(ExternalTrack t) => '${t.networkId} ${t.id}';

  static String _dirNameFor(String key) =>
      crypto.sha1.convert(utf8.encode(key)).toString().substring(0, 20);

  // -- lifecycle ------------------------------------------------------

  /// Resolve the cache directory, sweep whatever a previous (possibly
  /// crashed) run left behind, and arm the periodic sweeper.
  ///
  /// Idempotent. Called lazily on first use so the feature costs nothing
  /// until someone taps a result; calling it from app start as well is
  /// strictly better, because then a crash's leftovers are cleaned even
  /// if the user never opens Discover again.
  Future<void> init() {
    if (_initialised && _sweepTimer != null) return Future<void>.value();
    return _initFuture ??= _init();
  }

  Future<void> _init() async {
    if (!_initialised) {
      _cacheDir ??= await _resolveCacheDir();
      _initialised = true;
    }
    await sweep();
    _sweepTimer?.cancel();
    _sweepTimer = Timer.periodic(sweepInterval, (_) => unawaited(sweep()));
  }

  /// Pick a directory that is guaranteed to be outside every watched
  /// library folder, so LibraryScanner can never index a half-written
  /// file (and the sweeper can never delete one of the user's tracks).
  Future<Directory> _resolveCacheDir() async {
    Directory base;
    try {
      base = await getTemporaryDirectory();
    } catch (_) {
      base = Directory.systemTemp;
    }

    var candidate =
        Directory('${base.path}${Platform.pathSeparator}bopwire-predownload');

    if (await _collidesWithLibrary(candidate.path)) {
      // The OS temp dir sits inside a watched folder (someone added
      // their whole home directory, say). Fall back to the system temp
      // root, and if even that collides, refuse to guess and use a
      // uniquely-named sibling of it.
      candidate = await Directory.systemTemp
          .createTemp('bopwire-predownload-');
      if (await _collidesWithLibrary(candidate.path)) {
        throw StateError(
            'Every candidate cache directory is inside a watched library '
            'folder (${candidate.path}). Remove the library folder that '
            'covers the system temp directory.');
      }
    }

    if (!await candidate.exists()) await candidate.create(recursive: true);
    return candidate;
  }

  /// True when [path] is inside a watched folder or a watched folder is
  /// inside it. Both directions matter: the first would get our partial
  /// files scanned, the second would put a user folder under our
  /// sweeper's delete-everything pass.
  Future<bool> _collidesWithLibrary(String path) async {
    List<String> folders;
    try {
      final lib = LibraryService.instance;
      await lib.ensureLoaded();
      folders = lib.folders;
    } catch (_) {
      // No SharedPreferences (tests, early boot). Nothing to collide
      // with yet; the caller's default temp location is safe.
      return false;
    }
    return pathCollides(path, folders);
  }

  /// Pure containment check, split out so the "never inside a watched
  /// folder" rule is testable without SharedPreferences.
  @visibleForTesting
  static bool pathCollides(String path, List<String> folders) {
    final me = _normalise(path);
    final sep = Platform.pathSeparator;
    for (final f in folders) {
      final other = _normalise(f);
      if (me == other) return true;
      if (me.startsWith('$other$sep')) return true;
      if (other.startsWith('$me$sep')) return true;
    }
    return false;
  }

  static String _normalise(String p) {
    var s = p;
    while (s.length > 1 && s.endsWith(Platform.pathSeparator)) {
      s = s.substring(0, s.length - 1);
    }
    return Platform.isWindows ? s.toLowerCase() : s;
  }

  /// The entry the user last asked to preview. Distinct from
  /// [_playingKey], which only becomes set once audio actually starts —
  /// a second click during buffering must still cancel the first.
  String? _previewKey;

  /// Rate limit for progress-only rebuilds. 8/sec is smooth to the eye
  /// and cheap; per-chunk was neither.
  static const Duration _progressNotifyInterval =
      Duration(milliseconds: 125);
  DateTime _lastProgressNotify = DateTime.fromMillisecondsSinceEpoch(0);

  void _notifyThrottled() {
    final now = DateTime.now();
    if (now.difference(_lastProgressNotify) < _progressNotifyInterval) return;
    _lastProgressNotify = now;
    notifyListeners();
  }

  /// The cache root. Only valid after [init].
  Directory? get cacheDir => _cacheDir;

  // -- streaming ------------------------------------------------------

  /// Start (or reuse) a cache download for [track] and begin playback as
  /// soon as it is safe to do so.
  ///
  /// Returns the entry. Throws nothing: failures land on
  /// [CacheEntry.error] / [CacheState.failed] so the caller can show
  /// them without a try/catch dance.
  Future<CacheEntry> stream(ExternalTrack track) async {
    await init();
    final key = _keyOf(track);

    // Switching previews. Two things have to stop, not one:
    //
    //  * playback of the previous preview, and
    //  * its DOWNLOAD.
    //
    // A preview is a speculative fetch. Leaving the old transfer running
    // burns bandwidth on a song the user has moved on from and, on
    // Soulseek, keeps occupying that peer's upload slot — which is the
    // scarce resource that makes the next preview slow. It also meant
    // clicking a second song appeared to do nothing, because the first
    // transfer still owned the player.
    if (_previewKey != null && _previewKey != key) {
      await _abandonPreview(_previewKey!);
    }
    _previewKey = key;

    final entry = await _startDownload(track);
    unawaited(_playWhenReady(entry));
    return entry;
  }

  /// Stop previewing [key]: halt playback if it owns the player, and
  /// cancel its transfer if it is still running.
  ///
  /// The partial bytes are left on disk for the sweeper rather than
  /// deleted inline, but they are NOT resumable — neither protocol
  /// supports resume-from-offset, so coming back to this track restarts
  /// the transfer. [CacheState.paused] exists to distinguish "superseded
  /// by another preview" from a user-initiated [CacheState.cancelled] in
  /// the Downloads list.
  Future<void> _abandonPreview(String key) async {
    if (_playingKey == key) {
      _playingKey = null;
      await previewStopper?.call();
      previewProgress?.call(null);
    }
    final prev = _entries[key];
    // Do NOT abandon a transfer a real download is riding. Promotion
    // waits on entry.finished, so cancelling here would strand the
    // download AND leave the peer holding a half-finished transfer that
    // blocks the next request to it.
    if (prev != null && prev.promoting) {
      notifyListeners();
      return;
    }
    if (prev != null && prev.state == CacheState.downloading) {
      await prev._sub?.cancel();
      prev._sub = null;
      // Not `cancelled`: that state means "user threw this away". This
      // is a partial we deliberately keep, so a re-preview can resume.
      prev.state = CacheState.paused;
      prev._complete();
    }
    notifyListeners();
  }

  /// Warm the cache without playing (used by the UI's explicit
  /// "pre-download" affordance, and by promote-on-demand).
  Future<CacheEntry> prefetch(ExternalTrack track) async {
    await init();
    return _startDownload(track);
  }

  Future<CacheEntry> _startDownload(ExternalTrack track) async {
    final key = _keyOf(track);
    final existing = _entries[key];
    if (existing != null &&
        existing.state != CacheState.failed &&
        existing.state != CacheState.evicted &&
        existing.state != CacheState.cancelled &&
        // A paused partial cannot be resumed: neither Soulseek nor napstr
        // implements resume-from-offset here (the transfer offset is
        // always 0), so re-previewing has to start the fetch again.
        existing.state != CacheState.paused) {
      existing.lastUsedAtMs = DateTime.now().millisecondsSinceEpoch;
      // A ready entry whose file vanished behind our back (user cleared
      // temp) must be re-fetched, not served as a 0-byte track.
      if (existing.state == CacheState.ready) {
        final p = existing.path;
        if (p == null || !await File(p).exists()) {
          _entries.remove(key);
        } else {
          notifyListeners();
          return existing;
        }
      } else {
        notifyListeners();
        return existing;
      }
    }

    final root = _cacheDir!;
    final dir = Directory(
        '${root.path}${Platform.pathSeparator}${_dirNameFor(key)}');
    if (await dir.exists()) {
      await _deleteQuietly(dir);
    }
    await dir.create(recursive: true);

    final entry = CacheEntry(
      key: key,
      track: track,
      dir: dir,
      createdAtMs: DateTime.now().millisecondsSinceEpoch,
    )..total = track.sizeBytes;
    _entries[key] = entry;
    notifyListeners();

    final lookup = _lookupOverride ?? NetworkRegistry.instance.byId;
    final net = lookup(track.networkId);
    if (net == null) {
      entry
        ..state = CacheState.failed
        ..error = 'Network ${track.networkId} is not registered.';
      entry._complete();
      notifyListeners();
      return entry;
    }

    entry._sub = net.download(track, dir.path).listen(
      (p) {
        entry.received = p.receivedBytes;
        // Feed the transport bar's buffered indicator, but only for the
        // entry actually being previewed — other cache transfers are
        // background work and must not move the bar under a playing song.
        if (_playingKey == entry.key) {
          final total = entry.total;
          previewProgress?.call(
              (total != null && total > 0)
                  ? (entry.received / total).clamp(0.0, 1.0)
                  : null);
        }
        if (p.totalBytes != null) entry.total = p.totalBytes;
        if (entry.received >= minPlayableBytes &&
            !entry._hasPrefix.isCompleted) {
          entry._hasPrefix.complete();
        }
        if (p.error != null) {
          entry
            ..state = CacheState.failed
            ..error = p.error;
          entry._complete();
          notifyListeners();
          return;
        }
        if (p.done) {
          entry
            ..state = CacheState.ready
            ..path = p.localPath;
          entry._complete();
          notifyListeners();
          return;
        }
        // Progress-only tick: throttled. A transfer emits a
        // DownloadProgress per chunk, and notifying on every one rebuilt
        // Discover, the transport bar and the Downloads pane hundreds of
        // times a second — enough to starve the UI thread so clicks and
        // hover stopped registering while a preview ran. State changes
        // above still notify immediately; only the byte counter waits.
        _notifyThrottled();
      },
      onError: (Object e) {
        entry
          ..state = CacheState.failed
          ..error = '$e';
        entry._complete();
        notifyListeners();
      },
      onDone: () {
        if (entry.state == CacheState.downloading) {
          entry
            ..state = CacheState.failed
            ..error = 'Transfer ended without producing a file.';
        }
        entry._complete();
        notifyListeners();
      },
      cancelOnError: true,
    );

    return entry;
  }

  /// The growing file for [entry]: whatever the network implementation
  /// is currently writing inside the entry's private directory.
  ///
  /// Resolved by listing rather than by guessing `.part` suffixes, so it
  /// works for Soulseek's `*.slskpart`, napstr's `*.part` and any future
  /// implementation's convention alike.
  Future<File?> resolveGrowingFile(CacheEntry entry) async {
    final done = entry.path;
    if (done != null) {
      final f = File(done);
      if (await f.exists()) return f;
    }
    try {
      File? best;
      var bestLen = -1;
      await for (final e in entry.dir.list(followLinks: false)) {
        if (e is! File) continue;
        final len = await e.length();
        if (len > bestLen) {
          bestLen = len;
          best = e;
        }
      }
      return best;
    } on FileSystemException {
      return null;
    }
  }

  /// Loopback URL that serves [entry]'s bytes as they arrive.
  Future<String> playbackUrl(CacheEntry entry) async {
    _previewTrack = entry.track;
    final cached = _servedUrls[entry.key];
    if (cached != null) return cached;
    final url = await _server.serve(GrowingFileSource(
      resolve: () => resolveGrowingFile(entry),
      totalBytes: () => entry.total,
      isComplete: () => entry.state == CacheState.ready,
      isAborted: () =>
          entry.state == CacheState.failed ||
          entry.state == CacheState.cancelled ||
          entry.state == CacheState.evicted,
    ));
    _servedUrls[entry.key] = url;
    return url;
  }

  Future<void> _playWhenReady(CacheEntry entry) async {
    // Tail-indexed containers (m4a/mp4) cannot be decoded from a prefix:
    // the moov atom is usually written last. Rather than hand libmpv a
    // source it will stall on in a way the user reads as "broken", wait
    // for the transfer and play the finished file.
    final canStartEarly = isProgressiveFriendly(_extensionOf(entry.track));
    try {
      if (canStartEarly) {
        await entry._hasPrefix.future.timeout(prefixTimeout);
      } else {
        await entry.finished.timeout(const Duration(hours: 2));
      }
    } on TimeoutException {
      if (entry.state == CacheState.downloading) {
        entry.error = canStartEarly
            ? 'No data arrived within ${prefixTimeout.inSeconds}s — the peer '
                'may be queued or offline.'
            : 'Timed out waiting for the full file.';
        notifyListeners();
      }
      return;
    }

    if (entry.state == CacheState.failed ||
        entry.state == CacheState.cancelled ||
        entry.state == CacheState.evicted) {
      return;
    }

    final url = await playbackUrl(entry);
    entry.lastUsedAtMs = DateTime.now().millisecondsSinceEpoch;
    _playingKey = entry.key;
    notifyListeners();

    try {
      await _open(url);
    } catch (e) {
      entry.error = 'Playback failed: $e';
      _playingKey = null;
      notifyListeners();
    }
  }

  static String? _extensionOf(ExternalTrack t) {
    final ext = t.extension;
    if (ext != null && ext.isNotEmpty) return ext.toLowerCase();
    final src = t.remotePath ?? t.title;
    final dot = src.lastIndexOf('.');
    if (dot < 0 || dot == src.length - 1) return null;
    return src.substring(dot + 1).toLowerCase();
  }

  Future<void> _open(String url) async {
    final override = _openOverride;
    if (override != null) return override(url);
    // No player is constructed here on purpose. The app has exactly ONE
    // audio player — PlayerProvider — and a second libmpv instance would
    // mean two things making sound, a transport bar that doesn't drive
    // the audible one, and doubled decoder memory. main.dart installs a
    // hook that routes this to PlayerProvider.playPreview(); without one
    // installed we simply produce no sound rather than spawning our own.
    final hook = previewOpener;
    if (hook == null) return;
    await hook(url, _previewTrack);
  }

  /// Stop preview playback, if it is running.
  Future<void> stopPreview() async {
    _playingKey = null;
    final override = _stopOverride;
    if (override != null) {
      await override();
    } else {
      await previewStopper?.call();
    }
    notifyListeners();
  }

  bool get isPreviewing => _playingKey != null;

  /// Cancel a running transfer and drop the entry's bytes.
  Future<void> cancel(ExternalTrack track) async {
    final entry = _entries[_keyOf(track)];
    if (entry == null) return;
    if (entry.state == CacheState.downloading) {
      entry.state = CacheState.cancelled;
      await entry._sub?.cancel();
      entry._sub = null;
      entry._complete();
    }
    if (_playingKey == entry.key) await stopPreview();
    await _evict(entry);
    notifyListeners();
  }

  // -- promotion ------------------------------------------------------

  /// If [track] is cached (or already downloading), put its bytes in
  /// [destDir] and return the resulting path. Returns null when there is
  /// nothing cached, so the caller falls back to a network download.
  ///
  /// This is the "if they're already in the pre-download cache, copy
  /// from there instead of downloading again" half of the feature. It is
  /// always a local filesystem copy — never a second transfer.
  ///
  /// [onProgress] fires while waiting on an in-flight cache download so
  /// the Downloads pane still animates. [isCancelled] lets the caller
  /// abandon the wait.
  Future<String?> promoteInto(
    ExternalTrack track,
    String destDir, {
    void Function(int received, int? total)? onProgress,
    bool Function()? isCancelled,
  }) async {
    if (!_initialised) return null; // cache never used this session
    final entry = _entries[_keyOf(track)];
    if (entry == null) return null;
    if (entry.state == CacheState.failed ||
        entry.state == CacheState.cancelled ||
        entry.state == CacheState.evicted) {
      return null;
    }

    entry.promoting = true;
    notifyListeners();
    try {
      if (entry.state == CacheState.downloading) {
        // Riding an in-flight cache transfer beats starting a second
        // one against the same peer, which on Soulseek would just queue
        // behind ourselves.
        final ticker = Timer.periodic(const Duration(milliseconds: 200), (t) {
          if (isCancelled?.call() ?? false) {
            t.cancel();
            return;
          }
          onProgress?.call(entry.received, entry.total);
        });
        try {
          await entry.finished;
        } finally {
          ticker.cancel();
        }
      }
      if (isCancelled?.call() ?? false) return null;
      if (entry.state != CacheState.ready) return null;

      final src = entry.path;
      if (src == null) return null;
      final srcFile = File(src);
      if (!await srcFile.exists()) return null;

      final dir = Directory(destDir);
      if (!await dir.exists()) await dir.create(recursive: true);

      final name = src.split(Platform.pathSeparator).last;
      final target = await _uniquePath(dir.path, name);

      // copy(), not rename(): the cache lives on the OS temp filesystem,
      // which is very often a different device (tmpfs / separate
      // partition) from the library folder, and rename() across devices
      // fails. The cache copy is left in place so a second promote — or
      // continued playback — still works; the sweeper reclaims it.
      await srcFile.copy(target);
      entry.lastUsedAtMs = DateTime.now().millisecondsSinceEpoch;
      onProgress?.call(entry.received, entry.total);
      return target;
    } finally {
      entry.promoting = false;
      notifyListeners();
    }
  }

  static Future<String> _uniquePath(String dir, String name) async {
    var candidate = '$dir${Platform.pathSeparator}$name';
    if (!await File(candidate).exists()) return candidate;
    final dot = name.lastIndexOf('.');
    final stem = dot <= 0 ? name : name.substring(0, dot);
    final ext = dot <= 0 ? '' : name.substring(dot);
    for (var i = 2; i < 1000; i++) {
      candidate = '$dir${Platform.pathSeparator}$stem ($i)$ext';
      if (!await File(candidate).exists()) return candidate;
    }
    return '$dir${Platform.pathSeparator}$stem '
        '(${DateTime.now().millisecondsSinceEpoch})$ext';
  }

  // -- expiry ---------------------------------------------------------

  /// Delete everything past its hour, plus any directory in the cache
  /// root that no live entry claims (the crash-leftover case).
  ///
  /// Never touches an entry that is downloading, currently feeding the
  /// preview player, or mid-promotion.
  Future<void> sweep() async {
    final root = _cacheDir;
    if (root == null) return;
    final now = DateTime.now();
    final cutoff = now.subtract(ttl);

    for (final entry in _entries.values.toList()) {
      if (entry.key == _playingKey) continue;
      if (entry.promoting) continue;
      if (entry.state == CacheState.downloading) continue;
      if (entry.state == CacheState.evicted) continue;
      final used = DateTime.fromMillisecondsSinceEpoch(entry.lastUsedAtMs);
      if (used.isAfter(cutoff)) continue;
      await _evict(entry);
    }

    // Orphans: directories with no live entry. On a normal run there are
    // none; after a crash the whole cache root is orphaned, which is the
    // point — a crash must not leak files forever.
    final claimed = <String>{
      for (final e in _entries.values)
        if (e.state != CacheState.evicted) _normalise(e.dir.path),
    };
    try {
      if (!await root.exists()) return;
      await for (final child in root.list(followLinks: false)) {
        if (claimed.contains(_normalise(child.path))) continue;
        FileStat st;
        try {
          st = await child.stat();
        } catch (_) {
          continue;
        }
        if (st.modified.isAfter(cutoff)) continue;
        await _deleteQuietly(child);
      }
    } on FileSystemException {
      // Cache root vanished; nothing to sweep.
    }
    notifyListeners();
  }

  Future<void> _evict(CacheEntry entry) async {
    final url = _servedUrls.remove(entry.key);
    if (url != null) _server.release(url);
    entry.state = CacheState.evicted;
    entry.path = null;
    _entries.remove(entry.key);
    await _deleteQuietly(entry.dir);
  }

  /// Cleanly end every in-flight transfer, giving each peer its CANCEL
  /// frame, then stop the sweeper. Called on app shutdown.
  ///
  /// Without this, closing the app left the seeder holding a slot for a
  /// transfer that would never resume — so the NEXT launch timed out
  /// with "no seeder answered". Cancelling the subscription is what
  /// triggers each network's graceful abort.
  Future<void> shutdown() async {
    stopSweeper();
    final live = _entries.values
        .where((e) => e.state == CacheState.downloading)
        .toList();
    for (final e in live) {
      e.state = CacheState.cancelled;
      try {
        await e._sub?.cancel();   // -> stream onCancel -> graceful abort
      } catch (_) {}
      e._sub = null;
      e._complete();
    }
    // Give the CANCEL frames a moment to leave before the process dies.
    if (live.isNotEmpty) {
      await Future<void>.delayed(const Duration(milliseconds: 600));
    }
  }

  /// Drop everything, e.g. from a "clear cache" button.
  Future<void> clear() async {
    for (final entry in _entries.values.toList()) {
      if (entry.state == CacheState.downloading) {
        entry.state = CacheState.cancelled;
        await entry._sub?.cancel();
        entry._sub = null;
        entry._complete();
      }
      await _evict(entry);
    }
    _playingKey = null;
    notifyListeners();
  }

  static Future<void> _deleteQuietly(FileSystemEntity e) async {
    try {
      await e.delete(recursive: true);
    } catch (_) {/* locked or already gone */}
  }

  @override
  void dispose() {
    _sweepTimer?.cancel();
    _sweepTimer = null;
    super.dispose();
  }

  /// Test hook: shut the sweeper down without disposing the notifier.
  @visibleForTesting
  void stopSweeper() {
    _sweepTimer?.cancel();
    _sweepTimer = null;
  }
}
