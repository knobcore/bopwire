import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/library_provider.dart';
import '../providers/player_provider.dart';
import '../providers/download_provider.dart';
import '../models/song.dart';
import '../services/librats_discovery.dart';
import '../services/rats_client.dart';
import '../widgets/album_art.dart';
import '../widgets/cover_art.dart';
import '../widgets/lyrics_panel.dart';
import 'discover_screen.dart';
import 'local_library_screen.dart';
import 'chat_screen.dart';
import 'wallet_screen.dart';
import 'settings_screen.dart';

class HomeScreen extends StatefulWidget {
  const HomeScreen({super.key});

  @override
  State<HomeScreen> createState() => _HomeScreenState();
}

class _HomeScreenState extends State<HomeScreen> {
  // Selected tab. A ValueNotifier (not a plain setState field) because the
  // tab stack now lives INSIDE the nested shell navigator's home route,
  // and a route's builder does not re-run when this State rebuilds — the
  // IndexedStack listens to this directly instead.
  final ValueNotifier<int> _tab = ValueNotifier<int>(0);

  /// The nested "shell" navigator that pushed screens (a collection's
  /// track list, folders, chat rooms…) go onto. Because it sits inside
  /// the Expanded, everything it shows renders ABOVE the tab stack but
  /// BELOW the download banner, mini player and nav bar — so the play
  /// bar (and its lyrics button) stays visible on every pushed screen.
  /// Tab content resolves `Navigator.of(context)` to this navigator
  /// automatically; dialogs (`showDialog` defaults to the root
  /// navigator) and the wallet gate are unaffected.
  final GlobalKey<NavigatorState> _shellNavKey = GlobalKey<NavigatorState>();

  /// Whether the shell navigator has a pushed screen to pop — kept live
  /// by [_ShellDepthObserver] so the PopScope's canPop stays accurate
  /// for Android's (predictive) back gesture.
  final ValueNotifier<bool> _shellCanPop = ValueNotifier<bool>(false);
  late final _ShellDepthObserver _shellObserver =
      _ShellDepthObserver(_shellCanPop);

  static const _screens = [
    DiscoverScreen(),
    LocalLibraryScreen(),
    ChatScreen(),
    WalletScreen(),
    SettingsScreen(),
  ];

  @override
  void dispose() {
    _tab.dispose();
    _shellCanPop.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Column(
        children: [
          const _ConnectionBanner(),
          // The "top window": the nested shell navigator (tab stack as its
          // home route), with the lyrics panel laid over it when the play
          // bar's Lyrics button is tapped. A Stack (not a swap) so every
          // tab — and any pushed track list — keeps its state, and the
          // banners + mini player below keep running. Closing lyrics is a
          // BACK action: the screen underneath was never destroyed.
          Expanded(
            child: AnimatedBuilder(
              animation:
                  Listenable.merge([LyricsController.instance, _shellCanPop]),
              builder: (context, _) {
                final req = LyricsController.instance.request;
                return PopScope(
                  // System back: close lyrics first, then pop the shell
                  // navigator; only when neither has anything left does
                  // the pop reach the root (app exit on Android).
                  canPop: req == null && !_shellCanPop.value,
                  onPopInvokedWithResult: (didPop, _) {
                    if (didPop) return;
                    if (LyricsController.instance.request != null) {
                      LyricsController.instance.close();
                    } else {
                      _shellNavKey.currentState?.maybePop();
                    }
                  },
                  child: Stack(
                    fit: StackFit.expand,
                    children: [
                      Navigator(
                        key: _shellNavKey,
                        observers: [_shellObserver],
                        onGenerateRoute: (settings) => MaterialPageRoute(
                          settings: settings,
                          builder: (_) => ValueListenableBuilder<int>(
                            valueListenable: _tab,
                            builder: (_, idx, __) => IndexedStack(
                                index: idx, children: _screens),
                          ),
                        ),
                      ),
                      if (req != null)
                        LyricsPanel(
                          // Keyed per song so switching songs rebuilds the
                          // panel (and its scroll position) from scratch.
                          key: ValueKey('lyrics:${req.songKey}'),
                          request: req,
                          playback: LyricsPlayback.fromProvider(
                              context.read<PlayerProvider>()),
                          onClose: LyricsController.instance.close,
                        ),
                    ],
                  ),
                );
              },
            ),
          ),
          // Download banner sits between the body and the mini player so
          // the user can always glance at "what's downloading right now"
          // without leaving the screen they're on. Collapses to zero
          // height when idle.
          const _DownloadBanner(),
          // Always-visible playback bar: shows the current song + play
          // count + transport controls, or a "Nothing playing" placeholder
          // when idle. Replaces the old PlayerScreen navigation — there's
          // no separate now-playing route any more.
          Consumer<PlayerProvider>(
            builder: (context, player, _) =>
                _MiniPlayer(player: player, song: player.currentSong),
          ),
        ],
      ),
      bottomNavigationBar: ValueListenableBuilder<int>(
        valueListenable: _tab,
        builder: (context, selected, _) => NavigationBar(
        selectedIndex: selected,
        onDestinationSelected: (i) {
          // Picking a tab leaves lyrics mode, same as the website where
          // any nav click swaps the lyrics view out for the chosen one.
          LyricsController.instance.close();
          // And pops any pushed screen off the shell navigator — the tab
          // stack sits UNDER pushed routes, so switching tabs while a
          // collection was open would otherwise change an invisible view.
          _shellNavKey.currentState?.popUntil((r) => r.isFirst);
          if (i == 0 && selected != 0) {
            final lib = context.read<LibraryProvider>();
            lib.refresh();
            lib.loadCollections();
          }
          _tab.value = i;
        },
        destinations: const [
          NavigationDestination(icon: Icon(Icons.library_music),         label: 'Discover'),
          NavigationDestination(icon: Icon(Icons.folder_special),        label: 'My Library'),
          NavigationDestination(icon: Icon(Icons.chat_bubble_outline),   label: 'Chat'),
          NavigationDestination(icon: Icon(Icons.account_balance_wallet), label: 'Wallet'),
          NavigationDestination(icon: Icon(Icons.settings),              label: 'Settings'),
        ],
        ),
      ),
    );
  }
}

/// Keeps a "can the shell navigator pop?" flag live for the PopScope
/// above it. Route mutations happen from gesture handlers (never during
/// build), so flipping the ValueNotifier here is safe; the initial home
/// route push is a no-op (canPop() is false → value unchanged).
class _ShellDepthObserver extends NavigatorObserver {
  _ShellDepthObserver(this.canPopNotifier);
  final ValueNotifier<bool> canPopNotifier;

  void _sync() => canPopNotifier.value = navigator?.canPop() ?? false;

  @override
  void didPush(Route<dynamic> route, Route<dynamic>? previousRoute) => _sync();
  @override
  void didPop(Route<dynamic> route, Route<dynamic>? previousRoute) => _sync();
  @override
  void didRemove(Route<dynamic> route, Route<dynamic>? previousRoute) =>
      _sync();
  @override
  void didReplace({Route<dynamic>? newRoute, Route<dynamic>? oldRoute}) =>
      _sync();
}

// ---- Connection status banner ------------------------------------------

class _ConnectionBanner extends StatelessWidget {
  const _ConnectionBanner();

  @override
  Widget build(BuildContext context) {
    final disc  = context.watch<LibratsDiscovery>();
    final theme = Theme.of(context);

    final hasNode   = disc.autoSelectedRatsPeerId.isNotEmpty;
    final searching = disc.isRefreshing;
    final progress  = disc.vpsStatus;

    final rats        = RatsClient.instance;
    final peerCount   = rats.peerCount;
    final nodeCount   = disc.routes.length;

    final Color  dotColor;
    final String primaryLine;
    // Two-state banner per operator request: "offline" when we're not
    // attached to a full node, otherwise "N nodes · M peers connected".
    // No peer IDs or IPs — those moved to a debug screen.
    String? secondaryLine;

    if (searching) {
      dotColor    = Colors.orange;
      primaryLine = progress.isNotEmpty ? progress : 'Connecting…';
    } else if (hasNode) {
      dotColor    = Colors.green;
      primaryLine = 'online · '
                    '$nodeCount node${nodeCount == 1 ? '' : 's'} · '
                    '$peerCount peer${peerCount == 1 ? '' : 's'} connected';
    } else {
      dotColor    = Colors.red;
      primaryLine = 'offline';
    }

    // Tap → reconnect (or search if not connected).
    // Long-press → fully disconnect from every peer. Hidden but consistent:
    // a tooltip on the button advertises the gesture so it's discoverable.
    final connectTooltip = hasNode
        ? 'Tap to reconnect · long-press to disconnect'
        : 'Tap to search for nodes';

    Future<void> onLongPress() async {
      final messenger = ScaffoldMessenger.maybeOf(context);
      await disc.disconnect();
      messenger?.showSnackBar(const SnackBar(
        content: Text('Disconnected from all peers'),
        duration: Duration(seconds: 2),
      ));
    }

    return Material(
      color: theme.colorScheme.surface,
      child: Container(
        decoration: BoxDecoration(
          border: Border(
            bottom: BorderSide(color: theme.dividerColor, width: 0.5),
          ),
        ),
        padding: const EdgeInsets.fromLTRB(12, 4, 4, 4),
        child: SafeArea(
          bottom: false,
          child: Row(
            crossAxisAlignment: CrossAxisAlignment.center,
            children: [
              // status dot / spinner
              SizedBox(
                width: 12,
                height: 12,
                child: searching
                    ? CircularProgressIndicator(
                        strokeWidth: 2,
                        color: dotColor,
                      )
                    : Icon(Icons.circle, size: 10, color: dotColor),
              ),
              const SizedBox(width: 8),
              // primary + optional secondary labels
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    SelectableText(
                      primaryLine,
                      style: theme.textTheme.bodySmall?.copyWith(
                        color: searching
                            ? theme.colorScheme.onSurface.withOpacity(0.7)
                            : (hasNode
                                ? theme.colorScheme.onSurface
                                : Colors.red.shade300),
                        fontFamily: hasNode ? 'monospace' : null,
                      ),
                      maxLines: 1,
                    ),
                    if (secondaryLine != null)
                      SelectableText(
                        secondaryLine,
                        style: theme.textTheme.labelSmall?.copyWith(
                          color: theme.colorScheme.onSurface.withOpacity(0.55),
                          fontFamily: 'monospace',
                        ),
                        maxLines: 1,
                      ),
                  ],
                ),
              ),
              // Connect / disconnect button.
              Tooltip(
                message: connectTooltip,
                child: GestureDetector(
                  onLongPress: hasNode ? onLongPress : null,
                  child: TextButton.icon(
                    onPressed: searching ? null : disc.forceReconnect,
                    icon: searching
                        ? const SizedBox.shrink()
                        : Icon(hasNode ? Icons.power_settings_new
                                       : Icons.wifi_find, size: 14),
                    label: Text(
                      searching
                          ? 'Searching…'
                          : (hasNode ? 'Connected' : 'Connect'),
                      style: const TextStyle(fontSize: 11),
                    ),
                    style: TextButton.styleFrom(
                      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 4),
                      minimumSize: Size.zero,
                      tapTargetSize: MaterialTapTargetSize.shrinkWrap,
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
      ),
    );
  }
}

// ---- Download progress banner ------------------------------------------

class _DownloadBanner extends StatelessWidget {
  const _DownloadBanner();

  String _shortLabel(DownloadJob j) {
    if (j.batchLabel != null) return j.batchLabel!;
    final t = j.song.title.trim();
    return t.isEmpty
        ? 'Song ${j.song.contentHash.substring(0, 8)}'
        : t;
  }

  String _pctText(int received, int total) {
    if (total <= 0) return '${(received / 1024).toStringAsFixed(0)} KB';
    final pct = (received * 100) ~/ total;
    return '$pct%';
  }

  @override
  Widget build(BuildContext context) {
    final dl    = context.watch<DownloadProvider>();
    final theme = Theme.of(context);
    if (!dl.isBusy && dl.recentJobs.isEmpty) {
      return const SizedBox.shrink();
    }

    // Group active jobs by batchLabel so a 12-track album collapses to a
    // single line ("Album: Foo — 3/12, 28%") instead of 12 noisy rows.
    final byBatch = <String, List<DownloadJob>>{};
    final loose   = <DownloadJob>[];
    for (final j in dl.activeJobs) {
      final l = j.batchLabel;
      if (l == null) {
        loose.add(j);
      } else {
        (byBatch[l] ??= []).add(j);
      }
    }

    final rows = <Widget>[];
    byBatch.forEach((label, jobs) {
      final running = jobs.where((j) =>
          j.status == DownloadStatus.running).length;
      final done = jobs.where((j) =>
          j.status == DownloadStatus.done).length;
      final pendingOrRunning = jobs.length;
      final receivedSum = jobs.fold<int>(0, (a, j) => a + j.received);
      final totalSum    = jobs.fold<int>(0, (a, j) => a + j.total);
      final progress = totalSum > 0 ? receivedSum / totalSum : null;
      rows.add(_BannerRow(
        primary:   label,
        secondary: '${done + running}/${jobs.length + done} '
                   '— ${_pctText(receivedSum, totalSum)}',
        progress:  progress,
        showHint:  pendingOrRunning == 0 ? null : null,
      ));
    });
    for (final j in loose) {
      rows.add(_BannerRow(
        primary:   _shortLabel(j),
        secondary: _pctText(j.received, j.total),
        progress:  j.total > 0 ? j.received / j.total : null,
      ));
    }
    // Trailing line for the last finished job (good or bad) so the user
    // sees the outcome without watching the banner the whole time.
    if (!dl.isBusy && dl.recentJobs.isNotEmpty) {
      final last = dl.recentJobs.first;
      rows.add(_BannerRow(
        primary:   _shortLabel(last),
        secondary: last.status == DownloadStatus.done
            ? 'Done'
            : 'Failed: ${last.error ?? "unknown"}',
        progress:  last.status == DownloadStatus.done ? 1.0 : 0.0,
        success:   last.status == DownloadStatus.done,
        failure:   last.status == DownloadStatus.failed,
      ));
    }

    return Material(
      color: theme.colorScheme.surface,
      child: Container(
        decoration: BoxDecoration(
          color: theme.colorScheme.surfaceContainerHighest,
          border: Border(
            top: BorderSide(color: theme.dividerColor, width: 0.5),
          ),
        ),
        padding: const EdgeInsets.fromLTRB(12, 6, 4, 6),
        child: Row(
          children: [
            Expanded(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.stretch,
                children: rows,
              ),
            ),
            if (!dl.isBusy)
              IconButton(
                tooltip: 'Dismiss',
                iconSize: 18,
                padding: EdgeInsets.zero,
                constraints: const BoxConstraints(
                  minWidth: 28, minHeight: 28),
                icon: const Icon(Icons.close),
                onPressed: dl.clearRecent,
              ),
          ],
        ),
      ),
    );
  }
}

class _BannerRow extends StatelessWidget {
  const _BannerRow({
    required this.primary,
    required this.secondary,
    required this.progress,
    this.showHint,
    this.success = false,
    this.failure = false,
  });
  final String  primary;
  final String  secondary;
  /// 0–1 progress; null = indeterminate.
  final double? progress;
  final String? showHint;
  final bool    success;
  final bool    failure;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final color = failure
        ? theme.colorScheme.error
        : success
            ? Colors.green
            : theme.colorScheme.primary;
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 2),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          Row(
            children: [
              Icon(
                failure
                    ? Icons.error_outline
                    : success
                        ? Icons.check_circle_outline
                        : Icons.cloud_download_outlined,
                size: 14, color: color,
              ),
              const SizedBox(width: 6),
              Expanded(
                child: Text(
                  primary,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.bodySmall?.copyWith(
                    fontWeight: FontWeight.w600),
                ),
              ),
              const SizedBox(width: 8),
              Text(
                secondary,
                style: theme.textTheme.bodySmall?.copyWith(color: color),
              ),
            ],
          ),
          const SizedBox(height: 2),
          ClipRRect(
            borderRadius: BorderRadius.circular(2),
            child: LinearProgressIndicator(
              value: progress,
              minHeight: 3,
              color: color,
              backgroundColor: theme.colorScheme.surfaceVariant,
            ),
          ),
        ],
      ),
    );
  }
}

// ---- Mini player -------------------------------------------------------

class _MiniPlayer extends StatefulWidget {
  const _MiniPlayer({required this.player, required this.song});

  final PlayerProvider player;
  final Song?          song;

  @override
  State<_MiniPlayer> createState() => _MiniPlayerState();
}

class _MiniPlayerState extends State<_MiniPlayer> {
  // Non-null while the user is dragging the seek slider — overrides the
  // live position from PlayerProvider so the thumb stays where the
  // finger is even though `playing` keeps emitting new positions.
  double? _dragPositionMs;

  @override
  void initState() {
    super.initState();
    // Repaint the lyrics button when an availability lookup lands (the
    // glow). check() itself is a map hit on the rebuilds in between.
    LyricsAvailability.instance.addListener(_onLyricsAvailability);
  }

  @override
  void dispose() {
    LyricsAvailability.instance.removeListener(_onLyricsAvailability);
    super.dispose();
  }

  void _onLyricsAvailability() {
    if (mounted) setState(() {});
  }

  String _fmt(int ms) {
    if (ms <= 0) return '0:00';
    final total = ms ~/ 1000;
    final m = (total ~/ 60).toString();
    final s = (total % 60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  @override
  Widget build(BuildContext context) {
    final player    = widget.player;
    final song      = widget.song;
    final isPlaying = player.state == PlayerState.playing;
    final theme     = Theme.of(context);
    final idle      = song == null;

    // Dynamic accent derived from the current track's generated cover art —
    // the bar re-tints per song, matching the web player.
    final Color accent;
    if (idle) {
      accent = theme.colorScheme.primary;
    } else {
      final p = artParams(seedFromHash(song.contentHash));
      accent = hslColor(p.h1, 75, 62);
    }

    // Use the player's EFFECTIVE duration, not the Song's. A foreign
    // network preview has no chain metadata (durationMs 0), which left
    // sliderMax pinned to the live position — so the handle sat at the
    // end and every seek clamped to 0. PlayerProvider.durationMs prefers
    // the decoder's own value and falls back to the track metadata.
    final durationMs = idle ? 0 : player.durationMs;
    final livePosMs  = player.positionMs.clamp(0, durationMs == 0
        ? player.positionMs
        : durationMs);
    final sliderMs   = _dragPositionMs ?? livePosMs.toDouble();
    final sliderMax  = durationMs > 0
        ? durationMs.toDouble()
        : (livePosMs == 0 ? 1.0 : livePosMs.toDouble());

    return Container(
      decoration: BoxDecoration(
        color: theme.colorScheme.surfaceVariant,
        border: Border(top: BorderSide(color: theme.dividerColor, width: 0.5)),
      ),
      padding: const EdgeInsets.fromLTRB(12, 4, 12, 4),
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          // ---- Seek bar with elapsed / remaining labels ------------
          Row(
            children: [
              SizedBox(
                width: 38,
                child: Text(
                  _fmt(idle ? 0 : livePosMs),
                  style: theme.textTheme.bodySmall,
                  textAlign: TextAlign.center,
                ),
              ),
              Expanded(
                child: Stack(
                  alignment: Alignment.center,
                  children: [
                    // How much of the file is actually downloaded, drawn
                    // behind the handle. Without it, scrubbing past the
                    // downloaded edge of a still-arriving file just stalls
                    // with no indication of why.
                    if (player.bufferedFraction != null && !idle)
                      Padding(
                        padding: const EdgeInsets.symmetric(horizontal: 24),
                        child: SizedBox(
                          height: 3,
                          child: ClipRRect(
                            borderRadius: BorderRadius.circular(2),
                            child: LinearProgressIndicator(
                              value: player.bufferedFraction!.clamp(0.0, 1.0),
                              minHeight: 3,
                              backgroundColor: Colors.transparent,
                              valueColor: AlwaysStoppedAnimation(
                                  accent.withValues(alpha: .30)),
                            ),
                          ),
                        ),
                      ),
                    SliderTheme(
                  data: SliderTheme.of(context).copyWith(
                    trackHeight: 3,
                    activeTrackColor: accent,
                    thumbColor: accent,
                    thumbShape: const RoundSliderThumbShape(
                        enabledThumbRadius: 6),
                    overlayShape: const RoundSliderOverlayShape(
                        overlayRadius: 10),
                    // Let the buffered bar show through the unplayed part.
                    inactiveTrackColor: player.bufferedFraction == null
                        ? null
                        : theme.colorScheme.onSurface.withValues(alpha: .12),
                  ),
                  child: Slider(
                    min: 0,
                    max: sliderMax,
                    value: sliderMs.clamp(0, sliderMax),
                    onChanged: idle ? null : (v) {
                      setState(() => _dragPositionMs = v);
                    },
                    onChangeEnd: idle ? null : (v) async {
                      await player.seek(v.round());
                      setState(() => _dragPositionMs = null);
                    },
                  ),
                    ),
                  ],
                ),
              ),
              SizedBox(
                width: 38,
                child: Text(
                  _fmt(durationMs),
                  style: theme.textTheme.bodySmall,
                  textAlign: TextAlign.center,
                ),
              ),
            ],
          ),
          // ---- Title / artist / play count + transport controls -----
          Row(
            children: [
              if (!idle) ...[
                ClipRRect(
                  borderRadius: BorderRadius.circular(8),
                  // A preview's real cover (fetched from Cover Art Archive
                  // as it buffers) wins; otherwise the usual resolution
                  // (node-scraped cover, else generated art) applies.
                  child: player.previewArtBytes != null
                      ? Image.memory(
                          player.previewArtBytes!,
                          width: 40,
                          height: 40,
                          fit: BoxFit.cover,
                          gaplessPlayback: true,
                          errorBuilder: (_, __, ___) => CoverArt(
                              seed: seedFromHash(song.contentHash), size: 40),
                        )
                      : AlbumArt(
                          seed: seedFromHash(song.contentHash), size: 40,
                          artist: song.artist, album: song.album),
                ),
                const SizedBox(width: 10),
              ],
              Expanded(
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      idle
                          ? 'Nothing playing'
                          : (song.title.isEmpty ? 'Unknown' : song.title),
                      style: theme.textTheme.bodyMedium?.copyWith(
                          fontWeight: FontWeight.w600,
                          color: idle
                              ? theme.colorScheme.onSurface.withOpacity(0.5)
                              : null),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                    ),
                    if (!idle)
                      Row(
                        children: [
                          if (song.artist.isNotEmpty) ...[
                            Flexible(
                              child: Text(
                                song.artist,
                                style: theme.textTheme.bodySmall,
                                maxLines: 1,
                                overflow: TextOverflow.ellipsis,
                              ),
                            ),
                            const SizedBox(width: 6),
                            Text('•', style: theme.textTheme.bodySmall),
                            const SizedBox(width: 6),
                          ],
                          Icon(Icons.play_circle_outline,
                              size: 12,
                              color:
                                  theme.colorScheme.onSurface.withOpacity(0.6)),
                          const SizedBox(width: 2),
                          Text(
                            '${song.playCount} plays',
                            style: theme.textTheme.bodySmall,
                          ),
                        ],
                      ),
                  ],
                ),
              ),
              // Lyrics live in the play bar (not the track rows) so they
              // are reachable for WHATEVER is playing — Discover, search,
              // local library, even a foreign-network preview — from
              // anywhere in the app. Disabled when idle, matching the
              // transport controls around it. The button GLOWS (filled
              // icon, accent, soft halo) when lyrics actually resolved
              // for this track — local cache or node, cached per song.
              Builder(builder: (context) {
                final req =
                    song == null ? null : LyricsRequest.fromSong(song);
                final has = req != null &&
                    (LyricsAvailability.instance.check(req) ?? false);
                return IconButton(
                  iconSize: 20,
                  tooltip: has
                      ? 'Lyrics available'
                      : 'No lyrics for this track',
                  color: has ? accent : null,
                  icon: has
                      ? Icon(Icons.lyrics, shadows: [
                          Shadow(
                              color: accent.withValues(alpha: .9),
                              blurRadius: 12),
                        ])
                      : const Icon(Icons.lyrics_outlined),
                  // Only clickable once it GLOWS. The glow means lyrics
                  // actually resolved for this track, so an enabled button
                  // can never open an empty panel — while a lookup is still
                  // in flight check() returns null and this stays disabled,
                  // then the availability listener repaints it the moment
                  // the answer lands.
                  onPressed: (req == null || !has)
                      ? null
                      : () => openLyrics(context, req),
                );
              }),
              IconButton(
                iconSize: 20,
                icon: const Icon(Icons.skip_previous),
                onPressed: idle ? null : () => player.playPrev(),
              ),
              IconButton(
                iconSize: 28,
                color: idle ? null : accent,
                icon: Icon(isPlaying ? Icons.pause_circle_filled
                                      : Icons.play_circle_filled),
                onPressed: idle ? null : () => player.togglePlayPause(),
              ),
              IconButton(
                iconSize: 20,
                icon: const Icon(Icons.skip_next),
                onPressed: idle ? null : () => player.playNext(),
              ),
              IconButton(
                iconSize: 20,
                icon: const Icon(Icons.stop),
                onPressed: idle ? null : () => player.stop(),
              ),
            ],
          ),
        ],
      ),
    );
  }
}
