// Discover — the Spotify-shaped home for the chain catalog.
//
// Top-level switch: Home | Browse.
//   * Home   — vertical scroll of node-curated carousels (collections.list:
//              Rising / Top 50 / New Releases / per-genre / per-year), a hero
//              for the #1 Rising track, and a genre tile strip. Collections
//              are computed DETERMINISTICALLY on the full node from on-chain
//              data, so every honest node serves the same rows.
//   * Browse — the classic facet drill (LibraryScreen), unchanged.
//
// Cover art: no image assets — a deterministic integer-only algorithm turns
// the first 8 bytes of a song's content hash (or an FNV-1a hash of a facet
// name) into a two-hue gradient + geometric motif. The web player runs the
// SAME algorithm (coverArt() in webplayer/frontend/app.js), so identical
// hashes render identical art on both clients. Keep the two in lockstep.

import 'dart:async';
import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/collection.dart';
import '../models/song.dart';
import '../providers/library_provider.dart';
import '../providers/player_provider.dart';
import '../providers/wallet_provider.dart';
import '../services/node_client.dart';
import '../services/node_service.dart';
import '../widgets/album_art.dart';
import '../widgets/cover_art.dart';
import 'library_screen.dart';

class DiscoverScreen extends StatefulWidget {
  const DiscoverScreen({super.key});

  @override
  State<DiscoverScreen> createState() => _DiscoverScreenState();
}

class _DiscoverScreenState extends State<DiscoverScreen> {
  int _tab = 0; // 0 = Home, 1 = Browse

  // Topbar search (parity with the web player): debounced, server-side via
  // songs.search, results replace the body as a list with Play all/Shuffle.
  final _searchCtrl = TextEditingController();
  Timer? _searchDebounce;
  String _query = '';

  @override
  void dispose() {
    _searchDebounce?.cancel();
    _searchCtrl.dispose();
    super.dispose();
  }

  void _onSearchChanged(String v) {
    _searchDebounce?.cancel();
    _searchDebounce = Timer(const Duration(milliseconds: 300), () {
      if (mounted) setState(() => _query = v.trim());
    });
  }

  @override
  Widget build(BuildContext context) {
    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
          child: Row(
            children: [
              SegmentedButton<int>(
                segments: const [
                  ButtonSegment(value: 0,
                      icon: Icon(Icons.auto_awesome, size: 16),
                      label: Text('Home')),
                  ButtonSegment(value: 1,
                      icon: Icon(Icons.grid_view, size: 16),
                      label: Text('Browse')),
                ],
                selected: {_tab},
                showSelectedIcon: false,
                onSelectionChanged: (s) => setState(() => _tab = s.first),
              ),
              const SizedBox(width: 8),
              Expanded(
                child: SizedBox(
                  height: 38,
                  child: TextField(
                    controller: _searchCtrl,
                    onChanged: _onSearchChanged,
                    textInputAction: TextInputAction.search,
                    decoration: InputDecoration(
                      hintText: 'Search…',
                      prefixIcon: const Icon(Icons.search, size: 18),
                      suffixIcon: _query.isEmpty
                          ? null
                          : IconButton(
                              icon: const Icon(Icons.close, size: 16),
                              onPressed: () {
                                _searchCtrl.clear();
                                _searchDebounce?.cancel();
                                setState(() => _query = '');
                              },
                            ),
                      isDense: true,
                      contentPadding:
                          const EdgeInsets.symmetric(horizontal: 12),
                      border: OutlineInputBorder(
                        borderRadius: BorderRadius.circular(999),
                      ),
                    ),
                  ),
                ),
              ),
            ],
          ),
        ),
        Expanded(
          child: _query.isNotEmpty
              ? _SearchResultsView(query: _query)
              : IndexedStack(
                  index: _tab,
                  children: const [
                    _DiscoverHome(),
                    LibraryScreen(),
                  ],
                ),
        ),
      ],
    );
  }
}

// ─────────────────────────── Home ───────────────────────────

class _DiscoverHome extends StatefulWidget {
  const _DiscoverHome();

  @override
  State<_DiscoverHome> createState() => _DiscoverHomeState();
}

class _DiscoverHomeState extends State<_DiscoverHome> {
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addPostFrameCallback((_) {
      final lib = context.read<LibraryProvider>();
      lib.loadCollections();
      if (lib.songs.isEmpty) lib.refresh();
    });
    // Collections only change at epoch boundaries; a slow poll keeps a
    // long-lived session fresh without hammering the relay.
    _timer = Timer.periodic(const Duration(seconds: 90), (_) {
      if (mounted) context.read<LibraryProvider>().loadCollections();
    });
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  void _play(BuildContext context, List<Song> queue, int index) {
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    context.read<PlayerProvider>().playPlaylist(queue, index, wallet.address);
  }

  /// Queue for a collection: node order, availability-overlaid catalog rows,
  /// unseeded members skipped (they're shown dimmed but can't stream).
  List<Song> _playableQueue(LibraryProvider lib, SongCollection c) => [
        for (final s in c.songs)
          if (lib.onlineSong(s.contentHash) != null)
            lib.onlineSong(s.contentHash)!,
      ];

  void _playFromCollection(
      BuildContext context, LibraryProvider lib, SongCollection c, Song tapped) {
    final queue = _playableQueue(lib, c);
    if (queue.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
          content: Text('No seeders online for this row right now')));
      return;
    }
    var idx = queue.indexWhere((s) => s.contentHash == tapped.contentHash);
    if (idx < 0) idx = 0;
    _play(context, queue, idx);
  }

  void _openCollection(BuildContext context, SongCollection c) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => CollectionScreen(collection: c),
    ));
  }

  @override
  Widget build(BuildContext context) {
    final lib   = context.watch<LibraryProvider>();
    final set   = lib.collections;
    final theme = Theme.of(context);

    final rows = (set?.collections ?? const <SongCollection>[])
        .where((c) => c.songs.isNotEmpty)
        .toList();

    if (rows.isEmpty) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              if (lib.collectionsLoading)
                const CircularProgressIndicator()
              else
                Icon(Icons.explore_outlined,
                    size: 48, color: theme.colorScheme.onSurface.withOpacity(.4)),
              const SizedBox(height: 14),
              Text(
                lib.collectionsLoading
                    ? 'Loading the network’s picks…'
                    : (lib.collectionsError ??
                        'The network hasn’t curated anything yet.'),
                textAlign: TextAlign.center,
                style: theme.textTheme.bodyMedium,
              ),
              if (!lib.collectionsLoading) ...[
                const SizedBox(height: 12),
                OutlinedButton.icon(
                  onPressed: () => lib.loadCollections(),
                  icon: const Icon(Icons.refresh, size: 16),
                  label: const Text('Retry'),
                ),
              ],
            ],
          ),
        ),
      );
    }

    // Display order: rising, top, new, years, genres (tiles jump to genres).
    const order = ['rising', 'top', 'new', 'year', 'genre'];
    rows.sort((a, b) =>
        order.indexOf(a.kind).compareTo(order.indexOf(b.kind)));
    final genres = rows.where((c) => c.kind == 'genre').toList();
    final rising = rows.firstWhere((c) => c.kind == 'rising',
        orElse: () => rows.first);

    return RefreshIndicator(
      onRefresh: () async {
        final l = context.read<LibraryProvider>();
        await Future.wait([l.refresh(), l.loadCollections()]);
      },
      child: ListView(
        padding: const EdgeInsets.fromLTRB(12, 8, 12, 24),
        children: [
          _HeroCard(
            collection: rising,
            lib: lib,
            onPlay: (c, s) => _playFromCollection(context, lib, c, s),
            onMore: (c) => _openCollection(context, c),
          ),
          if (genres.isNotEmpty) ...[
            const SizedBox(height: 18),
            Text('Genres',
                style: theme.textTheme.titleMedium
                    ?.copyWith(fontWeight: FontWeight.w800)),
            const SizedBox(height: 8),
            SizedBox(
              height: 72,
              child: ListView.separated(
                scrollDirection: Axis.horizontal,
                itemCount: genres.length,
                separatorBuilder: (_, __) => const SizedBox(width: 10),
                itemBuilder: (_, i) => _GenreTile(
                  collection: genres[i],
                  onTap: () => _openCollection(context, genres[i]),
                ),
              ),
            ),
          ],
          const SizedBox(height: 6),
          for (final c in rows) ...[
            const SizedBox(height: 14),
            _CarouselRow(
              collection: c,
              lib: lib,
              onTapSong: (s) => _playFromCollection(context, lib, c, s),
              onSeeAll: () => _openCollection(context, c),
            ),
          ],
          const SizedBox(height: 18),
          if (set != null && set.contentDigest.isNotEmpty)
            Center(
              child: Text(
                'curated deterministically by the network · epoch ${set.epoch}'
                ' · digest ${set.contentDigest.substring(0, math.min(16, set.contentDigest.length))}…',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: theme.colorScheme.onSurface.withOpacity(.45),
                ),
                textAlign: TextAlign.center,
              ),
            ),
        ],
      ),
    );
  }
}

// ─────────────────────────── Hero ───────────────────────────

class _HeroCard extends StatelessWidget {
  const _HeroCard({
    required this.collection,
    required this.lib,
    required this.onPlay,
    required this.onMore,
  });

  final SongCollection collection;
  final LibraryProvider lib;
  final void Function(SongCollection, Song) onPlay;
  final void Function(SongCollection) onMore;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    // Feature the first currently-streamable song of the row.
    Song? hero;
    for (final s in collection.songs) {
      if (lib.onlineSong(s.contentHash) != null) { hero = s; break; }
    }
    hero ??= collection.songs.isNotEmpty ? collection.songs.first : null;
    if (hero == null) return const SizedBox.shrink();
    final live = lib.onlineSong(hero.contentHash) ?? hero;

    final p = artParams(seedFromHash(hero.contentHash));
    return Container(
      padding: const EdgeInsets.all(16),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: theme.dividerColor.withOpacity(.4)),
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [
            hslColor(p.h1, 45, 16),
            hslColor(p.h2, 45, 12),
          ],
        ),
      ),
      child: Row(
        children: [
          ClipRRect(
            borderRadius: BorderRadius.circular(12),
            child: AlbumArt(seed: seedFromHash(hero.contentHash), size: 104,
                artist: hero.artist, album: hero.album),
          ),
          const SizedBox(width: 14),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  '✦ RISING — EVERY LISTEN PAYS THE ARTIST IN FULL',
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.primary,
                    fontWeight: FontWeight.w800,
                    letterSpacing: 1.1,
                  ),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 4),
                Text(
                  hero.title.isEmpty ? '(untitled)' : hero.title,
                  style: theme.textTheme.titleLarge
                      ?.copyWith(fontWeight: FontWeight.w800),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                Text(
                  '${hero.artist}${live.playCount > 0 ? ' · ${live.playCount} plays' : ''}',
                  style: theme.textTheme.bodySmall,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 10),
                Row(
                  children: [
                    FilledButton.icon(
                      onPressed: () => onPlay(collection, hero!),
                      icon: const Icon(Icons.play_arrow, size: 18),
                      label: const Text('Play'),
                      style: FilledButton.styleFrom(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 16, vertical: 8),
                        visualDensity: VisualDensity.compact,
                      ),
                    ),
                    const SizedBox(width: 8),
                    OutlinedButton(
                      onPressed: () => onMore(collection),
                      style: OutlinedButton.styleFrom(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 14, vertical: 8),
                        visualDensity: VisualDensity.compact,
                      ),
                      child: const Text('Explore'),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ],
      ),
    );
  }
}

// ─────────────────────── Carousel row ───────────────────────

class _CarouselRow extends StatelessWidget {
  const _CarouselRow({
    required this.collection,
    required this.lib,
    required this.onTapSong,
    required this.onSeeAll,
  });

  final SongCollection collection;
  final LibraryProvider lib;
  final void Function(Song) onTapSong;
  final VoidCallback onSeeAll;

  @override
  Widget build(BuildContext context) {
    final theme   = Theme.of(context);
    final playing = context.select<PlayerProvider, String>(
        (p) => p.currentSong?.contentHash ?? '');
    final songs   = collection.songs.take(20).toList();

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Row(
          crossAxisAlignment: CrossAxisAlignment.baseline,
          textBaseline: TextBaseline.alphabetic,
          children: [
            ShaderMask(
              shaderCallback: (r) => LinearGradient(colors: [
                theme.colorScheme.primary,
                theme.colorScheme.tertiary,
              ]).createShader(r),
              child: Text(
                collection.title,
                style: theme.textTheme.titleMedium?.copyWith(
                    fontWeight: FontWeight.w800, color: Colors.white),
              ),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: Text(
                collection.subtitle,
                style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurface.withOpacity(.55)),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ),
            TextButton(
              onPressed: onSeeAll,
              style: TextButton.styleFrom(
                visualDensity: VisualDensity.compact,
                padding: const EdgeInsets.symmetric(horizontal: 8),
              ),
              child: const Text('See all ›'),
            ),
          ],
        ),
        const SizedBox(height: 6),
        SizedBox(
          height: 196,
          child: ListView.separated(
            scrollDirection: Axis.horizontal,
            itemCount: songs.length,
            separatorBuilder: (_, __) => const SizedBox(width: 12),
            itemBuilder: (_, i) => _SongCard(
              song: songs[i],
              live: lib.onlineSong(songs[i].contentHash),
              playing: playing == songs[i].contentHash,
              onTap: () => onTapSong(songs[i]),
            ),
          ),
        ),
      ],
    );
  }
}

class _SongCard extends StatelessWidget {
  const _SongCard({
    required this.song,
    required this.live,
    required this.playing,
    required this.onTap,
  });

  final Song  song;
  final Song? live;     // catalog overlay row; null = no seeders right now
  final bool  playing;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final off   = live == null;
    final shown = live ?? song;
    final rising = shown.playCount > 0 && shown.playCount < 10000;

    return Opacity(
      opacity: off ? .45 : 1,
      child: InkWell(
        onTap: () {
          if (off) {
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
                content: Text('No seeders online for this track right now')));
            return;
          }
          onTap();
        },
        borderRadius: BorderRadius.circular(12),
        child: Container(
          width: 136,
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            color: theme.colorScheme.surfaceContainerHighest.withOpacity(.35),
            border: Border.all(
              color: playing
                  ? theme.colorScheme.primary
                  : Colors.transparent,
            ),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Stack(
                children: [
                  ClipRRect(
                    borderRadius: BorderRadius.circular(9),
                    child: AlbumArt(
                        seed: seedFromHash(song.contentHash), size: 120,
                        artist: song.artist, album: song.album),
                  ),
                  if (off)
                    Positioned(
                      top: 6, left: 6,
                      child: Container(
                        padding: const EdgeInsets.symmetric(
                            horizontal: 7, vertical: 2),
                        decoration: BoxDecoration(
                          color: Colors.black54,
                          borderRadius: BorderRadius.circular(999),
                        ),
                        child: const Text('offline',
                            style: TextStyle(
                                fontSize: 9, fontWeight: FontWeight.w700)),
                      ),
                    ),
                  if (playing)
                    Positioned(
                      right: 6, bottom: 6,
                      child: CircleAvatar(
                        radius: 13,
                        backgroundColor: theme.colorScheme.primary,
                        child: Icon(Icons.graphic_eq,
                            size: 15, color: theme.colorScheme.onPrimary),
                      ),
                    ),
                ],
              ),
              const SizedBox(height: 6),
              Text(
                song.title.isEmpty ? '(untitled)' : song.title,
                style: theme.textTheme.bodySmall
                    ?.copyWith(fontWeight: FontWeight.w700),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
              Text(
                song.artist,
                style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurface.withOpacity(.6)),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
              const Spacer(),
              Row(
                children: [
                  if (rising) ...[
                    Container(
                      width: 6, height: 6,
                      decoration: BoxDecoration(
                        shape: BoxShape.circle,
                        color: theme.colorScheme.primary,
                      ),
                    ),
                    const SizedBox(width: 4),
                  ],
                  Text(
                    '${shown.playCount} plays',
                    style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.onSurface.withOpacity(.5)),
                  ),
                ],
              ),
            ],
          ),
        ),
      ),
    );
  }
}

class _GenreTile extends StatelessWidget {
  const _GenreTile({required this.collection, required this.onTap});

  final SongCollection collection;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final p = artParams(seedFromName(collection.facet));
    final label = collection.title.replaceFirst('Best of ', '');
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Container(
        width: 124,
        padding: const EdgeInsets.all(10),
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(12),
          gradient: LinearGradient(
            begin: Alignment.topLeft,
            end: Alignment.bottomRight,
            colors: [hslColor(p.h1, 60, 30), hslColor(p.h2, 70, 42)],
          ),
        ),
        child: Stack(
          children: [
            Positioned(
              top: 0, right: 0,
              child: Container(
                padding:
                    const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                decoration: BoxDecoration(
                  color: Colors.black38,
                  borderRadius: BorderRadius.circular(999),
                ),
                child: Text('${collection.songs.length}',
                    style: const TextStyle(
                        fontSize: 10, fontWeight: FontWeight.w700)),
              ),
            ),
            Align(
              alignment: Alignment.bottomLeft,
              child: Text(
                label,
                style: const TextStyle(
                  fontWeight: FontWeight.w800,
                  fontSize: 13.5,
                  color: Colors.white,
                  shadows: [Shadow(blurRadius: 6, color: Colors.black54)],
                ),
                maxLines: 2,
                overflow: TextOverflow.ellipsis,
              ),
            ),
          ],
        ),
      ),
    );
  }
}

// ───────────────────── Collection detail ─────────────────────

class CollectionScreen extends StatelessWidget {
  const CollectionScreen({super.key, required this.collection});

  final SongCollection collection;

  void _play(BuildContext context, List<Song> queue, int index) {
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    context.read<PlayerProvider>().playPlaylist(queue, index, wallet.address);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final lib   = context.watch<LibraryProvider>();
    final playing = context.select<PlayerProvider, String>(
        (p) => p.currentSong?.contentHash ?? '');

    final queue = <Song>[
      for (final s in collection.songs)
        if (lib.onlineSong(s.contentHash) != null)
          lib.onlineSong(s.contentHash)!,
    ];
    final seed = collection.facet.isNotEmpty
        ? seedFromName(collection.facet)
        : seedFromName(collection.id);

    return Scaffold(
      appBar: AppBar(title: Text(collection.title)),
      body: ListView(
        padding: const EdgeInsets.fromLTRB(14, 10, 14, 24),
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              ClipRRect(
                borderRadius: BorderRadius.circular(12),
                child: CoverArt(seed: seed, size: 104),
              ),
              const SizedBox(width: 14),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(collection.subtitle,
                        style: theme.textTheme.bodySmall),
                    Text('${collection.songs.length} songs · ${queue.length} streamable',
                        style: theme.textTheme.labelSmall?.copyWith(
                            color:
                                theme.colorScheme.onSurface.withOpacity(.55))),
                    const SizedBox(height: 10),
                    Row(
                      children: [
                        FilledButton.icon(
                          onPressed: queue.isEmpty
                              ? null
                              : () => _play(context, queue, 0),
                          icon: const Icon(Icons.play_arrow, size: 18),
                          label: const Text('Play all'),
                        ),
                        const SizedBox(width: 8),
                        OutlinedButton.icon(
                          onPressed: queue.isEmpty
                              ? null
                              : () {
                                  final shuffled = List.of(queue)..shuffle();
                                  _play(context, shuffled, 0);
                                },
                          icon: const Icon(Icons.shuffle, size: 16),
                          label: const Text('Shuffle'),
                        ),
                      ],
                    ),
                  ],
                ),
              ),
            ],
          ),
          const SizedBox(height: 14),
          for (var i = 0; i < collection.songs.length; i++)
            _trackRow(context, theme, lib, playing, i, queue),
        ],
      ),
    );
  }

  Widget _trackRow(BuildContext context, ThemeData theme, LibraryProvider lib,
      String playing, int i, List<Song> queue) {
    final s     = collection.songs[i];
    final live  = lib.onlineSong(s.contentHash);
    final off   = live == null;
    final shown = live ?? s;
    final isPlaying = playing == s.contentHash;

    return Opacity(
      opacity: off ? .45 : 1,
      child: ListTile(
        dense: true,
        contentPadding: const EdgeInsets.symmetric(horizontal: 4),
        leading: SizedBox(
          width: 62,
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              SizedBox(
                width: 20,
                child: Text('${i + 1}',
                    textAlign: TextAlign.right,
                    style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.onSurface.withOpacity(.5))),
              ),
              const SizedBox(width: 8),
              ClipRRect(
                borderRadius: BorderRadius.circular(6),
                child:
                    AlbumArt(seed: seedFromHash(s.contentHash), size: 34,
                        artist: s.artist, album: s.album),
              ),
            ],
          ),
        ),
        title: Text(
          s.title.isEmpty ? '(untitled)' : s.title,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: TextStyle(
            fontWeight: isPlaying ? FontWeight.w700 : FontWeight.w500,
            color: isPlaying ? theme.colorScheme.primary : null,
          ),
        ),
        subtitle: Text(s.artist, maxLines: 1, overflow: TextOverflow.ellipsis),
        trailing: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('${shown.playCount} plays', style: theme.textTheme.labelSmall),
            const SizedBox(width: 10),
            Text(shown.durationFormatted, style: theme.textTheme.labelSmall),
            const SizedBox(width: 4),
            Icon(
              off
                  ? Icons.cloud_off
                  : (isPlaying ? Icons.graphic_eq : Icons.play_arrow),
              size: 18,
              color: off
                  ? theme.colorScheme.onSurface.withOpacity(.4)
                  : theme.colorScheme.primary,
            ),
          ],
        ),
        onTap: () {
          if (off) {
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
                content: Text('No seeders online for this track right now')));
            return;
          }
          var idx = queue.indexWhere((q) => q.contentHash == s.contentHash);
          if (idx < 0) idx = 0;
          _play(context, queue, idx);
        },
      ),
    );
  }
}

// ─────────────────────── Search results ───────────────────────
// Server-side search (songs.search on the full node) rendered as the same
// list surface the web player uses: big generated art, Play all / Shuffle,
// track rows with availability dimming from the live catalog.

class _SearchResultsView extends StatefulWidget {
  const _SearchResultsView({required this.query});
  final String query;

  @override
  State<_SearchResultsView> createState() => _SearchResultsViewState();
}

class _SearchResultsViewState extends State<_SearchResultsView> {
  List<Song>? _results;   // null = loading
  String?     _error;
  int         _reqSeq = 0;

  @override
  void initState() {
    super.initState();
    _run();
  }

  @override
  void didUpdateWidget(_SearchResultsView old) {
    super.didUpdateWidget(old);
    if (old.query != widget.query) _run();
  }

  Future<void> _run() async {
    final seq = ++_reqSeq;
    setState(() { _results = null; _error = null; });
    try {
      final pid = await NodeService.getRatsPeerId(
          waitFor: const Duration(seconds: 8));
      if (pid.isEmpty) throw StateError('No full node discovered yet.');
      final songs =
          await NodeClient(ratsPeerId: pid).searchSongs(widget.query);
      if (!mounted || seq != _reqSeq) return;   // superseded while in flight
      // Most-played first, like the web search.
      songs.sort((a, b) => b.playCount.compareTo(a.playCount));
      setState(() => _results = songs);
    } catch (e) {
      if (!mounted || seq != _reqSeq) return;
      setState(() { _error = e.toString(); _results = const []; });
    }
  }

  void _play(BuildContext context, List<Song> queue, int index) {
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    context.read<PlayerProvider>().playPlaylist(queue, index, wallet.address);
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final lib   = context.watch<LibraryProvider>();
    final playing = context.select<PlayerProvider, String>(
        (p) => p.currentSong?.contentHash ?? '');

    if (_results == null) {
      return const Center(child: CircularProgressIndicator());
    }
    final results = _results!;
    // Availability overlay from the live catalog — dim, don't drop.
    final queue = <Song>[
      for (final s in results)
        if (lib.onlineSong(s.contentHash) != null)
          lib.onlineSong(s.contentHash)!,
    ];

    return ListView(
      padding: const EdgeInsets.fromLTRB(14, 10, 14, 24),
      children: [
        Row(
          crossAxisAlignment: CrossAxisAlignment.end,
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(12),
              child: CoverArt(seed: seedFromName(widget.query), size: 88),
            ),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text('SEARCH',
                      style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.primary,
                        fontWeight: FontWeight.w800,
                        letterSpacing: 1.1,
                      )),
                  Text('“${widget.query}”',
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis,
                      style: theme.textTheme.titleLarge
                          ?.copyWith(fontWeight: FontWeight.w800)),
                  Text(
                    _error != null
                        ? 'Search failed: $_error'
                        : '${results.length} result${results.length == 1 ? '' : 's'}'
                          ' · ${queue.length} streamable',
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                    style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.onSurface.withOpacity(.55)),
                  ),
                  const SizedBox(height: 8),
                  Row(
                    children: [
                      FilledButton.icon(
                        onPressed: queue.isEmpty
                            ? null
                            : () => _play(context, queue, 0),
                        icon: const Icon(Icons.play_arrow, size: 18),
                        label: const Text('Play all'),
                        style: FilledButton.styleFrom(
                          visualDensity: VisualDensity.compact,
                        ),
                      ),
                      const SizedBox(width: 8),
                      OutlinedButton.icon(
                        onPressed: queue.isEmpty
                            ? null
                            : () {
                                final shuffled = List.of(queue)..shuffle();
                                _play(context, shuffled, 0);
                              },
                        icon: const Icon(Icons.shuffle, size: 16),
                        label: const Text('Shuffle'),
                        style: OutlinedButton.styleFrom(
                          visualDensity: VisualDensity.compact,
                        ),
                      ),
                    ],
                  ),
                ],
              ),
            ),
          ],
        ),
        const SizedBox(height: 12),
        if (results.isEmpty && _error == null)
          Padding(
            padding: const EdgeInsets.all(28),
            child: Center(
              child: Text('No matches.',
                  style: theme.textTheme.bodyMedium?.copyWith(
                      color: theme.colorScheme.onSurface.withOpacity(.55))),
            ),
          ),
        for (var i = 0; i < results.length; i++)
          _resultRow(context, theme, lib, playing, results[i], i, queue),
      ],
    );
  }

  Widget _resultRow(BuildContext context, ThemeData theme,
      LibraryProvider lib, String playing, Song s, int i, List<Song> queue) {
    final live  = lib.onlineSong(s.contentHash);
    final off   = live == null;
    final shown = live ?? s;
    final isPlaying = playing == s.contentHash;

    return Opacity(
      opacity: off ? .45 : 1,
      child: ListTile(
        dense: true,
        contentPadding: const EdgeInsets.symmetric(horizontal: 4),
        leading: SizedBox(
          width: 62,
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              SizedBox(
                width: 20,
                child: Text('${i + 1}',
                    textAlign: TextAlign.right,
                    style: theme.textTheme.labelSmall?.copyWith(
                        color: theme.colorScheme.onSurface.withOpacity(.5))),
              ),
              const SizedBox(width: 8),
              ClipRRect(
                borderRadius: BorderRadius.circular(6),
                child:
                    AlbumArt(seed: seedFromHash(s.contentHash), size: 34,
                        artist: s.artist, album: s.album),
              ),
            ],
          ),
        ),
        title: Text(
          s.title.isEmpty ? '(untitled)' : s.title,
          maxLines: 1,
          overflow: TextOverflow.ellipsis,
          style: TextStyle(
            fontWeight: isPlaying ? FontWeight.w700 : FontWeight.w500,
            color: isPlaying ? theme.colorScheme.primary : null,
          ),
        ),
        subtitle: Text('${s.artist}${s.album.isNotEmpty ? ' · ${s.album}' : ''}',
            maxLines: 1, overflow: TextOverflow.ellipsis),
        trailing: Row(
          mainAxisSize: MainAxisSize.min,
          children: [
            Text('${shown.playCount} plays', style: theme.textTheme.labelSmall),
            const SizedBox(width: 10),
            Text(shown.durationFormatted, style: theme.textTheme.labelSmall),
            const SizedBox(width: 4),
            Icon(
              off
                  ? Icons.cloud_off
                  : (isPlaying ? Icons.graphic_eq : Icons.play_arrow),
              size: 18,
              color: off
                  ? theme.colorScheme.onSurface.withOpacity(.4)
                  : theme.colorScheme.primary,
            ),
          ],
        ),
        onTap: () {
          if (off) {
            ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
                content: Text('No seeders online for this track right now')));
            return;
          }
          var idx = queue.indexWhere((q) => q.contentHash == s.contentHash);
          if (idx < 0) idx = 0;
          _play(context, queue, idx);
        },
      ),
    );
  }
}
