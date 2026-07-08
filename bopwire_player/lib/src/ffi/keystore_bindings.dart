// Hand-written FFI bindings for the native scrypt + AES-256-GCM keystore
// (mc_keystore_encrypt / mc_keystore_decrypt in include/bopwire.h). Same lookup
// style as wallet_mnemonic_bindings.dart. Used by SecureStore (device-bound
// local vault) and by wallet export/import.

import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'native_library.dart';

typedef _TwoCharToCharC    = Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>);
typedef _TwoCharToCharDart = Pointer<Utf8> Function(Pointer<Utf8>, Pointer<Utf8>);

class KeystoreBindings {
  /// Encrypt [plaintext] under [password]; returns the keystore JSON string, or
  /// null on failure.
  static String? encrypt(String plaintext, String password) {
    final fn = _lookup('mc_keystore_encrypt');
    if (fn == null) return null;
    final pt = plaintext.toNativeUtf8();
    final pw = password.toNativeUtf8();
    try {
      final r = fn(pt, pw);
      if (r.address == 0) return null;
      final s = r.toDartString();
      NativeLibrary.bindings.mc_free(r.cast());
      return s;
    } finally {
      calloc.free(pt);
      calloc.free(pw);
    }
  }

  /// Decrypt keystore JSON under [password]; returns the plaintext, or null on
  /// wrong password / corrupt input.
  static String? decrypt(String keystoreJson, String password) {
    final fn = _lookup('mc_keystore_decrypt');
    if (fn == null) return null;
    final js = keystoreJson.toNativeUtf8();
    final pw = password.toNativeUtf8();
    try {
      final r = fn(js, pw);
      if (r.address == 0) return null;
      final s = r.toDartString();
      NativeLibrary.bindings.mc_free(r.cast());
      return s;
    } finally {
      calloc.free(js);
      calloc.free(pw);
    }
  }

  static _TwoCharToCharDart? _lookup(String name) {
    try {
      final ptr = NativeLibrary.lib.lookup<NativeFunction<_TwoCharToCharC>>(name);
      return ptr.asFunction<_TwoCharToCharDart>();
    } catch (_) {
      return null;
    }
  }
}
