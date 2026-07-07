// C-ABI declarations for the vendored sacad cover-art static library
// (bopwire/deps/sacad-ffi). Linked into bopwire-node only when MC_WITH_ART=ON.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Fetch a cover JPEG for (artist, album) at approximately `size` px.
// On success (returns 0), *out_ptr/*out_len receive a heap buffer that the caller
// must release with sacad_free(). Returns 1 when no cover was found, -3 on a
// transient source error (worth a later retry), other negatives on bad input/panic.
int sacad_fetch_cover(const char* artist, const char* album, uint32_t size,
                      uint8_t** out_ptr, size_t* out_len);

// Release a buffer returned by sacad_fetch_cover (no-op on NULL).
void sacad_free(uint8_t* ptr, size_t len);

#ifdef __cplusplus
}
#endif
