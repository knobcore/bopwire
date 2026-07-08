// Launch-time unlock: when an encrypted wallet vault exists on disk, the user
// enters their password to decrypt it (no auto-unlock). Offers recovery-phrase
// restore and a reset for a forgotten password.

import 'package:flutter/material.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../services/wallet_service.dart';

class WalletUnlockScreen extends StatefulWidget {
  final WalletService walletService;
  final VoidCallback onUnlocked;
  final VoidCallback onUseRecoveryPhrase;
  final VoidCallback onReset;

  const WalletUnlockScreen({
    super.key,
    required this.walletService,
    required this.onUnlocked,
    required this.onUseRecoveryPhrase,
    required this.onReset,
  });

  @override
  State<WalletUnlockScreen> createState() => _WalletUnlockScreenState();
}

class _WalletUnlockScreenState extends State<WalletUnlockScreen> {
  final _pw = TextEditingController();
  bool _busy = false;
  bool _obscure = true;
  String? _error;

  @override
  void dispose() {
    _pw.dispose();
    super.dispose();
  }

  Future<void> _unlock() async {
    final pw = _pw.text;
    if (pw.isEmpty) {
      setState(() => _error = 'Enter your password.');
      return;
    }
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final ok = await widget.walletService.unlock(pw);
      if (!ok) {
        if (mounted) setState(() { _busy = false; _error = 'Wrong password.'; });
        return;
      }
      final info = await widget.walletService.tryAutoLoad();
      if (info == null) {
        if (mounted) {
          setState(() {
            _busy = false;
            _error = 'Vault unlocked but empty — restore from your recovery phrase.';
          });
        }
        return;
      }
      try {
        if (mounted) context.read<WalletProvider>().setWallet(info);
      } catch (_) {}
      widget.onUnlocked();
    } catch (e) {
      if (mounted) setState(() { _busy = false; _error = 'Unlock failed: $e'; });
    }
  }

  Future<void> _confirmReset() async {
    final ok = await showDialog<bool>(
      context: context,
      builder: (_) => AlertDialog(
        title: const Text('Reset wallet on this device?'),
        content: const Text(
          'This deletes the encrypted wallet stored here. You can only get back '
          'in with your 12-word recovery phrase or an export file. Continue?',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Reset')),
        ],
      ),
    );
    if (ok == true) widget.onReset();
  }

  @override
  Widget build(BuildContext context) {
    final theme = Theme.of(context);
    return Scaffold(
      body: Center(
        child: ConstrainedBox(
          constraints: const BoxConstraints(maxWidth: 420),
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              mainAxisSize: MainAxisSize.min,
              crossAxisAlignment: CrossAxisAlignment.stretch,
              children: [
                Icon(Icons.lock_outline, size: 48, color: theme.colorScheme.primary),
                const SizedBox(height: 16),
                Text('Unlock your wallet',
                    textAlign: TextAlign.center, style: theme.textTheme.headlineSmall),
                const SizedBox(height: 8),
                Text('Enter your wallet password.',
                    textAlign: TextAlign.center, style: theme.textTheme.bodyMedium),
                const SizedBox(height: 24),
                TextField(
                  controller: _pw,
                  obscureText: _obscure,
                  autofocus: true,
                  enabled: !_busy,
                  decoration: InputDecoration(
                    labelText: 'Password',
                    errorText: _error,
                    border: const OutlineInputBorder(),
                    suffixIcon: IconButton(
                      icon: Icon(_obscure ? Icons.visibility : Icons.visibility_off),
                      onPressed: () => setState(() => _obscure = !_obscure),
                    ),
                  ),
                  onSubmitted: (_) { if (!_busy) _unlock(); },
                ),
                const SizedBox(height: 16),
                FilledButton(
                  onPressed: _busy ? null : _unlock,
                  child: _busy
                      ? const SizedBox(
                          height: 20, width: 20,
                          child: CircularProgressIndicator(strokeWidth: 2))
                      : const Text('Unlock'),
                ),
                const SizedBox(height: 8),
                TextButton(
                  onPressed: _busy ? null : widget.onUseRecoveryPhrase,
                  child: const Text('Forgot password? Restore with recovery phrase'),
                ),
                TextButton(
                  onPressed: _busy ? null : _confirmReset,
                  child: Text('Reset wallet',
                      style: TextStyle(color: theme.colorScheme.error)),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
