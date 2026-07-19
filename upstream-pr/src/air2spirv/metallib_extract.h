/*
 * libapplegfx-vulkan — metallib container parser (Phase 3.C.2)
 * src/air2spirv/metallib_extract.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Internal header. Not installed.
 *
 * Parses Apple's MTLB tagged-field container format (see
 * the internal spec §"Container format" and
 * the internal spec Finding 1)
 * to extract per-function LLVM Bitcode blobs. Each blob begins with
 * the LLVM Bitcode wrapper magic 0x0B17C0DE and is suitable for
 * hand-off to bitcode_retarget.{h,c} + an external `llc` invocation.
 *
 * Scope:
 *   - Read-only parser. Does NOT modify the input buffer; all
 *     extracted blobs are returned as (pointer, length) tuples that
 *     reference bytes inside the caller's buffer.
 *   - Lenient against unknown tags. Per the 2026-04-20 research
 *     burst we only need NAME / TYPE / MDSZ / OFFT to extract the
 *     bitcode; other tags (HASH, VERS, RFLT, RBUF) are recognised
 *     and skipped. Unknown tags are logged and skipped rather than
 *     fatal, in line with the "don't block" guidance in the
 *     scaffold spec.
 *   - No LLVM linkage. Emitting SPIR-V is the caller's job.
 *
 * Failure modes returned as LAGFX_ERR_*:
 *   - MTLB magic absent or file too short                → INVALID_ARG
 *   - OFFT + MDSZ extend past the file buffer            → INVALID_ARG
 *   - Function entry table offset past the file          → INVALID_ARG
 *   - Per-function record truncated                      → INVALID_ARG
 * All other oddities (unknown tag, unexpected stage enum) are
 * logged and the walker keeps going.
 */

#ifndef LIBAPPLEGFX_AIR2SPIRV_METALLIB_EXTRACT_H
#define LIBAPPLEGFX_AIR2SPIRV_METALLIB_EXTRACT_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Stage enum as carried by the MTLB TYPE field. Values match the
 * worthdoingbadly documentation. Other values are accepted and
 * exposed to callers via the stage_raw field for diagnostic
 * purposes. */
typedef enum {
    LAGFX_METALLIB_STAGE_UNKNOWN  = 0,
    LAGFX_METALLIB_STAGE_VERTEX   = 1,
    LAGFX_METALLIB_STAGE_FRAGMENT = 2,
    LAGFX_METALLIB_STAGE_KERNEL   = 3,
} lagfx_metallib_stage_t;

/* One per-function entry extracted from the metallib. Pointers
 * reference into the caller's input buffer and remain valid for
 * that buffer's lifetime. */
typedef struct {
    /* Human-readable function name from the NAME tag, up to
     * LAGFX_METALLIB_NAME_MAX-1 bytes. NUL-terminated. */
    char name[128];

    /* Stage enum as parsed + raw byte (for diagnostics when the
     * value is out of range). */
    lagfx_metallib_stage_t stage;
    uint8_t                stage_raw;

    /* Bitcode payload: pointer into the input buffer + length.
     * Starts with the LLVM Bitcode wrapper magic 0x0B17C0DE in
     * little-endian byte order. */
    const uint8_t *bitcode;
    size_t         bitcode_len;
} lagfx_metallib_function_t;

/* Walk the metallib buffer. On success writes up to
 * `out_capacity` function records to `out_funcs` and sets
 * *out_count to the actual number found (which may exceed
 * `out_capacity` — callers should resize + rerun if they need
 * all of them).
 *
 * The library ships a 5-function metallib as of Phase 0, so a
 * default capacity of 8 is comfortably enough for our own
 * corpus. Guest-submitted metallibs may be larger — callers
 * should probe with capacity=0 first to learn the count. */
lagfx_status_t lagfx_metallib_extract_functions(
    const uint8_t *buf,
    size_t buf_len,
    lagfx_metallib_function_t *out_funcs,
    size_t out_capacity,
    size_t *out_count);

/* Validate the four-byte MTLB magic without full parsing. Cheap
 * prefilter for "is this even a metallib?" cases. */
bool lagfx_metallib_has_magic(const uint8_t *buf, size_t buf_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_METALLIB_EXTRACT_H */
