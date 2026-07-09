// Browser listener wallet (Phase 3c — attribution-only).
//
// A persistent standard Ethereum keypair: secp256k1, address =
// keccak256(uncompressed pubkey X||Y)[-20:], which is EXACTLY the node's
// address_from_pubkey scheme (src/crypto/keys.cpp), so the same address is
// valid on chain. It is sent as `playerAddress` on /api/play/start; the gateway
// routes that into `tracking_address` (keeping player_address = 0x000, so the
// web listener still earns nothing) — turning the previous fresh random_addr()
// per play into ONE stable, capped identity, which is what closes the
// uncapped-0x000 seeder/mini farming hole. The private key stays in
// localStorage so the wallet is real + spendable later.
import { getPublicKey, utils } from '@noble/secp256k1';
import { keccak_256 } from '@noble/hashes/sha3';
import { bytesToHex, hexToBytes } from '@noble/hashes/utils';

const SK_KEY = 'bopwire_listener_sk';

function loadOrCreate() {
  let skHex = localStorage.getItem(SK_KEY);
  if (!skHex || !/^[0-9a-f]{64}$/i.test(skHex)) {
    skHex = bytesToHex(utils.randomPrivateKey());
    localStorage.setItem(SK_KEY, skHex);
  }
  const pub  = getPublicKey(hexToBytes(skHex), false); // 65B uncompressed: 04||X||Y
  const addr = keccak_256(pub.slice(1)).slice(-20);    // last 20 bytes
  return { privateKeyHex: skHex, address: '0x' + bytesToHex(addr) };
}

// Singleton — created (or restored) on first import.
export const listenerWallet = loadOrCreate();
