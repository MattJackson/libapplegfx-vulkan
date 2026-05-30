/*
 * libapplegfx-vulkan — extract only: metallib → AIR bitcode files
 * examples/triangle/extract_only.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives src/air2spirv/metallib_extract to extract AIR bitcode from a
 * metallib file, writing one .air.bc file per function to the output
 * directory. No retargeting is performed — the extracted bitcode is
 * written exactly as found in the metallib. This tool is intended for
 * static analysis of captured metallibs (e.g., intrinsic cataloguing).
 *
 * Exit codes:
 *   0  — all functions extracted successfully.
 *   1  — usage / I/O error.
 *   2  — library reported an error (extraction failure).
 */

#include "libapplegfx-vulkan.h"
#include "air2spirv/metallib_extract.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    *out_len = 0;
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return NULL; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) { free(buf); return NULL; }
    *out_len = n;
    return buf;
}

static int write_all(const char *path, const uint8_t *data, size_t len) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return -1; }
    size_t n = fwrite(data, 1u, len, f);
    fclose(f);
    if (n != len) {
        fprintf(stderr, "write_all: short write on %s (%zu != %zu)\n",
                path, n, len);
        return -1;
    }
    return 0;
}

static const char *stage_str(lagfx_metallib_stage_t stage) {
    switch (stage) {
        case LAGFX_METALLIB_STAGE_VERTEX:   return "vertex";
        case LAGFX_METALLIB_STAGE_FRAGMENT: return "fragment";
        case LAGFX_METALLIB_STAGE_KERNEL:   return "kernel";
        default:                            return "unknown";
    }
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr,
                "usage: %s <input.metallib> <out_dir>\n"
                "  build info: %s\n",
                argv[0], lagfx_build_info());
        return 1;
    }
    const char *in_path = argv[1];
    const char *out_dir = argv[2];

    (void)mkdir(out_dir, 0755);

    size_t buf_len = 0;
    uint8_t *buf = slurp(in_path, &buf_len);
    if (!buf) {
        fprintf(stderr, "cannot read metallib at %s\n", in_path);
        return 1;
    }
    fprintf(stdout, "loaded %zu bytes from %s\n", buf_len, in_path);

    if (!lagfx_metallib_has_magic(buf, buf_len)) {
        fprintf(stderr, "not a metallib (no MTLB magic)\n");
        free(buf);
        return 2;
    }

    /* *out_count is the TRUE function count, which may exceed any fixed
     * capacity (CoreUI=17, SkyLight=31, MetalFX=76). The old fixed
     * fns[16] + `i < n` loop ran off the end of the array into
     * uninitialised stack memory on every real framework metallib. Probe
     * with capacity=0 to learn the count, then allocate exactly that
     * many records. See metallib_extract.h for the documented contract. */
    size_t n = 0;
    lagfx_status_t st = lagfx_metallib_extract_functions(
        buf, buf_len, NULL, 0u, &n);
    if (st != LAGFX_OK) {
        fprintf(stderr, "lagfx_metallib_extract_functions (probe) "
                "failed: %d\n", (int)st);
        free(buf);
        return 2;
    }
    if (n == 0u) {
        fprintf(stdout, "extracted 0 function(s)\n");
        free(buf);
        return 0;
    }

    lagfx_metallib_function_t *fns =
        (lagfx_metallib_function_t *)calloc(n, sizeof(*fns));
    if (!fns) {
        fprintf(stderr, "out of memory allocating %zu function records\n", n);
        free(buf);
        return 2;
    }
    size_t got = 0;
    st = lagfx_metallib_extract_functions(buf, buf_len, fns, n, &got);
    if (st != LAGFX_OK) {
        fprintf(stderr, "lagfx_metallib_extract_functions failed: %d\n",
                (int)st);
        free(fns);
        free(buf);
        return 2;
    }
    if (got > n) {
        got = n;
    }
    fprintf(stdout, "extracted %zu function(s)\n", got);

    int rc = 0;
    for (size_t i = 0; i < got; ++i) {
        const char *stage_s = stage_str(fns[i].stage);
        fprintf(stdout, "  fn[%zu]: name=%s stage=%s bitcode_len=%zu\n",
                i, fns[i].name, stage_s, fns[i].bitcode_len);

        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s.air.bc",
                 out_dir, fns[i].name);
        if (write_all(out_path, fns[i].bitcode, fns[i].bitcode_len) != 0) {
            rc = 2;
            continue;
        }
        fprintf(stdout, "    wrote %zu bytes to %s\n",
                fns[i].bitcode_len, out_path);
    }

    free(fns);
    free(buf);
    return rc;
}
