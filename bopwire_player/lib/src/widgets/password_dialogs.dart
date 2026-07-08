import 'package:flutter/material.dart';

import '../util/password_policy.dart';

/// Prompt the user to SET a new password — two fields (password + confirm),
/// enforcing the shared policy (>=12 chars, uppercase, special). Returns the
/// chosen password, or null if cancelled. Used for the vault password
/// (first-launch / import) and the export passphrase.
Future<String?> showSetPasswordDialog(
  BuildContext context, {
  String title = 'Set a password',
  String? subtitle,
  String confirmLabel = 'Set',
}) {
  return showDialog<String>(
    context: context,
    barrierDismissible: false,
    builder: (_) =>
        _SetPasswordDialog(title: title, subtitle: subtitle, confirmLabel: confirmLabel),
  );
}

/// Prompt for an EXISTING password — a single field, no policy check. Returns
/// the entered value, or null if cancelled. Used for import-file passphrase and
/// the "current password" of a change-password flow.
Future<String?> showPasswordPrompt(
  BuildContext context, {
  String title = 'Enter password',
  String? subtitle,
  String confirmLabel = 'OK',
}) {
  return showDialog<String>(
    context: context,
    builder: (_) =>
        _PromptPasswordDialog(title: title, subtitle: subtitle, confirmLabel: confirmLabel),
  );
}

class _SetPasswordDialog extends StatefulWidget {
  const _SetPasswordDialog(
      {required this.title, this.subtitle, required this.confirmLabel});
  final String title;
  final String? subtitle;
  final String confirmLabel;

  @override
  State<_SetPasswordDialog> createState() => _SetPasswordDialogState();
}

class _SetPasswordDialogState extends State<_SetPasswordDialog> {
  final _pw = TextEditingController();
  final _confirm = TextEditingController();
  bool _obscure = true;
  String? _error;

  @override
  void dispose() {
    _pw.dispose();
    _confirm.dispose();
    super.dispose();
  }

  void _submit() {
    final pw = _pw.text;
    final policy = passwordPolicyError(pw);
    if (policy != null) {
      setState(() => _error = policy);
      return;
    }
    if (pw != _confirm.text) {
      setState(() => _error = 'Passwords do not match.');
      return;
    }
    Navigator.of(context).pop(pw);
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(widget.title),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (widget.subtitle != null) ...[
            Text(widget.subtitle!, style: Theme.of(context).textTheme.bodySmall),
            const SizedBox(height: 12),
          ],
          TextField(
            controller: _pw,
            obscureText: _obscure,
            autofocus: true,
            decoration: InputDecoration(
              labelText: 'Password',
              suffixIcon: IconButton(
                icon: Icon(_obscure ? Icons.visibility : Icons.visibility_off),
                onPressed: () => setState(() => _obscure = !_obscure),
              ),
            ),
            onChanged: (_) { if (_error != null) setState(() => _error = null); },
          ),
          const SizedBox(height: 10),
          TextField(
            controller: _confirm,
            obscureText: _obscure,
            decoration: const InputDecoration(labelText: 'Confirm password'),
            onSubmitted: (_) => _submit(),
          ),
          const SizedBox(height: 8),
          Text(
            _error ?? 'At least 12 characters, one uppercase letter, one special character.',
            style: TextStyle(
              fontSize: 12,
              color: _error != null
                  ? Theme.of(context).colorScheme.error
                  : Theme.of(context).textTheme.bodySmall?.color,
            ),
          ),
        ],
      ),
      actions: [
        TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Cancel')),
        FilledButton(onPressed: _submit, child: Text(widget.confirmLabel)),
      ],
    );
  }
}

class _PromptPasswordDialog extends StatefulWidget {
  const _PromptPasswordDialog(
      {required this.title, this.subtitle, required this.confirmLabel});
  final String title;
  final String? subtitle;
  final String confirmLabel;

  @override
  State<_PromptPasswordDialog> createState() => _PromptPasswordDialogState();
}

class _PromptPasswordDialogState extends State<_PromptPasswordDialog> {
  final _pw = TextEditingController();
  bool _obscure = true;

  @override
  void dispose() {
    _pw.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return AlertDialog(
      title: Text(widget.title),
      content: Column(
        mainAxisSize: MainAxisSize.min,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (widget.subtitle != null) ...[
            Text(widget.subtitle!, style: Theme.of(context).textTheme.bodySmall),
            const SizedBox(height: 12),
          ],
          TextField(
            controller: _pw,
            obscureText: _obscure,
            autofocus: true,
            decoration: InputDecoration(
              labelText: 'Password',
              suffixIcon: IconButton(
                icon: Icon(_obscure ? Icons.visibility : Icons.visibility_off),
                onPressed: () => setState(() => _obscure = !_obscure),
              ),
            ),
            onSubmitted: (v) => Navigator.of(context).pop(v),
          ),
        ],
      ),
      actions: [
        TextButton(onPressed: () => Navigator.of(context).pop(), child: const Text('Cancel')),
        FilledButton(
          onPressed: () => Navigator.of(context).pop(_pw.text),
          child: Text(widget.confirmLabel),
        ),
      ],
    );
  }
}
