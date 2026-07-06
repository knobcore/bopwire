import 'song.dart';

/// One curated Discover row ("Rising", "Best of Jazz", "2019 hits") from the
/// full node's deterministic `collections.list` verb. Songs are joined from
/// the reply's embedded metadata map, so a collection can carry songs that
/// are momentarily unseeded — the UI dims those instead of dropping them
/// (membership is deterministic; availability is a display overlay).
class SongCollection {
  final String id;        // stable, derived: "rising:", "genre:jazz", ...
  final String kind;      // "rising" | "top" | "new" | "genre" | "year"
  final String title;
  final String subtitle;
  final String facet;     // normalized facet value ("jazz", "2019"), '' for globals
  final List<Song> songs; // ordered exactly as the node emitted them

  const SongCollection({
    required this.id,
    required this.kind,
    required this.title,
    required this.subtitle,
    required this.facet,
    required this.songs,
  });
}

/// The whole per-epoch collection set, with the cross-check header. Two
/// honest nodes at the same [epoch] return an identical [contentDigest];
/// comparing digests across nodes corroborates the feed with no signing.
class CollectionSet {
  final int    epoch;
  final int    snapshotHeight;
  final String snapshotBlockHash;
  final String contentDigest;
  final List<SongCollection> collections;

  const CollectionSet({
    required this.epoch,
    required this.snapshotHeight,
    required this.snapshotBlockHash,
    required this.contentDigest,
    required this.collections,
  });

  factory CollectionSet.fromJson(Map<String, dynamic> json) {
    // Field-coerced like Song.fromJson — never hard-cast, one malformed
    // field must not abort the whole feed.
    int _i(dynamic v) {
      if (v is int) return v;
      if (v is num) return v.toInt();
      if (v is String) return int.tryParse(v) ?? 0;
      return 0;
    }

    // The embed join: hash → song metadata, shipped once even when a hash
    // appears in several rows.
    final songsByHash = <String, Song>{};
    final rawSongs = json['songs'];
    if (rawSongs is Map) {
      rawSongs.forEach((k, v) {
        if (v is Map) {
          songsByHash[k.toString()] =
              Song.fromJson(Map<String, dynamic>.from(v));
        }
      });
    }

    final rows = <SongCollection>[];
    final rawCols = json['collections'];
    if (rawCols is List) {
      for (final c in rawCols) {
        if (c is! Map) continue;
        final m = Map<String, dynamic>.from(c);
        final songs = <Song>[];
        final hashes = m['song_hashes'];
        if (hashes is List) {
          for (final h in hashes) {
            final s = songsByHash[h.toString()];
            if (s != null) songs.add(s);
          }
        }
        rows.add(SongCollection(
          id:       m['id']       as String? ?? '',
          kind:     m['kind']     as String? ?? '',
          title:    m['title']    as String? ?? '',
          subtitle: m['subtitle'] as String? ?? '',
          facet:    m['facet']    as String? ?? '',
          songs:    songs,
        ));
      }
    }

    return CollectionSet(
      epoch:             _i(json['epoch']),
      snapshotHeight:    _i(json['snapshot_height']),
      snapshotBlockHash: json['snapshot_block_hash'] as String? ?? '',
      contentDigest:     json['content_digest']      as String? ?? '',
      collections:       rows,
    );
  }
}
