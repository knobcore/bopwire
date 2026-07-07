// AlbumArt — generated cover art with a real-cover overlay.
//
// Renders [CoverArt] from [seed] immediately, then — when [artist] is given and
// the full node has a scraped cover for (artist, album) in DB2 — swaps in the
// real JPEG (fetched once per album via LibraryProvider.albumArt, cached for the
// process). A miss or decode error keeps the generated art, so surfaces with no
// real cover look exactly as before. Drop-in for CoverArt at song/album sites.

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
