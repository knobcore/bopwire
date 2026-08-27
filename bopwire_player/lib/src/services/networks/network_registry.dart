// Holds every ExternalNetwork the app knows about and owns the one piece
// of state the implementations deliberately don't: whether the user has
// ticked that network's search checkbox.
//
// The search screen and settings screen both bind to this, so neither
// has to name a concrete network class. Registering a third network
// later is a one-line change in [NetworkRegistry.bootstrap].

import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:shared_preferences/shared_preferences.dart';

import 'external_network.dart';
import 'network_credentials.dart';

class NetworkRegistry extends ChangeNotifier {
  NetworkRegistry._();
  static final NetworkRegistry instance = NetworkRegistry._();

  final List<ExternalNetwork> _networks = [];
  final Map<String, StreamSubscription<NetworkStatus>> _statusSubs = {};
  bool _ready = false;

  List<ExternalNetwork> get networks => List.unmodifiable(_networks);

  /// Networks the user has ticked AND that have their credentials — the
  /// set a search should actually go out to.
  List<ExternalNetwork> get activeNetworks =>
      _networks.where((n) => n.enabled && n.isConfigured).toList();

  bool get ready => _ready;

  static String _enabledKey(String id) => 'network.$id.enabled';

  /// Register implementations and restore persisted state. Call once at
  /// startup, before the first frame that reads [networks].
  Future<void> bootstrap(List<ExternalNetwork> impls) async {
    _networks
      ..clear()
      ..addAll(impls);

    final prefs = await SharedPreferences.getInstance();
    for (final n in _networks) {
      // Default off: a fresh install should not fan searches out to
      // foreign networks until the user opts in.
      n.enabled = prefs.getBool(_enabledKey(n.id)) ?? false;
      await NetworkCredentials.instance.load(n.id, n.credentialFields);

      _statusSubs[n.id]?.cancel();
      _statusSubs[n.id] = n.statusChanges.listen(
        (_) => notifyListeners(),
        onError: (_) => notifyListeners(),
      );
    }
    _ready = true;
    notifyListeners();
  }

  ExternalNetwork? byId(String id) {
    for (final n in _networks) {
      if (n.id == id) return n;
    }
    return null;
  }

  Future<void> setEnabled(String id, bool value) async {
    final n = byId(id);
    if (n == null) return;
    n.enabled = value;
    final prefs = await SharedPreferences.getInstance();
    await prefs.setBool(_enabledKey(id), value);
    notifyListeners();

    // Connecting is best-effort: a failure surfaces through the
    // network's own status stream, which we're already listening to.
    if (value && n.isConfigured && n.status == NetworkStatus.disconnected) {
      unawaited(n.connect().catchError((_) {}));
    } else if (!value && n.status != NetworkStatus.disconnected) {
      unawaited(n.disconnect().catchError((_) {}));
    }
  }

  /// Re-read credentials after the settings form saved, then reconnect
  /// anything that is enabled and now has what it needs.
  Future<void> credentialsChanged(String id) async {
    final n = byId(id);
    if (n == null) return;
    await NetworkCredentials.instance.load(n.id, n.credentialFields);
    notifyListeners();
    if (n.enabled && n.isConfigured) {
      unawaited(n.connect().catchError((_) {}));
    }
  }

  @override
  void dispose() {
    for (final s in _statusSubs.values) {
      s.cancel();
    }
    _statusSubs.clear();
    super.dispose();
  }
}
