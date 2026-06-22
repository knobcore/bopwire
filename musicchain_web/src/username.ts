// Locally-cached username for the player's UI pre-fill.
//
// The canonical record of `address -> username` lives on chain
// (see UsernameRegister tx in the home node). This module is just
// a UI convenience so the login screen can pre-fill the handle the
// user typed at wallet creation — the Dart player does the same
// (see wallet_service.dart `_walletUsernameKey`).
//
// We deliberately keep this in plain localStorage rather than the
// encrypted envelope: the username is public (it's literally on
// chain) and the login screen needs to read it BEFORE the user
// has typed the wallet password.

/** localStorage key. Namespaced to avoid colliding with any other
 *  "username" key a future app might write. */
const USERNAME_KEY = 'mc-wallet:username';

/** Max length we'll accept / round-trip. Matches the on-chain
 *  validator in handlers/username.go — 32 bytes UTF-8. Anything
 *  longer is silently rejected at set time so a tampered localStorage
 *  entry can't crash the UI. */
const USERNAME_MAX_BYTES = 32;

/**
 * Returns the cached username, or `null` if none was ever set or
 * localStorage is unavailable. Null (not empty-string) so callers
 * can distinguish "first run" from "user cleared their handle".
 */
export function getCachedUsername(): string | null {
  try {
    const ls = globalThis.localStorage;
    if (!ls) return null;
    const raw = ls.getItem(USERNAME_KEY);
    if (raw === null || raw === '') return null;
    // Length guard against a tampered entry.
    if (byteLength(raw) > USERNAME_MAX_BYTES) return null;
    return raw;
  } catch {
    // SecurityError in cookie-blocked iframes, QuotaExceededError, etc.
    return null;
  }
}

/**
 * Persist the user's chosen handle for UI pre-fill on next launch.
 * Silently no-ops when localStorage is unavailable — the on-chain
 * record is canonical, this is best-effort caching.
 *
 * Passing an empty string or a value longer than 32 UTF-8 bytes
 * clears the cache instead of writing, to keep the stored value
 * always renderable.
 */
export function setCachedUsername(name: string): void {
  try {
    const ls = globalThis.localStorage;
    if (!ls) return;
    const trimmed = name.trim();
    if (trimmed === '' || byteLength(trimmed) > USERNAME_MAX_BYTES) {
      ls.removeItem(USERNAME_KEY);
      return;
    }
    ls.setItem(USERNAME_KEY, trimmed);
  } catch {
    // Same swallow as above — caching is best-effort.
  }
}

/**
 * Drop the cached handle. Called from the "sign out" flow alongside
 * `WalletStorage.clear()` so the next first-launch screen starts
 * with a blank username field.
 */
export function clearCachedUsername(): void {
  try {
    const ls = globalThis.localStorage;
    if (!ls) return;
    ls.removeItem(USERNAME_KEY);
  } catch {
    // Same swallow as above.
  }
}

/** UTF-8 byte length, since `string.length` counts UTF-16 code units. */
function byteLength(s: string): number {
  return new TextEncoder().encode(s).byteLength;
}
