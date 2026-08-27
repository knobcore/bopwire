// LIVE-ish check of the mc_portmap_open_tcp / mc_portmap_close_tcp FFI
// path: loads a real libbopwire.so and asks the actual LAN router for a
// UPnP/NAT-PMP mapping of an ephemeral test port.
//
// Success depends on the network the machine sits on (a router with UPnP
// or NAT-PMP enabled), so the test asserts the CONTRACT, not success:
//   - the call returns either a plausible mapping or null,
//   - it honours its timeout instead of hanging,
//   - closeTcp returns cleanly whether or not a mapping exists.
// The observed outcome is printed so a human reading the log knows whether
// this network actually produced a mapping.
//
// The library is taken from BOPWIRE_LIB, defaulting to the linux build
// tree. Skips when the library is missing or predates the portmap exports.
//
//   BOPWIRE_LIB=/path/to/libbopwire.so \
//     flutter test test/networks/slsk_portmap_live_test.dart
import 'dart:ffi';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';

import 'package:bopwire_player/src/ffi/portmap_bindings.dart';

const _defaultLib =
    '/home/lain/blockchain/bopwire/build-linux-x64/Release/libbopwire.so';

void main() {
  final libPath = Platform.environment['BOPWIRE_LIB'] ?? _defaultLib;

  test('portmap FFI: open honours timeout, close is safe', () async {
    if (!File(libPath).existsSync()) {
      markTestSkipped('no native library at $libPath (set BOPWIRE_LIB)');
      return;
    }
    try {
      DynamicLibrary.open(libPath).lookup('mc_portmap_open_tcp');
    } on ArgumentError {
      markTestSkipped('$libPath predates the mc_portmap_* exports — '
          'relink libbopwire.so first');
      return;
    }

    PortMap.libraryPathOverride = libPath;
    addTearDown(() => PortMap.libraryPathOverride = null);

    // An ephemeral-ish port unlikely to collide with the app.
    const port = 52234;
    const timeoutMs = 12000;

    final sw = Stopwatch()..start();
    final mapping = await PortMap.openTcp(port, timeoutMs: timeoutMs);
    sw.stop();

    if (mapping == null) {
      stdout.writeln('[portmap-live] NO MAPPING after ${sw.elapsed} — this '
          'network has no reachable UPnP/NAT-PMP gateway (or is '
          'double-NATed). The player will degrade to direct-only peers.');
      // The C side waits up to timeoutMs and must not hang far beyond it.
      expect(sw.elapsedMilliseconds, lessThan(timeoutMs + 15000));
    } else {
      stdout.writeln('[portmap-live] MAPPED: $mapping');
      expect(mapping.internalPort, port);
      expect(mapping.externalPort, inInclusiveRange(1, 65535));
    }

    // Close must be clean in both cases.
    await PortMap.closeTcp(port);
  }, timeout: const Timeout(Duration(minutes: 2)));
}
