// Dart FFI binding for the embedded Arti (Tor) client.
//
// The Rust side is deps/tor-ffi, compiled to libtor_ffi.a and force-linked
// into libbopwire.so (whole-archive, since no C++ code references it — the
// only caller is this file, at runtime). That linkage is what lets Tor
// ship on every platform the player builds for, Android included, where
// spawning a `tor` binary is not an option.
//
// Scope: a SOCKS5 proxy on loopback for napstr downloads only. librats and
// the bopwire swarm are untouched and stay on clearnet.

import 'dart:ffi';
import 'dart:io';

import 'package:ffi/ffi.dart';

import 'native_library.dart';

/// Mirrors the constants in deps/tor-ffi/src/lib.rs.
enum TorState { stopped, bootstrapping, ready, error }

typedef _StartNative = Int32 Function(Uint16 port);
typedef _StartDart   = int Function(int port);
typedef _StatusNative = Int32 Function();
typedef _StatusDart   = int Function();
typedef _StopNative = Void Function();
typedef _StopDart   = void Function();
typedef _ErrNative = Pointer<Utf8> Function();
typedef _ErrDart   = Pointer<Utf8> Function();

class TorBindings {
  TorBindings._(this._start, this._status, this._stop, this._lastError);

  final _StartDart  _start;
  final _StatusDart _status;
  final _StopDart   _stop;
  final _ErrDart    _lastError;

  static TorBindings? _instance;
  static bool _probed = false;

  /// Null when the running build has no embedded Tor — a node-flavoured
  /// or MC_WITH_TOR=OFF library. Callers fall back to a system proxy
  /// rather than failing, so a build without Arti is degraded, not broken.
  static TorBindings? get maybe {
    if (_probed) return _instance;
    _probed = true;
    try {
      final lib = NativeLibrary.lib;
      _instance = TorBindings._(
        lib.lookupFunction<_StartNative, _StartDart>('tor_ffi_start'),
        lib.lookupFunction<_StatusNative, _StatusDart>('tor_ffi_status'),
        lib.lookupFunction<_StopNative, _StopDart>('tor_ffi_stop'),
        lib.lookupFunction<_ErrNative, _ErrDart>('tor_ffi_last_error'),
      );
    } on ArgumentError {
      // Symbol absent: built without MC_WITH_TOR.
      _instance = null;
    } catch (_) {
      _instance = null;
    }
    return _instance;
  }

  /// Start Arti and bind a local SOCKS5 listener.
  ///
  /// Pass 0 (the default) to let the OS pick — a fixed port would collide
  /// with a system tor the user may already be running. Returns the bound
  /// port, or null on failure with [lastError] populated.
  ///
  /// Returns as soon as the port is bound; bootstrapping the Tor directory
  /// continues in the background, so poll [state] before expecting a
  /// download to succeed.
  int? start({int port = 0}) {
    final rc = _start(port);
    if (rc <= 0) return null;
    return rc;
  }

  TorState get state {
    switch (_status()) {
      case 1:  return TorState.bootstrapping;
      case 2:  return TorState.ready;
      case 3:  return TorState.error;
      default: return TorState.stopped;
    }
  }

  void stop() => _stop();

  String? get lastError {
    final p = _lastError();
    if (p == nullptr) return null;
    return p.toDartString();
  }

  /// True on platforms where the embedded client is the expected route.
  /// On Android there is no alternative (no daemon to find unless Orbot
  /// is installed), which is precisely why this exists.
  static bool get supported =>
      Platform.isAndroid || Platform.isLinux ||
      Platform.isWindows || Platform.isMacOS;
}
