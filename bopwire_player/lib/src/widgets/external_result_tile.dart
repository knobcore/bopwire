// One search hit from a foreign network.
//
// Tap *streams* the track: it starts a throwaway download into the
// pre-download cache and begins playing from the partial file (see
// PredownloadCache). That is the cheap, reversible action, so it is the
// one bound to the primary gesture.
//
// Right-click (or long-press on touch) opens the menu, where the two
// committing actions live: "Download this track" and "Download whole
// folder". Those still go through NetworkDownloadManager, which
// fingerprints the file and registers it on chain — and which now
// promotes the cached bytes instead of re-downloading when the track was
// already streamed.

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/player_provider.dart';
import '../services/networks/external_network.dart';
import '../services/networks/network_download_manager.dart';
import '../services/networks/predownload_cache.dart';

class ExternalResultTile extends StatelessWidget {
  const ExternalResultTile({
    super.key,
    required this.track,
    required this.networkLabel,
  });

  final ExternalTrack track;
  final String networkLabel;

  static String _formatSize(int? bytes) {
    if (bytes == null || bytes <= 0) return '';
    const units = ['B', 'KB', 'MB', 'GB'];
    var v = bytes.toDouble();
    var u = 0;
    while (v >= 1024 && u < units.length - 1) {
      v /= 1024;
      u++;
    }
    // Drop a pointless trailing .0 so a whole number reads "7 MB", not
    // "7.0 MB", while 7.3 MB keeps its digit.
    final s = v.toStringAsFixed(v >= 10 || u == 0 ? 0 : 1);
    final trimmed = s.endsWith('.0') ? s.substring(0, s.length - 2) : s;
    return '$trimmed ${units[u]}';
  }

  /// Bare extension for the track, falling back to the remote path's
  /// suffix when the protocol did not report one (Soulseek often
  /// doesn't).
  static String? extensionOf(ExternalTrack t) {
    final ext = t.extension;
    if (ext != null && ext.isNotEmpty) return ext.toLowerCase();
    final src = t.remotePath ?? t.title;
    final dot = src.lastIndexOf('.');
    if (dot < 0 || dot == src.length - 1) return null;
    return src.substring(dot + 1).toLowerCase();
  }

  String get _subtitle {
    final parts = <String>[networkLabel];
    if (track.owner != null && track.owner!.isNotEmpty) parts.add(track.owner!);
    if (track.isFolder) {
      parts.add(track.childCount == null
          ? 'folder'
          : 'folder · ${track.childCount} files');
    } else {
      final size = _formatSize(track.sizeBytes);
      if (size.isNotEmpty) parts.add(size);
      if (track.bitrate != null) parts.add('${track.bitrate} kbps');
      if (track.extension != null) parts.add(track.extension!.toUpperCase());
    }
    return parts.join(' · ');
  }

  /// Tap action for a file: cache-and-play.
  Future<void> _stream(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);

    // The library player and the preview player are two independent
    // libmpv instances; leaving both running would mix two songs. The
    // preview is the thing the user just asked for, so the library
    // player yields.
    try {
      context.read<PlayerProvider>().stop();
    } on ProviderNotFoundException {
      // Tile used outside the app's provider tree (tests, previews).
    }

    final ext = extensionOf(track);
    // Say up front when the format cannot start early, instead of
    // letting the user watch a "playing" state that produces no sound
    // until the transfer lands.
    messenger?.showSnackBar(SnackBar(
      content: Text(isProgressiveFriendly(ext)
          ? 'Streaming "${track.title}" — playback starts as soon as '
              'enough of the file has arrived.'
          : isTailIndexed(ext)
              ? '"${track.title}" is ${ext!.toUpperCase()}, whose index sits '
                  'at the end of the file, so it can only play once the '
                  'download finishes. Fetching it now…'
              : 'Fetching "${track.title}" — it will play when the download '
                  'finishes.'),
      duration: const Duration(seconds: 4),
    ));

    final entry = await PredownloadCache.instance.stream(track);
    await entry.finished;
    if (entry.state == CacheState.failed) {
      messenger?.showSnackBar(SnackBar(
        content: Text('Could not stream "${track.title}": '
            '${entry.error ?? "transfer failed"}'),
        duration: const Duration(seconds: 5),
      ));
    }
  }

  Future<void> _downloadFile(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    final cached =
        PredownloadCache.instance.entryFor(track)?.state == CacheState.ready;
    await NetworkDownloadManager.instance.downloadTrack(track);
    messenger?.showSnackBar(SnackBar(
      content: Text(cached
          // Worth saying: the user just watched this stream, and a
          // download that finishes instantly otherwise looks like a bug.
          ? 'Copying "${track.title}" from the stream cache — no second '
              'download needed. It will be fingerprinted and added to your '
              'library.'
          : 'Downloading "${track.title}" — it will be '
              'fingerprinted and added to your library automatically.'),
      duration: const Duration(seconds: 3),
    ));
  }

  Future<void> _downloadFolder(BuildContext context) async {
    final messenger = ScaffoldMessenger.maybeOf(context);
    messenger?.showSnackBar(const SnackBar(
      content: Text('Listing folder…'),
      duration: Duration(seconds: 2),
    ));
    final jobs = await NetworkDownloadManager.instance.downloadFolder(track);
    if (jobs.isEmpty) {
      // Either the protocol has no folder concept here or the listing
      // failed — either way, saying "queued 0" would be a lie.
      messenger?.showSnackBar(const SnackBar(
        content: Text('That folder returned no downloadable files.'),
        duration: Duration(seconds: 4),
      ));
      return;
    }
    messenger?.showSnackBar(SnackBar(
      content: Text('Queued ${jobs.length} file'
          '${jobs.length == 1 ? "" : "s"} — each is imported automatically '
          'once it finishes.'),
      duration: const Duration(seconds: 4),
    ));
  }

  void _showMenu(BuildContext context, Offset globalPosition) {
    final overlay =
        Overlay.of(context).context.findRenderObject() as RenderBox?;
    if (overlay == null) return;
    showMenu<String>(
      context: context,
      position: RelativeRect.fromRect(
        globalPosition & const Size(1, 1),
        Offset.zero & overlay.size,
      ),
      items: [
        if (!track.isFolder)
          const PopupMenuItem(
            value: 'stream',
            child: ListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              leading: Icon(Icons.play_circle_outline),
              title: Text('Stream now'),
              subtitle: Text('Cached for 1 hour', style: TextStyle(fontSize: 10)),
            ),
          ),
        if (!track.isFolder)
          const PopupMenuItem(
            value: 'file',
            child: ListTile(
              dense: true,
              contentPadding: EdgeInsets.zero,
              leading: Icon(Icons.download_outlined),
              title: Text('Download this track'),
              subtitle: Text('Keeps it in your library',
                  style: TextStyle(fontSize: 10)),
            ),
          ),
        // Offered for plain files too: the file's containing folder is
        // usually the album, which is what people actually want.
        const PopupMenuItem(
          value: 'folder',
          child: ListTile(
            dense: true,
            contentPadding: EdgeInsets.zero,
            leading: Icon(Icons.folder_open_outlined),
            title: Text('Download whole folder'),
          ),
        ),
      ],
    ).then((choice) {
      if (!context.mounted || choice == null) return;
      if (choice == 'stream') _stream(context);
      if (choice == 'file') _downloadFile(context);
      if (choice == 'folder') _downloadFolder(context);
    });
  }

  @override
  Widget build(BuildContext context) {
    return GestureDetector(
      onSecondaryTapDown: (d) => _showMenu(context, d.globalPosition),
      onLongPressStart: (d) => _showMenu(context, d.globalPosition),
      child: AnimatedBuilder(
        // Repaints this row as its own cache entry progresses, so the
        // user sees the buffer filling rather than a dead row.
        animation: PredownloadCache.instance,
        builder: (context, _) {
          final entry =
              track.isFolder ? null : PredownloadCache.instance.entryFor(track);
          return ListTile(
            dense: true,
            leading: _leading(entry),
            title: Text(
              track.artist == null || track.artist!.isEmpty
                  ? track.title
                  : '${track.artist} — ${track.title}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
            subtitle: Text(_subtitleFor(entry),
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(fontSize: 11)),
            onTap: () =>
                track.isFolder ? _downloadFolder(context) : _stream(context),
          );
        },
      ),
    );
  }

  String _subtitleFor(CacheEntry? entry) {
    if (entry == null) return _subtitle;
    switch (entry.state) {
      case CacheState.downloading:
        final f = entry.fraction;
        final pct = f == null ? '' : ' · ${(f * 100).toStringAsFixed(0)}%';
        return '$_subtitle · buffering$pct';
      case CacheState.ready:
        return '$_subtitle · cached';
      case CacheState.failed:
        return '$_subtitle · ${entry.error ?? "failed"}';
      case CacheState.paused:
        // Superseded by another preview. Say so rather than showing a
        // stale "buffering 42%" for a transfer that has stopped.
        return '$_subtitle · preview stopped';
      case CacheState.cancelled:
      case CacheState.evicted:
        return _subtitle;
    }
  }

  Widget _leading(CacheEntry? entry) {
    if (track.isFolder) return const Icon(Icons.folder_outlined);
    if (entry != null && entry.state == CacheState.downloading) {
      return SizedBox(
        width: 24,
        height: 24,
        child: Center(
          child: SizedBox(
            width: 16,
            height: 16,
            child: CircularProgressIndicator(
              strokeWidth: 2,
              // Null value = indeterminate, which is the honest render
              // when the protocol never told us the file size.
              value: entry.fraction,
            ),
          ),
        ),
      );
    }
    if (entry != null && entry.state == CacheState.ready) {
      return const Icon(Icons.play_circle_outline);
    }
    return const Icon(Icons.cloud_download_outlined);
  }
}
