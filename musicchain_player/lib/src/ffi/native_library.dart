import 'dart:ffi';
import 'dart:io';

// ignore: implementation_imports
import 'bindings.dart';

class NativeLibrary {
  static late MusicChainBindings _bindings;
  static bool _initialized = false;

  static MusicChainBindings get bindings {
    assert(_initialized, 'Call NativeLibrary.initialize() first');
    return _bindings;
  }

  static Future<void> initialize() async {
    if (_initialized) return;

    final lib = _loadLibrary();
    _bindings = MusicChainBindings(lib);

    final result = _bindings.mc_init();
    if (result != 0) {
      throw Exception('mc_init() failed');
    }
    _initialized = true;
  }

  static DynamicLibrary _loadLibrary() {
    if (Platform.isWindows) {
      return DynamicLibrary.open('musicchain.dll');
    } else if (Platform.isMacOS) {
      return DynamicLibrary.open('libmusicchain.dylib');
    } else if (Platform.isAndroid || Platform.isLinux) {
      return DynamicLibrary.open('libmusicchain.so');
    } else if (Platform.isIOS) {
      // iOS uses static linking via xcframework
      return DynamicLibrary.process();
    }
    throw UnsupportedError('Unsupported platform: ${Platform.operatingSystem}');
  }

  static void cleanup() {
    if (_initialized) {
      _bindings.mc_cleanup();
      _initialized = false;
    }
  }
}
