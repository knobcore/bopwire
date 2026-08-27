// tor_service.dart — gets the player a working Tor SOCKS5 proxy.
//
// napstr moves its bytes over Tor onion services and PROTOCOL.md states
// there is no clearnet fallback, so a download is impossible without a
// SOCKS proxy. The SOCKS5 client itself already exists (socks5.dart);
// what was missing is actually having a proxy to talk to. napstr_network
// previously only read a hand-configured `tor_socks` field and noted
// "probed, never started — we cannot run Tor", which meant a user with
// Tor already running on the standard port still got nothing, and a user
// without Tor got a dead feature and no route forward.
//
// Resolution order, first hit wins:
//   1. whatever the user configured in Settings (respect it absolutely)
//   2. 127.0.0.1:9050  — system tor / Orbot on Android
//   3. 127.0.0.1:9150  — Tor Browser's bundled daemon
//   4. Arti, embedded as a static library and started in-process
//
// Step 4 is what makes this work on EVERY platform, Android included:
// deps/tor-ffi compiles Arti to libtor_ffi.a, which is force-linked into
// libbopwire.so, so there is no binary to spawn and no daemon to install.
// Steps 2-3 come first so we never start a second Tor when the machine
// already has one (system tor, Orbot, Tor Browser).
//
// This deliberately does NOT touch librats or the bopwire swarm. Tor here
// is a transport for one foreign network, nothing more.

import 'dart:async';

import 'package:path_provider/path_provider.dart';

import '../../../ffi/tor_bindings.dart';
import 'socks5.dart';

enum TorSource {
  /// The endpoint the user typed into Settings.
  configured,

  /// An already-running daemon we found (system tor, Orbot, Tor Browser).
  existing,

  /// Arti, embedded in the app and started in-process.
  embedded,

  /// Nothing usable.
  none,
}

class TorStatus {
  const TorStatus(this.source, this.endpoint, this.detail);

  final TorSource source;
  final Socks5Endpoint? endpoint;

  /// Human-readable account of what happened, for the UI to show. On
  /// failure this says what to do rather than just reporting absence.
  final String detail;

  bool get ok => endpoint != null;
}

class TorService {
  TorService._();
  static final TorService instance = TorService._();

  Socks5Endpoint? _resolved;
  TorStatus? _last;
  Future<TorStatus>? _inFlight;

  TorStatus? get last => _last;

  /// Ports a daemon is conventionally already listening on.
  static const _wellKnown = <int>[9050, 9150];

  /// Find (or start) a proxy. Safe to call repeatedly — the result is
  /// cached once a proxy answers, and concurrent calls share one attempt.
  Future<TorStatus> ensureProxy({String? configured}) {
    final cached = _resolved;
    if (cached != null) {
      return Future.value(_last ??
          TorStatus(TorSource.existing, cached, 'Tor proxy ready.'));
    }
    final running = _inFlight;
    if (running != null) return running;
    final fut = _resolve(configured).whenComplete(() => _inFlight = null);
    _inFlight = fut;
    return fut;
  }

  Future<TorStatus> _resolve(String? configured) async {
    // 1. the user's own setting wins, even if it looks unusual
    final wanted = Socks5Endpoint.tryParse(configured);
    if (wanted != null) {
      if (await socks5ProxyReachable(wanted)) {
        return _remember(TorStatus(TorSource.configured, wanted,
            'Using the Tor proxy you configured (${wanted.host}:${wanted.port}).'));
      }
      return _remember(TorStatus(
        TorSource.none,
        null,
        'The Tor proxy you configured (${wanted.host}:${wanted.port}) did not '
        'answer. Check that Tor is running, or clear the field to let '
        'bopwire find or start one.',
      ));
    }

    // 2/3. a daemon that already exists
    for (final port in _wellKnown) {
      final ep = Socks5Endpoint('127.0.0.1', port);
      if (await socks5ProxyReachable(ep)) {
        return _remember(TorStatus(TorSource.existing, ep,
            'Found a Tor proxy already running on 127.0.0.1:$port.'));
      }
    }

    // 4. the client we ship — works on every platform, Android included
    return _startEmbedded();
  }

  TorStatus _remember(TorStatus s) {
    _last = s;
    _resolved = s.endpoint;
    return s;
  }

  /// Start the embedded Arti client and wait for it to bootstrap.
  ///
  /// Bootstrapping a cold Tor directory can take anywhere from a couple of
  /// seconds to well over a minute, so this polls the FFI status rather
  /// than assuming a fixed delay.
  Future<TorStatus> _startEmbedded() async {
    final tor = TorBindings.maybe;
    if (tor == null) {
      return _remember(const TorStatus(
        TorSource.none,
        null,
        'This build has no embedded Tor, and none is running. Install Tor '
        '(it listens on 127.0.0.1:9050) or set a SOCKS proxy in '
        'Settings → Other networks.',
      ));
    }

    // Arti needs a writable place for its directory cache and state.
    // Its defaults resolve from platform conventions, which on Android
    // point outside the app sandbox — bootstrap then fails and no proxy
    // ever appears, which is what produced "napstr downloads run over
    // Tor... set the Tor SOCKS5 proxy field" on the APK. Handing it the
    // app-private support directory fixes that; on desktop this is
    // equally valid and keeps Tor's files with the app's own data.
    try {
      final dir = await getApplicationSupportDirectory();
      tor.setStateDir('${dir.path}/tor');
    } catch (e) {
      // Non-fatal: without this Arti falls back to its platform default,
      // which is correct on desktop and only fails on Android — where the
      // error below will say so rather than this silently mattering.
      // ignore: avoid_print
      print('[tor] could not set state dir: $e');
    }

    // Port 0: let the OS choose, so we never collide with a system tor.
    final port = tor.start(port: 0);
    if (port == null) {
      return _remember(TorStatus(TorSource.none, null,
          'Could not start the embedded Tor: ${tor.lastError ?? "unknown error"}'));
    }

    final deadline = DateTime.now().add(const Duration(seconds: 120));
    while (DateTime.now().isBefore(deadline)) {
      switch (tor.state) {
        case TorState.ready:
          final ep = Socks5Endpoint('127.0.0.1', port);
          if (await socks5ProxyReachable(ep)) {
            return _remember(TorStatus(TorSource.embedded, ep,
                'Embedded Tor ready on 127.0.0.1:$port.'));
          }
          return _remember(const TorStatus(TorSource.none, null,
              'Tor bootstrapped but its SOCKS port refused a connection.'));
        case TorState.error:
          return _remember(TorStatus(TorSource.none, null,
              'Tor failed to start: ${tor.lastError ?? "unknown error"}'));
        case TorState.stopped:
        case TorState.bootstrapping:
          await Future<void>.delayed(const Duration(milliseconds: 500));
      }
    }
    return _remember(const TorStatus(TorSource.none, null,
        'Tor did not finish bootstrapping within 120s. It may be blocked '
        'on this network.'));
  }

  /// Stop the embedded client. A proxy we merely found is left alone —
  /// it belongs to the user, not to us.
  Future<void> shutdown() async {
    if (_last?.source == TorSource.embedded) {
      TorBindings.maybe?.stop();
    }
    _resolved = null;
    _last = null;
  }
}
