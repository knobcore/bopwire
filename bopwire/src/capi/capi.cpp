/**
 * capi.cpp - Implementation of the public C API (bopwire.h).
 * Bridges C-style API to the C++ internals for Flutter FFI use.
 */
#include "../../include/bopwire.h"
#include "../crypto/hash.h"
#include "../crypto/keys.h"
#include "../crypto/keystore.h"
#include "../crypto/ecies.h"
#include "../crypto/signature.h"
#include "../crypto/bip39.h"
#include "../audio/ogg_validator.h"
#include "../audio/multi_decoder.h"
#include "../audio/ogg_decoder.h"
#include "../audio/fingerprint.h"
#include "../core/block.h"
#include "../util/hw_fingerprint.h"

// librats NAT traversal backends for mc_portmap_* (UPnP IGD + NAT-PMP).
// Headers are pulled in by explicit relative path so they can never be
// shadowed by src/network/upnp.h; the symbols come from the `rats` shared
// library that libbopwire already links (MC_LINK_LIBS).
#include "../../deps/librats/src/upnp.h"
#include "../../deps/librats/src/natpmp.h"
#include "../../deps/librats/src/network_utils.h"

#include <algorithm>
#include <condition_variable>
#include <cstring>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <memory>
#include <utility>
#include <vector>

// Thread-local last error string
static thread_local std::string g_last_error;

static void set_error(const std::string& msg) { g_last_error = msg; }

static char* make_cstring(const std::string& s) {
    char* p = static_cast<char*>(std::malloc(s.size() + 1));
    if (p) { std::memcpy(p, s.data(), s.size()); p[s.size()] = '\0'; }
    return p;
}

// ---- Init / cleanup -------------------------------------------------

int mc_init(void) {
    // Nothing required currently
    return 0;
}

void mc_cleanup(void) {}

void mc_free(void* ptr) { std::free(ptr); }

const char* mc_last_error(void) {
    return g_last_error.empty() ? nullptr : g_last_error.c_str();
}

// ---- Wallet ---------------------------------------------------------

struct WalletHandle {
    mc::crypto::KeyPair kp;
};

// ---- BIP39 mnemonic wallet flow -------------------------------------
//
// NB: the function definitions here drop the `extern "C" BOPWIRE_API`
// decoration. The header (`include/bopwire.h`) carries it via the
// `BOPWIRE_API` macro which expands to dllexport when BOPWIRE_BUILD
// is defined and dllimport otherwise. Re-decorating the definition
// triggers C2491 ("definition of dllimport function not allowed") in
// MSVC because we're building the static library here, not the export
// side.

char* mc_bip39_generate_12(void) {
    auto s = mc::crypto::bip39_generate_12();
    if (s.empty()) {
        set_error("bip39_generate failed");
        return nullptr;
    }
    return make_cstring(s);
}

int mc_bip39_validate(const char* mnemonic) {
    if (!mnemonic) return 0;
    return mc::crypto::bip39_validate(mnemonic) ? 1 : 0;
}

mc_wallet_t mc_wallet_from_mnemonic(
    const char* mnemonic, const char* passphrase) {
    if (!mnemonic) { set_error("mnemonic is null"); return nullptr; }
    try {
        auto kp = mc::crypto::bip39_mnemonic_to_keypair(
            mnemonic, passphrase ? passphrase : "");
        if (!kp) { set_error("mnemonic failed validation"); return nullptr; }
        return new WalletHandle{*kp};
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void mc_wallet_free(mc_wallet_t wallet) {
    delete static_cast<WalletHandle*>(wallet);
}

char* mc_wallet_get_address(mc_wallet_t wallet) {
    auto* w = static_cast<WalletHandle*>(wallet);
    if (!w) { set_error("null wallet"); return nullptr; }
    return make_cstring(mc::crypto::to_checksum_hex(w->kp.address));
}

char* mc_wallet_get_eth_address(mc_wallet_t wallet) {
    return mc_wallet_get_address(wallet);
}

char* mc_wallet_get_public_key(mc_wallet_t wallet) {
    auto* w = static_cast<WalletHandle*>(wallet);
    return make_cstring(mc::crypto::to_hex(w->kp.public_key.data(), 33));
}

char* mc_wallet_sign(mc_wallet_t wallet, const uint8_t* data, size_t len) {
    try {
        auto* w = static_cast<WalletHandle*>(wallet);
        auto sig = mc::crypto::sign_data(data, len, w->kp.private_key);
        return make_cstring(mc::crypto::to_hex(sig.data(), 64));
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

// ---- Keystore (password-encrypted wallet: local storage + export) ---

char* mc_keystore_encrypt(const char* plaintext, const char* password) {
    if (!plaintext || !password) { set_error("keystore: null argument"); return nullptr; }
    try {
        std::string js = mc::crypto::keystore_encrypt(plaintext, password);
        if (js.empty()) { set_error("keystore encrypt failed"); return nullptr; }
        return make_cstring(js);
    } catch (const std::exception& e) { set_error(e.what()); return nullptr; }
}

char* mc_keystore_decrypt(const char* keystore_json, const char* password) {
    if (!keystore_json || !password) { set_error("keystore: null argument"); return nullptr; }
    try {
        std::string out;
        if (!mc::crypto::keystore_decrypt(keystore_json, password, out)) {
            set_error("keystore decrypt failed (wrong password or corrupt)");
            return nullptr;
        }
        return make_cstring(out);
    } catch (const std::exception& e) { set_error(e.what()); return nullptr; }
}

// ---- Device fingerprint (#5 structural attestation) -----------------

char* mc_device_fingerprint(void) {
    try {
        const std::string fp = mc::util::device_fingerprint_hex();
        if (fp.empty()) {
            set_error("no hardware identifier readable");
            return nullptr;
        }
        return make_cstring(fp);
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

// Entropy tier of the hardware fingerprint: 2 = strong (a genuinely per-unit
// hardware id was present), 1 = weak (only MAC/OS/host-class sources), 0 =
// nothing readable. Lets the client set an honest attestation `level` instead
// of guessing from the fingerprint string length.
int mc_device_fingerprint_level(void) {
    try {
        const auto d = mc::util::device_fingerprint_ex();
        if (d.hex.empty()) return 0;
        return d.strong ? 2 : 1;
    } catch (...) {
        return 0;
    }
}

// ---- Audio decoding -------------------------------------------------

mc_decoder_t mc_decoder_open(const uint8_t* data, size_t len) {
    try {
        auto dec = mc::audio::OggDecoder::open(data, len);
        return dec.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

int mc_decode_any(const uint8_t* data, size_t len,
                  int16_t** out_pcm, size_t* out_samples,
                  int* out_sample_rate, int* out_channels) {
    if (out_pcm)         *out_pcm         = nullptr;
    if (out_samples)     *out_samples     = 0;
    if (out_sample_rate) *out_sample_rate = 0;
    if (out_channels)    *out_channels    = 0;
    if (!data || len == 0 || !out_pcm || !out_samples) {
        set_error("mc_decode_any: bad arguments");
        return 1;
    }
    try {
        auto pcm = mc::audio::decode_any(data, len);
        if (pcm.samples.empty() || pcm.sample_rate <= 0 || pcm.channels <= 0) {
            set_error("decode_any produced no audio (unsupported codec or "
                      "corrupt file)");
            return 2;
        }
        // Hand ownership to the caller: the std::vector dies with this
        // frame, so copy into a malloc'd block the FFI side can free.
        const size_t n = pcm.samples.size();
        auto* buf = static_cast<int16_t*>(std::malloc(n * sizeof(int16_t)));
        if (!buf) { set_error("mc_decode_any: out of memory"); return 3; }
        std::memcpy(buf, pcm.samples.data(), n * sizeof(int16_t));
        *out_pcm     = buf;
        *out_samples = n;
        if (out_sample_rate) *out_sample_rate = pcm.sample_rate;
        if (out_channels)    *out_channels    = pcm.channels;
        return 0;
    } catch (const std::exception& e) {
        set_error(e.what());
        return 4;
    }
}

void mc_pcm_free(int16_t* pcm) {
    std::free(pcm);
}

void mc_decoder_free(mc_decoder_t decoder) {
    delete static_cast<mc::audio::OggDecoder*>(decoder);
}

int mc_decoder_get_sample_rate(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->sample_rate();
}

int mc_decoder_get_channels(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->channels();
}

uint32_t mc_decoder_get_duration_ms(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->duration_ms();
}

int mc_decoder_read(mc_decoder_t decoder, int16_t* buf, int max_samples) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->read(buf, max_samples);
}

int mc_decoder_seek(mc_decoder_t decoder, uint32_t position_ms) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->seek(position_ms) ? 0 : -1;
}

uint32_t mc_decoder_position_ms(mc_decoder_t decoder) {
    return static_cast<mc::audio::OggDecoder*>(decoder)->position_ms();
}

// ---- Checksum -------------------------------------------------------

uint32_t mc_compute_checksum(const int16_t* samples, int count) {
    uint64_t acc = 0;
    for (int i = 0; i < count; ++i) {
        int16_t s = samples[i];
        acc += static_cast<uint64_t>(s < 0 ? -s : s);
    }
    return static_cast<uint32_t>(acc & 0xFFFFFFFFULL);
}

// ---- Fingerprinting -------------------------------------------------

mc_fingerprint_t mc_fingerprint_generate(const uint8_t* data, size_t len) {
    try {
        auto fp = mc::audio::Fingerprint::from_ogg(data, len);
        return fp.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

mc_fingerprint_t mc_fingerprint_from_compressed(const char* base64) {
    try {
        auto fp = mc::audio::Fingerprint::from_compressed(base64);
        if (!fp) { set_error("decompress failed"); return nullptr; }
        return fp.release();
    } catch (const std::exception& e) {
        set_error(e.what());
        return nullptr;
    }
}

void mc_fingerprint_free(mc_fingerprint_t fp) {
    delete static_cast<mc::audio::Fingerprint*>(fp);
}

char* mc_fingerprint_get_compressed(mc_fingerprint_t fp) {
    auto compressed = static_cast<mc::audio::Fingerprint*>(fp)->compressed();
    return make_cstring(compressed);
}

float mc_fingerprint_compare(mc_fingerprint_t fp_a, mc_fingerprint_t fp_b) {
    auto* a = static_cast<mc::audio::Fingerprint*>(fp_a);
    auto* b = static_cast<mc::audio::Fingerprint*>(fp_b);
    return a->similarity(*b);
}

// ---- Block parsing --------------------------------------------------

int64_t mc_block_find_separator(const uint8_t* data, size_t len) {
    if (len < mc::SEPARATOR_LENGTH) return -1;
    for (size_t i = 0; i <= len - mc::SEPARATOR_LENGTH; ++i) {
        bool found = true;
        for (size_t j = 0; j < mc::SEPARATOR_LENGTH; ++j) {
            if (data[i + j] != mc::SEPARATOR_BYTE) { found = false; break; }
        }
        if (found) return static_cast<int64_t>(i);
    }
    return -1;
}

int mc_block_extract_audio(const uint8_t* /*block_data*/, size_t /*block_len*/,
                            uint8_t** /*ogg_out*/, size_t* /*ogg_len_out*/) {
    // Block v2 no longer carries audio bytes; they live in the home
    // node's content-addressed audio store. Callers must fetch by
    // content_hash via verb_song_audio / stream.open instead. The export
    // is retained so the existing C API does not break ABI; it just
    // returns "unsupported" until something rewires it.
    set_error("mc_block_extract_audio: removed in block format v2 — "
              "fetch by content_hash from the audio store");
    return -1;
}

// ---- Utilities ------------------------------------------------------

void mc_sha256(const uint8_t* data, size_t len, uint8_t* out_hash) {
    auto h = mc::crypto::sha256(data, len);
    std::memcpy(out_hash, h.data(), 32);
}

char* mc_bytes_to_hex(const uint8_t* data, size_t len) {
    return make_cstring(mc::crypto::to_hex(data, len));
}

// ECIES-encrypt `plaintext` to a single recipient pubkey (66-hex compressed
// secp256k1). Used by the DMCA/KYC forms to seal a submission to the shared
// moderation key END-TO-END so the node never sees plaintext. Returns the
// ciphertext blob as a hex string (free with mc_free), nullptr on failure.
char* mc_ecies_encrypt(const uint8_t* plaintext, size_t len,
                       const char* recipient_pubkey_hex) {
    if (!plaintext || !recipient_pubkey_hex) {
        set_error("ecies: null argument"); return nullptr;
    }
    try {
        auto pk = mc::crypto::from_hex(recipient_pubkey_hex);
        if (pk.size() != 33) {
            set_error("ecies: recipient pubkey must be 33 bytes"); return nullptr;
        }
        mc::PubKey33 pub{};
        std::copy(pk.begin(), pk.end(), pub.begin());
        const mc::Address addr = mc::crypto::address_from_pubkey(pub);
        std::vector<uint8_t> pt(plaintext, plaintext + len);
        std::vector<std::pair<mc::Address, mc::PubKey33>> recips{{addr, pub}};
        auto blob = mc::crypto::ecies_encrypt(pt, recips);
        if (blob.empty()) { set_error("ecies encrypt failed"); return nullptr; }
        return make_cstring(mc::crypto::to_hex(blob.data(), blob.size()));
    } catch (const std::exception& e) { set_error(e.what()); return nullptr; }
}

int mc_hex_to_bytes(const char* hex, uint8_t** out) {
    auto bytes = mc::crypto::from_hex(hex);
    if (bytes.empty()) return -1;
    *out = static_cast<uint8_t*>(std::malloc(bytes.size()));
    if (*out) std::memcpy(*out, bytes.data(), bytes.size());
    return static_cast<int>(bytes.size());
}

// ---- Audio format detection and validation --------------------------

int mc_detect_format(const uint8_t* data, size_t len) {
    if (mc::audio::is_ogg_magic(data, len)) return MC_FORMAT_OGG;
    if (mc::audio::is_mp3_magic(data, len)) return MC_FORMAT_MP3;
    return MC_FORMAT_UNKNOWN;
}

int mc_validate_audio(const uint8_t* data, size_t len, char** error_out) {
    auto result = mc::audio::validate_audio(data, len);
    if (!result.valid) {
        if (error_out) *error_out = make_cstring(result.error);
        return 0;
    }
    return 1;
}

uint32_t mc_audio_duration_ms(const uint8_t* data, size_t len) {
    auto result = mc::audio::validate_audio(data, len);
    return result.valid ? result.info.duration_ms : 0;
}

// ---- NAT port mapping (UPnP IGD / NAT-PMP) --------------------------
//
// Thin synchronous facade over librats' UpnpClient + NatPmpClient. Both
// backends run in parallel; the first confirmed TCP mapping with a public
// (or unreported) external IP wins. The backends stay alive after
// mc_portmap_open_tcp returns so they keep the lease refreshed; they are
// only torn down (which also sends the best-effort delete to the gateway)
// by mc_portmap_close_tcp.

namespace {

struct PortMapSession {
    std::unique_ptr<librats::UpnpClient>   upnp;
    std::unique_ptr<librats::NatPmpClient> natpmp;

    std::mutex              m;
    std::condition_variable cv;
    bool        have_result   = false;
    uint16_t    external_port = 0;
    std::string external_ip;
    std::string last_error;   // most recent failure reason from a backend
    bool        saw_double_nat = false;
};

// Heap-allocated and deliberately never freed: destroying UpnpClient /
// NatPmpClient joins their worker threads, and doing that from a static
// destructor during process exit can deadlock. The player releases
// mappings explicitly via mc_portmap_close_tcp.
std::mutex g_portmap_mutex;
auto* g_portmap_sessions =
    new std::map<uint16_t, std::shared_ptr<PortMapSession>>();

} // namespace

uint16_t mc_portmap_open_tcp(uint16_t port, int timeout_ms,
                             char* out_ip, size_t out_ip_len) {
    if (out_ip && out_ip_len) out_ip[0] = '\0';
    if (port == 0) {
        set_error("mc_portmap_open_tcp: port must be non-zero");
        return 0;
    }
    if (timeout_ms <= 0) timeout_ms = 8000;

    std::shared_ptr<PortMapSession> sess;
    {
        std::lock_guard<std::mutex> lock(g_portmap_mutex);
        auto it = g_portmap_sessions->find(port);
        if (it != g_portmap_sessions->end()) {
            // Already mapped (or still being attempted): reuse the session.
            sess = it->second;
        } else {
            sess = std::make_shared<PortMapSession>();
            (*g_portmap_sessions)[port] = sess;

            auto on_result = [sess](const librats::PortMapResult& r) {
                if (r.protocol != librats::PortMapProtocol::TCP) return;
                std::lock_guard<std::mutex> lk(sess->m);
                if (!r.success) {
                    if (!r.error.empty()) sess->last_error = r.error;
                    return;
                }
                // A "successful" mapping on a gateway whose own external IP
                // is private is a double NAT: the port forward stops at the
                // inner router and we are still unreachable. Not a result.
                if (!r.external_ip.empty() &&
                    !librats::network_utils::is_public_ip(r.external_ip)) {
                    sess->saw_double_nat = true;
                    sess->last_error = "gateway is double-NATed (external IP " +
                                       r.external_ip + " is private)";
                    return;
                }
                if (sess->have_result) return;
                sess->have_result   = true;
                sess->external_port = r.external_port;
                sess->external_ip   = r.external_ip;
                sess->cv.notify_all();
            };

            sess->upnp = std::make_unique<librats::UpnpClient>();
            sess->upnp->set_callback(on_result);
            sess->upnp->add_mapping(librats::PortMapProtocol::TCP, port,
                                    /*external_port=*/0, "bopwire");
            sess->upnp->start();

            sess->natpmp = std::make_unique<librats::NatPmpClient>();
            sess->natpmp->set_callback(on_result);
            sess->natpmp->add_mapping(librats::PortMapProtocol::TCP, port);
            sess->natpmp->start();
        }
    }

    // Wait for the first confirmed mapping (or the timeout).
    std::unique_lock<std::mutex> lk(sess->m);
    sess->cv.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                      [&] { return sess->have_result; });

    if (!sess->have_result) {
        std::string why = sess->saw_double_nat || !sess->last_error.empty()
            ? sess->last_error
            : "no UPnP/NAT-PMP gateway answered within " +
              std::to_string(timeout_ms) + " ms";
        lk.unlock();
        // Tear the session down so a later retry starts fresh. stop() joins
        // the worker threads, which may briefly block on the delete request.
        std::shared_ptr<PortMapSession> doomed;
        {
            std::lock_guard<std::mutex> lock(g_portmap_mutex);
            auto it = g_portmap_sessions->find(port);
            if (it != g_portmap_sessions->end() && it->second == sess) {
                doomed = it->second;
                g_portmap_sessions->erase(it);
            }
        }
        if (doomed) {
            if (doomed->upnp)   doomed->upnp->stop();
            if (doomed->natpmp) doomed->natpmp->stop();
        }
        set_error("mc_portmap_open_tcp: " + why);
        return 0;
    }

    if (out_ip && out_ip_len) {
        const size_t n = std::min(sess->external_ip.size(), out_ip_len - 1);
        std::memcpy(out_ip, sess->external_ip.data(), n);
        out_ip[n] = '\0';
    }
    return sess->external_port;
}

void mc_portmap_close_tcp(uint16_t port) {
    std::shared_ptr<PortMapSession> sess;
    {
        std::lock_guard<std::mutex> lock(g_portmap_mutex);
        auto it = g_portmap_sessions->find(port);
        if (it == g_portmap_sessions->end()) return;
        sess = it->second;
        g_portmap_sessions->erase(it);
    }
    // stop() removes the mapping on the gateway (best-effort) and joins the
    // backend's worker thread.
    if (sess->upnp)   sess->upnp->stop();
    if (sess->natpmp) sess->natpmp->stop();
}
