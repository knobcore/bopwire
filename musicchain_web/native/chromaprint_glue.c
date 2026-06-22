/*
 * chromaprint_glue.c — extern "C" entry points the web player's TypeScript
 * wrapper calls via Module.cwrap() to fingerprint a locally-loaded audio
 * file before issuing `fingerprint.submit` to the home node.
 *
 * The vendored chromaprint (C) 2010-2016 Lukas Lalinsky already exposes a
 * C ABI that lines up almost 1:1 with what the WASM caller wants:
 *
 *   chromaprint_new(algorithm) -> ctx
 *   chromaprint_start(ctx, sr, ch) -> 0/1
 *   chromaprint_feed(ctx, int16_t*, n_samples) -> 0/1
 *   chromaprint_finish(ctx) -> 0/1
 *   chromaprint_get_fingerprint(ctx, char**) -> 0/1
 *       (returns a base64-encoded compressed fingerprint — exactly the
 *        wire format AcoustID / our home node expects)
 *   chromaprint_free(ctx)
 *
 * So this glue just rebrands those entry points under the `mc_web_*`
 * prefix the TS wrapper imports, and on get_compressed() it strdup's the
 * chromaprint-owned buffer into a malloc'd string so the JS caller can
 * free it via Module._free() (the standard pattern used by wallet_glue.c).
 *
 * Algorithm fixed to CHROMAPRINT_ALGORITHM_DEFAULT (= TEST2) to match
 * the Android player + home node fingerprint format.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "chromaprint.h"

void* mc_web_chromaprint_new(void) {
    return (void*)chromaprint_new(CHROMAPRINT_ALGORITHM_DEFAULT);
}

int mc_web_chromaprint_start(void* ctx, int sample_rate, int num_channels) {
    if (!ctx) return 0;
    return chromaprint_start((ChromaprintContext*)ctx, sample_rate, num_channels);
}

int mc_web_chromaprint_feed(void* ctx, const int16_t* samples, int sample_count) {
    if (!ctx || !samples || sample_count <= 0) return 0;
    return chromaprint_feed((ChromaprintContext*)ctx, samples, sample_count);
}

int mc_web_chromaprint_finish(void* ctx) {
    if (!ctx) return 0;
    return chromaprint_finish((ChromaprintContext*)ctx);
}

/*
 * Returns a malloc'd, null-terminated base64 string. Caller frees via
 * Module._free(). Returns NULL on failure.
 *
 * We can't just return the chromaprint-owned pointer because emscripten
 * tracks malloc/free against its own heap; chromaprint_dealloc routes
 * back to the C runtime free, which in a single-module WASM build is
 * the same allocator — but if the caller mistakenly calls Module._free()
 * on a pointer that originated inside chromaprint_get_fingerprint() we
 * want that to be safe. Cheapest fix: copy into our own malloc'd buffer.
 */
char* mc_web_chromaprint_get_compressed(void* ctx) {
    if (!ctx) return NULL;
    char* fp = NULL;
    if (!chromaprint_get_fingerprint((ChromaprintContext*)ctx, &fp) || !fp) {
        return NULL;
    }
    size_t n = strlen(fp);
    char* out = (char*)malloc(n + 1);
    if (out) {
        memcpy(out, fp, n);
        out[n] = '\0';
    }
    chromaprint_dealloc(fp);
    return out;
}

void mc_web_chromaprint_free(void* ctx) {
    if (ctx) chromaprint_free((ChromaprintContext*)ctx);
}
