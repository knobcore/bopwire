// Hand-written FFI binding for mc_ecies_encrypt (include/bopwire.h). Seals a
// UTF-8 payload to a recipient's compressed secp256k1 pubkey using the native
// multi-recipient ECIES (the MCE1 wire format). Used by the DMCA/KYC forms to
// encrypt a submission to the shared moderation key end-to-end, so the full
// node stores only ciphertext and never sees the plaintext takedown.
//
// Same hand-written lookup style as keystore_bindings.dart.

import 'dart:convert';
import 'dart:ffi';
import 'package:ffi/ffi.dart';

import 'native_library.dart';

typedef _EciesC    = Pointer<Utf8> Function(Pointer<Uint8>, Size, Pointer<Utf8>);
typedef _EciesDart = Pointer<Utf8> Function(Pointer<Uint8>, int, Pointer<Utf8>);

class EciesBindings {
  /// ECIES-encrypt [plaintext] to [recipientPubkeyHex] (66-hex compressed
  /// secp256k1). Returns the ciphertext blob as a hex string, or null on
  /// failure (bad pubkey, missing native symbol, etc.).
  static String? encryptToHex(String plaintext, String recipientPubkeyHex) {
    final fn = _lookup('mc_ecies_encrypt');
    if (fn == null) return null;
    final bytes = utf8.encode(plaintext);
    final buf = calloc<Uint8>(bytes.isEmpty ? 1 : bytes.length);
    final pk = recipientPubkeyHex.toNativeUtf8();
    try {
      if (bytes.isNotEmpty) buf.asTypedList(bytes.length).setAll(0, bytes);
      final r = fn(buf, bytes.length, pk);
      if (r.address == 0) return null;
      final s = r.toDartString();
      NativeLibrary.bindings.mc_free(r.cast());
      return s;
    } finally {
      calloc.free(buf);
      calloc.free(pk);
    }
  }

  static _EciesDart? _lookup(String name) {
    try {
      final ptr = NativeLibrary.lib.lookup<NativeFunction<_EciesC>>(name);
      return ptr.asFunction<_EciesDart>();
    } catch (_) {
      return null;
    }
  }
}
