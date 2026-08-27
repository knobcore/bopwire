// Soulseek message codes and the structs we actually exchange.
//
// Ported from Nicotine+ (`pynicotine/slskmessages.py`), GPL-3.0-or-later —
// see the licensing note in soulseek_network.dart.
//
// Only the messages this client needs are modelled. Codes for the rest are
// listed so unknown traffic can be logged rather than mistaken for garbage.

import 'dart:convert';
import 'dart:io' show ZLibCodec;
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

import 'slsk_codec.dart';

/// Server message codes (uint32 after the length prefix).
class ServerCode {
  static const login = 1;
  static const setWaitPort = 2;
  static const getPeerAddress = 3;
  static const connectToPeer = 18;
  static const fileSearch = 26;
  static const setStatus = 28;
  static const serverPing = 32;
  static const sharedFoldersFiles = 35;
  static const getUserStats = 36;
  static const relogged = 41;
  static const privilegedUsers = 69;
  static const haveNoParent = 71;
  static const parentMinSpeed = 83;
  static const parentSpeedRatio = 84;
  static const checkPrivileges = 92;
  static const embeddedMessage = 93;
  static const acceptChildren = 100;
  static const possibleParents = 102;
  static const wishlistInterval = 104;
  static const excludedSearchPhrases = 160;
  static const branchLevel = 126;
  static const branchRoot = 127;
  static const resetDistributed = 130;
  static const cantConnectToPeer = 1001;
}

/// Peer-init message codes (uint8 after the length prefix).
class PeerInitCode {
  static const pierceFireWall = 0;
  static const peerInit = 1;
}

/// Peer message codes (uint32 after the length prefix).
class PeerCode {
  static const sharedFileListRequest = 4;
  static const sharedFileListResponse = 5;
  static const fileSearchResponse = 9;
  static const userInfoRequest = 15;
  static const userInfoResponse = 16;
  static const folderContentsRequest = 36;
  static const folderContentsResponse = 37;
  static const transferRequest = 40;
  static const transferResponse = 41;
  static const queueUpload = 43;
  static const placeInQueueResponse = 44;
  static const uploadFailed = 46;
  static const uploadDenied = 50;
  static const placeInQueueRequest = 51;
}

class ConnType {
  static const peer = 'P';
  static const file = 'F';
  static const distributed = 'D';
}

class TransferDirection {
  static const download = 0;
  static const upload = 1;
}

/// Attribute keys inside a shared-file entry.
class FileAttributeKey {
  static const bitrate = 0;
  static const duration = 1;
  static const vbr = 2;
  static const sampleRate = 4;
  static const bitDepth = 5;
}

final ZLibCodec _zlib = ZLibCodec();

// ---------------------------------------------------------------------------
// Outgoing message builders (payload only — the caller frames them)
// ---------------------------------------------------------------------------

class SlskOut {
  /// Server code 1. The credential hash is `md5(username + password)` as a
  /// lowercase hex string, exactly as Nicotine+ computes it.
  static Uint8List login(
    String username,
    String password, {
    int majorVersion = kClientMajorVersion,
    int minorVersion = kClientMinorVersion,
  }) {
    final digest =
        crypto.md5.convert(utf8.encode('$username$password')).toString();
    return (SlskWriter()
          ..str(username)
          ..str(password)
          ..uint32(majorVersion)
          ..str(digest)
          ..uint32(minorVersion))
        .take();
  }

  /// Server code 2.
  static Uint8List setWaitPort(int port) => (SlskWriter()..uint32(port)).take();

  /// Server code 3.
  static Uint8List getPeerAddress(String username) =>
      (SlskWriter()..str(username)).take();

  /// Server code 18 — ask the server to have [username] connect back to us.
  static Uint8List connectToPeer(int token, String username, String connType) =>
      (SlskWriter()
            ..uint32(token)
            ..str(username)
            ..str(connType))
          .take();

  /// Server code 26.
  static Uint8List fileSearch(int token, String query) => (SlskWriter()
        ..uint32(token)
        ..str(query))
      .take();

  /// Server code 28. 1 = away, 2 = online.
  static Uint8List setStatus(int status) => (SlskWriter()..int32(status)).take();

  /// Server code 35.
  static Uint8List sharedFoldersFiles(int folders, int files) => (SlskWriter()
        ..uint32(folders)
        ..uint32(files))
      .take();

  /// Server code 71 / 100.
  static Uint8List boolMessage(bool v) => (SlskWriter()..boolean(v)).take();

  /// Server code 126.
  static Uint8List uint32Message(int v) => (SlskWriter()..uint32(v)).take();

  /// Server code 127.
  static Uint8List stringMessage(String v) => (SlskWriter()..str(v)).take();

  /// Server code 1001.
  static Uint8List cantConnectToPeer(int token, String username) =>
      (SlskWriter()
            ..uint32(token)
            ..str(username))
          .take();

  /// Peer-init code 1 — we dialled the peer directly.
  static Uint8List peerInit(String ourUsername, String connType) =>
      (SlskWriter()
            ..str(ourUsername)
            ..str(connType)
            ..uint32(0)) // token, always zero today
          .take();

  /// Peer-init code 0 — we dialled the peer in answer to a ConnectToPeer.
  static Uint8List pierceFireWall(int token) =>
      (SlskWriter()..uint32(token)).take();

  /// Peer code 36.
  static Uint8List folderContentsRequest(int token, String folder) =>
      (SlskWriter()
            ..uint32(token)
            ..str(folder))
          .take();

  /// Peer code 41 — answer to a peer's TransferRequest.
  static Uint8List transferResponse(int token, bool allowed,
      {String? reason, int? fileSize}) {
    final w = SlskWriter()
      ..uint32(token)
      ..boolean(allowed);
    if (reason != null) w.str(reason);
    if (fileSize != null) w.uint64(fileSize);
    return w.take();
  }

  /// Peer code 43 — "please queue this upload for me".
  static Uint8List queueUpload(String virtualPath) =>
      (SlskWriter()..str(virtualPath)).take();

  /// Peer code 50 — refuse (or explain) a peer's QueueUpload.
  static Uint8List uploadDenied(String virtualPath, String reason) =>
      (SlskWriter()
            ..str(virtualPath)
            ..str(reason))
          .take();

  /// Peer code 51.
  static Uint8List placeInQueueRequest(String virtualPath) =>
      (SlskWriter()..str(virtualPath)).take();

  /// Unframed 4-byte token sent first on an 'F' connection.
  static Uint8List fileTransferInit(int token) =>
      (SlskWriter()..uint32(token)).take();

  /// Unframed 8-byte resume offset sent by the downloader on an 'F'
  /// connection. Zero for a fresh download.
  static Uint8List fileOffset(int offset) =>
      (SlskWriter()..uint64(offset)).take();
}

/// Soulseek client version we announce.
///
/// The server uses this to tell clients apart. 160/3 is what Nicotine+ sends;
/// Nicotine+'s own protocol notes reserve major version 177 for experimental
/// third-party clients, which is arguably the more correct value for us but is
/// unverified against the live server. We send 160/3 because it is known to be
/// accepted. If a login is ever rejected with `INVALIDVERSION`, try 177/1.
const int kClientMajorVersion = 160;
const int kClientMinorVersion = 3;

// ---------------------------------------------------------------------------
// Incoming message structs
// ---------------------------------------------------------------------------

class LoginResponse {
  const LoginResponse({
    required this.success,
    this.greeting,
    this.ipAddress,
    this.isSupporter,
    this.failureReason,
    this.failureDetail,
  });

  final bool success;
  final String? greeting;
  final String? ipAddress;
  final bool? isSupporter;
  final String? failureReason;
  final String? failureDetail;

  static LoginResponse parse(Uint8List payload) {
    final r = SlskReader(payload);
    final ok = r.boolean();
    if (!ok) {
      final reason = r.hasMore ? r.str() : null;
      final detail = r.hasMore ? r.str() : null;
      return LoginResponse(
          success: false, failureReason: reason, failureDetail: detail);
    }
    final greeting = r.str();
    final ip = r.ipAddress();
    if (r.hasMore) r.str(); // md5 checksum of the password we sent
    final supporter = r.hasMore ? r.boolean() : null;
    return LoginResponse(
      success: true,
      greeting: greeting,
      ipAddress: ip,
      isSupporter: supporter,
    );
  }
}

/// True when [ip]:[port] is an address we could actually reach over the
/// internet.
///
/// Why this is stricter than it looks like it needs to be
/// -----------------------------------------------------
/// A Soulseek client that cannot determine its own public address
/// advertises whatever it found on its interface — routinely a
/// 192.168.x.x or 10.x.x.x LAN address. Dialling one of those from our
/// machine does not reach that peer; it either fails instantly with
/// "No route to host" or, worse, connects to a completely unrelated
/// device on OUR LAN that happens to hold the same address.
///
/// The previous check was `port > 0 && ip != '0.0.0.0'`, which let every
/// one of those through. A live run dialled 18 such peers and failed 18
/// times, which read as a NAT problem but was really us dialling
/// addresses that were never reachable from here.
///
/// Peers filtered out here are not lost: they are unreachable only in the
/// outbound direction, and the protocol's other path (asking the server
/// to have them dial us) is exactly what CantConnectToPeer triggers.
bool isRoutableAddress(String ip, int port, {bool allowLoopback = false}) {
  if (port <= 0 || port > 65535) return false;

  final parts = ip.split('.');
  if (parts.length != 4) return false;
  final o = <int>[];
  for (final p in parts) {
    final v = int.tryParse(p);
    if (v == null || v < 0 || v > 255) return false;
    o.add(v);
  }

  // 0.0.0.0/8      "this network" — also the classic "I don't know my IP"
  if (o[0] == 0) return false;
  // 10.0.0.0/8     RFC1918 private
  if (o[0] == 10) return false;
  // 127.0.0.0/8    loopback: would connect to ourselves. Tests drive
  //                fake peers over loopback and opt in explicitly.
  if (o[0] == 127) return allowLoopback;
  // 100.64.0.0/10  RFC6598 carrier-grade NAT
  if (o[0] == 100 && o[1] >= 64 && o[1] <= 127) return false;
  // 169.254.0.0/16 link-local (DHCP failed on that peer)
  if (o[0] == 169 && o[1] == 254) return false;
  // 172.16.0.0/12  RFC1918 private
  if (o[0] == 172 && o[1] >= 16 && o[1] <= 31) return false;
  // 192.168.0.0/16 RFC1918 private
  if (o[0] == 192 && o[1] == 168) return false;
  // 224.0.0.0/4    multicast, and 240.0.0.0/4 reserved — neither is a host
  if (o[0] >= 224) return false;

  return true;
}

class PeerAddress {
  const PeerAddress(this.username, this.ipAddress, this.port);
  final String username;
  final String ipAddress;
  final int port;

  bool get isRoutable => isRoutableAddress(ipAddress, port);

  static PeerAddress parse(Uint8List payload) {
    final r = SlskReader(payload);
    final user = r.str();
    final ip = r.ipAddress();
    final port = r.uint32();
    return PeerAddress(user, ip, port);
  }
}

/// Server code 18 as received: a peer wants us to connect back to them.
class ConnectToPeerRequest {
  const ConnectToPeerRequest({
    required this.username,
    required this.connType,
    required this.ipAddress,
    required this.port,
    required this.token,
    this.privileged = false,
  });

  final String username;
  final String connType;
  final String ipAddress;
  final int port;
  final int token;
  final bool privileged;

  static ConnectToPeerRequest parse(Uint8List payload) {
    final r = SlskReader(payload);
    final user = r.str();
    final type = r.str();
    final ip = r.ipAddress();
    final port = r.uint32();
    final token = r.uint32();
    final privileged = r.hasMore ? r.boolean() : false;
    return ConnectToPeerRequest(
      username: user,
      connType: type,
      ipAddress: ip,
      port: port,
      token: token,
      privileged: privileged,
    );
  }
}

/// Peer-init code 1 as received on an inbound connection.
class PeerInitRequest {
  const PeerInitRequest(this.username, this.connType);
  final String username;
  final String connType;

  static PeerInitRequest parse(Uint8List payload) {
    final r = SlskReader(payload);
    final user = r.str();
    final type = r.str();
    return PeerInitRequest(user, type);
  }
}

/// One shared file inside a search result or folder listing.
class SlskFile {
  const SlskFile({
    required this.path,
    required this.size,
    this.bitrate,
    this.durationSeconds,
    this.sampleRate,
    this.bitDepth,
    this.vbr,
  });

  final String path;
  final int size;
  final int? bitrate;
  final int? durationSeconds;
  final int? sampleRate;
  final int? bitDepth;
  final int? vbr;

  /// Everything before the final backslash. Soulseek paths are Windows-style.
  String get folder {
    final i = path.lastIndexOf('\\');
    return i < 0 ? '' : path.substring(0, i);
  }

  String get fileName {
    final i = path.lastIndexOf('\\');
    return i < 0 ? path : path.substring(i + 1);
  }

  String? get extension {
    final name = fileName;
    final i = name.lastIndexOf('.');
    if (i <= 0 || i == name.length - 1) return null;
    return name.substring(i + 1).toLowerCase();
  }

  /// Effective bitrate: the reported one, or derived from sample rate and
  /// bit depth for lossless files (Nicotine+ does the same).
  int? get effectiveBitrate {
    if (bitrate != null && bitrate! > 0) return bitrate;
    final sr = sampleRate, bd = bitDepth;
    if (sr != null && bd != null && sr > 0 && bd > 0) {
      return (sr * bd * 2) ~/ 1000;
    }
    return null;
  }

  static SlskFile read(SlskReader r) {
    r.uint8(); // entry code, always 1
    final name = r.str().replaceAll('/', '\\');
    final size = r.fileSize();
    final extLen = r.uint32(); // obsolete extension field
    r.skip(extLen);

    int? bitrate, duration, sampleRate, bitDepth, vbr;
    final attrCount = r.uint32();
    for (var i = 0; i < attrCount; i++) {
      final key = r.uint32();
      final value = r.uint32();
      switch (key) {
        case FileAttributeKey.bitrate:
          bitrate = value;
        case FileAttributeKey.duration:
          duration = value;
        case FileAttributeKey.vbr:
          vbr = value;
        case FileAttributeKey.sampleRate:
          sampleRate = value;
        case FileAttributeKey.bitDepth:
          bitDepth = value;
        default:
          break; // unknown attribute, ignore
      }
    }

    return SlskFile(
      path: name,
      size: size,
      bitrate: bitrate,
      durationSeconds: duration,
      sampleRate: sampleRate,
      bitDepth: bitDepth,
      vbr: vbr,
    );
  }

  /// Inverse of [read] — only needed by the tests and by any future
  /// share-serving code, but keeping it here keeps the layout in one place.
  static void write(SlskWriter w, SlskFile f) {
    w
      ..uint8(1)
      ..str(f.path)
      ..uint64(f.size)
      ..uint32(0); // empty obsolete extension

    final attrs = <int, int>{};
    if (f.bitDepth != null) {
      if (f.durationSeconds != null) {
        attrs[FileAttributeKey.duration] = f.durationSeconds!;
      }
      if (f.sampleRate != null) {
        attrs[FileAttributeKey.sampleRate] = f.sampleRate!;
      }
      attrs[FileAttributeKey.bitDepth] = f.bitDepth!;
    } else {
      if (f.bitrate != null) attrs[FileAttributeKey.bitrate] = f.bitrate!;
      if (f.durationSeconds != null) {
        attrs[FileAttributeKey.duration] = f.durationSeconds!;
      }
      if (f.bitrate != null) attrs[FileAttributeKey.vbr] = f.vbr ?? 0;
    }

    w.uint32(attrs.length);
    attrs.forEach((k, v) => w
      ..uint32(k)
      ..uint32(v));
  }
}

/// Peer code 9. Arrives zlib-compressed.
class FileSearchResponse {
  const FileSearchResponse({
    required this.username,
    required this.token,
    required this.files,
    this.freeUploadSlots = false,
    this.uploadSpeed = 0,
    this.queueLength = 0,
    this.privateFiles = const [],
  });

  final String username;
  final int token;
  final List<SlskFile> files;
  final bool freeUploadSlots;
  final int uploadSpeed;
  final int queueLength;
  final List<SlskFile> privateFiles;

  static FileSearchResponse parse(Uint8List compressed) {
    final raw = Uint8List.fromList(_zlib.decode(compressed));
    final r = SlskReader(raw);
    final username = r.str();
    final token = r.uint32();
    final files = _readFileList(r);
    final free = r.hasMore ? r.boolean() : false;
    final speed = r.hasMore ? r.uint32() : 0;
    final queue = r.hasMore ? r.uint32() : 0;
    if (r.hasMore) r.uint32(); // unknown/obsolete field
    final private = r.hasMore ? _readFileList(r) : const <SlskFile>[];
    return FileSearchResponse(
      username: username,
      token: token,
      files: files,
      freeUploadSlots: free,
      uploadSpeed: speed,
      queueLength: queue,
      privateFiles: private,
    );
  }

  /// Used by the round-trip tests; also the exact layout a peer expects.
  static Uint8List build(FileSearchResponse m) {
    final w = SlskWriter()
      ..str(m.username)
      ..uint32(m.token)
      ..uint32(m.files.length);
    for (final f in m.files) {
      SlskFile.write(w, f);
    }
    w
      ..boolean(m.freeUploadSlots)
      ..uint32(m.uploadSpeed)
      ..uint32(m.queueLength)
      ..uint32(0);
    if (m.privateFiles.isNotEmpty) {
      w.uint32(m.privateFiles.length);
      for (final f in m.privateFiles) {
        SlskFile.write(w, f);
      }
    }
    return Uint8List.fromList(_zlib.encode(w.take()));
  }
}

List<SlskFile> _readFileList(SlskReader r) {
  final n = r.uint32();
  final out = <SlskFile>[];
  for (var i = 0; i < n; i++) {
    out.add(SlskFile.read(r));
  }
  return out;
}

/// Peer code 37. Arrives zlib-compressed. Maps folder path -> files.
class FolderContentsResponse {
  const FolderContentsResponse({
    required this.token,
    required this.folder,
    required this.folders,
  });

  final int token;
  final String folder;
  final Map<String, List<SlskFile>> folders;

  static FolderContentsResponse parse(Uint8List compressed) {
    final raw = Uint8List.fromList(_zlib.decode(compressed));
    final r = SlskReader(raw);
    final token = r.uint32();
    final folder = r.str();
    final count = r.uint32();
    final map = <String, List<SlskFile>>{};
    for (var i = 0; i < count; i++) {
      final dir = r.str().replaceAll('/', '\\');
      map[dir] = _readFileList(r);
    }
    return FolderContentsResponse(token: token, folder: folder, folders: map);
  }

  static Uint8List build(FolderContentsResponse m) {
    final w = SlskWriter()
      ..uint32(m.token)
      ..str(m.folder)
      ..uint32(m.folders.length);
    m.folders.forEach((dir, files) {
      w
        ..str(dir)
        ..uint32(files.length);
      for (final f in files) {
        SlskFile.write(w, f);
      }
    });
    return Uint8List.fromList(_zlib.encode(w.take()));
  }
}

/// Peer code 40.
class TransferRequestMessage {
  const TransferRequestMessage({
    required this.direction,
    required this.token,
    required this.virtualPath,
    this.fileSize,
  });

  final int direction;
  final int token;
  final String virtualPath;
  final int? fileSize;

  static TransferRequestMessage parse(Uint8List payload) {
    final r = SlskReader(payload);
    final direction = r.uint32();
    final token = r.uint32();
    final path = r.str();
    final size =
        (direction == TransferDirection.upload && r.remaining >= 8) ? r.fileSize() : null;
    return TransferRequestMessage(
      direction: direction,
      token: token,
      virtualPath: path,
      fileSize: size,
    );
  }

  static Uint8List build(TransferRequestMessage m) {
    final w = SlskWriter()
      ..uint32(m.direction)
      ..uint32(m.token)
      ..str(m.virtualPath);
    if (m.direction == TransferDirection.upload) {
      w.uint64(m.fileSize ?? 0);
    }
    return w.take();
  }
}

/// Peer code 41.
class TransferResponseMessage {
  const TransferResponseMessage({
    required this.token,
    required this.allowed,
    this.reason,
    this.fileSize,
  });

  final int token;
  final bool allowed;
  final String? reason;
  final int? fileSize;

  static TransferResponseMessage parse(Uint8List payload) {
    final r = SlskReader(payload);
    final token = r.uint32();
    final allowed = r.boolean();
    if (!r.hasMore) {
      return TransferResponseMessage(token: token, allowed: allowed);
    }
    if (allowed) {
      return TransferResponseMessage(
          token: token, allowed: true, fileSize: r.uint64());
    }
    return TransferResponseMessage(
        token: token, allowed: false, reason: r.str());
  }
}

/// Peer code 44.
class PlaceInQueueResponse {
  const PlaceInQueueResponse(this.virtualPath, this.place);
  final String virtualPath;
  final int place;

  static PlaceInQueueResponse parse(Uint8List payload) {
    final r = SlskReader(payload);
    return PlaceInQueueResponse(r.str(), r.uint32());
  }

  static Uint8List build(PlaceInQueueResponse m) => (SlskWriter()
        ..str(m.virtualPath)
        ..uint32(m.place))
      .take();
}

/// Peer code 50.
class UploadDenied {
  const UploadDenied(this.virtualPath, this.reason);
  final String virtualPath;
  final String reason;

  static UploadDenied parse(Uint8List payload) {
    final r = SlskReader(payload);
    return UploadDenied(r.str(), r.str());
  }
}

/// Peer code 46.
class UploadFailed {
  const UploadFailed(this.virtualPath);
  final String virtualPath;

  static UploadFailed parse(Uint8List payload) =>
      UploadFailed(SlskReader(payload).str());
}

/// Rejection reasons a peer may send in a TransferResponse.
class TransferRejectReason {
  static const queued = 'Queued';
  static const complete = 'Complete';
  static const cancelled = 'Cancelled';
  static const banned = 'Banned';
  static const fileNotShared = 'File not shared.';
}
