// AlbumArt / ArtistArt — generated cover art with a real-cover overlay.
//
// AlbumArt renders [CoverArt] from [seed] immediately, then — when [artist] is
// given and the full node has a scraped cover for (artist, album) in DB2 — swaps
// in the real JPEG (fetched once per album via LibraryProvider.albumArt, cached
// for the process). A miss or decode error keeps the generated art.
//
// ArtistArt is the artist-thumbnail variant: given an artist and a handful of
// their album names, it fetches those albums' covers and cycles through the ones
// that resolve to real art (a gentle slideshow), so an artist tile shows real
// album covers instead of a generated motif. Falls back to CoverArt when the
// node has no covers for any of the artist's albums yet.

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/library_provider.dart';
import 'cover_art.dart';

class AlbumArt extends StatefulWidget {
  const AlbumArt({
    super.key,
    required this.seed,
    required this.size,
    this.artist,
    this.album,
  });

  final List<int> seed;
  final double size;
  final String? artist;
  final String? album;

  @override
  State<AlbumArt> createState() => _AlbumArtState();
}

class _AlbumArtState extends State<AlbumArt> {
  Uint8List? _bytes;

  @override
  void initState() {
    super.initState();
    _resolve();
  }

  @override
  void didUpdateWidget(AlbumArt old) {
    super.didUpdateWidget(old);
    if (old.artist != widget.artist || old.album != widget.album) {
      _bytes = null;
      _resolve();
    }
  }

  void _resolve() {
    final artist = widget.artist;
    if (artist == null || artist.trim().isEmpty) return; // no key → generated art
    final album = widget.album ?? '';
    context.read<LibraryProvider>().albumArt(artist, album).then((b) {
      if (!mounted || b == null) return;
      // Ignore a late result if this widget was recycled onto another album.
      if (widget.artist == artist && (widget.album ?? '') == album) {
        setState(() => _bytes = b);
      }
    });
  }

  @override
  Widget build(BuildContext context) {
    final bytes = _bytes;
    if (bytes != null) {
      return Image.memory(
        bytes,
        width: widget.size,
        height: widget.size,
        fit: BoxFit.cover,
        gaplessPlayback: true,
        errorBuilder: (_, __, ___) =>
            CoverArt(seed: widget.seed, size: widget.size),
      );
    }
    return CoverArt(seed: widget.seed, size: widget.size);
  }
}

/// Artist thumbnail: cycles through the real covers of the artist's albums.
class ArtistArt extends StatefulWidget {
  const ArtistArt({
    super.key,
    required this.seed,
    required this.size,
    required this.artist,
    required this.albums,
  });

  final List<int> seed;
  final double size;
  final String artist;
  final List<String> albums; // candidate album names for this artist

  // Cap the covers we fetch per artist so a grid of artist tiles doesn't fan out
  // into hundreds of RPCs; the node caches, and this is enough to cycle.
  static const int _maxAlbums = 6;
  static const Duration _cyclePeriod = Duration(seconds: 6);

  @override
  State<ArtistArt> createState() => _ArtistArtState();
}

class _ArtistArtState extends State<ArtistArt> {
  final List<Uint8List> _covers = [];
  int _idx = 0;
  Timer? _timer;

  @override
  void initState() {
    super.initState();
    _resolve();
  }

  @override
  void didUpdateWidget(ArtistArt old) {
    super.didUpdateWidget(old);
    if (old.artist != widget.artist ||
        !_sameList(old.albums, widget.albums)) {
      _timer?.cancel();
      _covers.clear();
      _idx = 0;
      _resolve();
    }
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }

  bool _sameList(List<String> a, List<String> b) {
    if (a.length != b.length) return false;
    for (var i = 0; i < a.length; i++) {
      if (a[i] != b[i]) return false;
    }
    return true;
  }

  void _resolve() {
    final artist = widget.artist;
    if (artist.trim().isEmpty) return;
    final prov = context.read<LibraryProvider>();
    // Distinct, non-empty album names, capped.
    final seen = <String>{};
    final albums = <String>[];
    for (final a in widget.albums) {
      final t = a.trim();
      if (t.isEmpty) continue;
      if (seen.add(t.toLowerCase())) albums.add(t);
      if (albums.length >= ArtistArt._maxAlbums) break;
    }
    for (final album in albums) {
      prov.albumArt(artist, album).then((b) {
        if (!mounted || b == null) return;
        if (widget.artist != artist) return; // recycled onto another artist
        setState(() {
          _covers.add(b);
          if (_covers.length == 1) {
            // Stagger the first shown cover so neighbouring tiles differ.
            _idx = 0;
          } else if (_covers.length == 2 && _timer == null) {
            _timer = Timer.periodic(ArtistArt._cyclePeriod, (_) {
              if (!mounted || _covers.length < 2) return;
              setState(() => _idx = (_idx + 1) % _covers.length);
            });
          }
        });
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    if (_covers.isEmpty) {
      return CoverArt(seed: widget.seed, size: widget.size);
    }
    final cover = _covers[_idx % _covers.length];
    return AnimatedSwitcher(
      duration: const Duration(milliseconds: 550),
      child: Image.memory(
        cover,
        key: ValueKey<int>(_idx % _covers.length),
        width: widget.size,
        height: widget.size,
        fit: BoxFit.cover,
        gaplessPlayback: true,
        errorBuilder: (_, __, ___) =>
            CoverArt(seed: widget.seed, size: widget.size),
      ),
    );
  }
}
