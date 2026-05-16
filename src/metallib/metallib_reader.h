/* SPDX-License-Identifier: AGPL-3.0-or-later */
#ifndef LIBAPPLEGFX_METALLIB_READER_H
#define LIBAPPLEGFX_METALLIB_READER_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * metallib_reader — parse Apple .metallib (MTLB) containers.
 *
 * MetalLib is Apple's serializer for AIR shader libraries shipped with
 * frameworks like SkyLightShaders.air64.metallib. The container format
 * uses an 88-byte fixed header, TLV-encoded function metadata section,
 * and embedded LLVM bitcode modules (one per shader entry point).
 *
 * This library provides a minimal MVP API for Stage 50: enumerate functions
 * and extract raw bitcode bytes. Consumer tooling (Stage 70 translator) feeds
 * extracted .bc files to llvm-bcanalyzer, llvm-dis, eventually spirv-llvm-translator.
 *
 * Derived from: mos/paravirt-re/library/stage50-metallib-reader-scoping.md §5.
 */

typedef struct lagfx_metallib lagfx_metallib_t;

/**
 * Function entry point metadata extracted from the TLV section.
 * All string pointers remain valid for the lifetime of the lagfx_metallib_t handle.
 */
typedef struct {
    const char *name;        /* function name, NUL-terminated, valid while ml is open */
    uint8_t     type_code;   /* 0=kernel, 1=vertex, 2=fragment, ... TBD full mapping */
    uint8_t     hash[32];    /* SHA-256-like digest per metallib-mtlb-container-format-2026-05-16.md:107 */
} lagfx_metallib_function_t;

/**
 * Parse a .metallib file from memory.
 *
 * Validates the MTLB magic signature and 88-byte header structure, then
 * walks the TLV-encoded function metadata section to build an internal index.
 * Returns NULL on parse failure (bad magic, truncated data, malformed TLV) with
 * error logged via LAGFX_ERR(). Caller must call lagfx_metallib_close() to free
 * resources when done.
 *
 * Uses alignment-safe LE readers from src/common/le.h: lagfx_le32(), lagfx_le64().
 *
 * @param data pointer to in-memory .metallib blob (must remain valid during lifetime)
 * @param len  length of the blob in bytes
 * @return     opaque handle on success, NULL on failure
 */
lagfx_metallib_t *lagfx_metallib_open(const uint8_t *data, size_t len);

/**
 * Enumerate functions in the metallib.
 *
 * Writes up to `cap` entries to out_funcs, returns actual count written
 * (may be less if metallib has fewer than cap functions). All string pointers
 * in returned lagfx_metallib_function_t entries remain valid until
 * lagfx_metallib_close() is called.
 *
 * Stage 50 MVP requirement: enumerate ≥20 functions from SkyLightShaders.air64.metallib.
 *
 * @param ml   metallib handle from lagfx_metallib_open()
 * @param out_funcs output buffer for function metadata entries
 * @param cap capacity of the output buffer (number of entries)
 * @return     actual number of functions written to out_funcs
 */
size_t lagfx_metallib_list_functions(lagfx_metallib_t *ml,
                                     lagfx_metallib_function_t *out_funcs,
                                     size_t cap);

/**
 * Get the LLVM bitcode bytes for a named function.
 *
 * Searches the metallib's internal index for a matching function name and returns
 * a pointer to its raw LLVM bitcode payload. Returns 0 on failure (function not found),
 * otherwise returns number of bytes written to out_bytes. The returned pointer is valid
 * until lagfx_metallib_close() is called.
 *
 * MVP strategy: hard-code SimpleVertexShadow lookup by walking first LLVM module from
 * bitcode blob. Full implementation will match on function name string or HASH field
 * (Stage 50.5).
 *
 * @param ml      metallib handle from lagfx_metallib_open()
 * @param name    function name to look up (e.g., "SimpleVertexShadow")
 * @param out_bytes output pointer that receives address of bitcode bytes
 * @return        number of bitcode bytes, or 0 if function not found
 */
size_t lagfx_metallib_get_bitcode(lagfx_metallib_t *ml,
                                  const char *name,
                                  const uint8_t **out_bytes);

/**
 * Free all resources associated with parsed metallib.
 *
 * Safe to call multiple times (idempotent). After this call, the lagfx_metallib_t*
 * handle and any pointers obtained from lagfx_metallib_list_functions() or
 * lagfx_metallib_get_bitcode() become invalid.
 *
 * @param ml metallib handle from lagfx_metallib_open(), may be NULL (no-op)
 */
void lagfx_metallib_close(lagfx_metallib_t *ml);

#ifdef __cplusplus
}
#endif

#endif /* LIBAPPLEGFX_METALLIB_READER_H */
