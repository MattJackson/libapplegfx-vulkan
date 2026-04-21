/*
 * libapplegfx-vulkan — LLVM Bitcode target-triple retarget (Phase 3.C.2)
 * src/air2spirv/bitcode_retarget.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Internal header. Not installed.
 *
 * Rewrites the target-triple embedded in an LLVM Bitcode blob
 * from `air64-apple-macosx*` (Apple's AIR triple) to
 * `spir64-unknown-vulkan1.3` so stock LLVM (20.1+) with the SPIR-V
 * backend can accept the module. This is step (b) in the pipeline
 * described in the internal spec
 * Finding 3 and the internal spec
 *
 * Implementation approach (two-tier, matches runbook §6.1/§6.2):
 *
 *   Tier 1 (in-place): If the bitcode wrapper magic is present
 *   (0x0B17C0DE) and the embedded target-triple string is at
 *   least as long as the replacement AND the replacement can
 *   be null-padded to the original length without breaking the
 *   bitcode framing, rewrite in place and return the same
 *   buffer. Call sites can save an allocation.
 *
 *   Tier 2 (copy-out): When in-place is not feasible (replacement
 *   triple is longer; or the bitcode framing doesn't tolerate
 *   padding), emit a new buffer. Caller owns the resulting
 *   allocation and frees via free().
 *
 * Phase 3.C.2 scaffold caveats:
 *
 *   - We do NOT rewrite the LLVM data layout string. The SPIR-V
 *     backend's default layout is close enough to Apple's AIR
 *     that the difference is usually tolerated; when it isn't,
 *     the runbook's §6.2 bitcode-rewrite path handles the data-
 *     layout fixup manually. Left as FIXME(phase-3c2-datalayout)
 *     for the sequel commit.
 *   - We match `air64-apple-macosx<...>` with a <version>
 *     suffix that is any combination of `[0-9v.]` so both
 *     `air64-apple-macosx14.0.0` and `air64_v28-apple-macosx26.3.0`
 *     are recognised.
 *   - LLVM Bitcode framing is complex (VBR-coded records inside
 *     blocks); locating the exact `MODULE_CODE_TRIPLE` record
 *     requires a bitcode reader. The scaffold instead treats the
 *     triple as a raw string and replaces it everywhere in the
 *     blob where the `air64*macosx*` pattern appears. This works
 *     because (a) the triple is stored as a VBR6-coded blob
 *     whose byte-level content is still ASCII, and (b) stock
 *     clang-generated bitcode doesn't contain the pattern in
 *     any other field. Recorded as FIXME(phase-3c2-bitcode-reader)
 *     for the sequel when we integrate LLVM directly.
 */

#ifndef LIBAPPLEGFX_AIR2SPIRV_BITCODE_RETARGET_H
#define LIBAPPLEGFX_AIR2SPIRV_BITCODE_RETARGET_H

#include "libapplegfx-vulkan.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The replacement triple. Chosen to match runbook §6 and SPIR-V
 * LLVM backend documentation. Exposed as a macro rather than a
 * static const so call sites can stringify + log it. */
#define LAGFX_BITCODE_SPIRV_TRIPLE "spir64-unknown-vulkan1.3"

/* True iff `buf` starts with the LLVM Bitcode wrapper magic
 * 0x0B17C0DE. Cheap prefilter. Size check included. */
bool lagfx_bitcode_has_magic(const uint8_t *buf, size_t buf_len);

/* Rewrite the embedded target-triple from Apple's AIR form to
 * the Vulkan SPIR-V form. Tier 1 (in-place) is attempted first;
 * on success *out_buf = in_buf and *out_len = in_len. Tier 2
 * (copy-out) is used when the replacement doesn't fit in-place;
 * *out_buf receives a malloc'd buffer that the caller owns.
 *
 * The in_buf / in_len pair is NEVER mutated by this function
 * (even on the Tier 1 path — callers pass a writable copy if
 * they want the mutation to land in their own buffer; the Tier
 * 1 path uses its own working copy internally).
 *
 * On success returns LAGFX_OK. Errors:
 *   LAGFX_ERR_INVALID_ARG — NULL inputs or buffer too small.
 *   LAGFX_ERR_PROTOCOL    — no AIR triple pattern found.
 *   LAGFX_ERR_OUT_OF_MEMORY — Tier 2 malloc failed.
 *
 * Callers must free *out_buf when done via free(), regardless of
 * tier — the contract is "caller owns the resulting buffer". */
lagfx_status_t lagfx_bitcode_retarget_to_spirv(
    const uint8_t *in_buf,
    size_t in_len,
    uint8_t **out_buf,
    size_t *out_len);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_BITCODE_RETARGET_H */
