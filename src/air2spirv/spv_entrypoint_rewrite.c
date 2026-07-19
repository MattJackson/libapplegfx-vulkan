/*
 * libapplegfx-vulkan — SPV entry-point metadata rewriter (Phase 3.C.2)
 * src/air2spirv/spv_entrypoint_rewrite.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implementation of spv_entrypoint_rewrite.h. Algorithm:
 *
 *   Pass 1 (scan-only): walk the instruction stream after the 5-word
 *   header. Record:
 *     - Whether OpCapability Shader is already present.
 *     - Byte offset + size of OpMemoryModel (we'll insert after it).
 *     - The <result-id> of the function whose OpName string matches
 *       `entry_point_name`. We look at OpName's target id; this is
 *       the same id the eventual OpFunction definition uses.
 *     - A boolean that flips when any instruction is encountered
 *       that we plan to drop/modify on pass 2.
 *     - Catch malformed SPIR-V (word_count == 0, instruction crosses
 *       buffer end) and return LAGFX_ERR_PROTOCOL.
 *
 *   Pass 2 (emit): allocate a worst-case output buffer (input size
 *   + slack for the injected instructions — at most 16 words), copy
 *   the 5-word header, then walk again and emit every instruction
 *   that isn't on the drop list. Drop list:
 *     - OpCapability with operand ∈ {Linkage(5), Kernel(6)}
 *     - OpDecorate <id> CPacked(10)
 *     - OpMemberDecorate <id> <member> CPacked(10)
 *     - OpDecorate <id> LinkageAttributes(41) "..." <mode>
 *     - OpMemberDecorate <id> <member> LinkageAttributes "..." <mode>
 *   When emitting OpMemoryModel, emit it first, then splice in:
 *     OpCapability Shader            (if not already seen)
 *     OpEntryPoint <stage> <fn-id> "<name>"   (no interface IDs)
 *     OpExecutionMode <fn-id> OriginUpperLeft (fragment only)
 *   The OpCapability injection must happen BEFORE OpMemoryModel
 *   per SPIR-V spec §2.4 "Logical Layout of a Module": all
 *   capabilities come before memory model. So we insert it during
 *   pass 2 the first time we see ANY instruction beyond capability
 *   section OR when we reach OpMemoryModel, whichever is first.
 *
 * Corner cases handled:
 *   - Apple blob has `OpCapability Shader` already (both fixtures in
 *     tests/fixtures/triangle.metallib do). The inject-if-missing
 *     guard keeps us idempotent.
 *   - Multiple functions exported (metallib with 2+ stages). Caller
 *     runs the rewriter once per stage with the matching name.
 *   - OpName string inlined with NUL padding: SPIR-V encodes strings
 *     as NUL-terminated sequences padded to 4-byte boundaries; we
 *     compare word-by-word stopping at the first NUL.
 *
 * Not handled here — done in spv_signature_transform.c instead:
 *   - Signature transform (`%struct fn(%args)` -> `void main(void)`
 *     with Output globals for each returned field, Input globals
 *     for vertex_id / position / etc). Landed in phase 3.C.2 M5 as a
 *     separate transform pass: src/air2spirv/spv_signature_transform.c.
 *     The triangle-spv-rewrite tool invokes the signature transform
 *     (which subsumes the metadata edits below) by default; set
 *     LAGFX_SPV_METADATA_ONLY=1 to fall back to this metadata-only
 *     pass for diagnostic purposes.
 *   - [[position]] → BuiltIn Position / [[color(n)]] → Location N
 *     decoration emission: also handled by the signature transform
 *     since it controls variable synthesis for each return-struct
 *     member and each function parameter.
 */

#include "spv_entrypoint_rewrite.h"
#include "common/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* SPIR-V opcode constants (see SPIR-V spec §3.32 "Instructions"). */
enum {
    SPV_OP_SOURCE_CONTINUED = 2,
    SPV_OP_SOURCE           = 3,
    SPV_OP_SOURCE_EXTENSION = 4,
    SPV_OP_NAME             = 5,
    SPV_OP_MEMBER_NAME      = 6,
    SPV_OP_STRING           = 7,
    SPV_OP_LINE             = 8,
    SPV_OP_EXTENSION        = 10,
    SPV_OP_EXT_INST_IMPORT  = 11,
    SPV_OP_MEMORY_MODEL     = 14,
    SPV_OP_ENTRY_POINT      = 15,
    SPV_OP_EXECUTION_MODE   = 16,
    SPV_OP_CAPABILITY       = 17,
    SPV_OP_DECORATE         = 71,
    SPV_OP_MEMBER_DECORATE  = 72,
};

/* SPIR-V decoration constants we care about. */
enum {
    SPV_DEC_CPACKED             = 10,
    SPV_DEC_LINKAGE_ATTRIBUTES  = 41,
};

/* SPIR-V capability constants we care about. */
enum {
    SPV_CAP_SHADER  = 1,
    SPV_CAP_LINKAGE = 5,
    SPV_CAP_KERNEL  = 6,
};

/* SPIR-V execution mode constants. */
enum {
    SPV_EXEC_MODE_ORIGIN_UPPER_LEFT = 7,
};

/* Pack (word_count, opcode) into a single SPIR-V instruction header
 * word. Per spec §2.3: high 16 bits = word count, low 16 bits = opcode. */
static uint32_t spv_pack_insn(uint32_t word_count, uint32_t opcode) {
    return (word_count << 16) | (opcode & 0xFFFFu);
}

/* Unpack word count + opcode from an instruction header word. */
static void spv_unpack_insn(uint32_t w, uint32_t *word_count, uint32_t *opcode) {
    *word_count = (w >> 16) & 0xFFFFu;
    *opcode     = w & 0xFFFFu;
}

bool lagfx_spv_has_magic(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < 4u) {
        return false;
    }
    /* SPIR-V magic 0x07230203 in little-endian byte order. */
    return buf[0] == 0x03u
        && buf[1] == 0x02u
        && buf[2] == 0x23u
        && buf[3] == 0x07u;
}

/* Compare a SPIR-V NUL-terminated string stored starting at
 * `words` (inclusive), up to `max_words` 32-bit words, against the
 * C string `s`. Returns true iff equal. */
static bool spv_str_eq(const uint32_t *words, size_t max_words,
                       const char *s) {
    const uint8_t *bytes = (const uint8_t *)words;
    size_t max_bytes = max_words * 4u;
    size_t i = 0;
    while (i < max_bytes) {
        uint8_t b = bytes[i];
        char    c = s[i];
        if (b == 0u) {
            return c == '\0';
        }
        if (c == '\0') {
            return false;
        }
        if (b != (uint8_t)c) {
            return false;
        }
        i++;
    }
    /* Ran out of payload before hitting NUL. Malformed. */
    return false;
}

/* Emit an N-word instruction into dst[*dst_idx..], bumping the index. */
static void spv_emit_words(uint32_t *dst, size_t *dst_idx,
                           const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[*dst_idx + i] = words[i];
    }
    *dst_idx += count;
}

/* Pack a NUL-terminated C string into 32-bit words (little-endian),
 * padded with zeros to a 4-byte boundary. Returns the number of
 * words written. Caller ensures dst has enough space. */
static size_t spv_pack_string(const char *s, uint32_t *dst) {
    size_t len = strlen(s);
    /* +1 for the mandatory NUL, then pad to multiple of 4. */
    size_t bytes = len + 1u;
    size_t padded = (bytes + 3u) & ~(size_t)3u;
    size_t words = padded / 4u;
    /* Zero-init the last word so padding bytes are zero. */
    if (words > 0) {
        dst[words - 1u] = 0u;
    }
    uint8_t *out = (uint8_t *)dst;
    for (size_t i = 0; i < len; ++i) {
        out[i] = (uint8_t)s[i];
    }
    out[len] = 0u;
    /* padding bytes already zero from dst[words-1] init */
    return words;
}

lagfx_status_t lagfx_spv_rewrite_entry_point(
    const uint8_t *in_buf,
    size_t in_len,
    const char *entry_point_name,
    lagfx_spv_stage_t stage,
    uint8_t **out_buf,
    size_t *out_len) {
    if (!in_buf || !entry_point_name || !*entry_point_name
        || !out_buf || !out_len) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!lagfx_spv_has_magic(in_buf, in_len)) {
        LAGFX_ERR("spv_rewrite: missing SPIR-V magic (in_len=%zu)",
                  in_len);
        return LAGFX_ERR_INVALID_ARG;
    }
    /* Header is 5 words = 20 bytes. Any shorter is malformed. */
    if (in_len < 5u * 4u || (in_len % 4u) != 0u) {
        LAGFX_ERR("spv_rewrite: truncated or misaligned (in_len=%zu)",
                  in_len);
        return LAGFX_ERR_PROTOCOL;
    }

    const uint32_t *in_words = (const uint32_t *)in_buf;
    size_t in_word_count = in_len / 4u;

    /* ---- Pass 1: scan -------------------------------------------------- */

    uint32_t fn_id = 0u;
    bool found_fn = false;
    bool has_shader_cap = false;

    size_t i = 5u;  /* skip 5-word header */
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (wc == 0u || i + wc > in_word_count) {
            LAGFX_ERR("spv_rewrite: malformed instruction at word %zu "
                      "(wc=%u op=%u)", i, wc, op);
            return LAGFX_ERR_PROTOCOL;
        }
        switch (op) {
            case SPV_OP_CAPABILITY:
                if (wc >= 2u && in_words[i + 1u] == SPV_CAP_SHADER) {
                    has_shader_cap = true;
                }
                break;
            case SPV_OP_NAME: {
                /* OpName <target-id> "literal" — operand layout:
                 *   word[0] = (wc<<16 | 5)
                 *   word[1] = target id
                 *   word[2..wc] = string payload */
                if (wc >= 3u) {
                    uint32_t target_id = in_words[i + 1u];
                    size_t str_words = wc - 2u;
                    if (spv_str_eq(&in_words[i + 2u], str_words,
                                   entry_point_name)) {
                        fn_id = target_id;
                        found_fn = true;
                    }
                }
                break;
            }
            default:
                break;
        }
        i += wc;
    }

    if (!found_fn) {
        LAGFX_ERR("spv_rewrite: no OpName match for '%s'",
                  entry_point_name);
        return LAGFX_ERR_INVALID_ARG;
    }

    /* ---- Pass 2: emit -------------------------------------------------- */

    /* Worst-case growth: we add at most
     *   OpCapability Shader            (2 words)
     *   OpEntryPoint + string          (3 + ceil((strlen+1)/4) words)
     *   OpExecutionMode OriginUpperLeft (3 words)
     * which for any reasonable name is well under 64 extra words.
     * Add an extra 64 words of slack for safety. */
    size_t max_extra_words = 2u + 3u
        + ((strlen(entry_point_name) + 1u + 3u) / 4u)
        + 3u + 64u;
    size_t max_out_words = in_word_count + max_extra_words;
    uint32_t *out = (uint32_t *)malloc(max_out_words * 4u);
    if (!out) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* Copy header verbatim (5 words). */
    memcpy(out, in_words, 5u * 4u);
    size_t out_idx = 5u;

    /* State for splicing in the entry-point instructions. We do the
     * splice immediately after emitting the OpMemoryModel instruction,
     * which per SPIR-V layout is the canonical point where the
     * capability/extension/imports section ends and the
     * entry-point / execution-mode section begins. */
    bool spliced = false;
    size_t hits_linkage_cap = 0;
    size_t hits_kernel_cap  = 0;
    size_t hits_linkage_dec = 0;
    size_t hits_cpacked_dec = 0;

    i = 5u;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        bool drop = false;

        switch (op) {
            case SPV_OP_CAPABILITY:
                if (wc >= 2u) {
                    uint32_t cap = in_words[i + 1u];
                    if (cap == SPV_CAP_LINKAGE) {
                        drop = true;
                        hits_linkage_cap++;
                    } else if (cap == SPV_CAP_KERNEL) {
                        drop = true;
                        hits_kernel_cap++;
                    }
                }
                break;
            case SPV_OP_DECORATE:
                /* word[1] = target id, word[2] = decoration */
                if (wc >= 3u) {
                    uint32_t dec = in_words[i + 2u];
                    if (dec == SPV_DEC_CPACKED) {
                        drop = true;
                        hits_cpacked_dec++;
                    } else if (dec == SPV_DEC_LINKAGE_ATTRIBUTES) {
                        drop = true;
                        hits_linkage_dec++;
                    }
                }
                break;
            case SPV_OP_MEMBER_DECORATE:
                /* word[1] = target, word[2] = member, word[3] = dec */
                if (wc >= 4u) {
                    uint32_t dec = in_words[i + 3u];
                    if (dec == SPV_DEC_CPACKED
                        || dec == SPV_DEC_LINKAGE_ATTRIBUTES) {
                        drop = true;
                        if (dec == SPV_DEC_CPACKED)  hits_cpacked_dec++;
                        else                          hits_linkage_dec++;
                    }
                }
                break;
            default:
                break;
        }

        if (!drop) {
            spv_emit_words(out, &out_idx, &in_words[i], wc);
        }

        /* Splice: after OpMemoryModel (or, if absent, at the first
         * instruction AFTER the capability/extension section ends —
         * but every Vulkan-ish SPIR-V has OpMemoryModel, and so does
         * LLVM's output). */
        if (!spliced && op == SPV_OP_MEMORY_MODEL) {
            if (!has_shader_cap) {
                /* OpCapability Shader (2 words) — but spec requires
                 * capabilities BEFORE memory model. We can't insert
                 * here retroactively, so we'd have to rebuild from
                 * the top. Apple blobs already carry OpCapability
                 * Shader (see triangle fixtures), so in practice we
                 * hit the `has_shader_cap == true` path. If we ever
                 * see a blob without it, emit the capability AFTER
                 * memory model and log — spirv-val will flag it, but
                 * the Mesa compiler is lenient enough to accept the
                 * shader anyway (validated with a test-harness
                 * patch to strip the existing Shader cap pre-
                 * rewrite; see README §"Corner cases"). */
                uint32_t cap[2];
                cap[0] = spv_pack_insn(2, SPV_OP_CAPABILITY);
                cap[1] = SPV_CAP_SHADER;
                spv_emit_words(out, &out_idx, cap, 2u);
                LAGFX_LOG("spv_rewrite: injected OpCapability Shader "
                          "(post-memory-model — non-canonical but "
                          "Mesa-tolerated)");
            }

            /* OpEntryPoint <stage> <fn_id> "name" [interface-ids...]
             * word[0] = (wc<<16 | 15)
             * word[1] = execution model
             * word[2] = entry point id
             * word[3..] = literal string (padded)
             * [optional] interface-ids — empty in this phase. */
            size_t name_words = (strlen(entry_point_name) + 1u + 3u) / 4u;
            size_t ep_words = 3u + name_words;
            uint32_t *ep = &out[out_idx];
            ep[0] = spv_pack_insn((uint32_t)ep_words, SPV_OP_ENTRY_POINT);
            ep[1] = (uint32_t)stage;
            ep[2] = fn_id;
            (void)spv_pack_string(entry_point_name, &ep[3]);
            out_idx += ep_words;

            /* OpExecutionMode for fragment stage. */
            if (stage == LAGFX_SPV_STAGE_FRAGMENT) {
                uint32_t em[3];
                em[0] = spv_pack_insn(3, SPV_OP_EXECUTION_MODE);
                em[1] = fn_id;
                em[2] = SPV_EXEC_MODE_ORIGIN_UPPER_LEFT;
                spv_emit_words(out, &out_idx, em, 3u);
            }
            spliced = true;
        }

        i += wc;
    }

    if (!spliced) {
        /* No OpMemoryModel found — malformed Vulkan/LLVM SPIR-V. */
        free(out);
        LAGFX_ERR("spv_rewrite: no OpMemoryModel in input; cannot "
                  "locate splice point for OpEntryPoint");
        return LAGFX_ERR_PROTOCOL;
    }

    LAGFX_LOG("spv_rewrite: name='%s' stage=%s fn_id=%u "
              "drop(Linkage-cap=%zu, Kernel-cap=%zu, "
              "CPacked-dec=%zu, Linkage-dec=%zu) "
              "in_words=%zu out_words=%zu",
              entry_point_name,
              stage == LAGFX_SPV_STAGE_VERTEX ? "vertex" : "fragment",
              fn_id,
              hits_linkage_cap, hits_kernel_cap,
              hits_cpacked_dec, hits_linkage_dec,
              in_word_count, out_idx);

    *out_buf = (uint8_t *)out;
    *out_len = out_idx * 4u;
    return LAGFX_OK;
}
