// "My Library" tab — same design language as the Discover tab: cover-art
// card grids (deterministic art, no image assets), gradient genre tiles,
// a stats header with Play all / Shuffle, and the same drill
// (Artist / Genre → Album, plus Playlists) into a resizable track pane.
// Only the data source differs: LibraryService (local files) instead of
// the chain catalog, so rows keep their local / downloading / remote
// state and the add/scan/delete/DMCA actions.

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../models/song.dart';
import '../providers/download_provider.dart';
import '../providers/player_provider.dart';
import '../providers/wallet_provider.dart';
import '../services/library_scanner.dart';
import '../services/library_service.dart';
import '../services/local_library_actions.dart';
import '../services/playlist_service.dart';
import '../widgets/cover_art.dart';
import 'dmca_screen.dart';
import 'folders_screen.dart';

enum _FacetMode { artist, genre, playlists }

extension _FacetModeLabel on _FacetMode {
  String get label => switch (this) {
    _FacetMode.artist    => 'Artist',
    _FacetMode.genre     => 'Genre',
    _FacetMode.playlists => 'Playlists',
  };
  IconData get icon => switch (this) {
    _FacetMode.artist    => Icons.person_outline,
    _FacetMode.genre     => Icons.style_outlined,
    _FacetMode.playlists => Icons.queue_music,
  };
  String get rootLabel => switch (this) {
    _FacetMode.artist    => 'Artists',
    _FacetMode.genre     => 'Genres',
    _FacetMode.playlists => 'Playlists',
  };
}

class LocalLibraryScreen extends StatefulWidget {
  const LocalLibraryScreen({super.key});

  @override
  State<LocalLibraryScreen> createState() => _LocalLibraryScreenState();
}

class _LocalLibraryScreenState extends State<LocalLibraryScreen> {
  _FacetMode _mode = _FacetMode.artist;
  String? _drillGenre;
  String? _drillArtist;
  String? _selectedAlbum;
  String? _selectedPlaylistId; // playlists facet → fills the track pane
  double  _topFraction = 0.5;
  bool    _scanning    = false;

  @override
  void initState() {
    super.initState();
    PlaylistService.instance
      ..ensureLoaded()
      ..addListener(_onPlaylistsChanged);
  }

  @override
  void dispose() {
    PlaylistService.instance.removeListener(_onPlaylistsChanged);
    super.dispose();
  }

  void _onPlaylistsChanged() {
    if (mounted) setState(() {});
  }

  Future<void> _openFolders() async {
    await Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => const FoldersScreen(),
    ));
  }

  Future<void> _scanNow() async {
    setState(() => _scanning = true);
    try {
      await LibraryScanner.instance.scanOnce(
        force: true,
        onProgress: () { if (mounted) setState(() {}); },
      );
    } finally {
      if (mounted) setState(() => _scanning = false);
    }
  }

  // ---- Bucket / sort helpers ------------------------------------------
  //
  // Tag spellings rarely line up — "FIDLAR" / "Fidlar", "Rock" / "rock"
  // etc. all collide on the same audio. We group case-insensitively
  // and pick the most-frequent original spelling for display so the
  // card shows what the user actually has.

  String _artistKey(LibraryEntry e) {
    final t = e.artist.trim();
    return t.isEmpty ? 'Unknown Artist' : t;
  }
  String _artistKeyNorm(LibraryEntry e) => _artistKey(e).toLowerCase();

  String _genreKey(LibraryEntry e) {
    final t = e.genre.trim();
    return t.isEmpty ? 'Unknown Genre' : t;
  }
  String _genreKeyNorm(LibraryEntry e) => _genreKey(e).toLowerCase();

  String _albumKey(LibraryEntry e) {
    final t = e.album.trim();
    return t.isEmpty ? 'Singles' : t;
  }
  String _albumKeyNorm(LibraryEntry e) => _albumKey(e).toLowerCase();

  int _sortAlpha(String a, String b) {
    final ua = a.startsWith('Unknown') || a == 'Singles';
    final ub = b.startsWith('Unknown') || b == 'Singles';
    if (ua && !ub) return 1;
    if (ub && !ua) return -1;
    return a.toLowerCase().compareTo(b.toLowerCase());
  }

  int _earliestYear(Iterable<LibraryEntry> tracks) {
    int y = 0;
    for (final e in tracks) {
      if (e.year > 0 && (y == 0 || e.year < y)) y = e.year;
    }
    return y;
  }

  Iterable<LibraryEntry> _drillFilter(List<LibraryEntry> entries) {
    final wantedGenre  = _drillGenre?.toLowerCase();
    final wantedArtist = _drillArtist?.toLowerCase();
    return entries.where((e) {
      if (wantedGenre  != null && _genreKeyNorm(e) != wantedGenre)  return false;
      if (wantedArtist != null && _artistKeyNorm(e) != wantedArtist) return false;
      return true;
    });
  }

  /// Distinct track count by dedup key (fingerprint > canonical > local
  /// content_hash). Mirrors the rule the previous local-library list
  /// used so card counts stay honest in the presence of variants.
  int _distinctTrackCount(Iterable<LibraryEntry> entries) {
    final seen = <String>{};
    for (final e in entries) {
      seen.add(_dedupKey(e));
    }
    return seen.length;
  }

  String _dedupKey(LibraryEntry e) {
    if (e.fingerprintHash.isNotEmpty) return 'fp:${e.fingerprintHash}';
    if (e.canonicalHash.isNotEmpty)   return 'ch:${e.canonicalHash}';
    return 'lh:${e.contentHash}';
  }

  // ---- Drill actions --------------------------------------------------

  void _selectMode(_FacetMode m) {
    setState(() {
      _mode = m;
      _drillGenre    = null;
      _drillArtist   = null;
      _selectedAlbum = null;
      _selectedPlaylistId = null;
    });
  }

  void _onCardTapped(String key, _DrillLevel level) {
    setState(() {
      switch (level) {
        case _DrillLevel.genre:
          _drillGenre    = _drillGenre == key ? null : key;
          _drillArtist   = null;
          _selectedAlbum = null;
        case _DrillLevel.artist:
          _drillArtist   = _drillArtist == key ? null : key;
          _selectedAlbum = null;
        case _DrillLevel.album:
          _selectedAlbum = _selectedAlbum == key ? null : key;
      }
    });
  }

  void _crumbBack(int targetDepth) {
    setState(() {
      if (targetDepth < 3) _selectedAlbum = null;
      if (targetDepth < 2) _drillArtist   = null;
      if (targetDepth < 1) _drillGenre    = null;
    });
  }

  // ---- Playback / queue helpers ---------------------------------------

  Song _toSong(LibraryEntry e) {
    final hash = e.canonicalHash.isNotEmpty ? e.canonicalHash : e.contentHash;
    return Song(
      contentHash:     hash,
      fingerprintHash: e.fingerprintHash,
      title:           e.title,
      artist:          e.artist,
      album:           e.album,
      genre:           e.genre,
      year:            e.year,
      trackNumber:     e.trackNumber,
      durationMs:      e.durationMs,
    );
  }

  /// Play [tracks] as a queue starting at [startIndex] (the row the user
  /// tapped). Remote / not-yet-downloaded entries get filtered out before
  /// they reach the player — they have no local file to stream — and the
  /// start index slides to the tapped track's position in that local
  /// subset (or to 0 if the tapped track itself isn't playable, which
  /// should be impossible since the row's tap handler is null for remote
  /// entries, but we guard anyway). This is what makes tapping a track in
  /// an open album play the whole album instead of just that one song.
  void _playFromAlbum(List<LibraryEntry> tracks, int startIndex) {
    if (tracks.isEmpty) return;
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    final tapped = (startIndex >= 0 && startIndex < tracks.length)
        ? tracks[startIndex]
        : null;
    final playable = tracks.where((e) => e.isLocal).toList();
    if (playable.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content:
          Text('Nothing in this album is downloaded yet.')),
      );
      return;
    }
    var idx = 0;
    if (tapped != null) {
      final foundAt = playable.indexWhere(
          (e) => e.contentHash == tapped.contentHash);
      if (foundAt >= 0) idx = foundAt;
    }
    final playlist = playable.map(_toSong).toList();
    context.read<PlayerProvider>()
        .playPlaylist(playlist, idx, wallet.address);
  }

  void _playGroup(List<LibraryEntry> entries, {bool shuffle = false}) {
    if (entries.isEmpty) return;
    final wallet = context.read<WalletProvider>().info;
    if (wallet == null) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content: Text('Create a wallet first')),
      );
      return;
    }
    var playable = _sortEntries(entries).where((e) => e.isLocal).toList();
    if (playable.isEmpty) {
      ScaffoldMessenger.of(context).showSnackBar(
        const SnackBar(content:
          Text('Nothing in this group is downloaded yet.')),
      );
      return;
    }
    if (shuffle) playable = List.of(playable)..shuffle();
    final playlist = playable.map(_toSong).toList();
    context.read<PlayerProvider>()
        .playPlaylist(playlist, 0, wallet.address);
  }

  List<LibraryEntry> _sortEntries(Iterable<LibraryEntry> entries) {
    final out = [...entries];
    out.sort((a, b) {
      if (_drillArtist != null && _drillGenre != null) {
        // Inside an album already — track number is enough.
      } else {
        final aa = a.album.trim();
        final ab = b.album.trim();
        if (aa.isEmpty && ab.isNotEmpty) return 1;
        if (ab.isEmpty && aa.isNotEmpty) return -1;
        if (a.year > 0 && b.year > 0 && a.year != b.year) {
          return a.year.compareTo(b.year);
        }
        final ac = aa.toLowerCase().compareTo(ab.toLowerCase());
        if (ac != 0) return ac;
      }
      if (a.trackNumber > 0 && b.trackNumber > 0
          && a.trackNumber != b.trackNumber) {
        return a.trackNumber.compareTo(b.trackNumber);
      }
      if (a.trackNumber > 0 && b.trackNumber == 0) return -1;
      if (b.trackNumber > 0 && a.trackNumber == 0) return 1;
      return a.title.toLowerCase().compareTo(b.title.toLowerCase());
    });
    return out;
  }

  // ---- Build ----------------------------------------------------------

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      body: Consumer<LibraryService>(
        builder: (context, lib, _) {
          final entries = lib.entries;
          if (entries.isEmpty) {
            return _EmptyLibrary(
              onFolders: _openFolders,
              onScan:    _scanning ? null : _scanNow,
              scanning:  _scanning,
            );
          }
          return Column(
            children: [
              _LibraryHeader(
                entries:    entries,
                localCount: entries.where((e) => e.isLocal).length,
                artistCount: entries.map(_artistKeyNorm).toSet().length,
                scanning:   _scanning,
                onPlayAll:  () => _playGroup(entries),
                onShuffle:  () => _playGroup(entries, shuffle: true),
                onFolders:  _openFolders,
                onScan:     _scanning ? null : _scanNow,
              ),
              _ModeToolbar(mode: _mode, onChange: _selectMode),
              _Breadcrumb(segments: _breadcrumb()),
              if (_scanning)
                _ScannerProgressBar(scanner: LibraryScanner.instance),
              Expanded(child: _splitBody(entries)),
            ],
          );
        },
      ),
    );
  }

  List<_CrumbSeg> _breadcrumb() {
    final out = <_CrumbSeg>[
      _CrumbSeg(label: _mode.rootLabel, onTap: () => _crumbBack(0)),
    ];
    if (_drillGenre != null) {
      out.add(_CrumbSeg(label: _drillGenre!, onTap: () => _crumbBack(1)));
    }
    if (_drillArtist != null) {
      out.add(_CrumbSeg(label: _drillArtist!, onTap: () => _crumbBack(2)));
    }
    if (_selectedAlbum != null) {
      out.add(_CrumbSeg(label: _selectedAlbum!, onTap: null));
    }
    return out;
  }

  Widget _splitBody(List<LibraryEntry> allEntries) {
    final isPlaylists = _mode == _FacetMode.playlists;

    // Bottom-pane tracks. In playlists mode, the selected playlist's songs
    // resolved to the entries the user actually has locally, in the playlist's
    // own order — playlists open in the exact same track pane albums do.
    final Playlist? selPlaylist =
        isPlaylists ? _currentPlaylist(PlaylistService.instance) : null;
    final List<LibraryEntry> selectedTracks;
    final String bottomTitle;
    if (isPlaylists) {
      if (selPlaylist != null) {
        // Key by songId (canonical) — a playlist stores song identities, so a
        // local variant resolves regardless of its file's content hash.
        final byHash = <String, LibraryEntry>{
          for (final e in allEntries) e.songId: e
        };
        selectedTracks = [
          for (final h in selPlaylist.songs)
            if (byHash[h] != null) byHash[h]!,
        ];
        bottomTitle = selPlaylist.name;
      } else {
        selectedTracks = const <LibraryEntry>[];
        bottomTitle = '';
      }
    } else {
      final wantedAlbum = _selectedAlbum?.toLowerCase();
      selectedTracks = wantedAlbum == null
          ? const <LibraryEntry>[]
          : _sortEntries(_drillFilter(allEntries)
              .where((e) => _albumKeyNorm(e) == wantedAlbum));
      bottomTitle = _selectedAlbum ?? '';
    }

    return LayoutBuilder(
      builder: (context, constraints) {
        final totalH   = constraints.maxHeight;
        final hasBottom =
            isPlaylists ? selPlaylist != null : _selectedAlbum != null;
        final topH    = hasBottom ? totalH * _topFraction : totalH;
        final bottomH = hasBottom ? totalH - topH - _kHandleHeight : 0.0;
        return Stack(
          children: [
            Positioned(
              top: 0,
              left: 0,
              right: 0,
              height: topH,
              child: isPlaylists ? _playlistPane() : _topPane(allEntries),
            ),
            if (hasBottom) ...[
              Positioned(
                top: topH,
                left: 0,
                right: 0,
                height: _kHandleHeight,
                child: _DragHandle(
                  onDelta: (dy) => setState(() {
                    _topFraction = (_topFraction + dy / totalH)
                        .clamp(0.20, 0.80);
                  }),
                ),
              ),
              Positioned(
                top: topH + _kHandleHeight,
                left: 0,
                right: 0,
                height: bottomH,
                child: _TrackPane(
                  albumName:      bottomTitle,
                  artistFallback: isPlaylists ? null : _drillArtist,
                  isPlaylist:     isPlaylists,
                  tracks:         selectedTracks,
                  onPlay:         _playFromAlbum,
                  onShuffle:      () =>
                      _playGroup(selectedTracks, shuffle: true),
                  onClose:        () => setState(() {
                    if (isPlaylists) {
                      _selectedPlaylistId = null;
                    } else {
                      _selectedAlbum = null;
                    }
                  }),
                ),
              ),
            ],
          ],
        );
      },
    );
  }

  Playlist? _currentPlaylist(PlaylistService svc) {
    final id = _selectedPlaylistId;
    if (id == null) return null;
    for (final p in svc.playlists) {
      if (p.id == id) return p;
    }
    return null; // selected playlist was deleted — bottom pane collapses
  }

  /// Top pane for the playlists facet: one gradient card per saved playlist
  /// (tap fills the track pane; long-press / right-click for
  /// add/rename/delete) plus a "New playlist" card.
  Widget _playlistPane() {
    final pls = PlaylistService.instance.playlists;
    return GridView.builder(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
      gridDelegate: const SliverGridDelegateWithMaxCrossAxisExtent(
        maxCrossAxisExtent: 190,
        mainAxisSpacing: 10,
        crossAxisSpacing: 10,
        childAspectRatio: 1.9,
      ),
      itemCount: pls.length + 1,
      itemBuilder: (context, i) {
        if (i == pls.length) {
          return _NewPlaylistCard(onTap: _createPlaylistDialog);
        }
        final pl = pls[i];
        return _PlaylistCard(
          playlist: pl,
          selected: _selectedPlaylistId == pl.id,
          onTap: () => setState(() {
            _selectedPlaylistId =
                _selectedPlaylistId == pl.id ? null : pl.id;
          }),
          onMenu: (pos) => _playlistMenu(pos, pl),
        );
      },
    );
  }

  Future<void> _playlistMenu(Offset position, Playlist pl) async {
    final overlay =
        Overlay.of(context).context.findRenderObject() as RenderBox;
    final picked = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        position & const Size(40, 40),
        Offset.zero & overlay.size,
      ),
      items: const [
        PopupMenuItem(
          value: 'add',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.playlist_add, size: 18),
            title: Text('Add songs…'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuItem(
          value: 'rename',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.edit_outlined, size: 18),
            title: Text('Rename'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuDivider(),
        PopupMenuItem(
          value: 'delete',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.delete_outline, size: 18),
            title: Text('Delete'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
      ],
    );
    if (!mounted) return;
    switch (picked) {
      case 'add':
        _addSongsSheet(pl);
      case 'rename':
        _renamePlaylistDialog(pl);
      case 'delete':
        if (_selectedPlaylistId == pl.id) {
          setState(() => _selectedPlaylistId = null);
        }
        PlaylistService.instance.delete(pl);
    }
  }

  void _createPlaylistDialog() {
    final ctrl = TextEditingController();
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('New playlist'),
        content: TextField(
            controller: ctrl,
            autofocus: true,
            decoration: const InputDecoration(hintText: 'Name')),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          FilledButton(
              onPressed: () {
                PlaylistService.instance.create(ctrl.text);
                Navigator.pop(ctx);
              },
              child: const Text('Create')),
        ],
      ),
    );
  }

  void _renamePlaylistDialog(Playlist pl) {
    final ctrl = TextEditingController(text: pl.name);
    showDialog<void>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Rename playlist'),
        content: TextField(
            controller: ctrl,
            autofocus: true,
            decoration: const InputDecoration(hintText: 'Name')),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(ctx), child: const Text('Cancel')),
          FilledButton(
              onPressed: () {
                PlaylistService.instance.rename(pl, ctrl.text);
                Navigator.pop(ctx);
              },
              child: const Text('Save')),
        ],
      ),
    );
  }

  /// Pick from the user's local library to add to [pl].
  void _addSongsSheet(Playlist pl) {
    final candidates = LibraryService.instance.entries
        .where((e) => !pl.songs.contains(e.songId))
        .toList();
    showModalBottomSheet<void>(
      context: context,
      builder: (ctx) => candidates.isEmpty
          ? const SizedBox(
              height: 160,
              child: Center(child: Text('No more local songs to add.')))
          : ListView.builder(
              itemCount: candidates.length,
              itemBuilder: (ctx, i) {
                final e = candidates[i];
                return ListTile(
                  leading: ClipRRect(
                    borderRadius: BorderRadius.circular(6),
                    child: CoverArt(
                        seed: seedFromHash(e.songId), size: 34),
                  ),
                  title: Text(
                      e.title.isEmpty ? e.contentHash.substring(0, 12) : e.title),
                  subtitle: Text(e.artist),
                  onTap: () {
                    PlaylistService.instance.addSong(pl, e.songId);
                    Navigator.pop(ctx);
                  },
                );
              },
            ),
    );
  }

  Widget _topPane(List<LibraryEntry> entries) {
    final level = _currentLevel();
    final cards = _cardsFor(level, entries);
    if (cards.isEmpty) {
      return Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Text(
            'Nothing under "${_breadcrumb().last.label}" yet.',
            textAlign: TextAlign.center,
            style: TextStyle(color: Theme.of(context)
                .colorScheme.onSurfaceVariant),
          ),
        ),
      );
    }
    // Genre root uses short gradient tiles; artist/album levels use
    // cover-art cards — both straight from the Discover design language.
    final isTiles = level == _DrillLevel.genre;
    return GridView.builder(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 12),
      gridDelegate: SliverGridDelegateWithMaxCrossAxisExtent(
        maxCrossAxisExtent: isTiles ? 190 : 150,
        mainAxisSpacing: 10,
        crossAxisSpacing: 10,
        childAspectRatio: isTiles ? 1.9 : 0.74,
      ),
      itemCount: cards.length,
      itemBuilder: (context, i) => cards[i],
    );
  }

  _DrillLevel _currentLevel() {
    if (_drillArtist != null) return _DrillLevel.album;
    if (_drillGenre  != null) return _DrillLevel.artist;
    return _mode == _FacetMode.artist ? _DrillLevel.artist : _DrillLevel.genre;
  }

  List<Widget> _cardsFor(_DrillLevel level, List<LibraryEntry> entries) {
    final scoped = _drillFilter(entries).toList();
    switch (level) {
      case _DrillLevel.genre:
        final buckets = _bucketByNorm(scoped, _genreKey, _genreKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _GenreTileCard(
              name:    k,
              count:   _distinctTrackCount(buckets[k]!),
              onTap:   () => _onCardTapped(k, _DrillLevel.genre),
              entries: buckets[k]!,
              onPlay:  () => _playGroup(buckets[k]!),
            ),
        ];
      case _DrillLevel.artist:
        final buckets = _bucketByNorm(scoped, _artistKey, _artistKeyNorm);
        final keys = buckets.keys.toList()..sort(_sortAlpha);
        return [
          for (final k in keys)
            _FacetCard(
              seed:     seedFromName(k),
              label:    k,
              sublabel: '${_distinctTrackCount(buckets[k]!)} '
                        'track${_distinctTrackCount(buckets[k]!) == 1 ? '' : 's'}',
              selected: false,
              onTap:    () => _onCardTapped(k, _DrillLevel.artist),
              entries:  buckets[k]!,
              onPlay:   () => _playGroup(buckets[k]!),
            ),
        ];
      case _DrillLevel.album:
        final buckets = _bucketByNorm(scoped, _albumKey, _albumKeyNorm);
        final keys = buckets.keys.toList()..sort((a, b) {
          final ya = _earliestYear(buckets[a]!);
          final yb = _earliestYear(buckets[b]!);
          if (ya > 0 && yb > 0 && ya != yb) return ya.compareTo(yb);
          if (ya > 0 && yb == 0) return -1;
          if (yb > 0 && ya == 0) return 1;
          return _sortAlpha(a, b);
        });
        return [
          for (final k in keys)
            _FacetCard(
              // Album art keys off the album's first track (sorted), so an
              // album keeps one face no matter where it shows up.
              seed:     seedFromHash(
                  _sortEntries(buckets[k]!).first.songId),
              label:    k,
              sublabel: _albumSublabel(k, buckets[k]!),
              selected: _selectedAlbum?.toLowerCase() == k.toLowerCase(),
              onTap:    () => _onCardTapped(k, _DrillLevel.album),
              entries:  buckets[k]!,
              onPlay:   () => _playGroup(buckets[k]!),
            ),
        ];
    }
  }

  /// Group `items` by `norm(item)` and pick the most-common spelling of
  /// `display(item)` as the bucket key. Mirrors the same fix in the
  /// Discover screen so "FIDLAR" + "Fidlar" coalesce into one card with
  /// whichever spelling dominates the user's tags.
  Map<String, List<LibraryEntry>> _bucketByNorm(
      List<LibraryEntry> items,
      String Function(LibraryEntry) display,
      String Function(LibraryEntry) norm) {
    final normToVariants = <String, Map<String, int>>{};
    final normToTracks   = <String, List<LibraryEntry>>{};
    for (final e in items) {
      final n = norm(e);
      final d = display(e);
      (normToTracks[n] ??= []).add(e);
      (normToVariants[n] ??= <String, int>{})[d] =
          (normToVariants[n]![d] ?? 0) + 1;
    }
    final out = <String, List<LibraryEntry>>{};
    normToVariants.forEach((n, variants) {
      String best = variants.keys.first;
      int    bestCount = variants[best]!;
      variants.forEach((d, c) {
        if (c > bestCount || (c == bestCount && d.length > best.length)) {
          best = d;
          bestCount = c;
        }
      });
      out[best] = normToTracks[n]!;
    });
    return out;
  }

  String _albumSublabel(String name, List<LibraryEntry> tracks) {
    final year = _earliestYear(tracks);
    final n = _distinctTrackCount(tracks);
    final t = '$n track${n == 1 ? '' : 's'}';
    return year > 0 ? '$year · $t' : t;
  }
}

// ---- Shared bits (same shape as the Discover tab) ----------------------

enum _DrillLevel { genre, artist, album }

class _CrumbSeg {
  _CrumbSeg({required this.label, this.onTap});
  final String        label;
  final VoidCallback? onTap;
}

const double _kHandleHeight = 14;

// ---- Header: stats + play all / shuffle + folders / scan ----------------

class _LibraryHeader extends StatelessWidget {
  const _LibraryHeader({
    required this.entries,
    required this.localCount,
    required this.artistCount,
    required this.scanning,
    required this.onPlayAll,
    required this.onShuffle,
    required this.onFolders,
    required this.onScan,
  });

  final List<LibraryEntry> entries;
  final int  localCount;
  final int  artistCount;
  final bool scanning;
  final VoidCallback  onPlayAll;
  final VoidCallback  onShuffle;
  final VoidCallback  onFolders;
  final VoidCallback? onScan;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final p = artParams(seedFromName('my library'));
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 10, 12, 4),
      padding: const EdgeInsets.fromLTRB(14, 12, 8, 12),
      decoration: BoxDecoration(
        borderRadius: BorderRadius.circular(16),
        border: Border.all(color: theme.dividerColor.withOpacity(.4)),
        gradient: LinearGradient(
          begin: Alignment.topLeft,
          end: Alignment.bottomRight,
          colors: [hslColor(p.h1, 45, 16), hslColor(p.h2, 45, 12)],
        ),
      ),
      child: Row(
        children: [
          ClipRRect(
            borderRadius: BorderRadius.circular(10),
            child: CoverArt(seed: seedFromName('my library'), size: 56),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                ShaderMask(
                  shaderCallback: (r) => LinearGradient(colors: [
                    theme.colorScheme.primary,
                    theme.colorScheme.tertiary,
                  ]).createShader(r),
                  child: Text(
                    'My Library',
                    style: theme.textTheme.titleMedium?.copyWith(
                        fontWeight: FontWeight.w800, color: Colors.white),
                  ),
                ),
                const SizedBox(height: 2),
                Text(
                  '${entries.length} track${entries.length == 1 ? '' : 's'}'
                  ' · $artistCount artist${artistCount == 1 ? '' : 's'}'
                  ' · $localCount on this device',
                  style: theme.textTheme.labelSmall?.copyWith(
                      color: theme.colorScheme.onSurface.withOpacity(.65)),
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                ),
                const SizedBox(height: 8),
                Row(
                  children: [
                    FilledButton.icon(
                      onPressed: onPlayAll,
                      icon: const Icon(Icons.play_arrow, size: 16),
                      label: const Text('Play all'),
                      style: FilledButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 12),
                        visualDensity: VisualDensity.compact,
                      ),
                    ),
                    const SizedBox(width: 6),
                    OutlinedButton.icon(
                      onPressed: onShuffle,
                      icon: const Icon(Icons.shuffle, size: 14),
                      label: const Text('Shuffle'),
                      style: OutlinedButton.styleFrom(
                        padding: const EdgeInsets.symmetric(horizontal: 10),
                        visualDensity: VisualDensity.compact,
                      ),
                    ),
                  ],
                ),
              ],
            ),
          ),
          Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              IconButton(
                tooltip: 'Folders',
                icon: const Icon(Icons.create_new_folder_outlined, size: 20),
                onPressed: onFolders,
              ),
              IconButton(
                tooltip: 'Scan now',
                icon: scanning
                    ? const SizedBox(
                        width: 18, height: 18,
                        child: CircularProgressIndicator(strokeWidth: 2))
                    : const Icon(Icons.refresh, size: 20),
                onPressed: onScan,
              ),
            ],
          ),
        ],
      ),
    );
  }
}

class _EmptyLibrary extends StatelessWidget {
  const _EmptyLibrary({
    required this.onFolders,
    required this.onScan,
    required this.scanning,
  });
  final VoidCallback  onFolders;
  final VoidCallback? onScan;
  final bool          scanning;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Center(
      child: Padding(
        padding: const EdgeInsets.all(32),
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            ClipRRect(
              borderRadius: BorderRadius.circular(16),
              child: CoverArt(seed: seedFromName('my library'), size: 88),
            ),
            const SizedBox(height: 16),
            Text('Your library is empty',
                style: theme.textTheme.titleMedium
                    ?.copyWith(fontWeight: FontWeight.w800)),
            const SizedBox(height: 6),
            Text(
              'Add a music folder, then scan it — your files are '
              'fingerprinted locally and never leave this device.',
              textAlign: TextAlign.center,
              style: theme.textTheme.bodySmall?.copyWith(
                  color: theme.colorScheme.onSurfaceVariant),
            ),
            const SizedBox(height: 16),
            Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                FilledButton.icon(
                  onPressed: onFolders,
                  icon: const Icon(Icons.create_new_folder_outlined, size: 18),
                  label: const Text('Add folder'),
                ),
                const SizedBox(width: 8),
                OutlinedButton.icon(
                  onPressed: onScan,
                  icon: scanning
                      ? const SizedBox(
                          width: 14, height: 14,
                          child: CircularProgressIndicator(strokeWidth: 2))
                      : const Icon(Icons.refresh, size: 16),
                  label: const Text('Scan'),
                ),
              ],
            ),
          ],
        ),
      ),
    );
  }
}

class _ModeToolbar extends StatelessWidget {
  const _ModeToolbar({required this.mode, required this.onChange});
  final _FacetMode               mode;
  final ValueChanged<_FacetMode> onChange;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(12, 8, 12, 4),
      child: Row(
        children: [
          Expanded(
            child: SegmentedButton<_FacetMode>(
              showSelectedIcon: false,
              segments: _FacetMode.values.map((m) => ButtonSegment(
                value: m,
                label: Text(m.label),
                icon:  Icon(m.icon, size: 16),
              )).toList(),
              selected: {mode},
              onSelectionChanged: (s) => onChange(s.first),
            ),
          ),
        ],
      ),
    );
  }
}

class _Breadcrumb extends StatelessWidget {
  const _Breadcrumb({required this.segments});
  final List<_CrumbSeg> segments;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return SizedBox(
      height: 32,
      child: ListView.separated(
        scrollDirection: Axis.horizontal,
        padding: const EdgeInsets.symmetric(horizontal: 12),
        itemCount: segments.length,
        separatorBuilder: (_, __) => Padding(
          padding: const EdgeInsets.symmetric(horizontal: 4),
          child: Icon(Icons.chevron_right,
              size: 14, color: theme.colorScheme.onSurfaceVariant),
        ),
        itemBuilder: (context, i) {
          final seg = segments[i];
          final isLast = i == segments.length - 1;
          final color = seg.onTap == null
              ? theme.colorScheme.onSurface
              : theme.colorScheme.primary;
          final text = Text(
            seg.label,
            style: theme.textTheme.bodyMedium?.copyWith(
              color: color,
              fontWeight: isLast ? FontWeight.w600 : FontWeight.w500,
            ),
          );
          return Center(
            child: seg.onTap == null
                ? text
                : InkWell(
                    onTap: seg.onTap,
                    borderRadius: BorderRadius.circular(4),
                    child: Padding(
                      padding: const EdgeInsets.symmetric(
                          horizontal: 4, vertical: 2),
                      child: text,
                    ),
                  ),
          );
        },
      ),
    );
  }
}

// ---- Facet menu (play / delete / DMCA) — shared by the card types ------

Future<void> _facetMenu(
  BuildContext context,
  Offset pos, {
  required List<LibraryEntry> entries,
  required VoidCallback onPlay,
}) async {
  final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
  final picked = await showMenu<String>(
    context: context,
    position: RelativeRect.fromRect(
      pos & const Size(40, 40),
      Offset.zero & overlay.size,
    ),
    items: const [
      PopupMenuItem(
        value: 'play',
        child: ListTile(
          dense: true,
          leading: Icon(Icons.play_arrow, size: 18),
          title: Text('Play'),
          contentPadding: EdgeInsets.zero,
        ),
      ),
      PopupMenuItem(
        value: 'delete',
        child: ListTile(
          dense: true,
          leading: Icon(Icons.delete_outline,
                        size: 18, color: Colors.red),
          title: Text('Delete'),
          contentPadding: EdgeInsets.zero,
        ),
      ),
      PopupMenuDivider(),
      PopupMenuItem(
        value: 'dmca',
        child: ListTile(
          dense: true,
          leading: Icon(Icons.gavel_outlined, size: 18),
          title: Text('About copyright / DMCA'),
          contentPadding: EdgeInsets.zero,
        ),
      ),
    ],
  );
  if (picked == 'play') {
    onPlay();
  } else if (picked == 'delete' && context.mounted) {
    await _confirmAndDeleteGroup(context, entries);
  } else if (picked == 'dmca' && context.mounted) {
    Navigator.of(context).push(MaterialPageRoute(
      builder: (_) => const DmcaScreen(),
    ));
  }
}

Future<void> _confirmAndDeleteGroup(
    BuildContext context, List<LibraryEntry> entries) async {
  final messenger = ScaffoldMessenger.maybeOf(context);
  final ok = await showDialog<bool>(
    context: context,
    builder: (ctx) => AlertDialog(
      title: const Text('Delete?'),
      content: Text(
        'Remove ${entries.length} track${entries.length == 1 ? "" : "s"} '
        'from your library? Downloaded files are deleted from disk; '
        'files in folders you scanned stay where they are.',
      ),
      actions: [
        TextButton(
          onPressed: () => Navigator.pop(ctx, false),
          child: const Text('Cancel'),
        ),
        FilledButton.tonal(
          style: FilledButton.styleFrom(
              foregroundColor: Theme.of(ctx).colorScheme.error),
          onPressed: () => Navigator.pop(ctx, true),
          child: const Text('Delete'),
        ),
      ],
    ),
  );
  if (ok != true) return;
  final svc = LocalLibraryActions.instance;
  int dropped = 0, fileDeleted = 0;
  for (final e in entries) {
    final r = await svc.deleteEntry(e);
    dropped++;
    if (r.fileDeleted) fileDeleted++;
  }
  messenger?.showSnackBar(SnackBar(
    content: Text(
      'Deleted $dropped track${dropped == 1 ? "" : "s"}'
      '${fileDeleted > 0 ? " ($fileDeleted file"
          "${fileDeleted == 1 ? "" : "s"} removed from disk)" : ""}.',
    ),
    duration: const Duration(seconds: 3),
  ));
}

// ---- Cards ---------------------------------------------------------------

/// Artist / album card: cover art + label + sublabel, Discover-style.
class _FacetCard extends StatelessWidget {
  const _FacetCard({
    required this.seed,
    required this.label,
    required this.sublabel,
    required this.selected,
    required this.onTap,
    required this.entries,
    required this.onPlay,
  });
  final List<int>          seed;
  final String             label;
  final String             sublabel;
  final bool               selected;
  final VoidCallback       onTap;
  final List<LibraryEntry> entries;
  final VoidCallback       onPlay;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return GestureDetector(
      onSecondaryTapDown: (d) => _facetMenu(context, d.globalPosition,
          entries: entries, onPlay: onPlay),
      onLongPressStart: (d) => _facetMenu(context, d.globalPosition,
          entries: entries, onPlay: onPlay),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(8),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            color: theme.colorScheme.surfaceContainerHighest.withOpacity(.35),
            border: Border.all(
              color: selected ? theme.colorScheme.primary : Colors.transparent,
            ),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Expanded(
                child: LayoutBuilder(
                  builder: (context, c) => Stack(
                    children: [
                      ClipRRect(
                        borderRadius: BorderRadius.circular(9),
                        child: CoverArt(seed: seed, size: c.maxWidth),
                      ),
                      if (selected)
                        Positioned(
                          right: 6, bottom: 6,
                          child: CircleAvatar(
                            radius: 12,
                            backgroundColor: theme.colorScheme.primary,
                            child: Icon(Icons.check,
                                size: 14,
                                color: theme.colorScheme.onPrimary),
                          ),
                        ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 6),
              Text(
                label,
                style: theme.textTheme.bodySmall
                    ?.copyWith(fontWeight: FontWeight.w700),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
              Text(
                sublabel,
                style: theme.textTheme.labelSmall?.copyWith(
                    color: theme.colorScheme.onSurface.withOpacity(.6)),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
              ),
            ],
          ),
        ),
      ),
    );
  }
}

/// Genre tile: the Discover genre-tile gradient, with count badge.
class _GenreTileCard extends StatelessWidget {
  const _GenreTileCard({
    required this.name,
    required this.count,
    required this.onTap,
    required this.entries,
    required this.onPlay,
  });
  final String             name;
  final int                count;
  final VoidCallback       onTap;
  final List<LibraryEntry> entries;
  final VoidCallback       onPlay;

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onSecondaryTapDown: (d) => _facetMenu(context, d.globalPosition,
          entries: entries, onPlay: onPlay),
      onLongPressStart: (d) => _facetMenu(context, d.globalPosition,
          entries: entries, onPlay: onPlay),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            gradient: tileGradient(seedFromName(name)),
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
                  child: Text('$count',
                      style: const TextStyle(
                          fontSize: 10,
                          fontWeight: FontWeight.w700,
                          color: Colors.white)),
                ),
              ),
              Align(
                alignment: Alignment.bottomLeft,
                child: Text(
                  name,
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
      ),
    );
  }
}

class _PlaylistCard extends StatelessWidget {
  const _PlaylistCard({
    required this.playlist,
    required this.selected,
    required this.onTap,
    required this.onMenu,
  });
  final Playlist              playlist;
  final bool                  selected;
  final VoidCallback          onTap;
  final ValueChanged<Offset>  onMenu;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return GestureDetector(
      onSecondaryTapDown: (d) => onMenu(d.globalPosition),
      onLongPressStart:   (d) => onMenu(d.globalPosition),
      child: InkWell(
        onTap: onTap,
        borderRadius: BorderRadius.circular(12),
        child: Container(
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            borderRadius: BorderRadius.circular(12),
            gradient: tileGradient(seedFromName(playlist.name)),
            border: Border.all(
              color: selected ? theme.colorScheme.primary : Colors.transparent,
              width: 1.5,
            ),
          ),
          child: Stack(
            children: [
              const Positioned(
                top: 0, left: 0,
                child: Icon(Icons.queue_music, size: 18, color: Colors.white70),
              ),
              Positioned(
                top: 0, right: 0,
                child: Container(
                  padding:
                      const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                  decoration: BoxDecoration(
                    color: Colors.black38,
                    borderRadius: BorderRadius.circular(999),
                  ),
                  child: Text('${playlist.songs.length}',
                      style: const TextStyle(
                          fontSize: 10,
                          fontWeight: FontWeight.w700,
                          color: Colors.white)),
                ),
              ),
              Align(
                alignment: Alignment.bottomLeft,
                child: Text(
                  playlist.name,
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
      ),
    );
  }
}

class _NewPlaylistCard extends StatelessWidget {
  const _NewPlaylistCard({required this.onTap});
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return InkWell(
      onTap: onTap,
      borderRadius: BorderRadius.circular(12),
      child: Container(
        decoration: BoxDecoration(
          borderRadius: BorderRadius.circular(12),
          border: Border.all(color: theme.dividerColor),
          color: theme.colorScheme.surfaceContainerHighest.withOpacity(.25),
        ),
        child: Center(
          child: Row(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.add, size: 18, color: theme.colorScheme.primary),
              const SizedBox(width: 6),
              Text('New playlist',
                  style: theme.textTheme.bodySmall?.copyWith(
                      fontWeight: FontWeight.w700,
                      color: theme.colorScheme.primary)),
            ],
          ),
        ),
      ),
    );
  }
}

// ---- Split-pane bits -----------------------------------------------------

class _DragHandle extends StatelessWidget {
  const _DragHandle({required this.onDelta});
  final ValueChanged<double> onDelta;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return MouseRegion(
      cursor: SystemMouseCursors.resizeRow,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onVerticalDragUpdate: (details) => onDelta(details.delta.dy),
        child: Container(
          height: _kHandleHeight,
          color: theme.colorScheme.surfaceContainerHighest,
          alignment: Alignment.center,
          child: Container(
            width: 36,
            height: 3,
            decoration: BoxDecoration(
              color: theme.colorScheme.outlineVariant,
              borderRadius: BorderRadius.circular(2),
            ),
          ),
        ),
      ),
    );
  }
}

class _TrackPane extends StatelessWidget {
  const _TrackPane({
    required this.albumName,
    required this.artistFallback,
    required this.isPlaylist,
    required this.tracks,
    required this.onPlay,
    required this.onShuffle,
    required this.onClose,
  });
  final String                          albumName;
  final String?                         artistFallback;
  final bool                            isPlaylist;
  final List<LibraryEntry>              tracks;
  /// Called when the user taps a row. The whole [tracks] list goes to the
  /// player as the queue; the second argument is the index of the tapped
  /// row, so playback starts there and auto-advances through the rest of
  /// the album.
  final void Function(List<LibraryEntry> tracks, int index) onPlay;
  final VoidCallback                    onShuffle;
  final VoidCallback                    onClose;

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final artist = (tracks.isNotEmpty && tracks.first.artist.trim().isNotEmpty)
        ? tracks.first.artist
        : (artistFallback ?? '');
    final year = tracks
        .map((e) => e.year)
        .firstWhere((y) => y > 0, orElse: () => 0);
    final localCount = tracks.where((e) => e.isLocal).length;
    final seed = isPlaylist || tracks.isEmpty
        ? seedFromName(albumName)
        : seedFromHash(tracks.first.songId);
    return Material(
      elevation: 4,
      color: theme.colorScheme.surface,
      child: Column(
        children: [
          Container(
            padding: const EdgeInsets.fromLTRB(12, 8, 6, 8),
            decoration: BoxDecoration(
              color: theme.colorScheme.surfaceContainerHighest,
            ),
            child: Row(
              children: [
                ClipRRect(
                  borderRadius: BorderRadius.circular(8),
                  child: CoverArt(seed: seed, size: 40),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: Column(
                    mainAxisSize: MainAxisSize.min,
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        albumName,
                        maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.titleSmall?.copyWith(
                          fontWeight: FontWeight.w700),
                      ),
                      Text(
                        [
                          if (artist.isNotEmpty) artist,
                          if (year > 0) '$year',
                          '${tracks.length} track${tracks.length == 1 ? "" : "s"}',
                          '$localCount local',
                        ].join(' · '),
                        maxLines: 1, overflow: TextOverflow.ellipsis,
                        style: theme.textTheme.bodySmall?.copyWith(
                          color: theme.colorScheme.onSurfaceVariant),
                      ),
                    ],
                  ),
                ),
                IconButton(
                  tooltip: 'Play all',
                  icon: Icon(Icons.play_circle_fill,
                      size: 26, color: theme.colorScheme.primary),
                  onPressed:
                      tracks.isEmpty ? null : () => onPlay(tracks, 0),
                ),
                IconButton(
                  tooltip: 'Shuffle',
                  icon: const Icon(Icons.shuffle, size: 20),
                  onPressed: tracks.isEmpty ? null : onShuffle,
                ),
                IconButton(
                  tooltip: 'Close',
                  icon: const Icon(Icons.close, size: 20),
                  onPressed: onClose,
                ),
              ],
            ),
          ),
          Expanded(
            child: ListView.builder(
              itemCount: tracks.length,
              itemBuilder: (context, i) => _LocalEntryRow(
                entry: tracks[i],
                index: i,
                onPlay: () => onPlay(tracks, i),
              ),
            ),
          ),
        ],
      ),
    );
  }
}

class _LocalEntryRow extends StatelessWidget {
  const _LocalEntryRow({
    required this.entry,
    required this.index,
    required this.onPlay,
  });
  final LibraryEntry entry;
  final int          index;
  final VoidCallback onPlay;

  String _fmtDuration(int ms) {
    if (ms <= 0) return '--:--';
    final total = (ms / 1000).round();
    final m = (total ~/ 60).toString();
    final s = (total % 60).toString().padLeft(2, '0');
    return '$m:$s';
  }

  Future<void> _showMenu(BuildContext context, Offset pos) async {
    final overlay = Overlay.of(context).context.findRenderObject() as RenderBox;
    final picked = await showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        pos & const Size(40, 40),
        Offset.zero & overlay.size,
      ),
      items: const [
        PopupMenuItem(
          value: 'delete',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.delete_outline,
                          size: 18, color: Colors.red),
            title: Text('Delete song'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
        PopupMenuDivider(),
        PopupMenuItem(
          value: 'dmca',
          child: ListTile(
            dense: true,
            leading: Icon(Icons.gavel_outlined, size: 18),
            title: Text('About copyright / DMCA'),
            contentPadding: EdgeInsets.zero,
          ),
        ),
      ],
    );
    if (picked == 'delete' && context.mounted) {
      await _confirmAndDelete(context);
    } else if (picked == 'dmca' && context.mounted) {
      Navigator.of(context).push(MaterialPageRoute(
        builder: (_) => const DmcaScreen(),
      ));
    }
  }

  Future<void> _confirmAndDelete(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final ok = await showDialog<bool>(
      context: context,
      builder: (ctx) => AlertDialog(
        title: const Text('Delete song?'),
        content: Text(
          'Remove "${entry.title.isEmpty ? "Untitled" : entry.title}" from your '
          'library? Downloaded files are deleted from disk; files in folders '
          'you scanned stay where they are.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(ctx, false),
            child: const Text('Cancel'),
          ),
          FilledButton.tonal(
            style: FilledButton.styleFrom(
                foregroundColor: Theme.of(ctx).colorScheme.error),
            onPressed: () => Navigator.pop(ctx, true),
            child: const Text('Delete'),
          ),
        ],
      ),
    );
    if (ok != true) return;
    final r = await LocalLibraryActions.instance.deleteEntry(entry);
    messenger?.showSnackBar(SnackBar(
      content: Text(
        r.fileDeleted
            ? 'Deleted "${entry.title}" + file from disk.'
            : 'Removed "${entry.title}" from library.',
      ),
      duration: const Duration(seconds: 2),
    ));
  }

  void _queueDownload(BuildContext context) {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final canonical = entry.canonicalHash.isNotEmpty
        ? entry.canonicalHash
        : entry.contentHash;
    final song = Song(
      contentHash:     canonical,
      fingerprintHash: entry.fingerprintHash,
      title:           entry.title,
      artist:          entry.artist,
      album:           entry.album,
      genre:           entry.genre,
      year:            entry.year,
      trackNumber:     entry.trackNumber,
      durationMs:      entry.durationMs,
    );
    DownloadProvider.instance.enqueueSong(song);
    messenger?.showSnackBar(SnackBar(
      content: Text('Queued "${entry.title.isEmpty
          ? entry.contentHash.substring(0, 8)
          : entry.title}"'),
      duration: const Duration(seconds: 2),
    ));
  }

  @override
  Widget build(BuildContext context) {
    final theme   = Theme.of(context);
    final isLocal = entry.isLocal;
    final playing = context.select<PlayerProvider, String>(
        (p) => p.currentSong?.contentHash ?? '');
    final isPlaying = playing == entry.songId ||
        playing == entry.contentHash;
    final dl = context.watch<DownloadProvider>();
    DownloadJob? activeJob;
    for (final j in dl.activeJobs) {
      final canonical = entry.canonicalHash.isNotEmpty
          ? entry.canonicalHash
          : entry.contentHash;
      if (j.song.contentHash == canonical) {
        activeJob = j;
        break;
      }
    }

    final subtitle = StringBuffer();
    if (entry.album.trim().isNotEmpty) {
      subtitle.write(entry.album);
      subtitle.write('  •  ');
    }
    if (entry.artist.trim().isNotEmpty) {
      subtitle.write(entry.artist);
      subtitle.write('  •  ');
    }
    subtitle.write(_fmtDuration(entry.durationMs));

    Widget trailing;
    if (isLocal) {
      trailing = Row(
        mainAxisSize: MainAxisSize.min,
        mainAxisAlignment: MainAxisAlignment.end,
        children: const [
          Icon(Icons.audiotrack, color: Colors.green, size: 18),
          SizedBox(width: 4),
          Text('local',
              style: TextStyle(fontSize: 10, color: Colors.green)),
        ],
      );
    } else if (activeJob != null) {
      trailing = SizedBox(
        width: 18, height: 18,
        child: CircularProgressIndicator(
          strokeWidth: 2.5,
          value: activeJob.total > 0
              ? activeJob.received / activeJob.total
              : null,
        ),
      );
    } else {
      trailing = Row(
        mainAxisSize: MainAxisSize.min,
        mainAxisAlignment: MainAxisAlignment.end,
        children: [
          IconButton(
            tooltip: 'Download to library',
            padding: EdgeInsets.zero,
            constraints: const BoxConstraints(minWidth: 28, minHeight: 28),
            icon: const Icon(Icons.download_for_offline_outlined, size: 18),
            onPressed: () => _queueDownload(context),
          ),
          const SizedBox(width: 2),
          const Text('remote',
              style: TextStyle(fontSize: 10, color: Colors.blueGrey)),
        ],
      );
    }

    return GestureDetector(
      behavior: HitTestBehavior.translucent,
      onSecondaryTapDown: (d) => _showMenu(context, d.globalPosition),
      onLongPressStart:   (d) => _showMenu(context, d.globalPosition),
      child: Opacity(
        opacity: isLocal ? 1 : .55,
        child: ListTile(
          dense: true,
          leading: SizedBox(
            width: 66,
            child: Row(
              mainAxisSize: MainAxisSize.min,
              children: [
                SizedBox(
                  width: 22,
                  child: Text(
                    entry.trackNumber > 0
                        ? '${entry.trackNumber}'
                        : '${index + 1}',
                    textAlign: TextAlign.right,
                    style: theme.textTheme.bodySmall,
                  ),
                ),
                const SizedBox(width: 8),
                ClipRRect(
                  borderRadius: BorderRadius.circular(6),
                  child: CoverArt(
                      seed: seedFromHash(entry.songId), size: 34),
                ),
              ],
            ),
          ),
          title: Text(
            entry.title.isEmpty ? '(untitled)' : entry.title,
            maxLines: 1, overflow: TextOverflow.ellipsis,
            style: TextStyle(
              fontWeight: isPlaying ? FontWeight.w700 : FontWeight.w500,
              color: isPlaying ? theme.colorScheme.primary : null,
            ),
          ),
          subtitle: Text(
            subtitle.toString(),
            style: const TextStyle(fontSize: 11),
            maxLines: 1, overflow: TextOverflow.ellipsis,
          ),
          onTap: isLocal ? onPlay : null,
          trailing: SizedBox(width: 96, child: trailing),
        ),
      ),
    );
  }
}

class _ScannerProgressBar extends StatelessWidget {
  const _ScannerProgressBar({required this.scanner});
  final LibraryScanner scanner;

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.fromLTRB(16, 4, 16, 8),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const LinearProgressIndicator(),
          const SizedBox(height: 4),
          Text(
            'scanned ${scanner.scanned}  matched ${scanner.matched}  '
            'registered ${scanner.registered}  errors ${scanner.errors}',
            style: const TextStyle(fontSize: 11, color: Colors.grey),
          ),
        ],
      ),
    );
  }
}
