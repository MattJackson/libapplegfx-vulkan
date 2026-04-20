/*
 * libapplegfx-vulkan — metallib container parser (Phase 3.C.2)
 * src/air2spirv/metallib_extract.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of the MTLB tagged-field container walker. See
 * metallib_extract.h for the API and paravirt-re/metallib-analysis.md
 * §"Container format" for the spec references. The Phase 0 corpus at
 * paravirt-re/metallib/default.metallib is the known-good test fixture
 * (5 functions, 24,500 bytes, MTLB v1.2.9).
 *
 * Format walk (condensed):
 *
 *   offset 0x00: 'M' 'T' 'L' 'B'                              magic
 *   offset 0x04: u32 version (little-endian mangled 1.2.9 = 0x00028001)
 *   offset 0x18: u64 function-entry-table offset              (FET_OFFSET)
 *   ...
 *   FET_OFFSET: <entry count u32> <entry records>
 *     each entry record is a series of tagged fields:
 *       "NAME" u32 len | <utf-8 bytes>
 *       "TYPE" u32 len | u8 stage_raw
 *       "HASH" u32 len | 32 bytes
 *       "MDSZ" u32 len | u64 bitcode_size
 *       "OFFT" u32 len | u64 bitcode_file_offset
 *       "VERS" u32 len | ...
 *       "ENDT" u32 len | -                                   (end marker)
 *
 * Tolerances:
 *   - Unknown tags: logged, skipped (len-driven advance).
 *   - Missing fields on an entry: record emitted with the fields it
 *     had; malformed entries don't abort the whole walk.
 *   - bitcode extents that point past buf_len: the function entry is
 *     emitted with bitcode=NULL/len=0 and the walker continues.
 */

#include "metallib_extract.h"
#include "common/log.h"

#include <stdint.h>
#include <string.h>

/* --- Little-endian load helpers ----------------------------- */

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

    /* At the FET offset: a u32 entry count. Followed by
     * <entry_count> entries, each terminated by an ENDT tag. */
    if (fet_off + 4u > buf_len) {
        LAGFX_ERR("metallib_extract: FET header truncated");
        return LAGFX_ERR_INVALID_ARG;
    }
    uint32_t entry_count = rd_u32_le(buf + fet_off);
    LAGFX_LOG("metallib_extract: FET @ 0x%llx, entry_count=%u",
              (unsigned long long)fet_off, entry_count);

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

    for (uint32_t e = 0; e < entry_count && pos < buf_len; ++e) {
        lagfx_metallib_function_t rec;
        memset(&rec, 0, sizeof(rec));
        rec.stage = LAGFX_METALLIB_STAGE_UNKNOWN;

        uint64_t bc_offt = 0ull;
        uint64_t bc_mdsz = 0ull;
        bool got_offt = false;
        bool got_mdsz = false;
        bool entry_ended = false;

        while (!entry_ended && pos + 8u <= buf_len) {
            const uint8_t *tagp = buf + pos;
            uint32_t flen = rd_u32_le(buf + pos + 4u);

            /* Length sanity: must fit in the remaining buffer. */
            if (flen > buf_len - (pos + 8u)) {
                LAGFX_ERR("metallib_extract: entry %u tag "
                          "%c%c%c%c len=%u overflows buffer",
                          e, tagp[0], tagp[1], tagp[2], tagp[3],
                          flen);
                return LAGFX_ERR_INVALID_ARG;
            }

            const uint8_t *fdata = buf + pos + 8u;

            if (tag_eq(tagp, "NAME")) {
                /* Copy up to name[]-1 bytes, NUL-terminate. */
                size_t n = flen;
                if (n >= sizeof(rec.name)) {
                    n = sizeof(rec.name) - 1u;
                }
                memcpy(rec.name, fdata, n);
                rec.name[n] = '\0';
            } else if (tag_eq(tagp, "TYPE") && flen >= 1u) {
                rec.stage_raw = fdata[0];
                switch (fdata[0]) {
                case 1: rec.stage = LAGFX_METALLIB_STAGE_VERTEX;   break;
                case 2: rec.stage = LAGFX_METALLIB_STAGE_FRAGMENT; break;
                case 3: rec.stage = LAGFX_METALLIB_STAGE_KERNEL;   break;
                default:
                    rec.stage = LAGFX_METALLIB_STAGE_UNKNOWN;
                    LAGFX_WARN("metallib_extract: entry %u stage_raw=%u "
                               "unknown [FIXME(phase-3c2-metallib-stages)]",
                               e, (unsigned)fdata[0]);
                }
            } else if (tag_eq(tagp, "OFFT") && flen >= 8u) {
                bc_offt = rd_u64_le(fdata);
                got_offt = true;
            } else if (tag_eq(tagp, "MDSZ") && flen >= 8u) {
                bc_mdsz = rd_u64_le(fdata);
                got_mdsz = true;
            } else if (tag_eq(tagp, "ENDT")) {
                entry_ended = true;
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
                           flen);
            }

            pos += 8u + (size_t)flen;
        }

        if (!entry_ended) {
            LAGFX_ERR("metallib_extract: entry %u truncated "
                      "(no ENDT before EOF)", e);
            return LAGFX_ERR_INVALID_ARG;
        }

        /* Resolve the bitcode payload. If either OFFT or MDSZ is
         * missing / the combined extent exceeds the buffer, emit
         * the record with bitcode=NULL so diagnostic callers can
         * still see the function name/stage. */
        if (got_offt && got_mdsz
            && bc_offt + bc_mdsz <= (uint64_t)buf_len) {
            rec.bitcode     = buf + (size_t)bc_offt;
            rec.bitcode_len = (size_t)bc_mdsz;
        } else {
            LAGFX_WARN("metallib_extract: entry %u bitcode "
                       "unresolvable (offt=%llu mdsz=%llu buflen=%zu)",
                       e, (unsigned long long)bc_offt,
                       (unsigned long long)bc_mdsz, buf_len);
            rec.bitcode     = NULL;
            rec.bitcode_len = 0u;
        }

        if (produced < out_capacity && out_funcs) {
            out_funcs[produced] = rec;
        }
        produced++;
    }

    *out_count = produced;
    LAGFX_LOG("metallib_extract: produced=%zu (capacity=%zu)",
              produced, out_capacity);
    return LAGFX_OK;
}
