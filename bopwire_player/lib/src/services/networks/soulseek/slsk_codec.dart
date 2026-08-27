// Soulseek wire codec — little-endian primitives and message framing.
//
// PROTOCOL PROVENANCE / LICENSING
// ------------------------------------------------------------------
// The wire format implemented here (field order, message codes, the
// zlib-compressed peer payloads, the uint64 file-size quirk) was derived by
// reading Nicotine+ (https://github.com/nicotine-plus/nicotine-plus),
// specifically `pynicotine/slskmessages.py` and `pynicotine/slskproto.py`.
// Nicotine+ is licensed GPL-3.0-or-later. No Nicotine+ code is copied
// verbatim, but this is a port of its protocol description and should be
// treated as carrying GPLv3 obligations until the project decides otherwise.
// See soulseek_network.dart for the full note.

import 'dart:convert';
import 'dart:typed_data';

/// Builds a little-endian Soulseek message body.
class SlskWriter {
  final BytesBuilder _b = BytesBuilder(copy: true);
  final Uint8List _scratch = Uint8List(8);
  late final ByteData _sv = ByteData.sublistView(_scratch);

  int get length => _b.length;

  void uint8(int v) => _b.addByte(v & 0xFF);

  void boolean(bool v) => _b.addByte(v ? 1 : 0);

  void uint32(int v) {
    _sv.setUint32(0, v & 0xFFFFFFFF, Endian.little);
    _b.add(Uint8List.sublistView(_scratch, 0, 4));
  }

  void int32(int v) {
    _sv.setInt32(0, v, Endian.little);
    _b.add(Uint8List.sublistView(_scratch, 0, 4));
  }

  void uint64(int v) {
    _sv.setUint64(0, v, Endian.little);
    _b.add(Uint8List.sublistView(_scratch, 0, 8));
  }

  /// uint32 byte-length prefix followed by the raw bytes.
  void bytes(List<int> data) {
    uint32(data.length);
    _b.add(data);
  }

  void raw(List<int> data) => _b.add(data);

  /// uint32 length prefix + UTF-8 bytes. Soulseek strings carry no encoding
  /// tag; modern clients send UTF-8 and legacy ones latin-1. We always write
  /// UTF-8 (what Nicotine+ does unless a peer is flagged legacy).
  void str(String s) => bytes(utf8.encode(s));

  Uint8List take() => _b.takeBytes();
}

/// Reads a little-endian Soulseek message body.
class SlskReader {
  SlskReader(this.data, [this.offset = 0])
      : _bd = ByteData.sublistView(data);

  final Uint8List data;
  final ByteData _bd;
  int offset;

  bool get hasMore => offset < data.length;
  int get remaining => data.length - offset;

  void _need(int n) {
    if (offset + n > data.length) {
      throw SlskParseException(
          'truncated message: need $n byte(s) at $offset of ${data.length}');
    }
  }

  int uint8() {
    _need(1);
    return data[offset++];
  }

  bool boolean() => uint8() != 0;

  int uint16() {
    _need(2);
    final v = _bd.getUint16(offset, Endian.little);
    offset += 2;
    return v;
  }

  int uint32() {
    _need(4);
    final v = _bd.getUint32(offset, Endian.little);
    offset += 4;
    return v;
  }

  int int32() {
    _need(4);
    final v = _bd.getInt32(offset, Endian.little);
    offset += 4;
    return v;
  }

  int uint64() {
    _need(8);
    final v = _bd.getUint64(offset, Endian.little);
    offset += 8;
    return v;
  }

  Uint8List bytes() {
    final len = uint32();
    _need(len);
    final out = Uint8List.sublistView(data, offset, offset + len);
    offset += len;
    return out;
  }

  String str() {
    final raw = bytes();
    try {
      return utf8.decode(raw);
    } on FormatException {
      return latin1.decode(raw, allowInvalid: true);
    }
  }

  /// Soulseek stores IPv4 addresses byte-reversed (Nicotine+ does
  /// `inet_ntoa(bytes[::-1])`).
  String ipAddress() {
    _need(4);
    final a = data[offset], b = data[offset + 1];
    final c = data[offset + 2], d = data[offset + 3];
    offset += 4;
    return '$d.$c.$b.$a';
  }

  void skip(int n) {
    _need(n);
    offset += n;
  }

  /// File sizes in peer messages. Works around the Soulseek NS bug where a
  /// >2 GiB size has garbage (0xFFFFFFFF) in the high word.
  int fileSize() {
    _need(8);
    if (data[offset + 7] == 255) {
      final size = uint32();
      offset += 4;
      return size;
    }
    return uint64();
  }
}

class SlskParseException implements Exception {
  SlskParseException(this.message);
  final String message;
  @override
  String toString() => 'SlskParseException: $message';
}

// ---------------------------------------------------------------------------
// Framing
// ---------------------------------------------------------------------------

/// Server and peer messages: uint32 (payloadLen + 4) | uint32 code | payload.
Uint8List frameWithUint32Code(int code, Uint8List payload) {
  final w = SlskWriter()
    ..uint32(payload.length + 4)
    ..uint32(code)
    ..raw(payload);
  return w.take();
}

/// Peer-init messages: uint32 (payloadLen + 1) | uint8 code | payload.
Uint8List framePeerInit(int code, Uint8List payload) {
  final w = SlskWriter()
    ..uint32(payload.length + 1)
    ..uint8(code)
    ..raw(payload);
  return w.take();
}

/// One decoded frame: the message code plus its (unframed) payload.
class SlskFrame {
  const SlskFrame(this.code, this.payload);
  final int code;
  final Uint8List payload;
}

/// Incremental de-framer. Feed socket chunks in, pull complete frames out.
///
/// [codeIsByte] selects peer-init framing (uint8 code) over the normal
/// uint32-code framing used by the server and by post-handshake peer messages.
class SlskFrameBuffer {
  SlskFrameBuffer({this.codeIsByte = false, this.maxMessageSize = 469762048});

  final bool codeIsByte;
  final int maxMessageSize;

  final BytesBuilder _buf = BytesBuilder(copy: true);
  Uint8List _pending = Uint8List(0);

  int get bufferedLength => _pending.length + _buf.length;

  void add(List<int> chunk) => _buf.add(chunk);

  void _coalesce() {
    if (_buf.isEmpty) return;
    final extra = _buf.takeBytes();
    if (_pending.isEmpty) {
      _pending = extra;
    } else {
      final merged = Uint8List(_pending.length + extra.length)
        ..setRange(0, _pending.length, _pending)
        ..setRange(_pending.length, _pending.length + extra.length, extra);
      _pending = merged;
    }
  }

  /// Removes and returns the leading [n] bytes, or null if not buffered yet.
  /// Used for the unframed uint32 token / uint64 offset on 'F' connections.
  Uint8List? takeRaw(int n) {
    _coalesce();
    if (_pending.length < n) return null;
    final out = Uint8List.sublistView(_pending, 0, n);
    _pending = Uint8List.sublistView(_pending, n);
    return out;
  }

  /// Everything currently buffered, consumed.
  Uint8List drain() {
    _coalesce();
    final out = _pending;
    _pending = Uint8List(0);
    return out;
  }

  /// Pulls the next complete frame, or null when one isn't fully buffered.
  SlskFrame? readFrame() {
    _coalesce();
    final headerLen = codeIsByte ? 5 : 8;
    if (_pending.length < headerLen) return null;

    final bd = ByteData.sublistView(_pending);
    final msgSize = bd.getUint32(0, Endian.little);
    if (msgSize > maxMessageSize || msgSize < (codeIsByte ? 1 : 4)) {
      throw SlskParseException('implausible message size $msgSize');
    }
    final total = msgSize + 4;
    if (_pending.length < total) return null;

    final code =
        codeIsByte ? _pending[4] : bd.getUint32(4, Endian.little);
    final payload = Uint8List.fromList(_pending.sublist(headerLen, total));
    _pending = Uint8List.sublistView(_pending, total);
    return SlskFrame(code, payload);
  }

  /// Pulls every complete frame currently buffered.
  List<SlskFrame> readFrames() {
    final frames = <SlskFrame>[];
    while (true) {
      final f = readFrame();
      if (f == null) return frames;
      frames.add(f);
    }
  }
}
