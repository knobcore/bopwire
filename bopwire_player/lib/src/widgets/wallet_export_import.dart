import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'package:file_picker/file_picker.dart';
import 'package:flutter/material.dart';

import '../ffi/keystore_bindings.dart';
import '../models/wallet.dart';
import '../services/wallet_service.dart';
import 'password_dialogs.dart';

void _snack(BuildContext c, String m, {bool err = false}) {
  ScaffoldMessenger.of(c).showSnackBar(SnackBar(
    content: Text(m),
    backgroundColor: err ? Theme.of(c).colorScheme.error : null,
  ));
}

/// Export the current wallet's mnemonic as a passphrase-encrypted keystore file
/// (portable, no device binding — safe to store in the cloud). The vault must be
/// unlocked (this is only reachable from inside the app).
Future<void> exportWalletToFile(BuildContext context, WalletService ws) async {
  final mnemonic = await ws.readSavedMnemonic();
  if (mnemonic == null || mnemonic.isEmpty) {
    if (context.mounted) _snack(context, 'No recovery phrase to export.', err: true);
    return;
  }
  if (!context.mounted) return;
  final pass = await showSetPasswordDialog(
    context,
    title: 'Export passphrase',
    subtitle: 'Encrypts the export file. You will need this exact passphrase to '
        'import on another device. The file is safe to keep in the cloud — only '
        'this passphrase can open it.',
    confirmLabel: 'Continue',
  );
  if (pass == null) return;
  final ks = KeystoreBindings.encrypt(mnemonic, pass);
  if (ks == null) {
    if (context.mounted) _snack(context, 'Encryption failed.', err: true);
    return;
  }
  final addr = ws.info?.address ?? '';
  final short = addr.length >= 10 ? addr.substring(2, 10) : 'wallet';
  final bytes = Uint8List.fromList(utf8.encode(ks));
  final path = await FilePicker.platform.saveFile(
    dialogTitle: 'Save wallet export',
    fileName: 'bopwire-wallet-$short.json',
    bytes: bytes,
  );
  if (path == null) return; // cancelled
  try {
    await File(path).writeAsString(ks, flush: true);
  } catch (_) {
    // Mobile sandbox: the `bytes:` path already wrote via the system picker.
  }
  if (context.mounted) {
    _snack(context, 'Wallet exported. Keep the passphrase safe — it is the only key.');
  }
}

/// Import a wallet from a passphrase-encrypted keystore file. Replaces any
/// wallet currently on this device. Returns the new WalletInfo (or null on
/// cancel / failure). The caller is responsible for updating WalletProvider +
/// navigating.
Future<WalletInfo?> importWalletFromFile(BuildContext context, WalletService ws) async {
  final picked = await FilePicker.platform.pickFiles(
    dialogTitle: 'Choose a wallet export file',
    withData: true,
  );
  if (picked == null || picked.files.isEmpty) return null;
  final f = picked.files.first;
  String contents;
  try {
    if (f.bytes != null) {
      contents = utf8.decode(f.bytes!);
    } else if (f.path != null) {
      contents = await File(f.path!).readAsString();
    } else {
      return null;
    }
  } catch (_) {
    if (context.mounted) _snack(context, 'Could not read the file.', err: true);
    return null;
  }
  if (!context.mounted) return null;
  final pass = await showPasswordPrompt(
    context,
    title: 'Import passphrase',
    subtitle: 'The passphrase you set when exporting this file.',
    confirmLabel: 'Decrypt',
  );
  if (pass == null) return null;
  final mnemonic = KeystoreBindings.decrypt(contents, pass);
  if (mnemonic == null || !ws.validateMnemonic(mnemonic)) {
    if (context.mounted) {
      _snack(context, 'Wrong passphrase or not a Bopwire wallet file.', err: true);
    }
    return null;
  }
  if (!context.mounted) return null;
  final vaultPw = await showSetPasswordDialog(
    context,
    title: 'Set a wallet password',
    subtitle: 'A password to lock this wallet on this device (required each launch).',
    confirmLabel: 'Import',
  );
  if (vaultPw == null) return null;
  try {
    await ws.clearLocalWallet(); // drop any existing vault first
    await ws.createVault(vaultPw);
    return await ws.createWalletFromMnemonic(mnemonic: mnemonic);
  } catch (e) {
    if (context.mounted) _snack(context, 'Import failed: $e', err: true);
    return null;
  }
}

/// Change the vault unlock password.
Future<void> changeWalletPassword(BuildContext context, WalletService ws) async {
  final oldPw =
      await showPasswordPrompt(context, title: 'Current password', confirmLabel: 'Next');
  if (oldPw == null) return;
  if (!context.mounted) return;
  final newPw = await showSetPasswordDialog(context, title: 'New password', confirmLabel: 'Change');
  if (newPw == null) return;
  final ok = await ws.changePassword(oldPw, newPw);
  if (context.mounted) {
    _snack(context, ok ? 'Password changed.' : 'Current password is wrong.', err: !ok);
  }
}
