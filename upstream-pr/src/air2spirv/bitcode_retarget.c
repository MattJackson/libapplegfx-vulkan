/*
 * libapplegfx-vulkan — LLVM Bitcode target-triple retarget (Phase 3.C.2)
 * src/air2spirv/bitcode_retarget.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implementation of the triple rewriter described in
 * bitcode_retarget.h. The algorithm:
 *
 *   1. Check the LLVM Bitcode wrapper magic (0x0B17C0DE) at
 *      in_buf[0..4]. If absent, LAGFX_ERR_INVALID_ARG.
 *   2. Scan the blob for an ASCII `air64*-apple-macosx*` triple
 *      pattern. For each hit:
 *        a. Measure the matched length (up to the first byte that
 *           isn't part of the ASCII triple character class).
 *        b. Record the (start, end) span.
 *   3. If no hits found → LAGFX_ERR_PROTOCOL.
 *   4. Plan the rewrite:
 *        - Tier 1 (in-place): every hit has span_len >= replacement
 *          length. Copy the replacement into the span, NUL-pad the
 *          tail to preserve the span length.
 *        - Tier 2 (copy-out): at least one hit's span_len <
 *          replacement length, or we didn't plan Tier 1. Emit a
 *          new buffer splicing in the replacement at each hit.
 *   5. Return the resulting buffer via (out_buf, out_len). Tier 2
 *      always malloc's; Tier 1 malloc's a copy for uniform
 *      ownership semantics (caller always free()s).
 *
 * The NUL-padding trick for Tier 1 is tolerated by LLVM's Bitcode
 * string-blob decoder because VBR6 null-terminates strings
 * implicitly via length fields at the bitcode record layer — the
 * trailing NUL bytes stay within the recorded blob but the string
 * reader reads only up to the original blob length. Left as
 * FIXME(phase-3c2-bitcode-reader) to replace with a real bitcode
 * record editor when we link against libLLVM.
 */

#include "bitcode_retarget.h"
#include "common/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* LLVM Bitcode wrapper magic (0x0B17C0DE) in little-endian: the
 * first 4 bytes of any wrapped bitcode module are DE C0 17 0B. */
static const uint8_t kLlvmBitcodeWrapperMagic[4] = {
    0xDE, 0xC0, 0x17, 0x0B,
};

/* The prefix pattern we scan for. Apple's triples always start
 * with `air64` followed by either `-` or `_v<n>-`; then the
 * literal `-apple-macosx`. We match `air64` then a short gap
 * containing only `[-_a-z0-9]`, then `apple-macosx`. */
static const char kAirPrefix[]   = "air64";
static const char kAppleInfix[]  = "apple-macosx";

/* Character class for the "version suffix" portion — everything
 * after `macosx` up to the terminator. Apple has used both
 * `macosx14.0.0` and `macosx26.3.0`; the suffix chars are digits
 * and dots. `v` is included for the `air64_v28` prefix variant. */
static bool is_triple_tail_char(uint8_t c) {
    return (c >= '0' && c <= '9') || c == '.';
}

/* Permissive char class for the "gap" between `air64` and
 * `apple-macosx`. Apple's variants: `air64-apple-macosx...`,
 * `air64_v28-apple-macosx...`. Allowed: lowercase letters,
 * digits, hyphen, underscore. */
static bool is_gap_char(uint8_t c) {
    return (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9')
        || c == '-' || c == '_';
}

bool lagfx_bitcode_has_magic(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < 4u) {
        return false;
    }
    return memcmp(buf, kLlvmBitcodeWrapperMagic, 4u) == 0;
}

/* Locate the first AIR triple match starting at or after `start`
 * in buf[]. On hit writes (*out_begin, *out_end) and returns true;
 * (*out_end is one past the last byte of the matched triple). */
static bool find_next_air_triple(const uint8_t *buf, size_t buf_len,
                                 size_t start,
                                 size_t *out_begin, size_t *out_end) {
    const size_t pref_len   = sizeof(kAirPrefix) - 1u;   /* 5 */
    const size_t infix_len  = sizeof(kAppleInfix) - 1u;  /* 12 */
    /* Need at least pref_len + 1 (separator) + infix_len bytes. */
    const size_t min_len    = pref_len + 1u + infix_len;
    if (buf_len < min_len) {
        return false;
    }
    for (size_t i = start; i + min_len <= buf_len; ++i) {
        if (memcmp(buf + i, kAirPrefix, pref_len) != 0) {
            continue;
        }
        /* Try each possible gap length from 1 up to 8. The gap is
         * a run of `[-_a-z0-9]` bytes between `air64` and
         * `apple-macosx`. We can't greedily consume gap_chars
         * because `apple-macosx` itself is all lowercase letters —
         * the greedy walk would slide right over it. Instead try
         * each length and test for the infix. */
        bool matched = false;
        size_t gap = 0u;
        for (size_t g = 1u; g <= 8u; ++g) {
            if (i + pref_len + g + infix_len > buf_len) {
                break;
            }
            /* Every byte in the candidate gap must be a gap char. */
            bool gap_ok = true;
            for (size_t k = 0; k < g; ++k) {
                if (!is_gap_char(buf[i + pref_len + k])) {
                    gap_ok = false;
                    break;
                }
            }
            if (!gap_ok) {
                break;
            }
            if (memcmp(buf + i + pref_len + g,
                       kAppleInfix, infix_len) == 0) {
                gap = g;
                matched = true;
                break;
            }
        }
        if (!matched) {
            continue;
        }
        /* Consume the trailing version chars. */
        size_t tail = 0u;
        size_t post = i + pref_len + gap + infix_len;
        while (post + tail < buf_len
               && is_triple_tail_char(buf[post + tail])) {
            tail++;
        }
        *out_begin = i;
        *out_end   = post + tail;
        return true;
    }
    return false;
}

lagfx_status_t lagfx_bitcode_retarget_to_spirv(
    const uint8_t *in_buf,
    size_t in_len,
    uint8_t **out_buf,
    size_t *out_len) {
    if (!in_buf || !out_buf || !out_len) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!lagfx_bitcode_has_magic(in_buf, in_len)) {
        LAGFX_ERR("bitcode_retarget: no LLVM Bitcode wrapper magic "
                  "(got %02x %02x %02x %02x) "
                  "[FIXME(phase-3c2-bitcode-reader)]",
                  in_len >= 4u ? in_buf[0] : 0,
                  in_len >= 4u ? in_buf[1] : 0,
                  in_len >= 4u ? in_buf[2] : 0,
                  in_len >= 4u ? in_buf[3] : 0);
        return LAGFX_ERR_INVALID_ARG;
    }

    const char  *replacement   = LAGFX_BITCODE_SPIRV_TRIPLE;
    const size_t replacement_l = strlen(replacement);

    /* First pass: find all hits, note whether every span is large
     * enough for Tier 1 in-place rewrite. */
    size_t pos = 0u;
    size_t begin = 0u, end = 0u;
    bool found_any = false;
    bool tier1_ok  = true;

    while (find_next_air_triple(in_buf, in_len, pos, &begin, &end)) {
        size_t span = end - begin;
        if (span < replacement_l) {
            tier1_ok = false;
        }
        found_any = true;
        pos = end;
    }

    if (!found_any) {
        LAGFX_ERR("bitcode_retarget: no AIR target-triple pattern "
                  "found [FIXME(phase-3c2-unknown-triple-format)]");
        return LAGFX_ERR_PROTOCOL;
    }

    /* Tier 1: allocate a working copy the same size as input,
     * copy, then rewrite every match in place. Tier 2: allocate
     * a worst-case larger buffer and splice in replacements. We
     * always return a caller-owned malloc'd buffer to keep the
     * ownership contract uniform. */
    if (tier1_ok) {
        uint8_t *copy = (uint8_t *)malloc(in_len);
        if (!copy) {
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        memcpy(copy, in_buf, in_len);

        pos = 0u;
        size_t hits = 0u;
        while (find_next_air_triple(copy, in_len, pos, &begin, &end)) {
            size_t span = end - begin;
            memcpy(copy + begin, replacement, replacement_l);
            /* NUL-pad the remainder. See the file header note. */
            if (span > replacement_l) {
                memset(copy + begin + replacement_l, 0,
                       span - replacement_l);
            }
            hits++;
            pos = end;
        }
        LAGFX_LOG("bitcode_retarget: Tier 1 in-place, hits=%zu, "
                  "len=%zu", hits, in_len);
        *out_buf = copy;
        *out_len = in_len;
        return LAGFX_OK;
    }

    /* Tier 2: compute the new length first. Each hit contributes
     * (replacement_l - span) bytes of delta. */
    size_t delta = 0u;
    size_t hit_count = 0u;
    pos = 0u;
    while (find_next_air_triple(in_buf, in_len, pos, &begin, &end)) {
        size_t span = end - begin;
        if (replacement_l > span) {
            delta += (replacement_l - span);
        }
        /* If replacement_l < span (unusual here because Tier 1
         * handles it) we'd shrink — accounted for by a signed
         * delta, but we use unsigned here for simplicity and
         * assume only growth in Tier 2. */
        hit_count++;
        pos = end;
    }
    size_t new_len = in_len + delta;
    uint8_t *dst = (uint8_t *)malloc(new_len);
    if (!dst) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    size_t src_pos = 0u;
    size_t dst_pos = 0u;
    pos = 0u;
    while (find_next_air_triple(in_buf, in_len, pos, &begin, &end)) {
        /* Copy the pre-hit region. */
        size_t pre = begin - src_pos;
        if (pre > 0u) {
            memcpy(dst + dst_pos, in_buf + src_pos, pre);
            dst_pos += pre;
        }
        /* Copy the replacement string. */
        memcpy(dst + dst_pos, replacement, replacement_l);
        dst_pos += replacement_l;
        src_pos = end;
        pos = end;
    }
    /* Tail. */
    if (src_pos < in_len) {
        size_t tail = in_len - src_pos;
        memcpy(dst + dst_pos, in_buf + src_pos, tail);
        dst_pos += tail;
    }
    LAGFX_LOG("bitcode_retarget: Tier 2 copy-out, hits=%zu, "
              "in_len=%zu out_len=%zu", hit_count, in_len, dst_pos);
    *out_buf = dst;
    *out_len = dst_pos;
    return LAGFX_OK;
}
