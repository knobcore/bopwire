// PlayerServer wires RatsClient.onRequest to handle peer-to-peer verbs
// other players (or the full node steering them) call against us:
//
//   stream.open(content_hash)  — legacy single-peer streaming: if the file
//                                is in our local library, allocate a stream
//                                id, reply, and stream chunks back via
//                                librats binary sends. Kept for backward
//                                compatibility with older players that
//                                don't speak audio.piece_get yet.
//
//   audio.piece_get(content_hash, offset, length)
//                               — synchronous byte range from a held file.
//                                 PieceDownloader fans this verb out across
//                                 many peers in parallel and reassembles in
//                                 order, giving resumable, multi-source
//                                 downloads on top of librats's vanilla
//                                 messaging channel. Length is capped at
//                                 kMaxPieceBytes so a malicious caller can't
//                                 ask us for the whole file in one breath.
//
//   library.list()              — return our entire local library so
//                                another wallet can browse what we have
//                                (off-chain).

import 'dart:async';
import 'dart:convert';
import 'dart:ffi';
import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import '../ffi/rats_bindings.dart' as ffi;
import 'library_service.dart';
import 'rats_client.dart';

class PlayerServer {
  PlayerServer._(this._lib, this._bindings, this._handle);

  static PlayerServer? _instance;
  static Future<PlayerServer> initialize() async {
    if (_instance != null) return _instance!;
    final rats = RatsClient.instance;
    final lib  = LibraryService.instance;
    await lib.ensureLoaded();
    final inst = PlayerServer._(lib, rats.bindingsForServer,
                                rats.handleForServer);
    // Chain our dispatcher behind any previously-installed handler so
    // multiple subsystems can register without clobbering each other.
    // We're first in the chain; the prior handler (if any) becomes the
    // fallback for verbs we don't know.
    final prior = rats.onRequest;
    rats.onRequest = (peerId, type, body, {originator = ''}) async {
      final mine = await inst._dispatch(
          peerId, type, body, originator: originator);
      if (mine != null) return mine;
      if (prior != null) {
        return prior(peerId, type, body, originator: originator);
      }
      return null;
    };
    _instance = inst;
    return inst;
  }

  static PlayerServer get instance {
    final i = _instance;
    if (i == null) throw StateError('PlayerServer.initialize() not called');
    return i;
  }

  final LibraryService    _lib;
  final ffi.RatsBindings  _bindings;
  final Pointer<Void>     _handle;
  final _rng = Random.secure();

  static const int chunkPayload = 16 * 1024;

  /// PieceDownloader requests at most 256 KB per call; we cap at 512 KB
  /// so a small protocol-version mismatch doesn't let a peer pull
  /// the whole file in one go. Replies are base64-encoded in JSON, so
  /// the on-wire size is ~33% larger than the byte length.
  static const int kMaxPieceBytes = 512 * 1024;

  Future<Map<String, dynamic>?> _dispatch(
      String peerId, String type, Map<String, dynamic> body,
      {String originator = ''}) async {
    switch (type) {
      case 'audio.piece_get': return _handlePieceGet(body);
      case 'stream.open':     return _handleStreamOpen(peerId, body, originator);
      case 'library.list':    return _handleLibraryList();
      default: return null;
    }
  }

  /// audio.piece_get — return a base64-encoded slice of a local file.
  /// Always synchronous in the JSON reply, no out-of-band binary stream.
  Future<Map<String, dynamic>?> _handlePieceGet(
      Map<String, dynamic> body) async {
    final v = body['v'];
    if (v != null && v != 1) {
      return {'v': 1, 'status': 'bad_version', 'error': 'expected v=1, got $v'};
    }
    final hash = body['content_hash'] as String? ?? '';
    if (hash.length != 64) {
      return {'v': 1, 'status': 'bad_hash',
              'error': 'content_hash must be 64-hex SHA-256'};
    }
    final offset = (body['offset'] as num?)?.toInt() ?? 0;
    final length = (body['length'] as num?)?.toInt() ?? 0;
    if (offset < 0 || length <= 0 || length > kMaxPieceBytes) {
      return {'v': 1, 'status': 'bad_range',
              'error': 'offset>=0, 0<length<=$kMaxPieceBytes'};
    }
    final entry = _lib.entryByHash(hash);
    if (entry == null || !entry.isLocal) {
      return {'v': 1, 'status': 'not_held', 'content_hash': hash};
    }
    final file = File(entry.filePath);
    int totalSize;
    Uint8List slice;
    RandomAccessFile? raf;
    try {
      // Open first, then stat through the same handle. Doing it the
      // other way (file.length() then file.open()) opens TWO handles
      // and leaves a window where another process could swap the file
      // underneath us — peers would receive bytes from one file but a
      // total_size from another. With one handle this is impossible.
      raf = await file.open();
      totalSize = await raf.length();
      if (offset >= totalSize) {
        return {'v': 1, 'status': 'bad_range',
                'error': 'offset $offset >= file size $totalSize'};
      }
      final readLen =
          (offset + length > totalSize) ? (totalSize - offset) : length;
      await raf.setPosition(offset);
      slice = await raf.read(readLen);
    } on FileSystemException catch (e) {
      return {'v': 1, 'status': 'io_error', 'error': e.message};
    } finally {
      await raf?.close();
    }
    return {
      'v':            1,
      'status':       'ok',
      'content_hash': hash,
      'offset':       offset,
      'length':       slice.length,
      'total_size':   totalSize,
      'data_b64':     base64Encode(slice),
    };
  }

  Future<Map<String, dynamic>?> _handleStreamOpen(
      String peerId, Map<String, dynamic> body, String originator) async {
    final hash = body['content_hash'] as String? ?? '';
    if (hash.isEmpty) return null;
    final entry = _lib.entryByHash(hash);
    if (entry == null || !entry.isLocal) {
      return {'matched': false, 'content_hash': hash};
    }
    final file  = File(entry.filePath);
    final bytes = await file.readAsBytes();
    final sid   = _rng.nextInt(0xFFFFFFFF);

    // If [originator] is non-empty, the request arrived via VPS relay.
    // The VPS doesn't track stream chunks by req_id — only the 'F' tag
    // + 40-byte hex target — so the chunks have to be prefixed and sent
    // to peerId (the VPS) instead of dropped at the immediate sender.
    // Direct peer-to-peer (originator empty) sends straight to peerId.
    unawaited(_streamChunks(
      sendTo:     peerId,
      relayTo:    originator,
      streamId:   sid,
      bytes:      bytes,
    ));

    return {
      'matched':      true,
      'stream_id':    sid,
      'total_bytes':  bytes.length,
      'chunk_bytes':  chunkPayload,
      'content_type': entry.audioFormat == 'ogg' ? 'audio/ogg' : 'audio/mpeg',
      'source':       originator.isEmpty ? 'peer' : 'peer-relay',
    };
  }

  Future<void> _streamChunks({
    required String   sendTo,
    required String   relayTo,
    required int      streamId,
    required Uint8List bytes,
  }) async {
    final relay      = relayTo.isNotEmpty;
    const kTagF      = 0x46; // 'F'
    final prefixLen  = relay ? (1 + 40) : 0;
    final peerPtr    = sendTo.toNativeUtf8();
    final native     = malloc<Uint8>(prefixLen + 9 + chunkPayload);
    try {
      if (relay) {
        native[0] = kTagF;
        // 40 hex characters of the originator's peer id, ASCII-encoded.
        for (int i = 0; i < 40; ++i) {
          native[1 + i] = relayTo.codeUnitAt(i);
        }
      }
      var offset = 0;
      var seq    = 0;
      while (offset < bytes.length) {
        final n = (offset + chunkPayload > bytes.length)
            ? bytes.length - offset
            : chunkPayload;
        final eof = (offset + n) >= bytes.length;
        native[prefixLen + 0] = streamId & 0xFF;
        native[prefixLen + 1] = (streamId >> 8) & 0xFF;
        native[prefixLen + 2] = (streamId >> 16) & 0xFF;
        native[prefixLen + 3] = (streamId >> 24) & 0xFF;
        native[prefixLen + 4] = seq & 0xFF;
        native[prefixLen + 5] = (seq >> 8) & 0xFF;
        native[prefixLen + 6] = (seq >> 16) & 0xFF;
        native[prefixLen + 7] = (seq >> 24) & 0xFF;
        native[prefixLen + 8] = eof ? 1 : 0;
        for (int i = 0; i < n; ++i) native[prefixLen + 9 + i] = bytes[offset + i];
        _bindings.sendBinary(_handle, peerPtr,
            native.cast<Void>(), prefixLen + 9 + n);
        offset += n;
        ++seq;
        if (seq % 8 == 0) {
          // Yield to the event loop without sleeping 1ms — `delayed(0)`
          // routes through the microtask queue, which is enough to let
          // a pending Flutter frame / progress callback fire, but adds
          // no measurable per-stream latency. The 1ms version cost
          // ~8 ms per 128 KB streamed.
          await Future<void>.delayed(Duration.zero);
        }
      }
    } finally {
      malloc.free(native);
      malloc.free(peerPtr);
    }
  }

  Future<Map<String, dynamic>?> _handleLibraryList() async {
    final out = <Map<String, dynamic>>[];
    for (final e in _lib.entries) {
      out.add({
        'content_hash':     e.contentHash,
        'fingerprint_hash': e.fingerprintHash,
        'title':            e.title,
        'artist':           e.artist,
        'album':            e.album,
        'genre':            e.genre,
        'duration_ms':      e.durationMs,
        'audio_format':     e.audioFormat,
        'is_local':         e.isLocal,
      });
    }
    return {'songs': out};
  }
}
