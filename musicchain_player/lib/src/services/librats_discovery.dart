import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'node_service.dart';
import 'rats_client.dart';

/// Pure-librats peer discovery. Asks the VPS mini-node for its current
/// routing table via the `routes.get` RPC, then sorts the returned full
/// nodes by `last_seen_ms` and picks the freshest one. No HTTP probes,
/// no DHT crawl.
class LibratsDiscovery extends ChangeNotifier {
  LibratsDiscovery() {
    _instance = this;
    _startPeriodicRefresh();
  }

  /// Lazy global so providers / services that can't easily reach the
  /// widget tree (LibraryProvider's RPC retry path, for one) can still
  /// trigger a forced rediscovery when an RPC fails.
  static LibratsDiscovery? _instance;
  static LibratsDiscovery? get current => _instance;

  /// Last route list pulled from the VPS, keyed by node_id.
  final Map<String, Map<String, dynamic>> routes = {};

  /// rats_peer_id of the currently-auto-selected full node.
  String autoSelectedRatsPeerId = '';

  /// Caller invoked when the auto-selected full node identity changes
  /// (first discovery, or it cycled to a different node). The player wires
  /// this to LibraryScanner.reAnnounce so swarm membership rebuilds when
  /// the full node restarts. Without this, a home-only restart left the
  /// player serving songs nobody knew about.
  void Function(String newHomePeerId)? onAutoNodeChanged;

  String _lastNotifiedNodePid = '';

  /// Best-effort HTTP URL, kept for compatibility with screens that still
  /// display "connected to <url>" — derived from the route's api_url field.
  String autoSelectedUrl = '';

  /// How we reach the auto-selected node: 'direct' when the mini-node's
  /// inbound probe confirmed it and a public address is known, 'relay'
  /// when every RPC has to tunnel via the VPS mini-node. Useful for the
  /// settings screen to show a "via tunnel" indicator.
  String autoSelectedReachability = 'unknown';

  String  lastError       = '';
  bool    isRefreshing    = false;
  String  vpsStatus       = '';

  Timer? _timer;

  void _startPeriodicRefresh() {
    // Don't sleep-then-poke. Wait until the VPS rats handshake actually
    // completes before the first routes.get; on slow networks the 2-second
    // sleep used to fall short and we'd time out + sit empty for 30 s.
    _runWhenVpsReady();
    _timer = Timer.periodic(const Duration(seconds: 30), (_) => refresh());
  }

  Future<void> _runWhenVpsReady() async {
    final rats = RatsClient.instance;
    for (int i = 0; i < 30; ++i) {
      if (rats.validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(seconds: 1));
    }
    await refresh();
  }

  Future<void> refresh() async {
    if (isRefreshing) return;
    isRefreshing = true;
    lastError    = '';
    vpsStatus    = 'Asking VPS for routes...';
    notifyListeners();

    try {
      final rats = RatsClient.instance;
      final list = await rats.requestRoutes();
      routes
        ..clear()
        ..addEntries(list
            .where((m) => (m['node_id'] as String? ?? '').isNotEmpty)
            .map((m) => MapEntry(m['node_id'] as String, m)));

      // Wire reachability into RatsClient's relay map. Nodes the mini-node
      // tagged "relay" are unreachable from outside their NAT — their
      // RPCs must be tunneled through the mini-node. Nodes tagged "direct"
      // are reachable so we send to them straight.
      final vpsPeer = rats.validatedPeerIds.isNotEmpty
          ? rats.validatedPeerIds.first
          : '';
      for (final r in routes.values) {
        final pid   = r['rats_peer_id']   as String? ?? '';
        final reach = r['reachability']   as String? ?? 'unknown';
        final pub   = r['public_address'] as String? ?? '';
        if (pid.isEmpty) continue;
        // Direct only when the mini-node's probe confirmed inbound is open
        // AND we have a public address to dial. Anything else (relay /
        // unknown / no public_address) routes through the VPS tunnel.
        final canDirect = reach == 'direct' && pub.isNotEmpty;
        if (!canDirect && vpsPeer.isNotEmpty) {
          rats.setRelayVia(pid, vpsPeer);
        } else {
          rats.setRelayVia(pid, null);
        }
      }

      if (routes.isEmpty) {
        vpsStatus = 'No full nodes registered with VPS yet';
      } else {
        // Pick the most recently-seen full node with a non-empty
        // rats_peer_id. last_seen_ms is set server-side per route_publish.
        final sorted = routes.values.toList()
          ..sort((a, b) => ((b['last_seen_ms'] as int? ?? 0))
              .compareTo(a['last_seen_ms'] as int? ?? 0));
        final pick = sorted.firstWhere(
          (m) => (m['rats_peer_id'] as String? ?? '').isNotEmpty,
          orElse: () => sorted.first,
        );
        autoSelectedRatsPeerId   = pick['rats_peer_id'] as String? ?? '';
        autoSelectedUrl          = pick['api_url']      as String? ?? '';
        // Surface the chosen node's reachability so UI can show "via tunnel"
        // when relayed. The setRelayVia loop above already configured the
        // routing in RatsClient — this is purely a display hint.
        final pickReach = pick['reachability'] as String? ?? 'unknown';
        final pickPub   = pick['public_address'] as String? ?? '';
        autoSelectedReachability =
            (pickReach == 'direct' && pickPub.isNotEmpty) ? 'direct' : 'relay';

        await NodeService.updateAutoNode(ratsPeerId: autoSelectedRatsPeerId);

        // Notify listeners (LibraryScanner.reAnnounce hook in main.dart)
        // whenever the auto-selected full node id changes — first
        // discovery, or a different node won the freshness sort. With
        // SwarmIndex persistence this fires only on actual identity
        // changes, not every 30 s refresh.
        if (autoSelectedRatsPeerId.isNotEmpty &&
            autoSelectedRatsPeerId != _lastNotifiedNodePid) {
          _lastNotifiedNodePid = autoSelectedRatsPeerId;
          try { onAutoNodeChanged?.call(autoSelectedRatsPeerId); } catch (_) {}
        }

        // Save peer ids for offline restart (mirrors old DHT cache).
        final prefs = await SharedPreferences.getInstance();
        await prefs.setStringList(
          'discovered_rats_peers',
          routes.values
              .map((m) => m['rats_peer_id'] as String? ?? '')
              .where((s) => s.isNotEmpty)
              .toList(),
        );

        final via = autoSelectedReachability == 'direct'
                    ? 'direct'
                    : 'tunneled via VPS';
        vpsStatus = '${routes.length} full node'
                    '${routes.length == 1 ? '' : 's'} via VPS '
                    '(selected: $via)';
      }
    } catch (e) {
      lastError = e.toString();
      vpsStatus = 'VPS error: $e';
    } finally {
      isRefreshing = false;
      notifyListeners();
    }
  }

  /// User-initiated "go offline" path. Drops every connected librats
  /// peer and clears the auto-selected node so the UI shows "No nodes —
  /// tap Connect to search". Differs from forceReconnect() in that we
  /// do NOT immediately re-dial the VPS — the user explicitly asked to
  /// stop talking to peers. The next tap on Connect re-dials.
  Future<void> disconnect() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('auto_node_url');
    await prefs.remove('auto_rats_peer_id');
    await prefs.remove('discovered_rats_peers');
    routes.clear();
    autoSelectedUrl          = '';
    autoSelectedRatsPeerId   = '';
    autoSelectedReachability = 'unknown';
    vpsStatus                = 'disconnected';
    notifyListeners();

    RatsClient.instance.disconnectAll();
  }

  Future<void> forceReconnect() async {
    final prefs = await SharedPreferences.getInstance();
    await prefs.remove('auto_node_url');
    await prefs.remove('auto_rats_peer_id');
    await prefs.remove('discovered_rats_peers');
    routes.clear();
    autoSelectedUrl          = '';
    autoSelectedRatsPeerId   = '';
    autoSelectedReachability = 'unknown';
    notifyListeners();

    // Kick the librats client into re-dialing the VPS rendezvous. Without
    // this, clearing the prefs only resets UI state — the underlying
    // librats socket stays dead and refresh() returns nothing.
    RatsClient.instance.connect(kVpsHost, kVpsRatsPort);
    // Give the handshake up to a few seconds to land before asking for
    // routes; otherwise refresh() racing the connect produces a brief
    // "No nodes" flash before the watchdog tick recovers.
    for (int i = 0; i < 20; ++i) {
      if (RatsClient.instance.validatedPeerIds.isNotEmpty) break;
      await Future.delayed(const Duration(milliseconds: 250));
    }
    await refresh();
  }

  @override
  void dispose() {
    _timer?.cancel();
    super.dispose();
  }
}
