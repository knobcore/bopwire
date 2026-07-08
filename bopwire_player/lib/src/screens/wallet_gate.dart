// Routes between the wallet setup screens and the main app surface
// based on what's stored on the device.
//
//   * No wallet on disk → first-launch (create or restore)
//   * Wallet on disk but auto-unlock failed → login (password or
//     re-enter mnemonic)
//   * Wallet unlocked in memory → HomeScreen
//
// We do this at the top of the widget tree (above HomeScreen) so the
// rest of the app can assume there's always a logged-in wallet.

import 'package:flutter/material.dart';

import '../services/wallet_service.dart';
import 'wallet_first_launch_screen.dart';
import 'wallet_login_screen.dart';
import 'wallet_unlock_screen.dart';

enum _GateState { loading, firstLaunch, locked, login, home }

class WalletGate extends StatefulWidget {
  /// The host app's primary surface — shown once the wallet is unlocked.
  final Widget child;

  const WalletGate({super.key, required this.child});

  @override
  State<WalletGate> createState() => _WalletGateState();
}

class _WalletGateState extends State<WalletGate> {
  final _walletService = WalletService();
  _GateState _state = _GateState.loading;

  @override
  void initState() {
    super.initState();
    _decide();
  }

  Future<void> _decide() async {
    // Two outcomes now that the vault is password-locked:
    //   1. No vault file on disk → first-launch (create / restore, sets a
    //      password).
    //   2. A vault exists → locked → show the unlock screen (the user enters
    //      the password; there is no auto-unlock). Unlocking there loads the
    //      wallet and transitions to home.
    //
    // Wrapped so any storage/init failure degrades to first-launch rather than
    // wedging the gate on the loading spinner.
    try {
      final hasVault = await _walletService.hasSavedWallet();
      if (mounted) {
        setState(() =>
            _state = hasVault ? _GateState.locked : _GateState.firstLaunch);
      }
    } catch (e) {
      // ignore: avoid_print
      print('[wallet-gate] _decide failed ($e) — falling back to first-launch');
      if (mounted) setState(() => _state = _GateState.firstLaunch);
    }
  }

  void _onLoggedIn() {
    // ignore: avoid_print
    print('[wallet-gate] _onLoggedIn called, mounted=$mounted, '
          'current=$_state');
    if (mounted) {
      setState(() => _state = _GateState.home);
      // ignore: avoid_print
      print('[wallet-gate] _state -> home (rebuild scheduled)');
    }
  }

  @override
  Widget build(BuildContext context) {
    // ignore: avoid_print
    print('[wallet-gate] build: _state=$_state');
    return switch (_state) {
      _GateState.loading => const Scaffold(
          body: Center(child: CircularProgressIndicator()),
        ),
      _GateState.firstLaunch => WalletFirstLaunchScreen(
          walletService: _walletService,
          onComplete: _onLoggedIn,
        ),
      _GateState.locked => WalletUnlockScreen(
          walletService: _walletService,
          onUnlocked: _onLoggedIn,
          onUseRecoveryPhrase: () {
            if (mounted) setState(() => _state = _GateState.login);
          },
          onReset: () async {
            await _walletService.clearLocalWallet();
            if (mounted) {
              setState(() => _state = _GateState.loading);
              _decide();
            }
          },
        ),
      _GateState.login => WalletLoginScreen(
          walletService: _walletService,
          onLoggedIn:    _onLoggedIn,
          onResetWallet: () {
            // ignore: avoid_print
            print('[wallet-gate] onResetWallet — re-running _decide');
            // Don't jump straight to firstLaunch: that skips the loading
            // state and trusts that reset fully cleared the wallet from
            // disk. Re-run the decision tree so we actually re-check
            // hasSavedWallet() / tryAutoLoad() against the filesystem.
            if (mounted) {
              setState(() => _state = _GateState.loading);
              _decide();
            }
          },
        ),
      _GateState.home => widget.child,
    };
  }
}
