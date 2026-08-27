// Lyrics mode for the player — the Flutter twin of the web player's
// lyrics view (bopwire/webplayer/frontend/app.js: parseLRC / openLyrics /
// syncLyrics). A per-row Lyrics button in the track lists opens this panel
// over the main content area; the transport bar keeps running underneath,
// exactly like the website replacing its main view while the now-playing
// bar stays.
//
// Sources, in order:
//   1. the local per-song cache (MetadataLookup.storedLyrics — filled in
//      at import time when the LRCLIB toggle is on), then
//   2. the full node (lyrics.get), so a song we never imported ourselves
//      (someone else's track in Discover) still shows lyrics.
//
// Lyrics are decoration: no source, no network, malformed LRC — the panel
// shows its empty state and playback is never touched.

import 'package:flutter/material.dart';

import '../models/song.dart';
import '../providers/player_provider.dart';
import '../services/metadata_lookup.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';

// ─────────────────────── LRC parsing (pure) ───────────────────────

/// One timed lyric line. A repeated chorus line in the source LRC (one
/// text with several timestamps) becomes several [LrcLine]s.
class LrcLine {
  const LrcLine(this.timeMs, this.text);
  final int timeMs;
  final String text;
}

final RegExp _stampRe = RegExp(r'\[(\d{1,3}):(\d{2})(?:[.:](\d{1,3}))?\]');
final RegExp _tagRe = RegExp(r'\[[^\]]*\]');

/// Parse LRC text into time-sorted lines. Mirrors the web player's
/// parseLRC exactly so the two clients agree on every file:
///   - a line may carry SEVERAL stamps (repeated chorus) — one entry each;
///   - metadata tags ([ar:], [ti:], …) never become lyric lines (their
///     "minutes" field isn't numeric, so the stamp regex skips them);
///   - a 2-digit fraction is centiseconds, a 3-digit one is milliseconds.
/// Garbage in → empty list out; this never throws.
List<LrcLine> parseLrc(String lrc) {
  final out = <LrcLine>[];
  for (final raw in lrc.split(RegExp(r'\r?\n'))) {
    final stamps = _stampRe.allMatches(raw).toList();
    if (stamps.isEmpty) continue;
    final text = raw.replaceAll(_tagRe, '').trim();
    for (final m in stamps) {
      final min = int.tryParse(m.group(1)!) ?? 0;
      final sec = int.tryParse(m.group(2)!) ?? 0;
      final frac = m.group(3);
      final fracMs = frac == null
          ? 0
          : (frac.length == 3
              ? (int.tryParse(frac) ?? 0)
              : (int.tryParse(frac) ?? 0) * 10);
      out.add(LrcLine(min * 60000 + sec * 1000 + fracMs, text));
    }
  }
  out.sort((a, b) => a.timeMs.compareTo(b.timeMs));
  return out;
}

/// Index of the line active at [posMs]: the LAST line whose stamp is
/// <= the position, or -1 before the first stamp. [lines] must be sorted
/// (parseLrc's output always is).
int activeLrcIndex(List<LrcLine> lines, int posMs) {
  var i = -1;
  for (var k = 0; k < lines.length; k++) {
    if (lines[k].timeMs <= posMs) {
      i = k;
    } else {
      break;
    }
  }
  return i;
}

// ─────────────────────── Open / close plumbing ───────────────────────

/// What the panel is showing lyrics FOR. [hashes] is every content hash
/// that counts as "this song" (a local file, its canonical chain hash…)
/// so the highlight only follows the clock when THIS song is the one
/// actually playing.
class LyricsRequest {
  LyricsRequest({
    required this.songKey,
    required this.title,
    required this.artist,
    Set<String>? hashes,
  }) : hashes = {...?hashes}..removeWhere((h) => h.isEmpty);

  factory LyricsRequest.fromSong(Song s) => LyricsRequest(
        songKey: s.contentHash,
        title: s.title,
        artist: s.artist,
        hashes: {s.contentHash},
      );

  /// Key of the local lyrics cache file (the library's songId — canonical
  /// chain hash when known, else content hash).
  final String songKey;
  final String title;
  final String artist;
  final Set<String> hashes;

  bool matchesPlaying(String playingHash) =>
      playingHash.isNotEmpty && hashes.contains(playingHash);
}

/// Which song's lyrics the main content area should be showing, if any.
/// HomeScreen listens and swaps the "top window" between the tab stack
/// and the lyrics panel — the Flutter equivalent of the website's
/// showView('lyrics').
class LyricsController extends ChangeNotifier {
  LyricsController._();
  static final LyricsController instance = LyricsController._();

  LyricsRequest? _request;
  LyricsRequest? get request => _request;

  void open(LyricsRequest req) {
    _request = req;
    notifyListeners();
  }

  void close() {
    if (_request == null) return;
    _request = null;
    notifyListeners();
  }
}

/// Play-bar entry point. The panel overlays the home shell's nested
/// navigator, ABOVE whatever screen is showing (a pushed collection's
/// track list included) — so nothing is popped here: closing lyrics is a
/// genuine BACK that reveals the untouched screen underneath.
void openLyrics(BuildContext context, LyricsRequest req) {
  LyricsController.instance.open(req);
}

// ─────────────────────── Player linkage ───────────────────────

/// The slice of the player the panel needs: a tick source, the position,
/// what's playing, and seek. Kept as injectable functions so widget tests
/// can drive the panel without constructing the real (native-backed)
/// player.
class LyricsPlayback {
  LyricsPlayback({
    required this.listenable,
    required this.positionMs,
    required this.playingHash,
    required this.seek,
  });

  factory LyricsPlayback.fromProvider(PlayerProvider p) => LyricsPlayback(
        listenable: p,
        positionMs: () => p.positionMs,
        playingHash: () => p.currentSong?.contentHash ?? '',
        seek: (ms) => p.seek(ms),
      );

  final Listenable listenable;
  final int Function() positionMs;
  final String Function() playingHash;
  final Future<void> Function(int ms) seek;
}

// ─────────────────────── Lyrics loading ───────────────────────

typedef LyricsLoader = Future<LyricsResult?> Function(LyricsRequest req);

/// Local cache first, node second. Never throws — every failure is just
/// "no lyrics" to the reader, same as the website's fetchLyrics.
Future<LyricsResult?> defaultLyricsLoader(LyricsRequest req) async {
  try {
    final stored = await MetadataLookup.instance.storedLyrics(req.songKey);
    if (stored != null && stored.hasAny) return stored;
  } catch (_) {/* fall through to the node */}

  final artist = req.artist.trim();
  final title = req.title.trim();
  // Artist + title are the node's lookup key; without both there is
  // nothing safe to ask for (a title-only hit would happily return the
  // wrong song's lyrics).
  if (artist.isEmpty || title.isEmpty) return null;
  try {
    final pid =
        await NodeService.getRatsPeerId(waitFor: const Duration(seconds: 5));
    if (pid.isEmpty) return null;
    final m = await NodeClient(ratsPeerId: pid).fetchLyrics(artist, title);
    if (m == null) return null;
    final res = LyricsResult(
      plain: (m['plain'] as String?) ?? '',
      synced: (m['synced'] as String?) ?? '',
      instrumental: m['instrumental'] == true,
      source: 'node',
    );
    return res.hasAny ? res : null;
  } catch (_) {
    return null;
  }
}

// ─────────────────────── Availability (play-bar glow) ───────────────────────

/// Cheap "does this song have lyrics?" answer for the play bar, which
/// wants its Lyrics button to glow when lyrics actually exist for the
/// current track. The bar rebuilds on every position tick, so [check]
/// MUST stay a map lookup: the async resolution (same two sources as the
/// panel — local cache, then node) runs AT MOST ONCE per song, and both
/// hits and misses are remembered for the process. Failures are simply
/// "no lyrics"; nothing here can block or stutter playback.
class LyricsAvailability extends ChangeNotifier {
  LyricsAvailability._();
  static final LyricsAvailability instance = LyricsAvailability._();

  /// Injectable for tests; production uses the panel's own loader.
  LyricsLoader loader = defaultLyricsLoader;

  final Map<String, bool> _known = {};
  final Set<String> _inFlight = {};

  static String _key(LyricsRequest req) =>
      '${req.songKey}|${req.artist.trim().toLowerCase()}'
      '|${req.title.trim().toLowerCase()}';

  /// true/false once known; null while a (single) resolution is still in
  /// flight — listeners are notified when it lands.
  bool? check(LyricsRequest req) {
    final k = _key(req);
    final known = _known[k];
    if (known != null) return known;
    if (_inFlight.add(k)) {
      () async {
        var has = false;
        try {
          final res = await loader(req);
          has = res != null && res.hasAny;
        } catch (_) {/* a failure is just "no lyrics" */}
        _known[k] = has;
        _inFlight.remove(k);
        notifyListeners();
      }();
    }
    return null;
  }

  /// Test hook: forget everything and restore the default loader.
  void debugReset() {
    _known.clear();
    _inFlight.clear();
    loader = defaultLyricsLoader;
  }
}

// ─────────────────────── The panel ───────────────────────

class LyricsPanel extends StatefulWidget {
  const LyricsPanel({
    super.key,
    required this.request,
    required this.playback,
    required this.onClose,
    this.loader = defaultLyricsLoader,
  });

  final LyricsRequest request;
  final LyricsPlayback playback;
  final VoidCallback onClose;
  final LyricsLoader loader;

  @override
  State<LyricsPanel> createState() => _LyricsPanelState();
}

class _LyricsPanelState extends State<LyricsPanel> {
  bool _loading = true;
  LyricsResult? _result;
  List<LrcLine> _lines = const [];
  // One stable key per synced line: ensureVisible needs the line's
  // context, and stable keys keep the tap recognisers glued to their
  // rows across the per-position rebuilds.
  List<GlobalKey> _lineKeys = const [];
  int _active = -1;
  int _loadSeq = 0;
  final ScrollController _scroll = ScrollController();

  @override
  void initState() {
    super.initState();
    widget.playback.listenable.addListener(_onTick);
    _load();
  }

  @override
  void didUpdateWidget(LyricsPanel old) {
    super.didUpdateWidget(old);
    if (!identical(old.playback.listenable, widget.playback.listenable)) {
      old.playback.listenable.removeListener(_onTick);
      widget.playback.listenable.addListener(_onTick);
    }
    if (old.request.songKey != widget.request.songKey ||
        old.request.title != widget.request.title ||
        old.request.artist != widget.request.artist) {
      _load();
    }
  }

  @override
  void dispose() {
    widget.playback.listenable.removeListener(_onTick);
    _scroll.dispose();
    super.dispose();
  }

  void _resetForLoad() {
    _loading = true;
    _result = null;
    _lines = const [];
    _lineKeys = const [];
    _active = -1;
  }

  Future<void> _load() async {
    final seq = ++_loadSeq;
    if (seq == 1) {
      _resetForLoad(); // initial load: fields already pre-build, no setState
    } else {
      setState(_resetForLoad);
    }
    LyricsResult? res;
    try {
      res = await widget.loader(widget.request);
    } catch (_) {
      res = null;
    }
    // The user may have moved to another song (or the panel may be gone)
    // while this was in flight; a stale reply must never paint one song's
    // lyrics over another's.
    if (!mounted || seq != _loadSeq) return;
    final lines =
        (res != null && res.synced.isNotEmpty) ? parseLrc(res.synced) : const <LrcLine>[];
    setState(() {
      _loading = false;
      _result = res;
      _lines = lines;
      _lineKeys = [for (var i = 0; i < lines.length; i++) GlobalKey()];
      _active = -1;
    });
    _onTick();
  }

  /// Recompute the highlighted line from the live position. Only
  /// meaningful while the lyrics on screen belong to the track actually
  /// playing — lyrics opened for some OTHER song must not follow the
  /// current one's clock.
  void _onTick() {
    if (!mounted || _lines.isEmpty) return;
    if (!widget.request.matchesPlaying(widget.playback.playingHash())) {
      if (_active != -1) setState(() => _active = -1);
      return;
    }
    final i = activeLrcIndex(_lines, widget.playback.positionMs());
    if (i == _active) return;
    setState(() => _active = i);
    if (i >= 0) _scrollToActive(i);
  }

  void _scrollToActive(int i) {
    // Post-frame: the highlighted line's new context must exist first.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (!mounted || i >= _lineKeys.length) return;
      final ctx = _lineKeys[i].currentContext;
      if (ctx == null) return;
      // alignment .3 keeps the active line about a third from the top —
      // not pinned to the edge — so upcoming lines stay readable.
      Scrollable.ensureVisible(
        ctx,
        alignment: 0.3,
        duration: const Duration(milliseconds: 250),
        curve: Curves.easeOut,
      );
    });
  }

  void _tapLine(int i) {
    // Same guard as the website's seekTo: taps only steer playback when
    // this song is the one playing.
    if (!widget.request.matchesPlaying(widget.playback.playingHash())) return;
    if (i < 0 || i >= _lines.length) return;
    widget.playback.seek(_lines[i].timeMs);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final req = widget.request;
    return Material(
      color: theme.colorScheme.surface,
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.fromLTRB(16, 10, 6, 10),
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerHighest,
              border: Border(
                bottom: BorderSide(color: theme.dividerColor, width: 0.5),
              ),
            ),
            child: Row(
              children: [
                // Reads as BACK, because that is what it does now: the
                // screen the lyrics opened over (tab or pushed track
                // list) is still there underneath, untouched.
                IconButton(
                  tooltip: 'Back',
                  icon: const Icon(Icons.arrow_back, size: 20),
                  onPressed: widget.onClose,
                ),
                const SizedBox(width: 2),
                Icon(Icons.lyrics_outlined,
                    size: 20, color: theme.colorScheme.primary),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        req.title.isEmpty ? '(untitled)' : req.title,
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.titleSmall
                            ?.copyWith(fontWeight: FontWeight.w700),
                      ),
                      if (req.artist.isNotEmpty)
                        Text(
                          req.artist,
                          maxLines: 1,
                          overflow: TextOverflow.ellipsis,
                          style: theme.textTheme.bodySmall?.copyWith(
                              color: theme.colorScheme.onSurfaceVariant),
                        ),
                    ],
                  ),
                ),
              ],
            ),
          ),
          Expanded(child: _body(theme)),
        ],
      ),
    );
  }

  Widget _body(ThemeData theme) {
    if (_loading) {
      return const Center(
        child: SizedBox(
          width: 24,
          height: 24,
          child: CircularProgressIndicator(strokeWidth: 2.5),
        ),
      );
    }

    final res = _result;
    final plain = res?.plain ?? '';
    final hasSynced = _lines.isNotEmpty;

    if (!hasSynced && plain.trim().isEmpty) {
      // Includes the malformed-LRC case: synced text that parsed to zero
      // lines and no plain fallback reads as "no lyrics", never an error.
      final instrumental = res?.instrumental ?? false;
      return Center(
        child: Text(
          instrumental
              ? 'This track is instrumental.'
              : 'No lyrics found for this track.',
          textAlign: TextAlign.center,
          style: theme.textTheme.bodyMedium?.copyWith(
              color: theme.colorScheme.onSurface.withOpacity(.55)),
        ),
      );
    }

    if (hasSynced) return _syncedBody(theme);
    return _plainBody(theme, plain);
  }

  Widget _syncedBody(ThemeData theme) {
    final dim = theme.colorScheme.onSurface.withOpacity(.45);
    return SingleChildScrollView(
      controller: _scroll,
      padding: const EdgeInsets.symmetric(vertical: 40, horizontal: 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (var i = 0; i < _lines.length; i++)
            InkWell(
              key: _lineKeys[i],
              borderRadius: BorderRadius.circular(6),
              onTap: () => _tapLine(i),
              child: Padding(
                padding:
                    const EdgeInsets.symmetric(vertical: 6, horizontal: 4),
                child: Text(
                  _lines[i].text.isEmpty ? ' ' : _lines[i].text,
                  textAlign: TextAlign.center,
                  style: theme.textTheme.titleMedium?.copyWith(
                    fontWeight:
                        i == _active ? FontWeight.w800 : FontWeight.w500,
                    color: i == _active ? theme.colorScheme.primary : dim,
                  ),
                ),
              ),
            ),
        ],
      ),
    );
  }

  Widget _plainBody(ThemeData theme, String plain) {
    // No timing information, so no current line: plain lyrics render
    // undimmed rather than looking permanently inactive.
    return SingleChildScrollView(
      controller: _scroll,
      padding: const EdgeInsets.symmetric(vertical: 40, horizontal: 24),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          for (final line in plain.split(RegExp(r'\r?\n')))
            Padding(
              padding: const EdgeInsets.symmetric(vertical: 4, horizontal: 4),
              child: Text(
                line.isEmpty ? ' ' : line,
                textAlign: TextAlign.center,
                style: theme.textTheme.titleMedium
                    ?.copyWith(fontWeight: FontWeight.w500),
              ),
            ),
        ],
      ),
    );
  }
}
