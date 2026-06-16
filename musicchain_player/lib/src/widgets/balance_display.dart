import 'package:flutter/material.dart';

class BalanceDisplay extends StatelessWidget {
  final String balance;
  final Future<void> Function() onRefresh;

  const BalanceDisplay({super.key, required this.balance, required this.onRefresh});

  @override
  Widget build(BuildContext context) {
    return Card(
      child: Padding(
        padding: const EdgeInsets.all(16),
        child: Row(
          children: [
            const Icon(Icons.token, size: 32),
            const SizedBox(width: 12),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  const Text('Balance', style: TextStyle(fontWeight: FontWeight.bold)),
                  Text(balance,
                      style: const TextStyle(fontSize: 24, fontFamily: 'monospace')),
                ],
              ),
            ),
            IconButton(
              icon: const Icon(Icons.refresh),
              onPressed: onRefresh,
            ),
          ],
        ),
      ),
    );
  }
}
