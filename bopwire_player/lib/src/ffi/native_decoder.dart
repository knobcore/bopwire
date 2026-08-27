// native_decoder.dart — audio decode for desktop Linux/macOS via the
// decoder already inside libbopwire.so.
//
// Why this exists
// ---------------
// Fingerprinter.ofFile handled exactly two platforms: Android (a
// MethodChannel to MediaCodec) and Windows (mc_decoder.dll). Everything
// else hit:
//
//     throw UnsupportedError('Audio decode not yet wired for linux')
//
// and because LibraryScanner._processFile wrapped the whole body in a
// bare `catch (_) { _errors += 1; }`, that exception was swallowed
// without a trace. The visible symptom was a track that downloaded
// successfully, reported as "fingerprinted", and then simply never
// appeared in the library — with nothing in any log to say why. A plain
// disk scan failed the same way for the same reason; nothing on Linux
// could ever be fingerprinted.
//
// Note on which native decoder: the pre-existing mc_decoder_* API is
// Ogg/Vorbis ONLY (it wraps mc::audio::OggDecoder), so it returns null
// for the FLAC/MP3/M4A files people actually have — I tried it first and
// it failed on a real FLAC. The node also has mc::audio::decode_any,
// which covers every container FFmpeg can read, but it was not exposed
// to C. It now is (mc_decode_any / mc_pcm_free in bopwire.h), and that
// is what this binds.

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'native_library.dart';

/// PCM as chromaprint wants it: interleaved signed 16-bit.
class DecodedAudio {
  const DecodedAudio({
    required this.pcm,
    required this.sampleRate,
    required this.channelCount,
  });

  final Uint8List pcm;
  final int sampleRate;
  final int channelCount;
}

typedef _DecodeNative = Int32 Function(Pointer<Uint8>, IntPtr,
    Pointer<Pointer<Int16>>, Pointer<IntPtr>, Pointer<Int32>, Pointer<Int32>);
typedef _DecodeDart = int Function(Pointer<Uint8>, int,
    Pointer<Pointer<Int16>>, Pointer<IntPtr>, Pointer<Int32>, Pointer<Int32>);
typedef _PcmFreeNative = Void Function(Pointer<Int16>);
typedef _PcmFreeDart = void Function(Pointer<Int16>);

class NativeDecoder {
  NativeDecoder._(this._decode, this._pcmFree);

  static NativeDecoder? _instance;
  static NativeDecoder get instance => _instance ??= fromLibrary(NativeLibrary.lib);

  /// Bind against an explicit handle. Also used by tests, which have no
  /// AppDir on their library search path.
  static NativeDecoder fromLibrary(DynamicLibrary lib) => NativeDecoder._(
        lib.lookupFunction<_DecodeNative, _DecodeDart>('mc_decode_any'),
        lib.lookupFunction<_PcmFreeNative, _PcmFreeDart>('mc_pcm_free'),
      );

  final _DecodeDart _decode;
  final _PcmFreeDart _pcmFree;

  /// True where this decode path is usable. mc_decode_any is compiled
  /// into the main library on every desktop target (bopwire.dll on
  /// Windows, libbopwire.so/.dylib elsewhere), so this works anywhere
  /// that library is loaded. Android is excluded on purpose: its
  /// MediaCodec channel is hardware-accelerated and much kinder to
  /// battery.
  static bool get supported =>
      Platform.isLinux || Platform.isMacOS || Platform.isWindows;

  /// Decode [path] fully to interleaved 16-bit PCM.
  DecodedAudio decodeFile(String path) =>
      decodeBytes(File(path).readAsBytesSync());

  DecodedAudio decodeBytes(Uint8List bytes) {
    final src      = malloc<Uint8>(bytes.length);
    final outPcm   = malloc<Pointer<Int16>>();
    final outCount = malloc<IntPtr>();
    final outRate  = malloc<Int32>();
    final outChans = malloc<Int32>();
    try {
      src.asTypedList(bytes.length).setAll(0, bytes);
      final rc = _decode(
          src, bytes.length, outPcm, outCount, outRate, outChans);
      if (rc != 0 || outPcm.value.address == 0) {
        throw StateError('mc_decode_any failed (rc=$rc) — unsupported codec '
            'or corrupt audio (${bytes.length} bytes)');
      }
      final count = outCount.value;
      final pcmPtr = outPcm.value;
      try {
        // Copy out before freeing the native block.
        final pcm = Uint8List.fromList(
            pcmPtr.cast<Uint8>().asTypedList(count * 2));
        return DecodedAudio(
          pcm: pcm,
          sampleRate: outRate.value,
          channelCount: outChans.value,
        );
      } finally {
        _pcmFree(pcmPtr);
      }
    } finally {
      malloc.free(src);
      malloc.free(outPcm);
      malloc.free(outCount);
      malloc.free(outRate);
      malloc.free(outChans);
    }
  }
}

/// Test-only alias kept for clarity at call sites.
class NativeDecoderTestHook {
  static DecodedAudio decode(DynamicLibrary lib, Uint8List bytes) =>
      NativeDecoder.fromLibrary(lib).decodeBytes(bytes);
}
