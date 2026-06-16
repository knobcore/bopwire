import 'dart:ffi';
import 'dart:typed_data';
import 'package:ffi/ffi.dart';
import 'package:flutter_secure_storage/flutter_secure_storage.dart';
import 'package:path_provider/path_provider.dart';
import 'dart:io';

import '../ffi/native_library.dart';
import '../models/wallet.dart';

class WalletService {
  static const _secureStorage = FlutterSecureStorage();
  static const _walletPathKey     = 'mc_wallet_path';
  // Stored alongside the path so the next app launch can auto-load
  // without prompting. flutter_secure_storage backs onto Keychain on
  // iOS / KeyStore on Android / DPAPI on Windows, so the password
  // doesn't sit in plain prefs.
  static const _walletPasswordKey = 'mc_wallet_password';

  Pointer<Void>? _walletHandle;
  WalletInfo? _cachedInfo;

  bool get hasWallet => _walletHandle != null;
  WalletInfo? get info => _cachedInfo;

  /// True when a wallet file exists on disk (regardless of whether we've
  /// loaded it into memory yet). Used by the auto-load path to decide
  /// whether prompting the user for a password is even necessary.
  Future<bool> hasSavedWallet() async {
    return (await _secureStorage.read(key: _walletPathKey)) != null;
  }

  /// Auto-load the persisted wallet using the password we cached in
  /// secure storage at create/import/load time. Returns null when there
  /// is no saved wallet (first run) or when the keyring entry is
  /// missing — caller falls back to the password prompt in that case.
  Future<WalletInfo?> tryAutoLoad() async {
    final path = await _secureStorage.read(key: _walletPathKey);
    final pw   = await _secureStorage.read(key: _walletPasswordKey);
    if (path == null || pw == null) return null;
    return loadWallet(pw);
  }

  // Create a new wallet with password
  Future<WalletInfo> createWallet(String password) async {
    final passwordPtr = password.toNativeUtf8();
    try {
      final handle = NativeLibrary.bindings
          .mc_wallet_create(passwordPtr.cast());
      if (handle == nullptr) {
        throw Exception('Failed to create wallet');
      }
      _walletHandle = handle;
      await _saveAndCache(password);
      return _cachedInfo!;
    } finally {
      calloc.free(passwordPtr);
    }
  }

  // Load wallet from secure storage
  Future<WalletInfo?> loadWallet(String password) async {
    final pathStr = await _secureStorage.read(key: _walletPathKey);
    if (pathStr == null) return null;

    final pathPtr     = pathStr.toNativeUtf8();
    final passwordPtr = password.toNativeUtf8();
    try {
      final handle = NativeLibrary.bindings
          .mc_wallet_load(pathPtr.cast(), passwordPtr.cast());
      if (handle == nullptr) return null;
      _walletHandle = handle;
      _updateCache();
      // Store the password so the next app launch auto-loads via
      // tryAutoLoad() — the keyring is the only persistent place we
      // keep it, never plain prefs.
      await _secureStorage.write(key: _walletPasswordKey, value: password);
      return _cachedInfo;
    } finally {
      calloc.free(pathPtr);
      calloc.free(passwordPtr);
    }
  }

  // Import from hex private key
  Future<WalletInfo> importWallet(String privateKeyHex, String password) async {
    final keyPtr      = privateKeyHex.toNativeUtf8();
    final passwordPtr = password.toNativeUtf8();
    try {
      final handle = NativeLibrary.bindings
          .mc_wallet_import(keyPtr.cast(), passwordPtr.cast());
      if (handle == nullptr) throw Exception('Invalid private key');
      _walletHandle = handle;
      await _saveAndCache(password);
      return _cachedInfo!;
    } finally {
      calloc.free(keyPtr);
      calloc.free(passwordPtr);
    }
  }

  // Sign data, returns hex signature
  String sign(Uint8List data) {
    if (_walletHandle == null) throw Exception('No wallet loaded');
    final dataPtr = malloc.allocate<Uint8>(data.length);
    dataPtr.asTypedList(data.length).setAll(0, data);
    final sigPtr = NativeLibrary.bindings
        .mc_wallet_sign(_walletHandle!, dataPtr, data.length);
    malloc.free(dataPtr);
    if (sigPtr == nullptr) throw Exception('Signing failed');
    final sig = sigPtr.cast<Utf8>().toDartString();
    NativeLibrary.bindings.mc_free(sigPtr.cast());
    return sig;
  }

  void freeWallet() {
    if (_walletHandle != null) {
      NativeLibrary.bindings.mc_wallet_free(_walletHandle!);
      _walletHandle = null;
      _cachedInfo   = null;
    }
  }

  // ---- Internal -------------------------------------------------------

  Future<void> _saveAndCache(String password) async {
    final dir = await getApplicationSupportDirectory();
    final path = '${dir.path}/wallet/player.key';
    await Directory('${dir.path}/wallet').create(recursive: true);

    final pathPtr = path.toNativeUtf8();
    NativeLibrary.bindings.mc_wallet_save(_walletHandle!, pathPtr.cast());
    calloc.free(pathPtr);

    await _secureStorage.write(key: _walletPathKey,     value: path);
    await _secureStorage.write(key: _walletPasswordKey, value: password);
    _updateCache();
  }

  void _updateCache() {
    if (_walletHandle == null) return;
    final addrPtr   = NativeLibrary.bindings.mc_wallet_get_address(_walletHandle!);
    final pubkeyPtr = NativeLibrary.bindings.mc_wallet_get_public_key(_walletHandle!);

    final addr   = addrPtr.cast<Utf8>().toDartString();
    final pubkey = pubkeyPtr.cast<Utf8>().toDartString();

    NativeLibrary.bindings.mc_free(addrPtr.cast());
    NativeLibrary.bindings.mc_free(pubkeyPtr.cast());

    _cachedInfo = WalletInfo(address: addr, publicKey: pubkey, balance: '0.00000000');
  }
}
