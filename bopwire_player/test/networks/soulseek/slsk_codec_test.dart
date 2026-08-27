// Wire-format round-trips for the Soulseek codec.
//
// These are the only checks that can be made without a live account: they
// prove our packing and unpacking are mutually consistent and that the byte
// layout matches what was read out of Nicotine+, but they do not prove the
// server accepts any of it.

import 'dart:convert';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/services/networks/soulseek/slsk_codec.dart';

void main() {
  group('primitives', () {
    test('uint8 / bool / uint16 / uint32 / int32 / uint64 round-trip', () {
      final w = SlskWriter()
        ..uint8(0)
        ..uint8(255)
        ..boolean(true)
        ..boolean(false)
        ..uint32(0)
        ..uint32(0xFFFFFFFF)
        ..uint32(1234567)
        ..int32(-42)
        ..uint64(0)
        ..uint64(9007199254740993); // > 2^53, exercises real 64-bit ints

      final r = SlskReader(w.take());
      expect(r.uint8(), 0);
      expect(r.uint8(), 255);
      expect(r.boolean(), isTrue);
      expect(r.boolean(), isFalse);
      expect(r.uint32(), 0);
      expect(r.uint32(), 0xFFFFFFFF);
      expect(r.uint32(), 1234567);
      expect(r.int32(), -42);
      expect(r.uint64(), 0);
      expect(r.uint64(), 9007199254740993);
      expect(r.hasMore, isFalse);
    });

    test('uint32 is little-endian on the wire', () {
      final bytes = (SlskWriter()..uint32(0x01020304)).take();
      expect(bytes, orderedEquals([0x04, 0x03, 0x02, 0x01]));
    });

    test('strings are a uint32 length prefix plus UTF-8 bytes', () {
      final bytes = (SlskWriter()..str('héllo')).take();
      final utf8Bytes = utf8.encode('héllo');
      expect(bytes.length, 4 + utf8Bytes.length);
      expect(bytes.sublist(0, 4), orderedEquals([utf8Bytes.length, 0, 0, 0]));
      expect(SlskReader(bytes).str(), 'héllo');
    });

    test('non-UTF-8 strings fall back to latin-1 instead of throwing', () {
      // 0xE9 alone is invalid UTF-8 but valid latin-1 ('é'), which is what
      // legacy Soulseek NS clients still emit.
      final raw = Uint8List.fromList([1, 0, 0, 0, 0xE9]);
      expect(SlskReader(raw).str(), 'é');
    });

    test('empty string round-trips', () {
      expect(SlskReader((SlskWriter()..str('')).take()).str(), '');
    });

    test('IPv4 addresses are stored byte-reversed', () {
      // 192.168.1.10 arrives as 0a 01 a8 c0
      final raw = Uint8List.fromList([10, 1, 168, 192]);
      expect(SlskReader(raw).ipAddress(), '192.168.1.10');
    });

    test('reading past the end throws rather than returning garbage', () {
      final r = SlskReader(Uint8List.fromList([1, 2, 3]));
      expect(r.uint32, throwsA(isA<SlskParseException>()));
    });

    test('fileSize reads a normal uint64', () {
      final bytes = (SlskWriter()..uint64(5000000000)).take();
      expect(SlskReader(bytes).fileSize(), 5000000000);
    });

    test('fileSize works around the Soulseek NS >2 GiB garbage high word', () {
      // Low word = real size, high word = 0xFFFFFFFF (the NS bug). Nicotine+
      // detects this by checking the last byte for 255.
      final w = SlskWriter()
        ..uint32(3000000000)
        ..uint32(0xFFFFFFFF)
        ..uint32(7); // a following field, to prove the offset advanced by 8
      final r = SlskReader(w.take());
      expect(r.fileSize(), 3000000000);
      expect(r.uint32(), 7);
    });
  });

  group('framing', () {
    test('uint32-code frames round-trip through the buffer', () {
      final payload = Uint8List.fromList(List.generate(40, (i) => i));
      final framed = frameWithUint32Code(26, payload);

      // length prefix covers code + payload
      expect(SlskReader(framed).uint32(), payload.length + 4);

      final buf = SlskFrameBuffer()..add(framed);
      final frames = buf.readFrames();
      expect(frames, hasLength(1));
      expect(frames.single.code, 26);
      expect(frames.single.payload, orderedEquals(payload));
    });

    test('peer-init frames use a uint8 code', () {
      final payload = Uint8List.fromList([9, 8, 7]);
      final framed = framePeerInit(1, payload);
      expect(SlskReader(framed).uint32(), payload.length + 1);
      expect(framed[4], 1);

      final buf = SlskFrameBuffer(codeIsByte: true)..add(framed);
      final f = buf.readFrame()!;
      expect(f.code, 1);
      expect(f.payload, orderedEquals(payload));
    });

    test('a frame split across many socket chunks is reassembled', () {
      final payload = Uint8List.fromList(List.generate(300, (i) => i & 0xFF));
      final framed = frameWithUint32Code(9, payload);

      final buf = SlskFrameBuffer();
      // Feed one byte at a time; nothing should surface until the last byte.
      for (var i = 0; i < framed.length - 1; i++) {
        buf.add([framed[i]]);
        expect(buf.readFrames(), isEmpty);
      }
      buf.add([framed.last]);
      final frames = buf.readFrames();
      expect(frames, hasLength(1));
      expect(frames.single.payload, orderedEquals(payload));
    });

    test('several frames in one chunk all come out, in order', () {
      final buf = SlskFrameBuffer();
      buf.add([
        ...frameWithUint32Code(1, Uint8List.fromList([1])),
        ...frameWithUint32Code(2, Uint8List.fromList([2, 2])),
        ...frameWithUint32Code(3, Uint8List.fromList([3, 3, 3])),
      ]);
      final frames = buf.readFrames();
      expect(frames.map((f) => f.code), orderedEquals([1, 2, 3]));
      expect(frames.last.payload, hasLength(3));
    });

    test('a trailing partial frame stays buffered for the next chunk', () {
      final whole = frameWithUint32Code(5, Uint8List.fromList([1, 2, 3, 4]));
      final buf = SlskFrameBuffer()
        ..add(whole)
        ..add(whole.sublist(0, 3));
      expect(buf.readFrames(), hasLength(1));
      buf.add(whole.sublist(3));
      expect(buf.readFrames(), hasLength(1));
      expect(buf.readFrames(), isEmpty);
    });

    test('an implausible length prefix is rejected', () {
      final buf = SlskFrameBuffer(maxMessageSize: 1024);
      buf.add((SlskWriter()
            ..uint32(999999)
            ..uint32(1))
          .take());
      expect(buf.readFrames, throwsA(isA<SlskParseException>()));
    });

    test('takeRaw / drain support the unframed "F" connection prologue', () {
      // An 'F' connection sends a bare uint32 token then raw file bytes.
      final buf = SlskFrameBuffer();
      buf.add((SlskWriter()..uint32(0xDEADBEEF)).take());
      buf.add([1, 2, 3, 4, 5]);

      final token = buf.takeRaw(4)!;
      expect(ByteData.sublistView(token).getUint32(0, Endian.little),
          0xDEADBEEF);
      expect(buf.drain(), orderedEquals([1, 2, 3, 4, 5]));
      expect(buf.takeRaw(1), isNull);
    });
  });
}
