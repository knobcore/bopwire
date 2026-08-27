// Contract every foreign music network plugs into.
//
// bopwire's own swarm is the native case; Soulseek and napstr are
// "external" networks we search alongside it and import from. The search
// screen, the settings credential forms and the download→library import
// pipeline are all written against THIS interface and know nothing about
// any specific protocol, so adding a third network later means adding one
// implementation and registering it — no UI changes.
//
// Implementations live in sibling directories (soulseek/, napstr/) and
// must not reach into UI code.

import 'dart:async';

/// One searchable/downloadable item on a foreign network.
///
/// A result is either a single track ([isFolder] false) or a folder that
/// can be expanded and bulk-downloaded ([isFolder] true). Soulseek shares
/// whole directories, so folders are common there; napstr may or may not
/// expose them depending on how the publisher grouped things.
class ExternalTrack {
  const ExternalTrack({
    required this.networkId,
    required this.id,
    required this.title,
    this.artist,
    this.album,
    this.owner,
    this.remotePath,
    this.sizeBytes,
    this.bitrate,
    this.durationSeconds,
    this.extension,
    this.isFolder = false,
    this.childCount,
  });

  /// Which network this came from — matches [ExternalNetwork.id].
  final String networkId;

  /// Opaque, network-specific handle. Must round-trip back to the
  /// implementation's download()/listFolder() unchanged.
  final String id;

  final String title;
  final String? artist;
  final String? album;

  /// Peer / publisher the file is served by. Shown in the result row so
  /// the user can tell two copies apart.
  final String? owner;

  /// Path as the remote names it. Used to derive a sensible local
  /// filename and, for folders, the destination directory name.
  final String? remotePath;

  final int? sizeBytes;
  final int? bitrate;
  final int? durationSeconds;

  /// Bare extension without the dot ('flac', 'mp3').
  final String? extension;

  final bool isFolder;

  /// For folders: how many files it holds, when the protocol says so.
  final int? childCount;
}

/// Progress for a single in-flight download.
class DownloadProgress {
  const DownloadProgress({
    required this.trackId,
    required this.receivedBytes,
    this.totalBytes,
    this.localPath,
    this.done = false,
    this.error,
  });

  final String trackId;
  final int receivedBytes;
  final int? totalBytes;

  /// Set once the bytes are on disk. This is what the import pipeline
  /// fingerprints and hands to LibraryService.
  final String? localPath;

  final bool done;
  final String? error;

  double? get fraction {
    final t = totalBytes;
    if (t == null || t <= 0) return null;
    return (receivedBytes / t).clamp(0.0, 1.0);
  }
}

/// What a network needs from the user before it can connect. The settings
/// screen renders one form per network from this, so implementations do
/// not build their own UI.
class NetworkCredentialField {
  const NetworkCredentialField({
    required this.key,
    required this.label,
    this.hint,
    this.secret = false,
    this.required_ = true,
  });

  final String key;
  final String label;
  final String? hint;

  /// Render obscured and store via SecureStore rather than prefs.
  final bool secret;

  final bool required_;
}

enum NetworkStatus { disconnected, connecting, connected, error }

abstract class ExternalNetwork {
  /// Stable short id: 'slsk', 'napstr'. Persisted in settings and in
  /// ExternalTrack.networkId, so never change it.
  String get id;

  /// Human label for checkboxes and settings ('Soulseek').
  String get displayName;

  /// Credential fields the settings screen should render. Empty when the
  /// network needs no login.
  List<NetworkCredentialField> get credentialFields;

  /// True when every required credential is populated.
  bool get isConfigured;

  /// True when the user has ticked this network's search checkbox.
  bool get enabled;
  set enabled(bool v);

  NetworkStatus get status;

  /// Emits on every status change so the UI can show a badge.
  Stream<NetworkStatus> get statusChanges;

  /// Bring the connection up. Safe to call when already connected.
  Future<void> connect();

  Future<void> disconnect();

  /// Search. Implementations should stream partial results where the
  /// protocol delivers them incrementally (Soulseek does), so the UI can
  /// fill in as peers answer rather than blocking on a timeout.
  Stream<List<ExternalTrack>> search(String query);

  /// Expand a folder result into its files. Only called when
  /// [ExternalTrack.isFolder] is true.
  Future<List<ExternalTrack>> listFolder(ExternalTrack folder);

  /// Download one file into [destDir]. The returned stream must emit a
  /// final [DownloadProgress] with done=true and localPath set, or one
  /// with error set. Cancelling the subscription must abort the transfer.
  Stream<DownloadProgress> download(ExternalTrack track, String destDir);
}
