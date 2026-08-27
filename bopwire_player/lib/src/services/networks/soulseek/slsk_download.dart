// One in-flight Soulseek download: the queue wait on a 'P' connection, then
// the bytes arriving on an 'F' connection and landing on disk.
//
// Split out of soulseek_network.dart so the transfer state machine — offset
// handshake, truncation at the advertised size, completion, abort and cleanup
// of partial files — can be driven directly from tests.
//
// Ported from Nicotine+ (GPL-3.0-or-later) — see soulseek_network.dart.

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import '../external_network.dart';
import 'slsk_codec.dart';
import 'slsk_connections.dart';
import 'slsk_messages.dart';

/// One in-flight download: queue wait, then bytes onto disk.
class SlskDownload {
  SlskDownload({
    required this.trackId,
    required this.username,
    required this.virtualPath,
    required this.expectedSize,
    required this.destDir,
    required StreamController<DownloadProgress> controller,
    required void Function(String) log,
    required void Function(SlskDownload) onFinished,
  })  : _ctl = controller,
        _log = log,
        _onFinished = onFinished;

  final String trackId;
  final String username;
  final String virtualPath;
  final String destDir;
  final StreamController<DownloadProgress> _ctl;
  final void Function(String) _log;
  final void Function(SlskDownload) _onFinished;

  int? expectedSize;
  int? token;
  int? queuePosition;

  int receivedBytes = 0;
  bool isFinished = false;

  Socket? _socket;
  StreamSubscription<Uint8List>? _sub;
  IOSink? _sink;
  File? _partFile;
  DateTime _lastEmit = DateTime.fromMillisecondsSinceEpoch(0);
  int _sinceFlush = 0;

  static const int _flushEvery = 4 * 1024 * 1024;

  void attach(Socket socket, StreamSubscription<Uint8List>? sub) {
    _socket = socket;
    _sub = sub;
    final name = localFileName();
    _partFile = File('$destDir${Platform.pathSeparator}$name.slskpart');
    try {
      Directory(destDir).createSync(recursive: true);
      _sink = _partFile!.openWrite();
    } catch (e) {
      fail('Cannot write to $destDir: $e');
    }
  }

  /// Sanitised leaf name of the remote (Windows-style) path.
  String localFileName() => sanitizeFileName(virtualPath);

  static String sanitizeFileName(String virtualPath) {
    final i = virtualPath.lastIndexOf('\\');
    var name = i < 0 ? virtualPath : virtualPath.substring(i + 1);
    // Strip path separators, Windows-reserved punctuation and control chars.
    name = name.replaceAll(RegExp(r'[\\/:*?"<>|]'), '_');
    name = name.replaceAll(RegExp(r'[\x00-\x1f\x7f]'), '').trim();
    if (name.isEmpty) name = 'soulseek-download';
    return name;
  }

  void onBytes(Uint8List chunk) {
    if (isFinished) return;
    final sink = _sink;
    if (sink == null) return;

    var data = chunk;
    final total = expectedSize;
    if (total != null && receivedBytes + data.length > total) {
      data = Uint8List.sublistView(data, 0, total - receivedBytes);
    }
    if (data.isEmpty) return;

    sink.add(data);
    receivedBytes += data.length;
    _sinceFlush += data.length;

    if (_sinceFlush >= _flushEvery) {
      _sinceFlush = 0;
      final sub = _sub;
      sub?.pause();
      unawaited(sink.flush().then((_) => sub?.resume()).catchError((Object e) {
        fail('Write error: $e');
      }));
    }

    final now = DateTime.now();
    if (now.difference(_lastEmit).inMilliseconds >= 250) {
      _lastEmit = now;
      _emit(DownloadProgress(
        trackId: trackId,
        receivedBytes: receivedBytes,
        totalBytes: total,
      ));
    }

    if (total != null && receivedBytes >= total) {
      unawaited(_complete());
    }
  }

  void onSocketClosed(String? error) {
    if (isFinished) return;
    final total = expectedSize;
    if (error == null && total != null && receivedBytes >= total) {
      unawaited(_complete());
      return;
    }
    if (error == null && total == null && receivedBytes > 0) {
      // No size was ever advertised; treat a clean close as the end.
      unawaited(_complete());
      return;
    }
    fail(error ??
        'Transfer ended early ($receivedBytes of ${total ?? "?"} bytes)');
  }

  Future<void> _complete() async {
    if (isFinished) return;
    isFinished = true;
    try {
      await _sink?.flush();
      await _sink?.close();
      _sink = null;

      final part = _partFile!;
      final finalPath = '$destDir${Platform.pathSeparator}${localFileName()}';
      final target = await _uniquePath(finalPath);
      await part.rename(target);

      _emit(DownloadProgress(
        trackId: trackId,
        receivedBytes: receivedBytes,
        totalBytes: expectedSize,
        localPath: target,
        done: true,
      ));
      _log('downloaded $virtualPath -> $target');
    } catch (e) {
      _emit(DownloadProgress(
          trackId: trackId,
          receivedBytes: receivedBytes,
          totalBytes: expectedSize,
          error: 'Could not finalise the download: $e'));
    } finally {
      await _cleanupSocket();
      _onFinished(this);
      await _close();
    }
  }

  static Future<String> _uniquePath(String path) async {
    if (!await File(path).exists()) return path;
    final dot = path.lastIndexOf('.');
    final stem = dot > 0 ? path.substring(0, dot) : path;
    final ext = dot > 0 ? path.substring(dot) : '';
    for (var i = 2; i < 1000; i++) {
      final candidate = '$stem ($i)$ext';
      if (!await File(candidate).exists()) return candidate;
    }
    return '$stem (${DateTime.now().millisecondsSinceEpoch})$ext';
  }

  /// Set by [fail]/[abort] so shutdown paths can wait for the socket and
  /// the partial file to actually be released, not merely scheduled.
  Future<void>? _cleanup;

  void fail(String message) {
    if (isFinished) return;
    isFinished = true;
    _emit(DownloadProgress(
      trackId: trackId,
      receivedBytes: receivedBytes,
      totalBytes: expectedSize,
      error: message,
    ));
    _cleanup = () async {
      await _cleanupSocket();
      await _discardPartial();
      _onFinished(this);
      await _close();
    }();
  }

  /// Caller cancelled the subscription.
  ///
  /// Closing the 'F' connection IS the protocol's cancel signal for an
  /// active transfer (Nicotine+ does the same). For a transfer that is
  /// still queued remotely there is no un-queue message; the network
  /// layer answers the uploader's eventual TransferRequest with
  /// "Cancelled" instead, which is what frees their slot.
  Future<void> abort() async {
    if (isFinished) {
      await (_cleanup ?? Future<void>.value());
      return;
    }
    isFinished = true;
    final done = () async {
      await _cleanupSocket();
      await _discardPartial();
      _onFinished(this);
    }();
    _cleanup = done;
    await done;
  }

  /// Fail AND wait until every resource is released — for app shutdown,
  /// where scheduling cleanup after the event loop dies is the same as
  /// not cleaning up.
  Future<void> shutdown(String reason) async {
    fail(reason);
    await (_cleanup ?? Future<void>.value());
  }

  Future<void> _cleanupSocket() async {
    await _sub?.cancel();
    _sub = null;
    try {
      _socket?.destroy();
    } catch (_) {}
    _socket = null;
  }

  Future<void> _discardPartial() async {
    try {
      await _sink?.close();
    } catch (_) {}
    _sink = null;
    try {
      final p = _partFile;
      if (p != null && await p.exists()) await p.delete();
    } catch (_) {}
  }

  void _emit(DownloadProgress p) {
    if (!_ctl.isClosed) _ctl.add(p);
  }

  Future<void> _close() async {
    if (!_ctl.isClosed) await _ctl.close();
  }
}

/// Wires up an 'F' connection.
///
/// The uploader opens with an unframed uint32 transfer token. We look it up
/// with [lookup], answer with an unframed uint64 resume offset, and from then
/// on every byte is file content.
///
/// [data] is the byte stream for the connection: the socket itself when we
/// dialled out, or the listener's relay when the peer dialled us (a Socket can
/// only be listened to once).
void attachFileConnection({
  required Socket socket,
  required Stream<Uint8List> data,
  required SlskDownload? Function(int token) lookup,
  SlskLog? log,
}) {
  final acc = SlskFrameBuffer();
  final logger = log ?? (String _) {};

  SlskDownload? bound;
  StreamSubscription<Uint8List>? sub;
  var dropped = false;

  void pump(Uint8List chunk) {
    if (dropped) return;
    final current = bound;
    if (current != null) {
      current.onBytes(chunk);
      return;
    }
    acc.add(chunk);
    final tokenBytes = acc.takeRaw(4);
    if (tokenBytes == null) return;

    final token = ByteData.sublistView(tokenBytes).getUint32(0, Endian.little);
    final d = lookup(token);
    if (d == null || d.isFinished) {
      logger('file connection with unknown token $token — dropping');
      dropped = true;
      unawaited(sub?.cancel());
      socket.destroy();
      return;
    }

    bound = d;
    d.attach(socket, sub);
    socket.add(SlskOut.fileOffset(d.receivedBytes));
    final rest = acc.drain();
    if (rest.isNotEmpty) d.onBytes(rest);
  }

  sub = data.listen(
    pump,
    onError: (Object e, StackTrace _) {
      bound?.onSocketClosed('transfer connection error: $e');
      socket.destroy();
    },
    onDone: () => bound?.onSocketClosed(null),
    cancelOnError: true,
  );
}
