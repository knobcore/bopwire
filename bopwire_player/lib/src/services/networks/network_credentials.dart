// Credential storage shared by every ExternalNetwork implementation.
//
// Two backing stores, chosen per field:
//   * secret fields (passwords, Nostr nsec) go into SecureStore, the
//     password-encrypted vault the wallet already uses. They are only
//     readable while that vault is unlocked.
//   * non-secret fields (usernames, relay URLs) go into SharedPreferences
//     so a network can still show "configured" and reconnect before the
//     user has unlocked their wallet.
//
// Implementations must not invent their own persistence — use this, so
// the settings screen can render and clear credentials generically.

import 'package:shared_preferences/shared_preferences.dart';

import '../secure_store.dart';
import 'external_network.dart';

class NetworkCredentials {
  NetworkCredentials._();
  static final NetworkCredentials instance = NetworkCredentials._();

  /// In-memory cache so a network can read credentials synchronously
  /// while connecting. Populated by [load].
  final Map<String, Map<String, String>> _cache = {};

  static String _prefsKey(String networkId, String field) =>
      'network.$networkId.$field';

  static String _vaultKey(String networkId, String field) =>
      'network.$networkId.$field';

  /// Load every field for [networkId] into the cache. Secret fields are
  /// skipped silently when the vault is locked — [isConfigured] then
  /// reports false for them, which is the honest answer.
  Future<Map<String, String>> load(
    String networkId,
    List<NetworkCredentialField> fields,
  ) async {
    final prefs = await SharedPreferences.getInstance();
    final out = <String, String>{};
    for (final f in fields) {
      String? v;
      if (f.secret) {
        try {
          v = await SecureStore().read(key: _vaultKey(networkId, f.key));
        } catch (_) {
          v = null; // vault locked or absent
        }
      } else {
        v = prefs.getString(_prefsKey(networkId, f.key));
      }
      if (v != null && v.isNotEmpty) out[f.key] = v;
    }
    _cache[networkId] = out;
    return out;
  }

  /// Cached value, or null. Call [load] first.
  String? get(String networkId, String field) => _cache[networkId]?[field];

  Future<void> set(
    String networkId,
    NetworkCredentialField field,
    String value,
  ) async {
    if (field.secret) {
      await SecureStore()
          .write(key: _vaultKey(networkId, field.key), value: value);
    } else {
      final prefs = await SharedPreferences.getInstance();
      await prefs.setString(_prefsKey(networkId, field.key), value);
    }
    (_cache[networkId] ??= {})[field.key] = value;
  }

  Future<void> clear(
    String networkId,
    List<NetworkCredentialField> fields,
  ) async {
    final prefs = await SharedPreferences.getInstance();
    for (final f in fields) {
      if (f.secret) {
        try {
          await SecureStore().delete(key: _vaultKey(networkId, f.key));
        } catch (_) {/* vault locked — nothing to remove from memory */}
      } else {
        await prefs.remove(_prefsKey(networkId, f.key));
      }
    }
    _cache.remove(networkId);
  }

  /// True when every field marked required has a non-empty cached value.
  bool hasRequired(String networkId, List<NetworkCredentialField> fields) {
    final c = _cache[networkId];
    if (c == null) return fields.where((f) => f.required_).isEmpty;
    for (final f in fields) {
      if (!f.required_) continue;
      final v = c[f.key];
      if (v == null || v.isEmpty) return false;
    }
    return true;
  }
}
