import 'package:flutter/material.dart';
import 'package:flutter/services.dart';
import 'package:provider/provider.dart';

import '../providers/wallet_provider.dart';
import '../widgets/balance_display.dart';

class WalletScreen extends StatelessWidget {
  const WalletScreen({super.key});

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(title: const Text('Wallet')),
      body: Consumer<WalletProvider>(
        builder: (context, wallet, _) {
          if (!wallet.hasWallet) {
            return _NoWalletView(wallet: wallet);
          }
          return _WalletView(wallet: wallet);
        },
      ),
    );
  }
}

class _NoWalletView extends StatefulWidget {
  final WalletProvider wallet;
  const _NoWalletView({required this.wallet});

  @override
  State<_NoWalletView> createState() => _NoWalletViewState();
}

class _NoWalletViewState extends State<_NoWalletView> {
  final _passCtrl = TextEditingController();

  @override
  Widget build(BuildContext context) {
    return Padding(
      padding: const EdgeInsets.all(24),
      child: Column(
        mainAxisAlignment: MainAxisAlignment.center,
        children: [
          const Icon(Icons.account_balance_wallet_outlined, size: 80),
          const SizedBox(height: 16),
          const Text('No wallet found'),
          const SizedBox(height: 24),
          TextField(
            controller: _passCtrl,
            obscureText: true,
            decoration: const InputDecoration(
              labelText: 'Password',
              border: OutlineInputBorder(),
            ),
          ),
          const SizedBox(height: 16),
          ElevatedButton(
            onPressed: () => widget.wallet.createWallet(_passCtrl.text),
            child: const Text('Create New Wallet'),
          ),
          const SizedBox(height: 8),
          OutlinedButton(
            onPressed: () => widget.wallet.tryLoadWallet(_passCtrl.text),
            child: const Text('Load Existing Wallet'),
          ),
          if (widget.wallet.error != null) ...[
            const SizedBox(height: 8),
            Text(widget.wallet.error!, style: const TextStyle(color: Colors.red)),
          ],
        ],
      ),
    );
  }
}

class _WalletView extends StatelessWidget {
  final WalletProvider wallet;
  const _WalletView({required this.wallet});

  @override
  Widget build(BuildContext context) {
    final info = wallet.info!;
    return ListView(
      padding: const EdgeInsets.all(16),
      children: [
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                const Text('Address', style: TextStyle(fontWeight: FontWeight.bold)),
                const SizedBox(height: 4),
                Row(
                  children: [
                    Expanded(
                      child: Text(info.address,
                          style: const TextStyle(fontFamily: 'monospace', fontSize: 12)),
                    ),
                    IconButton(
                      icon: const Icon(Icons.copy, size: 18),
                      onPressed: () => Clipboard.setData(ClipboardData(text: info.address)),
                    ),
                  ],
                ),
              ],
            ),
          ),
        ),
        const SizedBox(height: 8),
        BalanceDisplay(balance: info.balance, onRefresh: wallet.refreshBalance),
        const SizedBox(height: 16),
        const Divider(),
        ListTile(
          leading: const Icon(Icons.send),
          title: const Text('Send Tokens'),
          onTap: () => _showSendDialog(context),
        ),
        ListTile(
          leading: const Icon(Icons.delete_forever),
          title: const Text('Remove Wallet'),
          onTap: () {
            wallet.freeWallet();
          },
        ),
      ],
    );
  }

  void _showSendDialog(BuildContext context) {
    final toCtrl     = TextEditingController();
    final amountCtrl = TextEditingController();
    showDialog(
      context: context,
      builder: (dialogCtx) => AlertDialog(
        title: const Text('Send Tokens'),
        content: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            TextField(
              controller: toCtrl,
              decoration: const InputDecoration(labelText: 'To Address'),
            ),
            TextField(
              controller: amountCtrl,
              decoration: const InputDecoration(labelText: 'Amount'),
              keyboardType: const TextInputType.numberWithOptions(decimal: true),
            ),
          ],
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.pop(dialogCtx),
            child: const Text('Cancel'),
          ),
          ElevatedButton(
            onPressed: () async {
              Navigator.pop(dialogCtx);
              final err = await context.read<WalletProvider>().sendTokens(
                toCtrl.text.trim(),
                amountCtrl.text.trim(),
              );
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(SnackBar(
                content: Text(
                  err != null ? 'Transfer failed: $err' : 'Transfer sent!',
                ),
              ));
            },
            child: const Text('Send'),
          ),
        ],
      ),
    );
  }
}
