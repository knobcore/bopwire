/// Shared wallet-password policy: at least 12 characters, at least one uppercase
/// letter, and at least one special (non-alphanumeric) character. Applied to the
/// vault unlock password and the export passphrase. The node TUIs enforce the
/// same rule.
///
/// Returns null when [pw] satisfies the policy, otherwise a human-readable
/// message describing the first unmet requirement.
String? passwordPolicyError(String pw) {
  if (pw.length < 12) return 'Password must be at least 12 characters.';
  if (!RegExp(r'[A-Z]').hasMatch(pw)) {
    return 'Add at least one uppercase letter.';
  }
  if (!RegExp(r'[^A-Za-z0-9\s]').hasMatch(pw)) {
    return 'Add at least one special character (e.g. ! @ # \$ % & *).';
  }
  return null;
}

/// True when [pw] meets the policy.
bool isPasswordvalid(String pw) => passwordPolicyError(pw) == null;
