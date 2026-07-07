#pragma once
//
// Album-art keying, shared by the ArtScraper (writer) and the art.get RPC
// (reader) so the derivation lives in exactly ONE place. Clients pass raw
// artist+album strings and never reimplement normalization → no cross-impl key
// drift (the players' generated-art seed is a separate concern).
//
#include "../crypto/hash.h"

#include <cctype>
#include <string>

namespace mc::art {

// normKey: trim leading/trailing ASCII whitespace → lowercase → collapse each
// internal whitespace run to a single space.
inline std::string norm_key(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    bool pending_space = false;  // saw whitespace after content started
    bool started       = false;
    for (unsigned char c : s) {
        if (std::isspace(c)) {
            pending_space = started;  // ignore leading ws; defer internal ws
            continue;
        }
        if (pending_space) {
            out.push_back(' ');
            pending_space = false;
        }
        out.push_back(static_cast<char>(std::tolower(c)));
        started = true;
    }
    return out;  // trailing ws never flushed → trimmed
}

// 64-char hex of sha256(normKey(artist) + 0x1f + normKey(album)). An empty album
// maps to "singles" (matches the players' album grouping). Callers prepend the
// "art:" (JPEG blob) or "arts:" (miss tombstone) prefix.
inline std::string album_key_hex(const std::string& artist, const std::string& album) {
    std::string composite = norm_key(artist);
    composite.push_back('\x1f');
    std::string alb = norm_key(album);
    composite += alb.empty() ? "singles" : alb;
    return crypto::to_hex(crypto::sha256(composite));
}

inline std::string art_blob_key(const std::string& artist, const std::string& album) {
    return "art:" + album_key_hex(artist, album);
}
inline std::string art_status_key(const std::string& artist, const std::string& album) {
    return "arts:" + album_key_hex(artist, album);
}

}  // namespace mc::art
