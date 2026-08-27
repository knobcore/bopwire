// Settings block for the foreign networks (Soulseek, napstr).
//
// Renders generically from each ExternalNetwork's `credentialFields`, so
// adding a network never means touching this file. Secret fields land in
// the wallet vault via NetworkCredentials, which means they can only be
// saved while the wallet is unlocked — the form says so rather than
// failing silently when it isn't.

import 'package:flutter/material.dart';

import '../services/networks/external_network.dart';
import '../services/networks/network_credentials.dart';
import '../services/networks/network_registry.dart';

class NetworkSettingsSection extends StatefulWidget {
  const NetworkSettingsSection({super.key});

  @override
  State<NetworkSettingsSection> createState() => _NetworkSettingsSectionState();
}

class _NetworkSettingsSectionState extends State<NetworkSettingsSection> {
  /// networkId -> fieldKey -> controller
  final Map<String, Map<String, TextEditingController>> _controllers = {};
  final Set<String> _saving = {};

  @override
  void initState() {
    super.initState();
    _hydrate();
    NetworkRegistry.instance.addListener(_onRegistryChanged);
  }

  @override
  void dispose() {
    NetworkRegistry.instance.removeListener(_onRegistryChanged);
    for (final byField in _controllers.values) {
      for (final c in byField.values) {
        c.dispose();
      }
    }
    super.dispose();
  }

  void _onRegistryChanged() {
    if (mounted) setState(() {});
  }

  void _hydrate() {
    for (final net in NetworkRegistry.instance.networks) {
      final byField = _controllers.putIfAbsent(net.id, () => {});
      for (final f in net.credentialFields) {
        final existing = NetworkCredentials.instance.get(net.id, f.key) ?? '';
        byField
            .putIfAbsent(f.key, () => TextEditingController())
            .text = existing;
      }
    }
  }

  Future<void> _save(ExternalNetwork net) async {
    setState(() => _saving.add(net.id));
    final messenger = ScaffoldMessenger.maybeOf(context);
    try {
      for (final f in net.credentialFields) {
        final ctrl = _controllers[net.id]?[f.key];
        if (ctrl == null) continue;
        await NetworkCredentials.instance.set(net.id, f, ctrl.text.trim());
      }
      await NetworkRegistry.instance.credentialsChanged(net.id);
      messenger?.showSnackBar(SnackBar(
        content: Text('${net.displayName} settings saved.'),
        duration: const Duration(seconds: 2),
      ));
    } catch (e) {
      // The usual cause is a locked wallet vault: secret fields cannot be
      // written until the user unlocks it. Say that instead of "failed".
      messenger?.showSnackBar(SnackBar(
        content: Text(
          'Could not save ${net.displayName} settings. If this network '
          'needs a password, unlock your wallet first, then try again. '
          '($e)',
        ),
        duration: const Duration(seconds: 6),
      ));
    } finally {
      if (mounted) setState(() => _saving.remove(net.id));
    }
  }

  Widget _statusChip(ExternalNetwork net) {
    final (String label, Color color) = switch (net.status) {
      NetworkStatus.connected    => ('Connected',    Colors.green),
      NetworkStatus.connecting   => ('Connecting…',  Colors.amber),
      NetworkStatus.error        => ('Error',        Colors.redAccent),
      NetworkStatus.disconnected => ('Offline',      Colors.grey),
    };
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 2),
      decoration: BoxDecoration(
        color: color.withValues(alpha: 0.15),
        borderRadius: BorderRadius.circular(10),
      ),
      child: Text(label, style: TextStyle(fontSize: 11, color: color)),
    );
  }

  @override
  Widget build(BuildContext context) {
    final networks = NetworkRegistry.instance.networks;
    if (networks.isEmpty) return const SizedBox.shrink();

    _hydrate();

    return Column(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        const SizedBox(height: 24),
        const Text('Other networks',
            style: TextStyle(fontWeight: FontWeight.bold)),
        const SizedBox(height: 8),
        Text(
          'Search Soulseek and napstr alongside bopwire. Anything you '
          'download is fingerprinted and tag-imported automatically, then '
          'registered on the chain like any other track in your library.',
          style: TextStyle(fontSize: 12, color: Colors.grey.shade400),
        ),
        for (final net in networks) ...[
          const SizedBox(height: 16),
          Card(
            child: Padding(
              padding: const EdgeInsets.all(12),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Expanded(
                        child: Text(net.displayName,
                            style: const TextStyle(
                                fontWeight: FontWeight.w700, fontSize: 15)),
                      ),
                      _statusChip(net),
                      const SizedBox(width: 8),
                      Switch(
                        value: net.enabled,
                        onChanged: (v) =>
                            NetworkRegistry.instance.setEnabled(net.id, v),
                      ),
                    ],
                  ),
                  if (net.enabled && !net.isConfigured)
                    Padding(
                      padding: const EdgeInsets.only(top: 4),
                      child: Text(
                        'Enabled, but not searchable yet — fill in the '
                        'fields below and save.',
                        style: TextStyle(
                            fontSize: 11, color: Colors.amber.shade300),
                      ),
                    ),
                  for (final f in net.credentialFields) ...[
                    const SizedBox(height: 10),
                    TextField(
                      controller: _controllers[net.id]?[f.key],
                      obscureText: f.secret,
                      decoration: InputDecoration(
                        labelText:
                            f.required_ ? f.label : '${f.label} (optional)',
                        hintText: f.hint,
                        isDense: true,
                        border: const OutlineInputBorder(),
                      ),
                    ),
                  ],
                  if (net.credentialFields.isNotEmpty) ...[
                    const SizedBox(height: 10),
                    Row(
                      mainAxisAlignment: MainAxisAlignment.end,
                      children: [
                        TextButton(
                          onPressed: _saving.contains(net.id)
                              ? null
                              : () async {
                                  await NetworkCredentials.instance
                                      .clear(net.id, net.credentialFields);
                                  for (final c
                                      in _controllers[net.id]?.values ??
                                          <TextEditingController>[]) {
                                    c.clear();
                                  }
                                  await NetworkRegistry.instance
                                      .credentialsChanged(net.id);
                                },
                          child: const Text('Clear'),
                        ),
                        const SizedBox(width: 8),
                        FilledButton(
                          onPressed: _saving.contains(net.id)
                              ? null
                              : () => _save(net),
                          child: _saving.contains(net.id)
                              ? const SizedBox(
                                  width: 14,
                                  height: 14,
                                  child: CircularProgressIndicator(
                                      strokeWidth: 2))
                              : const Text('Save'),
                        ),
                      ],
                    ),
                  ],
                ],
              ),
            ),
          ),
        ],
      ],
    );
  }
}
