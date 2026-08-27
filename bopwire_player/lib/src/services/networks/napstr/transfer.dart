// napstr transfer protocol version 2: length-prefixed JSON control frames
// over a TCP stream carried by a Tor onion service, followed by raw file
// bytes.
//
// Frame layout is a four-byte big-endian payload length then that many
// bytes of UTF-8 JSON, capped at 64 KiB. After a FILE_DATA frame exactly
// `size` raw bytes follow, and the SHA-256 of those bytes must equal the
// catalogue file id or the download is rejected.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' as crypto;

import 'catalogue.dart';
import 'socks5.dart';

const int kTransferProtocolVersion = 2;
const int kMaxControlFrame = 64 * 1024;

class TransferException implements Exception {
  TransferException(this.message);
  final String message;
  @override
  String toString() => message;
}

/// Encodes one control frame: u32 big-endian length, then the JSON body.
Uint8List encodeFrame(Map<String, Object?> value) {
  final payload = Uint8List.fromList(utf8.encode(jsonEncode(value)));
  if (payload.isEmpty || payload.length > kMaxControlFrame) {
    throw TransferException('control frame exceeds protocol limit');
  }
  final out = Uint8List(4 + payload.length);
  final bd = ByteData.sublistView(out);
  bd.setUint32(0, payload.length, Endian.big);
  out.setAll(4, payload);
  return out;
}

/// Decodes a frame body. Returns null when the payload is not a JSON object
/// with a string `type`.
Map<String, Object?>? decodeFrameBody(List<int> payload) {
  try {
    final decoded = jsonDecode(utf8.decode(payload));
    if (decoded is! Map) return null;
    if (decoded['type'] is! String) return null;
    return decoded.cast<String, Object?>();
  } catch (_) {
    return null;
  }
}

/// Reads one control frame from [reader].
Future<Map<String, Object?>> readFrame(
  ByteReader reader, {
  required Duration timeout,
}) async {
  final header = await reader.readExactly(4, timeout: timeout);
  final size = ByteData.sublistView(header).getUint32(0, Endian.big);
  if (size == 0 || size > kMaxControlFrame) {
    throw TransferException('invalid control frame size $size');
  }
  final body = await reader.readExactly(size, timeout: timeout);
  final frame = decodeFrameBody(body);
  if (frame == null) throw TransferException('malformed control frame');
  return frame;
}

/// A validated DOWNLOAD_OFFER received over NIP-17.
class DownloadOffer {
  const DownloadOffer({
    required this.requestId,
    required this.fileId,
    required this.onion,
    required this.port,
    required this.capability,
    required this.expiresAt,
    required this.seederPubkey,
  });

  final String requestId;
  final String fileId;
  final String onion;
  final int port;
  final String capability;
  final int expiresAt;
  final String seederPubkey;

  bool isExpired({DateTime? now}) =>
      expiresAt <= (now ?? DateTime.now()).millisecondsSinceEpoch ~/ 1000;

  /// Validates a DOWNLOAD_OFFER body against the request it must answer.
  ///
  /// Every check here is a PROTOCOL.md MUST: the offer is rejected when it
  /// is expired, its onion is not a valid v3 hostname, its request or file
  /// id does not match an outstanding request, or its sender was not one
  /// of the seeders we explicitly asked.
  static DownloadOffer? validate(
    Object? body, {
    required String expectedRequestId,
    required String expectedFileId,
    required String senderPubkey,
    required Set<String> requestedSeeders,
    DateTime? now,
  }) {
    if (body is! Map) return null;
    if (body['protocol'] != kProtocolVersion) return null;
    if (body['type'] != 'DOWNLOAD_OFFER') return null;
    final offer = body['offer'];
    if (offer is! Map) return null;

    final requestId = offer['requestId'];
    final fileId = offer['fileId'];
    final onion = offer['onion'];
    final port = offer['port'];
    final capability = offer['capability'];
    final expiresAt = offer['expiresAt'];
    if (requestId is! String ||
        fileId is! String ||
        onion is! String ||
        port is! int ||
        capability is! String ||
        expiresAt is! int) {
      return null;
    }
    if (requestId != expectedRequestId) return null;
    if (fileId != expectedFileId || !validFileId(fileId)) return null;
    if (!requestedSeeders.contains(senderPubkey)) return null;
    if (!isV3Onion(onion)) return null;
    if (port <= 0 || port > 65535) return null;
    if (capability.length != 64) return null;
    final nowSeconds = (now ?? DateTime.now()).millisecondsSinceEpoch ~/ 1000;
    if (expiresAt <= nowSeconds) return null;

    return DownloadOffer(
      requestId: requestId,
      fileId: fileId,
      onion: onion,
      port: port,
      capability: capability,
      expiresAt: expiresAt,
      seederPubkey: senderPubkey,
    );
  }
}

/// Bytes-so-far callback for [downloadFromOffer].
typedef TransferProgress = void Function(int received, int total);

/// Abort handle for an in-flight transfer.
///
/// The ExternalNetwork contract requires that cancelling the download
/// stream subscription aborts the transfer, and a blocking socket read
/// will not notice a flag on its own — so [abort] also tears down the
/// socket, which unblocks the read immediately.
class TransferCancel {
  bool _cancelled = false;
  Socks5Connection? _connection;
  final Completer<void> _cancelledSignal = Completer<void>();

  bool get isCancelled => _cancelled;

  /// Completes when [abort]/[abortGracefully] is called. Lets long waits
  /// (the 60-second offer wait, most of all) end immediately on cancel
  /// instead of holding relay subscriptions until a timeout fires.
  Future<void> get whenCancelled => _cancelledSignal.future;

  void _attach(Socks5Connection c) {
    _connection = c;
    if (_cancelled) unawaited(c.destroy());
  }

  /// Abort the transfer, telling the seeder about it first.
  ///
  /// This used to call destroy() straight away. That looked fine locally
  /// but was the cause of "transfers get stuck": the seeder serves ONE
  /// transfer at a time, and a hard TCP teardown through a Tor circuit
  /// can take a long time to register at the far end — until it does,
  /// the slot stays occupied and every later request to that seeder
  /// times out waiting for an offer that will never come.
  ///
  /// The protocol already has the right signal (a CANCEL control frame,
  /// which the error path below sends). The bug was ordering: destroying
  /// the socket first meant the frame could never leave, and the send
  /// failed silently into an empty catch. So: send CANCEL, flush it,
  /// THEN tear down.
  void abort() => unawaited(abortGracefully());

  /// Awaitable form, so shutdown paths can let the frame actually leave
  /// before the process exits.
  Future<void> abortGracefully() async {
    if (_cancelled) return;
    _cancelled = true;
    if (!_cancelledSignal.isCompleted) _cancelledSignal.complete();
    final c = _connection;
    if (c == null) return;
    try {
      c.socket.add(encodeFrame({'type': 'CANCEL'}));
      // Bounded: a wedged circuit must not hang app shutdown. Two
      // seconds is far more than a 20-byte frame needs, and the seeder
      // frees the slot the moment it arrives.
      await c.socket.flush().timeout(const Duration(seconds: 2));
    } catch (_) {
      // Socket already gone — nothing to tell anyone.
    }
    try {
      await c.destroy();
    } catch (_) {}
  }
}

/// Runs transfer protocol v2 against the seeder named in [offer], writing
/// to [destinationPath] via a `.part` temporary file.
///
/// The SHA-256 of the received bytes is checked against the catalogue file
/// id before the temporary file is renamed into place; on any mismatch the
/// partial file is deleted and a [TransferException] is thrown.
///
/// Note: the audio-content re-validation PROTOCOL.md also requires is left
/// to bopwire's existing import pipeline, which already sniffs and
/// fingerprints every file it takes into the library.
Future<String> downloadFromOffer({
  required Socks5Endpoint proxy,
  required DownloadOffer offer,
  required int expectedSize,
  required String destinationPath,
  TransferProgress? onProgress,
  TransferCancel? cancel,
  Duration helloTimeout = const Duration(seconds: 30),
  Duration headerTimeout = const Duration(seconds: 60),
  Duration stallTimeout = const Duration(seconds: 45),
}) async {
  if (!isV3Onion(offer.onion)) {
    throw TransferException('refusing an offer without a valid v3 onion');
  }
  if (cancel?.isCancelled ?? false) {
    throw TransferException('download cancelled');
  }
  final connection =
      await socks5Connect(proxy, offer.onion, offer.port, timeout: headerTimeout);
  cancel?._attach(connection);
  final socket = connection.socket;
  final reader = connection.reader;
  final partial = File('$destinationPath.part');
  IOSink? sink;

  try {
    socket.add(encodeFrame({
      'type': 'HELLO',
      'version': kTransferProtocolVersion,
      'capability': offer.capability,
      'file_id': offer.fileId,
    }));
    await socket.flush();

    final welcome = await readFrame(reader, timeout: helloTimeout);
    _throwIfError(welcome);
    if (welcome['type'] != 'WELCOME') {
      throw TransferException('expected WELCOME, got ${welcome['type']}');
    }
    if (welcome['version'] != kTransferProtocolVersion) {
      throw TransferException('unsupported transfer version ${welcome['version']}');
    }
    if (welcome['file_id'] != offer.fileId) {
      throw TransferException('WELCOME names a different file');
    }
    final welcomeSize = welcome['size'];
    if (welcomeSize is! int || welcomeSize != expectedSize) {
      throw TransferException(
          'WELCOME size $welcomeSize disagrees with the signed catalogue size $expectedSize');
    }

    socket.add(encodeFrame({'type': 'REQUEST_FILE'}));
    await socket.flush();

    final header = await readFrame(reader, timeout: headerTimeout);
    _throwIfError(header);
    if (header['type'] != 'FILE_DATA') {
      throw TransferException('expected FILE_DATA, got ${header['type']}');
    }
    final size = header['size'];
    if (size is! int || size != expectedSize) {
      throw TransferException('FILE_DATA size disagrees with the catalogue');
    }
    if (header['sha256'] != offer.fileId) {
      throw TransferException('FILE_DATA announces a different digest');
    }

    if (await partial.exists()) await partial.delete();
    await partial.parent.create(recursive: true);
    sink = partial.openWrite();
    crypto.Digest? actualDigest;
    final digest = crypto.sha256
        .startChunkedConversion(_CollectDigest((d) => actualDigest = d));

    var received = 0;
    while (received < size) {
      if (cancel?.isCancelled ?? false) {
        throw TransferException('download cancelled');
      }
      final want = size - received;
      final chunk = await reader.readExactly(
        want < 64 * 1024 ? want : 64 * 1024,
        timeout: stallTimeout,
      );
      digest.add(chunk);
      sink.add(chunk);
      received += chunk.length;
      onProgress?.call(received, size);
    }
    digest.close();
    await sink.flush();
    await sink.close();
    sink = null;

    final actual = actualDigest;
    if (actual == null || actual.toString() != offer.fileId) {
      throw TransferException(
          'downloaded bytes do not hash to the catalogue file id');
    }

    socket.add(encodeFrame({'type': 'TRANSFER_COMPLETE'}));
    await socket.flush();

    final target = File(destinationPath);
    if (await target.exists()) await target.delete();
    await partial.rename(destinationPath);
    return destinationPath;
  } catch (_) {
    try {
      socket.add(encodeFrame({'type': 'CANCEL'}));
      // Bounded for the same reason abortGracefully() bounds it: a wedged
      // Tor circuit must not hang the error path forever.
      await socket.flush().timeout(const Duration(seconds: 2));
    } catch (_) {}
    rethrow;
  } finally {
    try {
      await sink?.close();
    } catch (_) {}
    if (await partial.exists()) {
      try {
        await partial.delete();
      } catch (_) {}
    }
    await connection.destroy();
  }
}

void _throwIfError(Map<String, Object?> frame) {
  if (frame['type'] != 'ERROR') return;
  final code = frame['code'];
  // Error text is remote-controlled: report the code, not the free text.
  throw TransferException(
      'seeder refused the transfer (${code is String && code.length <= 32 ? code : 'ERROR'})');
}

class _CollectDigest implements Sink<crypto.Digest> {
  _CollectDigest(this.onDigest);
  final void Function(crypto.Digest) onDigest;
  @override
  void add(crypto.Digest data) => onDigest(data);
  @override
  void close() {}
}
