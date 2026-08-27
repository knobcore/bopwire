// Client-side filtering for foreign-network search results.
//
// Foreign hits (Soulseek / napstr) stream in progressively over several
// seconds, and a popular query can return hundreds of rows from dozens of
// peers. This file holds the filter model — a small immutable value the
// results view keeps across batches — and the chip bar that edits it.
//
// Two design rules worth keeping:
//   * The filter stores which file types are turned OFF, not which are on.
//     Results arrive batch by batch, so the set of available types grows
//     while the user is looking at the list; a type that appears mid-search
//     must default to visible, and the user's off-toggles must survive
//     every new batch. An allowlist would need re-seeding on each arrival
//     and would silently hide late-arriving types.
//   * The chip set is built from the extensions actually present in the
//     current results — a result set with no WAV shows no WAV chip.

import 'package:flutter/material.dart';

import '../services/networks/external_network.dart';
import 'external_result_tile.dart';

/// Immutable filter state over a list of [ExternalTrack]s.
class ExternalResultFilter {
  const ExternalResultFilter({
    this.disabledTypes = const {},
    this.foldersOnly = false,
    this.minBitrate = 0,
    this.text = '',
  });

  /// Pseudo-type key used for folder results, which have no extension.
  static const folderType = 'folder';

  /// Type key for files whose extension cannot be determined at all.
  static const unknownType = 'other';

  /// Lossless containers are exempt from the bitrate floor: Soulseek
  /// rarely reports a bitrate for FLAC, and "give me ≥320" is a quality
  /// filter that must never hide lossless files behind missing metadata.
  static const losslessTypes = {'flac', 'wav', 'alac', 'ape', 'aiff', 'aif'};

  static const empty = ExternalResultFilter();

  /// Type keys ([folderType], extensions, [unknownType]) the user turned
  /// off. Everything not listed here is visible — see the header comment
  /// for why this is a denylist.
  final Set<String> disabledTypes;

  /// Show only folder results (Soulseek directory shares).
  final bool foldersOnly;

  /// Minimum bitrate in kbps; 0 = no floor. Applies to lossy files with a
  /// known bitrate; lossless files and folders always pass.
  final int minBitrate;

  /// Case-insensitive substring match over artist/title/path/owner.
  final String text;

  bool get isActive =>
      disabledTypes.isNotEmpty ||
      foldersOnly ||
      minBitrate > 0 ||
      text.isNotEmpty;

  ExternalResultFilter copyWith({
    Set<String>? disabledTypes,
    bool? foldersOnly,
    int? minBitrate,
    String? text,
  }) =>
      ExternalResultFilter(
        disabledTypes: disabledTypes ?? this.disabledTypes,
        foldersOnly: foldersOnly ?? this.foldersOnly,
        minBitrate: minBitrate ?? this.minBitrate,
        text: text ?? this.text,
      );

  ExternalResultFilter toggleType(String type) {
    final next = Set<String>.from(disabledTypes);
    if (!next.remove(type)) next.add(type);
    return copyWith(disabledTypes: next);
  }

  /// Filter type key for a track: [folderType] for folders, the lowercase
  /// extension otherwise (derived from the remote path when the protocol
  /// didn't report one — same rule the result tile uses), [unknownType]
  /// when there is nothing to derive it from.
  static String typeOf(ExternalTrack t) {
    if (t.isFolder) return folderType;
    return ExternalResultTile.extensionOf(t) ?? unknownType;
  }

  bool allows(ExternalTrack t) {
    if (text.isNotEmpty) {
      final hay = '${t.artist ?? ''} ${t.title} '
              '${t.remotePath ?? ''} ${t.owner ?? ''}'
          .toLowerCase();
      if (!hay.contains(text.toLowerCase())) return false;
    }
    // "Only folders" wins over a disabled folder chip — the user's most
    // recent intent is to see folders, and honouring both at once would
    // render an inexplicably empty list.
    if (foldersOnly) return t.isFolder;
    final type = typeOf(t);
    if (disabledTypes.contains(type)) return false;
    if (t.isFolder) return true; // bitrate has no meaning for a directory
    if (minBitrate > 0 && !losslessTypes.contains(type)) {
      final b = t.bitrate;
      if (b == null || b < minBitrate) return false;
    }
    return true;
  }

  List<ExternalTrack> apply(List<ExternalTrack> tracks) =>
      [for (final t in tracks) if (allows(t)) t];
}

/// Compact filter chip row for the foreign-results block, visually in the
/// same family as the network toggles beside the search box: dense
/// [FilterChip]s, labelSmall text, no panel chrome.
class ExternalResultFilterBar extends StatefulWidget {
  const ExternalResultFilterBar({
    super.key,
    required this.filter,
    required this.results,
    required this.hiddenCount,
    required this.onChanged,
  });

  final ExternalResultFilter filter;

  /// Current UNFILTERED foreign results (all networks pooled); the chip
  /// set is derived from what is actually in here.
  final List<ExternalTrack> results;

  /// How many results the current filter hides, across all networks.
  final int hiddenCount;

  final ValueChanged<ExternalResultFilter> onChanged;

  static const bitrateOptions = [0, 128, 192, 256, 320];

  @override
  State<ExternalResultFilterBar> createState() =>
      _ExternalResultFilterBarState();
}

class _ExternalResultFilterBarState extends State<ExternalResultFilterBar> {
  late final TextEditingController _textCtrl =
      TextEditingController(text: widget.filter.text);

  @override
  void didUpdateWidget(ExternalResultFilterBar old) {
    super.didUpdateWidget(old);
    // Keep the field in sync when the filter is replaced from outside
    // (reset button, new query) without fighting the user's own typing —
    // after a local edit the two are already equal.
    if (widget.filter.text != _textCtrl.text) {
      _textCtrl.text = widget.filter.text;
    }
  }

  @override
  void dispose() {
    _textCtrl.dispose();
    super.dispose();
  }

  void _emit(ExternalResultFilter f) => widget.onChanged(f);

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    final filter = widget.filter;

    // Types present in the current results, most common first so the
    // chips that matter sit at the front of the row.
    final counts = <String, int>{};
    var hasFolders = false;
    var hasBitrate = false;
    for (final t in widget.results) {
      final type = ExternalResultFilter.typeOf(t);
      counts[type] = (counts[type] ?? 0) + 1;
      if (t.isFolder) hasFolders = true;
      if (!t.isFolder && t.bitrate != null) hasBitrate = true;
    }
    final fileTypes = counts.keys
        .where((k) => k != ExternalResultFilter.folderType)
        .toList()
      ..sort((a, b) {
        final byCount = counts[b]!.compareTo(counts[a]!);
        return byCount != 0 ? byCount : a.compareTo(b);
      });

    Widget chip({
      required String label,
      required bool selected,
      required ValueChanged<bool> onSelected,
      String? tooltip,
      Widget? avatar,
    }) {
      final c = FilterChip(
        visualDensity: VisualDensity.compact,
        materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
        label: Text(label, style: theme.textTheme.labelSmall),
        avatar: avatar,
        selected: selected,
        onSelected: onSelected,
      );
      return tooltip == null ? c : Tooltip(message: tooltip, child: c);
    }

    return Wrap(
      spacing: 6,
      runSpacing: 4,
      crossAxisAlignment: WrapCrossAlignment.center,
      children: [
        // Free-text refine over what already came back — no re-query.
        SizedBox(
          width: 180,
          height: 32,
          child: TextField(
            controller: _textCtrl,
            onChanged: (v) => _emit(filter.copyWith(text: v.trim())),
            style: theme.textTheme.bodySmall,
            decoration: InputDecoration(
              hintText: 'Filter results…',
              prefixIcon: const Icon(Icons.filter_alt_outlined, size: 15),
              prefixIconConstraints:
                  const BoxConstraints(minWidth: 30, minHeight: 30),
              isDense: true,
              contentPadding: const EdgeInsets.symmetric(horizontal: 10),
              border: OutlineInputBorder(
                borderRadius: BorderRadius.circular(999),
              ),
            ),
          ),
        ),
        for (final type in fileTypes)
          chip(
            label: '${type.toUpperCase()} · ${counts[type]}',
            tooltip: 'Show ${type.toUpperCase()} files',
            selected: !filter.disabledTypes.contains(type),
            onSelected: (_) => _emit(filter.toggleType(type)),
          ),
        if (hasFolders) ...[
          chip(
            label: 'Folders · ${counts[ExternalResultFilter.folderType]}',
            tooltip: 'Show folder results',
            selected: !filter.disabledTypes
                .contains(ExternalResultFilter.folderType),
            onSelected: (_) =>
                _emit(filter.toggleType(ExternalResultFilter.folderType)),
          ),
          chip(
            label: 'Only folders',
            tooltip: 'Hide everything except folders',
            avatar: const Icon(Icons.folder_outlined, size: 14),
            selected: filter.foldersOnly,
            onSelected: (v) => _emit(filter.copyWith(foldersOnly: v)),
          ),
        ],
        if (hasBitrate)
          PopupMenuButton<int>(
            tooltip: 'Minimum bitrate (lossless always shown)',
            onSelected: (v) => _emit(filter.copyWith(minBitrate: v)),
            itemBuilder: (_) => [
              for (final v in ExternalResultFilterBar.bitrateOptions)
                PopupMenuItem(
                  value: v,
                  child: Text(v == 0 ? 'Any bitrate' : '≥ $v kbps'),
                ),
            ],
            child: Chip(
              visualDensity: VisualDensity.compact,
              materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
              avatar: Icon(Icons.speed,
                  size: 14,
                  color: filter.minBitrate > 0
                      ? theme.colorScheme.primary
                      : null),
              label: Text(
                filter.minBitrate == 0
                    ? 'Bitrate'
                    : '≥ ${filter.minBitrate} kbps',
                style: theme.textTheme.labelSmall?.copyWith(
                  color: filter.minBitrate > 0
                      ? theme.colorScheme.primary
                      : null,
                ),
              ),
            ),
          ),
        // An over-filtered list must never look like a broken search:
        // whenever anything is hidden, say how much, and offer the way
        // back out.
        if (widget.hiddenCount > 0)
          Text(
            '${widget.hiddenCount} hidden by filters',
            style: theme.textTheme.labelSmall?.copyWith(
                color: theme.colorScheme.onSurface.withOpacity(.55)),
          ),
        if (filter.isActive)
          ActionChip(
            visualDensity: VisualDensity.compact,
            materialTapTargetSize: MaterialTapTargetSize.shrinkWrap,
            avatar: const Icon(Icons.filter_alt_off_outlined, size: 14),
            label: Text('Reset', style: theme.textTheme.labelSmall),
            onPressed: () => _emit(ExternalResultFilter.empty),
          ),
      ],
    );
  }
}
