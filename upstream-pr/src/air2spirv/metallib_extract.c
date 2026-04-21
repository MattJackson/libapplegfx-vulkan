/*
 * libapplegfx-vulkan — metallib container parser (Phase 3.C.2)
 * src/air2spirv/metallib_extract.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the MTLB tagged-field container walker. See
 * metallib_extract.h for the API and the internal spec
 * §"Container format" for the spec references. The Phase 0 corpus at
 * the internal spec is the known-good test fixture
 * (5 functions, 24,500 bytes, MTLB v1.2.9) — see
 * tests/fixtures/default.metallib.
 *
 * Format walk (condensed, as observed in the real 2026-04-20 corpus):
 *
 *   offset 0x00: 'M' 'T' 'L' 'B'                              magic
 *   offset 0x04: u32 version (little-endian mangled 1.2.9 = 0x00028001)
 *   offset 0x18: u64 function-entry-table offset              (FET_OFFSET)
 *   offset 0x48: u64 bitcode-region base offset               (BC_BASE)
 *   ...
 *   FET_OFFSET:
 *     u32 entry_count
 *     for each entry:
 *       u32 entry_size      (INCLUDES the u32 itself, so body spans
 *                            [entry_off + 4, entry_off + entry_size))
 *       entry body: a run of tagged fields, terminated by ENDT.
 *
 *   entry body tagged fields:
 *       "NAME" u16 len | <utf-8 bytes, NUL-terminated>
 *       "TYPE" u16 len | u8 stage_raw
 *       "HASH" u16 len | 32 bytes
 *       "MDSZ" u16 len | u64 bitcode_size
 *       "OFFT" u16 len | 3x u64 (see below)
 *       "VERS" u16 len | ...
 *       "RFLT" u16 len | ...
 *       "ENDT" (no length field — just the 4-byte tag)
 *
 *   OFFT payload (24 bytes): three little-endian u64s. The third is
 *   the bitcode offset relative to BC_BASE (the u64 at header offset
 *   0x48). The first two are secondary offsets for the function's
 *   reflection metadata, not used at the Phase 3.C.2 scaffold.
 *   Absolute file offset of the bitcode payload = BC_BASE + OFFT[2].
 *
 *   TYPE payload stage enum (observed in default.metallib):
 *       0 = vertex, 1 = fragment, 2 = kernel
 *
 * Tolerances:
 *   - Unknown tags: logged, skipped (len-driven advance).
 *   - Missing fields on an entry: record emitted with the fields it
 *     had; malformed entries don't abort the whole walk.
 *   - bitcode extents that point past buf_len: the function entry is
 *     emitted with bitcode=NULL/len=0 and the walker continues.
 *   - Older pre-v1.2.9 metallibs have historically been documented
 *     as using u32 tag-length fields. We still default to u16 (the
 *     2026-04-20 real-corpus finding) because the worthdoingbadly
 *     writeup predated our direct bytes inspection and, on the
 *     only file we have, u16 is definitively correct.
 */

#include "metallib_extract.h"
#include "common/log.h"

#include <stdint.h>
#include <string.h>

/* --- Little-endian load helpers ----------------------------- */

static uint16_t rd_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0]
                   | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32_le(const uint8_t *p) {
    return  (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

static uint64_t rd_u64_le(const uint8_t *p) {
    return  (uint64_t)p[0]
         | ((uint64_t)p[1] <<  8)
         | ((uint64_t)p[2] << 16)
         | ((uint64_t)p[3] << 24)
         | ((uint64_t)p[4] << 32)
         | ((uint64_t)p[5] << 40)
         | ((uint64_t)p[6] << 48)
         | ((uint64_t)p[7] << 56);
}

/* 4-char tag compared against the tag bytes at *p. */
static bool tag_eq(const uint8_t *p, const char *tag) {
    return p[0] == (uint8_t)tag[0]
        && p[1] == (uint8_t)tag[1]
        && p[2] == (uint8_t)tag[2]
        && p[3] == (uint8_t)tag[3];
}

/* --- Magic check --------------------------------------------- */

bool lagfx_metallib_has_magic(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < 4u) {
        return false;
    }
    return tag_eq(buf, "MTLB");
}

/* --- Main extractor ----------------------------------------- */

lagfx_status_t lagfx_metallib_extract_functions(
    const uint8_t *buf,
    size_t buf_len,
    lagfx_metallib_function_t *out_funcs,
    size_t out_capacity,
    size_t *out_count) {
    if (!buf || !out_count) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out_count = 0u;

    /* Bare minimum for header + FET offset field (0x18 + 8). */
    if (buf_len < 0x20u) {
        LAGFX_ERR("metallib_extract: buf_len=%zu too small for header",
                  buf_len);
        return LAGFX_ERR_INVALID_ARG;
    }
    if (!lagfx_metallib_has_magic(buf, buf_len)) {
        LAGFX_ERR("metallib_extract: MTLB magic missing (first 4 bytes "
                  "%02x %02x %02x %02x)",
                  buf[0], buf[1], buf[2], buf[3]);
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Function-entry-table offset lives at header offset 0x18. */
    uint64_t fet_off = rd_u64_le(buf + 0x18);
    if (fet_off == 0u || fet_off >= buf_len) {
        LAGFX_ERR("metallib_extract: FET offset 0x%llx out of bounds "
                  "(buf_len=%zu)", (unsigned long long)fet_off, buf_len);
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Bitcode-region base offset lives at header offset 0x48. Per
     * function OFFT[2] is a relative offset from this base. We
     * require the header to extend at least to 0x50 (i.e. to the
     * byte after the 0x48 u64) to read it. When the header is
     * shorter we fall back to bc_base=0 and interpret OFFT[2] as
     * absolute — this keeps the synthesised Phase 3.C.2 fixture
     * (which writes an absolute bitcode offset with no BC_BASE
     * field) working. */
    uint64_t bc_base = 0ull;
    if (buf_len >= 0x50u) {
        bc_base = rd_u64_le(buf + 0x48);
        /* Sanity: base must land inside the buffer. A zero base is
         * legal for synth fixtures (absolute mode). */
        if (bc_base != 0ull && bc_base >= buf_len) {
            LAGFX_WARN("metallib_extract: BC_BASE 0x%llx >= buf_len "
                       "%zu — falling back to absolute OFFT[2]",
                       (unsigned long long)bc_base, buf_len);
            bc_base = 0ull;
        }
    }

    /* At the FET offset: a u32 entry count. Followed by
     * <entry_count> entries, each prefixed by a u32 entry_size
     * (which includes the size field itself) and terminated by an
     * ENDT tag inside the body. */
    if (fet_off + 4u > buf_len) {
        LAGFX_ERR("metallib_extract: FET header truncated");
        return LAGFX_ERR_INVALID_ARG;
    }
    uint32_t entry_count = rd_u32_le(buf + fet_off);
    LAGFX_LOG("metallib_extract: FET @ 0x%llx, entry_count=%u, "
              "BC_BASE=0x%llx",
              (unsigned long long)fet_off, entry_count,
              (unsigned long long)bc_base);

    /* Sanity-cap the entry count — anything past a few hundred
     * is almost certainly a parse error on a corrupt metallib. */
    const uint32_t kMaxEntries = 1024u;
    if (entry_count > kMaxEntries) {
        LAGFX_ERR("metallib_extract: entry_count=%u exceeds cap (%u)",
                  entry_count, kMaxEntries);
        return LAGFX_ERR_INVALID_ARG;
    }

    size_t pos = (size_t)fet_off + 4u;
    size_t produced = 0u;

    for (uint32_t e = 0; e < entry_count; ++e) {
        /* Need at least 4 bytes for the entry size prefix, and 4
         * more for the first tag inside the body. */
        if (pos + 8u > buf_len) {
            LAGFX_ERR("metallib_extract: entry %u size-prefix "
                      "truncated (pos=%zu buf_len=%zu)",
                      e, pos, buf_len);
            return LAGFX_ERR_INVALID_ARG;
        }

        uint32_t entry_size = rd_u32_le(buf + pos);
        /* entry_size includes the u32 itself, so the smallest
         * conceivable entry is 4 (size field only — but that's
         * degenerate; a real entry is at least size(4) + ENDT(4)
         * = 8). */
        if (entry_size < 8u || entry_size > buf_len - pos) {
            LAGFX_ERR("metallib_extract: entry %u size=%u invalid "
                      "(pos=%zu buf_len=%zu)",
                      e, entry_size, pos, buf_len);
            return LAGFX_ERR_INVALID_ARG;
        }

        size_t body_end = pos + entry_size;
        size_t bp       = pos + 4u;

        lagfx_metallib_function_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.stage = LAGFX_METALLIB_STAGE_UNKNOWN;

        uint64_t bc_offt_rel = 0ull;
        uint64_t bc_mdsz     = 0ull;
        bool got_offt = false;
        bool got_mdsz = false;
        bool entry_ended = false;

        while (!entry_ended && bp + 4u <= body_end) {
            const uint8_t *tagp = buf + bp;

            /* ENDT has no length field — just a 4-byte tag. */
            if (tag_eq(tagp, "ENDT")) {
                entry_ended = true;
                bp += 4u;
                break;
            }

            /* Every other tag is followed by a u16 length. */
            if (bp + 6u > body_end) {
                LAGFX_ERR("metallib_extract: entry %u tag "
                          "%c%c%c%c length field overruns entry",
                          e, tagp[0], tagp[1], tagp[2], tagp[3]);
                return LAGFX_ERR_INVALID_ARG;
            }
            uint16_t flen = rd_u16_le(buf + bp + 4u);

            /* Length must fit inside the entry body. */
            if ((size_t)flen > body_end - (bp + 6u)) {
                LAGFX_ERR("metallib_extract: entry %u tag "
                          "%c%c%c%c len=%u overflows entry "
                          "(body_end=%zu bp=%zu)",
                          e, tagp[0], tagp[1], tagp[2], tagp[3],
                          (unsigned)flen, body_end, bp);
                return LAGFX_ERR_INVALID_ARG;
            }

            const uint8_t *fdata = buf + bp + 6u;

            if (tag_eq(tagp, "NAME")) {
                /* Copy up to name[]-1 bytes, NUL-terminate.
                 * Apple's NAME payloads already include a trailing
                 * NUL inside the length, so strncpy-style truncate
                 * is safe either way. */
                size_t n = flen;
                if (n > 0u && fdata[n - 1u] == 0u) {
                    /* Strip the payload's trailing NUL. */
                    n -= 1u;
                }
                if (n >= sizeof(rec.name)) {
                    n = sizeof(rec.name) - 1u;
                }
                memcpy(rec.name, fdata, n);
                rec.name[n] = '\0';
            } else if (tag_eq(tagp, "TYPE") && flen >= 1u) {
                rec.stage_raw = fdata[0];
                /* Stage enum as observed in
                 * tests/fixtures/default.metallib (Apple MTLB v1.2.9):
                 *   0 = vertex, 1 = fragment, 2 = kernel.
                 * This differs from the earlier Phase 3.C.2 guess
                 * (1/2/3) which was never validated against a real
                 * file. */
                switch (fdata[0]) {
                case 0: rec.stage = LAGFX_METALLIB_STAGE_VERTEX;   break;
                case 1: rec.stage = LAGFX_METALLIB_STAGE_FRAGMENT; break;
                case 2: rec.stage = LAGFX_METALLIB_STAGE_KERNEL;   break;
                default:
                    rec.stage = LAGFX_METALLIB_STAGE_UNKNOWN;
                    LAGFX_WARN("metallib_extract: entry %u stage_raw=%u "
                               "unknown [FIXME(phase-3c2-metallib-stages)]",
                               e, (unsigned)fdata[0]);
                }
            } else if (tag_eq(tagp, "OFFT")) {
                /* Real OFFT payload is 24 bytes (3 u64s). The
                 * third is the bitcode offset relative to
                 * BC_BASE (header 0x48). The synth fixture uses an
                 * 8-byte absolute offset; accept both. */
                if (flen >= 24u) {
                    bc_offt_rel = rd_u64_le(fdata + 16u);
                } else if (flen >= 8u) {
                    bc_offt_rel = rd_u64_le(fdata);
                } else {
                    LAGFX_WARN("metallib_extract: entry %u OFFT "
                               "len=%u too small", e, (unsigned)flen);
                    bc_offt_rel = 0ull;
                }
                got_offt = true;
            } else if (tag_eq(tagp, "MDSZ") && flen >= 8u) {
                bc_mdsz = rd_u64_le(fdata);
                got_mdsz = true;
            } else if (tag_eq(tagp, "HASH")
                    || tag_eq(tagp, "VERS")
                    || tag_eq(tagp, "RFLT")
                    || tag_eq(tagp, "RBUF")) {
                /* Known-but-not-interesting at the Phase 3.C.2
                 * scaffold. Silently skip. */
            } else {
                /* Unknown tag — log and skip per the "lenient
                 * against unknown tags" policy. */
                LAGFX_WARN("metallib_extract: entry %u unknown tag "
                           "%02x%02x%02x%02x len=%u (skipping) "
                           "[FIXME(phase-3c2-unknown-tag)]",
                           e, tagp[0], tagp[1], tagp[2], tagp[3],
                           (unsigned)flen);
            }

            bp += 6u + (size_t)flen;
        }

        if (!entry_ended) {
            LAGFX_ERR("metallib_extract: entry %u truncated "
                      "(no ENDT inside body)", e);
            return LAGFX_ERR_INVALID_ARG;
        }

        /* Resolve the bitcode payload. Absolute file offset is
         * bc_base + bc_offt_rel. If either OFFT or MDSZ is missing
         * or the combined extent exceeds the buffer, emit the
         * record with bitcode=NULL so diagnostic callers can still
         * see the function name/stage. */
        if (got_offt && got_mdsz) {
            uint64_t abs_off = bc_base + bc_offt_rel;
            if (abs_off <= (uint64_t)buf_len
                && bc_mdsz <= (uint64_t)buf_len - abs_off) {
                rec.bitcode     = buf + (size_t)abs_off;
                rec.bitcode_len = (size_t)bc_mdsz;
            } else {
                LAGFX_WARN("metallib_extract: entry %u bitcode "
                           "unresolvable (abs_off=%llu mdsz=%llu "
                           "buflen=%zu)",
                           e, (unsigned long long)abs_off,
                           (unsigned long long)bc_mdsz, buf_len);
                rec.bitcode     = NULL;
                rec.bitcode_len = 0u;
            }
        } else {
            LAGFX_WARN("metallib_extract: entry %u missing OFFT or "
                       "MDSZ (got_offt=%d got_mdsz=%d)",
                       e, (int)got_offt, (int)got_mdsz);
            rec.bitcode     = NULL;
            rec.bitcode_len = 0u;
        }

        if (produced < out_capacity && out_funcs) {
            out_funcs[produced] = rec;
        }
        produced++;

        /* Advance to the next entry regardless of where the inner
         * walker ended up — the entry_size framing is authoritative. */
        pos = body_end;
    }

    *out_count = produced;
    LAGFX_LOG("metallib_extract: produced=%zu (capacity=%zu)",
              produced, out_capacity);
    return LAGFX_OK;
}
