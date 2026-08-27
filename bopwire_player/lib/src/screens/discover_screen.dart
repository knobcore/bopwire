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
import '../services/networks/external_network.dart';
import '../services/networks/network_registry.dart';
import '../widgets/external_result_filters.dart';
import '../widgets/external_result_tile.dart';
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
  /// Bumped on every Enter press. The results view keys off this as well
  /// as the query text, so hitting Enter on an UNCHANGED query still
  /// re-runs the search — which is what you want for the foreign
  /// networks, where a retry is how you pick up peers that were slow or
  /// offline a moment ago.
  int _searchNonce = 0;

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
    // Home/Browse + the network toggles + the search field used to share
    // one Row unconditionally. On a portrait phone that is three
    // competing widths in one line, and the search box — the one thing
    // people actually type into — got squeezed down to whatever space
    // the other two left over. Landscape has the width to spare, so it
    // keeps the original single-row layout; portrait gets the search box
    // its own full-width row underneath instead.
    final tabsRow = Row(
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
        // Per-network toggles: turning Soulseek or napstr into a query is
        // a one-tap decision at the moment you search, not a trip into
        // Settings. In the single-row (landscape) layout this sits right
        // beside the search box instead of trailing the tabs.
        const _NetworkToggles(),
      ],
    );

    final searchField = SizedBox(
      height: 38,
      child: TextField(
        controller: _searchCtrl,
        onChanged: _onSearchChanged,
        onSubmitted: (v) {
          _searchDebounce?.cancel();
          setState(() {
            _query = v.trim();
            _searchNonce++;   // force a re-query
          });
        },
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
          contentPadding: const EdgeInsets.symmetric(horizontal: 12),
          border: OutlineInputBorder(
            borderRadius: BorderRadius.circular(999),
          ),
        ),
      ),
    );

    return Column(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
          child: OrientationBuilder(
            builder: (context, orientation) {
              if (orientation == Orientation.landscape) {
                // Unchanged: tabs, search, and network toggles all in one
                // row, exactly as before this change.
                return Row(
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
                    Expanded(child: searchField),
                    const _NetworkToggles(),
                  ],
                );
              }
              // Portrait: tabs + toggles on top, search gets its own
              // full-width row below.
              return Column(
                children: [
                  tabsRow,
                  const SizedBox(height: 8),
                  searchField,
                ],
              );
            },
          ),
        ),
        Expanded(
          child: _query.isNotEmpty
              ? _SearchResultsView(query: _query, nonce: _searchNonce)
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

  /// A copy of [c] containing only members the swarm can currently serve,
  /// resolved to the live catalogue row (which carries the current swarm
  /// size). Returns null when nothing in the collection is online, so the
  /// caller can drop the row.
  SongCollection? _onlineOnly(LibraryProvider lib, SongCollection c) {
    final live = _playableQueue(lib, c);
    if (live.isEmpty) return null;
    return SongCollection(
      id:       c.id,
      kind:     c.kind,
      title:    c.title,
      subtitle: c.subtitle,
      facet:    c.facet,
      songs:    live,
    );
  }

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

    // Discover is "what you can play right now", so offline members are
    // DROPPED rather than dimmed, and a row with nothing playable left is
    // dropped entirely. Dimming meant rows padded out with dead entries
    // that fail on tap, and whole sections that looked full but had
    // nothing streamable in them.
    final rows = [
      for (final c in (set?.collections ?? const <SongCollection>[]))
        if (_onlineOnly(lib, c) case final oc? ) oc,
    ];

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
                  '✦ RISING',
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

    // Online-only, same rule as the Discover rows that opened this. Also
    // what gets RENDERED below, not just what gets queued — otherwise the
    // list shows dead rows the play button silently skips.
    final queue = <Song>[
      for (final s in collection.songs)
        if (lib.onlineSong(s.contentHash) != null)
          lib.onlineSong(s.contentHash)!,
    ];
    final visible = queue;
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
          for (var i = 0; i < visible.length; i++)
            _trackRow(context, theme, lib, playing, i, queue),
        ],
      ),
    );
  }

  Widget _trackRow(BuildContext context, ThemeData theme, LibraryProvider lib,
      String playing, int i, List<Song> queue) {
    // `queue` is the online-only list the rows are built from, so
    // indexing it keeps row order and playback order identical.
    final s     = queue[i];
    final live  = lib.onlineSong(s.contentHash);
    final off   = live == null;
    final shown = live ?? s;
    final isPlaying = playing == s.contentHash;

    // Stable key: rows shift as the live catalog / results change, and
    // without a key Flutter re-associates elements by position, which can
    // drop an in-flight tap on the row's buttons (see the external-result
    // rows for the long version of this story).
    return Opacity(
      key: ValueKey('row:${s.contentHash}'),
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
  const _SearchResultsView({required this.query, this.nonce = 0});
  final String query;

  /// Changes on every Enter press so an identical query still re-runs.
  final int nonce;

  @override
  State<_SearchResultsView> createState() => _SearchResultsViewState();
}

class _SearchResultsViewState extends State<_SearchResultsView> {
  List<Song>? _results;   // null = loading
  String?     _error;
  int         _reqSeq = 0;

  // Foreign-network hits (Soulseek / napstr), accumulated per network.
  // They stream in over several seconds — Soulseek peers answer one by
  // one — so results are appended as they land rather than awaited.
  final Map<String, List<ExternalTrack>> _external = {};
  final Map<String, StreamSubscription<List<ExternalTrack>>> _extSubs = {};
  bool _extSearching = false;

  // User's narrowing of the foreign results (file types, folders, bitrate,
  // free text). Lives here — not in the bar widget — so it survives every
  // progressive result batch, and survives an Enter-press retry of the
  // SAME query (which just fishes for more peers). It resets only when
  // the query text actually changes.
  ExternalResultFilter _extFilter = ExternalResultFilter.empty;

  @override
  void initState() {
    super.initState();
    _run();
  }

  @override
  void didUpdateWidget(_SearchResultsView old) {
    super.didUpdateWidget(old);
    if (old.query != widget.query) _extFilter = ExternalResultFilter.empty;
    if (old.query != widget.query || old.nonce != widget.nonce) _run();
  }

  @override
  void dispose() {
    _cancelExternal();
    super.dispose();
  }

  void _cancelExternal() {
    for (final sub in _extSubs.values) {
      sub.cancel();
    }
    _extSubs.clear();
  }

  /// Fan the same query out to every ticked + configured foreign network.
  void _searchExternal(String query) {
    _cancelExternal();
    setState(() {
      _external.clear();
      _extSearching = false;
    });
    final active = NetworkRegistry.instance.activeNetworks;
    if (active.isEmpty) return;
    setState(() => _extSearching = true);
    for (final net in active) {
      _external[net.id] = [];
      _extSubs[net.id] = net.search(query).listen(
        (batch) {
          if (!mounted) return;
          setState(() => _external[net.id]!.addAll(batch));
        },
        onError: (_) {},
        onDone: () {
          if (!mounted) return;
          setState(() {
            _extSubs.remove(net.id);
            if (_extSubs.isEmpty) _extSearching = false;
          });
        },
      );
    }
  }

  Future<void> _run() async {
    final seq = ++_reqSeq;
    setState(() { _results = null; _error = null; });
    _searchExternal(widget.query);
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
    // Discover is a catalogue of what you can actually play RIGHT NOW, so
    // offline songs are dropped, not dimmed. A row you cannot stream is
    // just a dead end: tapping it fails, and it pushes real results down
    // the list. (My Library is the place where files you hold locally
    // show up regardless of swarm state.)
    final all = _results!;
    final results = <Song>[
      for (final s in all)
        if (lib.onlineSong(s.contentHash) != null)
          lib.onlineSong(s.contentHash)!,
    ];
    final queue = results;
    final hiddenOffline = all.length - results.length;

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
                        : '${results.length} online result'
                          '${results.length == 1 ? '' : 's'}'
                          '${hiddenOffline > 0 ? ' · $hiddenOffline offline hidden' : ''}',
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

        // ---- other networks -------------------------------------------
        if (_extSearching)
          const Padding(
            padding: EdgeInsets.symmetric(vertical: 10),
            child: LinearProgressIndicator(minHeight: 2),
          ),
        ..._externalBlock(theme),
      ],
    );
  }

  /// Filter bar + one titled block per foreign network. Tapping a row
  /// downloads it; the download manager fingerprints and tag-imports it
  /// on completion, so a hit from Soulseek or napstr ends up in the local
  /// library the same as a locally-scanned file.
  List<Widget> _externalBlock(ThemeData theme) {
    final all = [for (final l in _external.values) ...l];
    if (all.isEmpty) return const [];

    // Apply the user's filter per network so section counts stay honest.
    final filtered = {
      for (final e in _external.entries) e.key: _extFilter.apply(e.value),
    };
    final shownTotal =
        filtered.values.fold<int>(0, (n, l) => n + l.length);

    return [
      Padding(
        padding: const EdgeInsets.fromLTRB(4, 14, 4, 2),
        // The bar derives its chips from the CURRENT results, so as
        // Soulseek peers trickle in, new extensions grow new chips while
        // the user's existing selections stay put (the filter is a
        // denylist — see external_result_filters.dart).
        child: ExternalResultFilterBar(
          filter: _extFilter,
          results: all,
          hiddenCount: all.length - shownTotal,
          onChanged: (f) => setState(() => _extFilter = f),
        ),
      ),
      ..._externalSections(theme, filtered),
    ];
  }

  List<Widget> _externalSections(
      ThemeData theme, Map<String, List<ExternalTrack>> filtered) {
    final out = <Widget>[];
    for (final entry in _external.entries) {
      if (entry.value.isEmpty) continue;
      final shown = filtered[entry.key] ?? const <ExternalTrack>[];
      final net = NetworkRegistry.instance.byId(entry.key);
      final label = net?.displayName ?? entry.key;
      // When the filter bites, the header says "shown of total" so a
      // fully filtered-out network still reads as "0 of 37" rather than
      // vanishing as if the search came back empty.
      final counts = shown.length == entry.value.length
          ? '${entry.value.length} result'
              '${entry.value.length == 1 ? '' : 's'}'
          : '${shown.length} of ${entry.value.length} results';
      out.add(Padding(
        padding: const EdgeInsets.fromLTRB(4, 18, 4, 6),
        child: Text(
          '$label · $counts',
          style: theme.textTheme.labelSmall?.copyWith(
            fontWeight: FontWeight.w800,
            letterSpacing: 1.1,
            color: theme.colorScheme.primary,
          ),
        ),
      ));
      // Stable keys: results stream in over seconds (Soulseek peers reply
      // one at a time), and without a key Flutter matches children by
      // position. A row arriving above yours re-associates the element
      // under your finger with a different widget, which destroys the
      // gesture recogniser mid-tap — the click is then simply lost while
      // hover keeps working. Filtering rows in and out has exactly the
      // same effect, so the keys matter even more now.
      out.addAll(shown.map((t) => ExternalResultTile(
            key: ValueKey('${t.networkId}:${t.id}'),
            track: t,
            networkLabel: label,
          )));
    }
    return out;
  }

  Widget _resultRow(BuildContext context, ThemeData theme,
      LibraryProvider lib, String playing, Song s, int i, List<Song> queue) {
    final live  = lib.onlineSong(s.contentHash);
    final off   = live == null;
    final shown = live ?? s;
    final isPlaying = playing == s.contentHash;

    // Stable key: rows shift as the live catalog / results change, and
    // without a key Flutter re-associates elements by position, which can
    // drop an in-flight tap on the row's buttons (see the external-result
    // rows for the long version of this story).
    return Opacity(
      key: ValueKey('row:${s.contentHash}'),
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


/// Compact on/off chips for each foreign network, shown next to the
/// Discover search field.
///
/// Ticking one only enables querying — credentials still live in
/// Settings. A network that isn't set up yet stays visibly dim and says
/// so when tapped, rather than silently doing nothing (which is what the
/// old Settings-only toggle did).
class _NetworkToggles extends StatefulWidget {
  const _NetworkToggles();

  @override
  State<_NetworkToggles> createState() => _NetworkTogglesState();
}

class _NetworkTogglesState extends State<_NetworkToggles> {
  @override
  void initState() {
    super.initState();
    NetworkRegistry.instance.addListener(_onChanged);
  }

  @override
  void dispose() {
    NetworkRegistry.instance.removeListener(_onChanged);
    super.dispose();
  }

  void _onChanged() {
    if (mounted) setState(() {});
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final networks = NetworkRegistry.instance.networks;
    if (networks.isEmpty) return const SizedBox.shrink();

    return Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        for (final net in networks) ...[
          const SizedBox(width: 6),
          Tooltip(
            message: net.isConfigured
                ? 'Include ${net.displayName} in searches'
                : '${net.displayName} needs setting up in '
                    'Settings → Other networks',
            child: FilterChip(
              visualDensity: VisualDensity.compact,
              materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
              label: Text(net.displayName,
                  style: theme.textTheme.labelSmall?.copyWith(
                    color: net.isConfigured
                        ? null
                        : theme.colorScheme.onSurfaceVariant,
                  )),
              avatar: net.status == NetworkStatus.error
                  ? const Icon(Icons.error_outline,
                      size: 14, color: Colors.redAccent)
                  : null,
              selected: net.enabled,
              onSelected: (v) {
                if (v && !net.isConfigured) {
                  ScaffoldMessenger.maybeOf(context)?.showSnackBar(SnackBar(
                    content: Text(
                      '${net.displayName} needs to be set up first — '
                      'Settings → Other networks.',
                    ),
                    duration: const Duration(seconds: 4),
                  ));
                }
                NetworkRegistry.instance.setEnabled(net.id, v);
              },
            ),
          ),
        ],
      ],
    );
  }
}
