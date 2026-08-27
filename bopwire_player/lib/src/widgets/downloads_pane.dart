// Downloads pane — one transfer list covering EVERY network the player
// pulls from, shown as a facet of the My Library tab.
//
// Two independent download systems feed it:
//   * DownloadProvider       — bopwire's own swarm (piece-wise, multi-seeder)
//   * NetworkDownloadManager — Soulseek / napstr
// They have separate job types and separate lifecycles, so this pane
// normalises both into one row shape rather than showing two lists. A
// download is a download; which network it came from is a column, not a
// reason for a different screen.
//
// Downloads used to be invisible: you tapped a result, a snackbar said
// "downloading", then nothing until the track silently appeared in the
// library — or didn't. Soulseek transfers in particular can sit queued
// behind a peer's upload slots for minutes, so "stuck or just waiting?"
// was unanswerable. Per job this shows name, network, file type, byte
// progress, percentage, and which phase it is in — including the import
// step after the bytes land, which is when fingerprinting and chain
// registration happen and is a visible pause on a large file.

import 'package:flutter/material.dart';

import '../providers/download_provider.dart' as native;
import '../services/networks/external_network.dart';
import '../services/networks/network_download_manager.dart' as foreign;
import '../services/networks/network_registry.dart';

/// One row, whichever system produced it.
class _Row {
  const _Row({
    required this.title,
    required this.network,
    required this.fileType,
    required this.status,
    required this.colour,
    required this.received,
    this.total,
    this.fraction,
    this.indeterminate = false,
    this.error,
    this.detail,
    this.onCancel,
  });

  final String  title;
  final String  network;
  final String  fileType;
  final String  status;
  final Color   colour;
  final int     received;
  final int?    total;
  final double? fraction;
  final bool    indeterminate;
  final String? error;
  final String? detail;
  final VoidCallback? onCancel;

  bool get showBar => fraction != null || indeterminate;
}

class DownloadsPane extends StatelessWidget {
  const DownloadsPane({super.key});

  static String _bytes(int n) {
    if (n <= 0) return '0 B';
    const units = ['B', 'KB', 'MB', 'GB'];
    var v = n.toDouble();
    var u = 0;
    while (v >= 1024 && u < units.length - 1) {
      v /= 1024;
      u++;
    }
    final s = v.toStringAsFixed(v >= 10 || u == 0 ? 0 : 1);
    return '${s.endsWith('.0') ? s.substring(0, s.length - 2) : s} ${units[u]}';
  }

  static String _extOf(String? explicit, String fallbackPath) {
    if (explicit != null && explicit.isNotEmpty) return explicit.toUpperCase();
    final dot = fallbackPath.lastIndexOf('.');
    if (dot > 0 && dot < fallbackPath.length - 1) {
      final e = fallbackPath.substring(dot + 1);
      if (e.length <= 5) return e.toUpperCase();
    }
    return '—';
  }

  // ---- adapters -------------------------------------------------------

  static _Row _fromForeign(BuildContext ctx, foreign.DownloadJob j) {
    final scheme = Theme.of(ctx).colorScheme;
    final (status, colour) = switch (j.state) {
      foreign.JobState.queued    => ('Queued',      scheme.onSurfaceVariant),
      foreign.JobState.running   => ('Downloading', scheme.primary),
      // Its own state on purpose: bytes are on disk, but the track is not
      // in the library or on chain until fingerprint + tag import finish.
      foreign.JobState.importing => ('Indexing',    Colors.amber),
      foreign.JobState.done      => ('In library',  Colors.green),
      foreign.JobState.failed    => ('Failed',      scheme.error),
      foreign.JobState.cancelled => ('Cancelled',   scheme.onSurfaceVariant),
    };
    final ExternalTrack t = j.track;
    final net = NetworkRegistry.instance.byId(j.networkId);
    return _Row(
      title: (t.artist == null || t.artist!.isEmpty)
          ? t.title
          : '${t.artist} — ${t.title}',
      network: net?.displayName ?? j.networkId,
      fileType: _extOf(t.extension, t.remotePath ?? t.title),
      status: status,
      colour: colour,
      received: j.received,
      total: j.total,
      fraction: j.state == foreign.JobState.running ? j.fraction : null,
      indeterminate: j.state == foreign.JobState.importing,
      error: j.error,
      detail: j.state == foreign.JobState.done && j.localPath != null
          ? 'Fingerprinted and registered · ${j.localPath}'
          : null,
      onCancel: (j.state == foreign.JobState.running ||
                 j.state == foreign.JobState.queued)
          ? () => foreign.NetworkDownloadManager.instance.cancel(j)
          : null,
    );
  }

  static _Row _fromNative(BuildContext ctx, native.DownloadJob j) {
    final scheme = Theme.of(ctx).colorScheme;
    final (status, colour) = switch (j.status) {
      native.DownloadStatus.queued  => ('Queued',      scheme.onSurfaceVariant),
      native.DownloadStatus.running => ('Downloading', scheme.primary),
      native.DownloadStatus.done    => ('In library',  Colors.green),
      native.DownloadStatus.failed  => ('Failed',      scheme.error),
    };
    // bopwire's swarm reports total as 0 until the manifest lands, so a
    // fraction would read 0% for a track that is actually moving.
    final double? frac = (j.status == native.DownloadStatus.running &&
                          j.total > 0)
        ? (j.received / j.total).clamp(0.0, 1.0)
        : null;
    final artist = j.song.artist;
    return _Row(
      title: artist.isEmpty ? j.song.title : '$artist — ${j.song.title}',
      network: 'bopwire',
      fileType: _extOf(null, j.song.title),
      status: status,
      colour: colour,
      received: j.received,
      total: j.total > 0 ? j.total : null,
      fraction: frac,
      indeterminate: j.status == native.DownloadStatus.running && j.total <= 0,
      error: j.error,
      detail: j.batchLabel,
    );
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    // Both managers mutate their jobs in place as bytes arrive, so the
    // pane rebuilds on their notifications rather than a parent setState.
    return AnimatedBuilder(
      animation: Listenable.merge([
        foreign.NetworkDownloadManager.instance,
        native.DownloadProvider.instance,
      ]),
      builder: (context, _) {
        final rows = <_Row>[
          for (final j in foreign.NetworkDownloadManager.instance.jobs)
            _fromForeign(context, j),
          for (final j in native.DownloadProvider.instance.activeJobs)
            _fromNative(context, j),
          for (final j in native.DownloadProvider.instance.recentJobs)
            _fromNative(context, j),
        ];

        if (rows.isEmpty) return _empty(theme);

        final active = foreign.NetworkDownloadManager.instance.activeCount +
            native.DownloadProvider.instance.activeJobs.length;

        return Column(
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(12, 8, 8, 4),
              child: Row(
                children: [
                  Expanded(
                    child: Text(
                      active > 0
                          ? '$active active · ${rows.length} total'
                          : '${rows.length} download'
                              '${rows.length == 1 ? '' : 's'}',
                      style: theme.textTheme.labelMedium?.copyWith(
                          fontWeight: FontWeight.w700,
                          color: theme.colorScheme.onSurfaceVariant),
                    ),
                  ),
                  TextButton.icon(
                    onPressed: () {
                      foreign.NetworkDownloadManager.instance.clearFinished();
                      native.DownloadProvider.instance.clearRecent();
                    },
                    icon: const Icon(Icons.clear_all, size: 16),
                    label: const Text('Clear finished'),
                    style: TextButton.styleFrom(
                        visualDensity: VisualDensity.compact),
                  ),
                ],
              ),
            ),
            Expanded(
              child: ListView.separated(
                padding: const EdgeInsets.fromLTRB(8, 0, 8, 16),
                itemCount: rows.length,
                separatorBuilder: (_, __) => const Divider(height: 1),
                itemBuilder: (context, i) => _tile(theme, rows[i]),
              ),
            ),
          ],
        );
      },
    );
  }

  Widget _empty(ThemeData theme) => Center(
        child: Padding(
          padding: const EdgeInsets.all(32),
          child: Column(
            mainAxisSize: MainAxisSize.min,
            children: [
              Icon(Icons.download_outlined,
                  size: 56, color: theme.colorScheme.onSurfaceVariant),
              const SizedBox(height: 14),
              Text('No downloads yet',
                  style: theme.textTheme.titleSmall
                      ?.copyWith(fontWeight: FontWeight.w700)),
              const SizedBox(height: 6),
              Text(
                'Downloads from bopwire, Soulseek and napstr all show up '
                'here. Finished files are fingerprinted, tag-imported and '
                'registered on chain automatically.',
                textAlign: TextAlign.center,
                style: theme.textTheme.bodySmall
                    ?.copyWith(color: theme.colorScheme.onSurfaceVariant),
              ),
            ],
          ),
        ),
      );

  Widget _tile(ThemeData theme, _Row r) {
    final pct = r.fraction == null ? null : r.fraction! * 100;
    // Total is unknown on some transfers until the peer answers, so show
    // what we have rather than inventing a denominator.
    final bytes = r.total != null
        ? '${_bytes(r.received)} / ${_bytes(r.total!)}'
        : _bytes(r.received);

    return ListTile(
      dense: true,
      contentPadding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
      title: Text(r.title, maxLines: 1, overflow: TextOverflow.ellipsis),
      subtitle: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          const SizedBox(height: 3),
          Row(
            children: [
              Text(r.status,
                  style: theme.textTheme.labelSmall?.copyWith(
                      color: r.colour, fontWeight: FontWeight.w800)),
              const SizedBox(width: 8),
              Expanded(
                child: Text(
                  '${r.network} · ${r.fileType}'
                  '${pct == null ? '' : ' · ${pct.toStringAsFixed(0)}%'}'
                  ' · $bytes',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.labelSmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant),
                ),
              ),
            ],
          ),
          const SizedBox(height: 5),
          if (r.showBar)
            ClipRRect(
              borderRadius: BorderRadius.circular(3),
              child: LinearProgressIndicator(
                value: r.indeterminate ? null : r.fraction,
                minHeight: 4,
              ),
            ),
          if (r.error != null)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text(r.error!,
                  style: theme.textTheme.labelSmall
                      ?.copyWith(color: theme.colorScheme.error)),
            ),
          if (r.detail != null)
            Padding(
              padding: const EdgeInsets.only(top: 4),
              child: Text(r.detail!,
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: theme.textTheme.labelSmall?.copyWith(
                      color: theme.colorScheme.onSurfaceVariant)),
            ),
        ],
      ),
      trailing: r.onCancel == null
          ? null
          : IconButton(
              tooltip: 'Cancel',
              icon: const Icon(Icons.close, size: 18),
              onPressed: r.onCancel,
            ),
    );
  }
}
