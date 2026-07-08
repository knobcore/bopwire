import 'dart:convert';
import 'dart:io';

import 'package:path_provider/path_provider.dart';

import '../ffi/keystore_bindings.dart';

/// Password-locked wallet vault — replaces `flutter_secure_storage` on every
/// platform (no OS keychain, no gnome-keyring/D-Bus).
///
/// The `{key: value}` map (the BIP39 mnemonic + cached username) is encrypted at
/// `<app-support>/wallet.vault` via the native keystore (scrypt + AES-256-GCM,
/// FFI `mc_keystore_encrypt`) with the user's password. A **mandatory** password
/// unlocks it — there is no auto-unlock. The password is the only protection at
/// rest (no device binding), so a strong password matters; the seed is never
/// stored in the clear, and the 12-word phrase / export file remain the recovery
/// path if the password is forgotten.
///
/// Exposes the same `read/write/delete({key/value})` surface `WalletService`
/// used on `FlutterSecureStorage`, plus vault lifecycle: `hasVault`,
/// `createVault`, `unlock`, `changePassword`, `lock`, `destroy`.
class SecureStore {
  SecureStore._();
  static final SecureStore _instance = SecureStore._();
  factory SecureStore() => _instance;

  static const _vaultName = 'wallet.vault';

  Map<String, String>? _map; // decrypted, in-memory. null => locked.
  String? _password;         // held for re-encrypt on write.
  File? _file;

  bool get isUnlocked => _map != null;

  Future<File> _vaultPath() async {
    if (_file != null) return _file!;
    final dir = await getApplicationSupportDirectory();
    _file = File('${dir.path}/$_vaultName');
    return _file!;
  }

  /// True when a vault file already exists (a wallet is set up but locked).
  Future<bool> hasVault() async => (await _vaultPath()).exists();

  /// Start a fresh, empty vault protected by [password] (first-launch).
  Future<void> createVault(String password) async {
    _map = {};
    _password = password;
    await _persist();
  }

  /// Decrypt the existing vault with [password]. Returns false on wrong password
  /// / corrupt file (leaves the store locked).
  Future<bool> unlock(String password) async {
    final f = await _vaultPath();
    if (!await f.exists()) return false;
    String contents;
    try {
      contents = await f.readAsString();
    } catch (_) {
      return false;
    }
    final json = KeystoreBindings.decrypt(contents, password);
    if (json == null) return false; // wrong password or tampered
    try {
      final m = jsonDecode(json) as Map<String, dynamic>;
      _map = m.map((k, v) => MapEntry(k, v?.toString() ?? ''));
    } catch (_) {
      _map = {}; // decrypted but unparseable — treat as empty
    }
    _password = password;
    return true;
  }

  /// Re-encrypt the current (unlocked) vault under a new password. Returns false
  /// if [oldPassword] is wrong.
  Future<bool> changePassword(String oldPassword, String newPassword) async {
    if (!isUnlocked) {
      if (!await unlock(oldPassword)) return false;
    } else if (oldPassword != _password) {
      return false;
    }
    _password = newPassword;
    await _persist();
    return true;
  }

  /// Forget the decrypted secret (e.g. on lock/timeout). The file stays.
  void lock() {
    _map = null;
    _password = null;
  }

  Future<void> _persist() async {
    final map = _map, pw = _password;
    if (map == null || pw == null) {
      throw StateError('SecureStore: vault is locked (unlock/createVault first)');
    }
    final js = KeystoreBindings.encrypt(jsonEncode(map), pw);
    if (js == null) throw Exception('SecureStore: vault encryption failed');
    final f = await _vaultPath();
    await f.writeAsString(js, flush: true);
  }

  /// Delete the vault file entirely (sign-out / reset). Leaves the store locked.
  Future<void> destroy() async {
    lock();
    try {
      final f = await _vaultPath();
      if (await f.exists()) await f.delete();
    } catch (_) {}
  }

  // ---- FlutterSecureStorage-compatible surface (unlocked map) ----------
  Future<String?> read({required String key}) async => _map?[key];

  Future<void> write({required String key, required String value}) async {
    if (_password == null) {
      throw StateError('SecureStore.write: vault is locked');
    }
    (_map ??= {})[key] = value;
    await _persist();
  }

  Future<void> delete({required String key}) async {
    if (_map == null) return;
    _map!.remove(key);
    if (_password != null) await _persist();
  }
}
