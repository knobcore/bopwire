// Dart FFI binding for the NAT port-mapping exports in bopwire.h:
//
//   uint16_t mc_portmap_open_tcp(uint16_t port, int timeout_ms,
//                                char* out_ip, size_t out_ip_len);
//   void     mc_portmap_close_tcp(uint16_t port);
//
// These wrap librats' UPnP-IGD + NAT-PMP clients (deps/librats) so a host
// behind NAT can ask its router to forward a TCP port. The Soulseek client
// uses this to make the listen port it announces to the server actually
// reachable from the internet — without it, two NATed peers can never
// transfer (neither side can accept the other's connection).
//
// mc_portmap_open_tcp BLOCKS its calling thread for up to timeout_ms while
// the gateway is discovered, so the async wrappers run the call in a helper
// isolate via Isolate.run. Since a DynamicLibrary handle cannot cross an
// isolate boundary, the helper re-opens the library by name — cheap, because
// the process has already loaded it (dlopen returns the existing handle).

import 'dart:ffi';
import 'dart:io';
import 'dart:isolate';

import 'package:ffi/ffi.dart';

import 'native_library.dart';

typedef _NativeOpen = Uint16 Function(Uint16, Int32, Pointer<Utf8>, Size);
typedef _DartOpen = int Function(int, int, Pointer<Utf8>, int);
typedef _NativeClose = Void Function(Uint16);
typedef _DartClose = void Function(int);

/// A confirmed router port mapping: local TCP [internalPort] is reachable
/// from the internet as [externalIp]:[externalPort].
class PortMapping {
  const PortMapping({
    required this.internalPort,
    required this.externalPort,
    required this.externalIp,
  });

  final int internalPort;
  final int externalPort;

  /// Public IPv4 the gateway reported, or '' when it did not report one.
  final String externalIp;

  @override
  String toString() =>
      'PortMapping(local $internalPort -> ${externalIp.isEmpty ? "?" : externalIp}:$externalPort)';
}

/// NAT port mapping (UPnP IGD / NAT-PMP) via libbopwire.
class PortMap {
  PortMap._();

  /// Test-only: absolute path of the shared library to open instead of
  /// resolving through [NativeLibrary]. `flutter test` has no bundled
  /// libbopwire.so on the loader path, so live tests point this at a
  /// build-tree copy.
  static String? libraryPathOverride;

  /// Ask the gateway to forward a TCP port to local [port] and keep the
  /// lease alive. Resolves to the mapping on success, or null when no
  /// UPnP/NAT-PMP gateway answered within [timeoutMs] (or the gateway is
  /// double-NATed). Runs in a helper isolate; never blocks the caller.
  static Future<PortMapping?> openTcp(int port, {int timeoutMs = 8000}) {
    final override = libraryPathOverride;
    return Isolate.run(() => _openTcpWith(_load(override), port, timeoutMs));
  }

  /// Remove the mapping for local TCP [port] (best-effort on the gateway)
  /// and stop refreshing its lease. Safe if [port] was never mapped.
  static Future<void> closeTcp(int port) {
    final override = libraryPathOverride;
    return Isolate.run(() {
      _load(override)
          .lookupFunction<_NativeClose, _DartClose>('mc_portmap_close_tcp')(
              port);
    });
  }

  /// Synchronous variant of [openTcp] — blocks the current isolate for up
  /// to [timeoutMs]. Only for callers that are already off the UI isolate.
  static PortMapping? openTcpSync(int port, {int timeoutMs = 8000}) =>
      _openTcpWith(_load(libraryPathOverride), port, timeoutMs);

  static PortMapping? _openTcpWith(
      DynamicLibrary lib, int port, int timeoutMs) {
    final open = lib
        .lookupFunction<_NativeOpen, _DartOpen>('mc_portmap_open_tcp');
    const ipCap = 64;
    final ipBuf = calloc<Uint8>(ipCap);
    try {
      final external = open(port, timeoutMs, ipBuf.cast(), ipCap);
      if (external == 0) return null;
      return PortMapping(
        internalPort: port,
        externalPort: external,
        externalIp: ipBuf.cast<Utf8>().toDartString(),
      );
    } finally {
      calloc.free(ipBuf);
    }
  }

  static DynamicLibrary _load(String? override) {
    if (override != null) return DynamicLibrary.open(override);
    try {
      // Same handle the rest of the app uses, when this isolate has it.
      return NativeLibrary.lib;
    } catch (_) {
      // Helper isolate: NativeLibrary was initialized on another isolate.
      // Re-open by platform name; the loader dedups already-loaded libs.
    }
    if (Platform.isWindows) return DynamicLibrary.open('bopwire.dll');
    if (Platform.isMacOS) return DynamicLibrary.open('libbopwire.dylib');
    if (Platform.isAndroid || Platform.isLinux) {
      return DynamicLibrary.open('libbopwire.so');
    }
    if (Platform.isIOS) return DynamicLibrary.process();
    throw UnsupportedError(
        'Unsupported platform: ${Platform.operatingSystem}');
  }
}
