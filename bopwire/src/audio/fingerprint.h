#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace mc::audio {

// Shared chromaprint same-song similarity threshold (0.70). A pair at or above
// this is the SAME song. The CONSENSUS duplicate-block verdict (song_on_chain)
// does NOT read this float — it calls Fingerprint::same_song(), which encodes
// the identical 0.70 as an integer 70/100 cross-multiply so no float rounding
// can fork two honest nodes near the threshold. This constant remains the
// single source of truth for the NON-consensus float compare (rats_api.cpp's
// swarm-join hint); keep it and same_song()'s 70/100 in lockstep. Raised
// 0.55 -> 0.70 together with the similarity() offset-alignment fix + minimum-
// overlap guard: with those, different songs collapse toward ~0 and same-song
// re-encodes sit in the high band, so 0.70 separates them cleanly (0.55 sat
// inside the inflated different-song band).
inline constexpr float kChromaprintSimThreshold = 0.70f;

// Chromaprint-based audio fingerprinting
class Fingerprint {
public:
    // Generate fingerprint from raw Ogg data (decodes audio internally)
    static std::unique_ptr<Fingerprint> from_ogg(const uint8_t* data, size_t len);

    // Generate fingerprint from any container/codec FFmpeg can read
    // (MP3 / FLAC / WAV / AAC / Opus / …). Used by DeepAuditor so the
    // chromaprint↔audio gate isn't restricted to Ogg-format blocks.
    static std::unique_ptr<Fingerprint> from_any(const uint8_t* data,
                                                  size_t len);

    // Load from stored base64-compressed string
    static std::unique_ptr<Fingerprint> from_compressed(const std::string& base64);

    // Return base64-compressed fingerprint (for storage)
    std::string compressed() const;

    // Raw uint32 array
    const std::vector<uint32_t>& raw() const { return raw_; }

    // Compute similarity [0.0, 1.0] between this and another fingerprint.
    // FLOAT result — for display / logging / non-consensus heuristics only
    // (DeepAuditor, the swarm-join hint, the C API). NEVER compare this against
    // a threshold on the consensus path: the float rounds differently across
    // FPU / FMA-contraction / SIMD codegen (MSVC-x64 vs GCC/Clang-Linux), so a
    // near-threshold pair can land on opposite sides on two honest nodes.
    float similarity(const Fingerprint& other) const;

    // CONSENSUS same-song verdict: true iff `other` is the SAME song as this
    // (a re-encode / re-rip) at the shared kChromaprintSimThreshold (0.70). Bit-
    // identical on every compiler/arch because the threshold test is done in
    // pure integer/fixed-point (scaled-integer Hamming match count), so no float
    // rounding can flip the verdict and fork two honest nodes. This is the ONLY
    // similarity test allowed inside song_on_chain (the duplicate-song gate that
    // enqueue / build / validate / replay all share).
    bool same_song(const Fingerprint& other) const;

    // Compute bucket IDs for inverted index (up to 100)
    std::vector<uint16_t> bucket_ids() const;

private:
    std::vector<uint32_t> raw_;
};

// Base64 encode / decode helpers
std::string base64_encode(const uint8_t* data, size_t len);
std::vector<uint8_t> base64_decode(const std::string& s);

} // namespace mc::audio
