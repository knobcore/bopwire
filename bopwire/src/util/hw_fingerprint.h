#pragma once
//
// Hardware-derived device fingerprint (#5 structural attestation).
//
// Collects the host's STABLE hardware identifiers and returns the lowercase
// hex of their SHA-256. "Stable" = survives reboots and app reinstalls, so
// the full node's per-device rate limiter (≈2,880 mints/device/day) buckets
// real hardware instead of a resettable random token. This is the desktop
// (Windows / Linux / macOS) source; Android reports its own fingerprint over
// a Kotlin MethodChannel because the NDK can't read these identifiers.
//
// Identifiers folded in, best-effort (any that fail to read are skipped):
//   - primary non-loopback MAC address
//   - OS name + version string
//   - hostname / computer name
//   - Windows: HKLM\SOFTWARE\Microsoft\Cryptography\MachineGuid
//   - Linux:   /etc/machine-id, /sys/class/dmi/id/product_uuid (if readable)
//   - macOS:   IOPlatformUUID
//
// Deliberately NOT the wallet — device_id must be per-machine across wallets
// so a Sybil farmer can't mint a fresh device per wallet. The wallet binding
// happens one layer up (the attestation travels inside a wallet-signed
// bundle / session.start). Returns "" if NOTHING could be read, so the
// caller can fall back to a software-level random id.

#include <string>

namespace mc::util {

// A fingerprint plus its entropy quality. `strong` is true iff at least one
// genuinely per-UNIT hardware id (SMBIOS/IOPlatform UUID, disk/board serial,
// machine-id, non-empty ANDROID_ID) was folded in — i.e. two otherwise-
// identical machines would differ. Material with only weak sources
// (MAC/OS/hostname/CPU-model), or none at all, yields strong=false so the
// caller can route it to the software tier (per-install random) instead of
// minting a confident, collision-prone device_id.
struct DeviceFingerprint {
    std::string hex;            // lowercase hex SHA-256, or "" if nothing readable
    bool        strong = false;
};

// Full result (hex + strong bit).
DeviceFingerprint device_fingerprint_ex();

// Lowercase hex SHA-256 of the concatenated hardware identifiers, or "" if
// none could be read on this platform. (Delegates to device_fingerprint_ex.)
std::string device_fingerprint_hex();

} // namespace mc::util
