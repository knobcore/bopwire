// slsk_share.dart — the one folder bopwire shares on Soulseek.
//
// Why this exists
// ---------------
// Soulseek is a reciprocal network. A large share of clients (Nicotine+
// and SoulseekQt both ship this behaviour, and many users turn it up)
// automatically refuse uploads to peers who share nothing — you get an
// UploadDenied with reason "Banned" or "Nothing shared". Until now this
// client announced 0 folders / 0 files and ignored browse requests, so
// to any such peer we looked exactly like a leech and our downloads were
// refused before they started.
//
// So we share exactly one folder containing one small text file that
// explains what this client is. That is enough to satisfy the
// share-something check without republishing anyone's music: we do NOT
// expose the user's library to Soulseek, because their bopwire library
// is chain-registered content and pushing it onto another network is not
// a decision this client should make for them.
//
// Three things have to line up or peers still see an empty share:
//   1. SharedFoldersFiles (server code 35) must report non-zero counts
//   2. GetSharedFileList (peer code 4) must be answered with a real list
//   3. a TransferRequest for the file must actually be honoured
// Doing only (1) is worse than nothing: a peer that browses and finds an
// empty share treats the counts as a lie.

import 'dart:io';
import 'dart:typed_data';

import 'package:path_provider/path_provider.dart';

import 'slsk_codec.dart';
import 'slsk_messages.dart';

/// Peer payloads are zlib-compressed on this protocol.
final ZLibCodec _zlib = ZLibCodec();

/// Soulseek paths are Windows-style; the separator is a backslash even
/// when the client runs on Linux.
const String kSlskShareFolder = r'bopwire';

/// Deliberately descriptive: this filename is what a browsing peer sees
/// first, and it should explain itself without being opened.
const String kSlskShareFileName = 'music shared on bopwire client.txt';

/// Full virtual path a peer will request.
const String kSlskSharePath = '$kSlskShareFolder\\$kSlskShareFileName';

class SlskShare {
  SlskShare._();
  static final SlskShare instance = SlskShare._();

  File? _file;
  int _size = 0;

  /// Test hook: when set, the share lives under this directory instead of
  /// the app-support directory. `flutter test` has no path_provider
  /// channel, and without this the share silently degrades to 0/0 in any
  /// test run — including the live one this exists to support.
  Directory? baseDirOverride;

  /// The on-disk file backing the share, created on first use.
  File? get file => _file;
  int get sizeBytes => _size;

  /// One folder, one file — reported to the server so we don't look like
  /// a non-sharer.
  int get folderCount => 1;
  int get fileCount => 1;

  static String _contents() => '''
music shared on bopwire client
==============================

This user is sharing through bopwire, a decentralised music player and
network. Their library lives on the bopwire network rather than in a
Soulseek share, which is why you see this file instead of a folder full
of audio.

  Web:      https://bopwire.com
  Client:   bopwire player
  Protocol: bopwire (librats) + chain-registered fingerprints

Why this file exists
--------------------
Many Soulseek clients refuse uploads to users who share nothing. This
client does not republish its owner's music onto Soulseek, because that
content is registered on the bopwire chain and re-sharing it here is not
a decision the client makes on its owner's behalf. This file is here so
that check has something honest to find.

If you would like the music, the bopwire network is open — the client is
free and anything shared there is fetchable directly.
''';

  /// Create (or refresh) the shared folder and file. Idempotent.
  ///
  /// Lives under the app's support directory rather than anywhere the
  /// user browses: it is protocol furniture, not something they should
  /// find in their music folders and wonder about.
  Future<File> ensureReady() async {
    final existing = _file;
    if (existing != null && await existing.exists()) return existing;

    final base = baseDirOverride ?? await getApplicationSupportDirectory();
    final dir = Directory('${base.path}${Platform.pathSeparator}slsk-share'
        '${Platform.pathSeparator}$kSlskShareFolder');
    if (!await dir.exists()) await dir.create(recursive: true);

    final f = File('${dir.path}${Platform.pathSeparator}$kSlskShareFileName');
    await f.writeAsString(_contents(), flush: true);
    _file = f;
    _size = await f.length();
    return f;
  }

  /// The single entry, in the shape the wire format expects.
  SlskFile asSlskFile() => SlskFile(
        path: kSlskSharePath,
        size: _size,
        // No audio attributes: this is a text file, and inventing a
        // bitrate would make it look like a track and get queued as one.
      );

  /// True when [path] is the file we share. Compared case-insensitively
  /// and with both separators, because clients echo the path back in
  /// whatever form they stored it.
  bool isSharedPath(String path) {
    String norm(String s) => s.replaceAll('/', r'\').toLowerCase();
    return norm(path) == norm(kSlskSharePath) ||
        norm(path).endsWith(norm(kSlskShareFileName));
  }

  /// Bytes to send when a peer actually downloads it.
  Future<Uint8List> readBytes() async {
    final f = await ensureReady();
    return f.readAsBytes();
  }

  /// Peer code 5 payload: our whole share, zlib-compressed.
  ///
  /// Layout mirrors what [FileSearchResponse.parse] reads, minus the
  /// username/token header:
  ///   uint32 folderCount
  ///     string folderName, uint32 fileCount, <file>*
  ///   uint32 unknown(0)
  ///   uint32 privateFolderCount(0)
  Uint8List buildSharedFileListResponse() {
    final w = SlskWriter()
      ..uint32(1)                 // one folder
      ..str(kSlskShareFolder)
      ..uint32(1);                // holding one file
    SlskFile.write(w, asSlskFile());
    w
      ..uint32(0)                 // unknown / obsolete
      ..uint32(0);                // no private folders
    return Uint8List.fromList(_zlib.encode(w.take()));
  }
}
