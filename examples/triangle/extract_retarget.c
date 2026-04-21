/*
 * libapplegfx-vulkan — triangle E2E: metallib extract + retarget tool
 * examples/triangle/extract_retarget.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Drives src/air2spirv/ end-to-end against a REAL metallib. Reads the
 * metallib file named on argv[1], extracts every function's LLVM
 * Bitcode via lagfx_metallib_extract_functions, retargets each
 * function's triple from Apple's AIR form to the Vulkan SPIR-V form
 * via lagfx_bitcode_retarget_to_spirv, and writes one .bc file per
 * function to argv[2]/<name>.bc.
 *
 * The emitted .bc files are intended for the stage-3 LLVM SPIR-V
 * backend — `llc -mtriple=spirv64-unknown-vulkan1.3 -filetype=obj
 * <name>.bc -o <name>.spv`. That lowering step is external to the
 * library (no libLLVM linkage) and is driven by the companion shell
 * script examples/triangle/build_spirv.sh.
 *
 * Downstream of this tool (after llc) the sibling binary
 * triangle-spv-rewrite runs src/air2spirv/spv_entrypoint_rewrite.c
 * on each .spv to convert LLVM's OpenCL-flavour output (no
 * OpEntryPoint, CPacked decorations, LinkageAttributes, Linkage
 * capability) into a Vulkan-shaped variant that vkCreateShaderModule
 * accepts. Wiring lives in build_spirv.sh's stage 4 (post-llc).
 *
 * Exit codes:
 *   0  — all functions extracted + retargeted successfully.
 *   1  — usage / I/O error.
 *   2  — library reported an error (extraction or retarget failure).
 */

#include "libapplegfx-vulkan.h"
#include "air2spirv/metallib_extract.h"
#include "air2spirv/bitcode_retarget.h"

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

    lagfx_metallib_function_t fns[16];
    memset(fns, 0, sizeof(fns));
    size_t n = 0;
    lagfx_status_t st = lagfx_metallib_extract_functions(
        buf, buf_len, fns,
        sizeof(fns) / sizeof(fns[0]), &n);
    if (st != LAGFX_OK) {
        fprintf(stderr, "lagfx_metallib_extract_functions failed: %d\n",
                (int)st);
        free(buf);
        return 2;
    }
    fprintf(stdout, "extracted %zu function(s)\n", n);

    int rc = 0;
    for (size_t i = 0; i < n; ++i) {
        const char *stage_str = "unknown";
        switch (fns[i].stage) {
            case LAGFX_METALLIB_STAGE_VERTEX:   stage_str = "vertex";   break;
            case LAGFX_METALLIB_STAGE_FRAGMENT: stage_str = "fragment"; break;
            case LAGFX_METALLIB_STAGE_KERNEL:   stage_str = "kernel";   break;
            default: break;
        }
        fprintf(stdout, "  fn[%zu]: name=%s stage=%s bitcode_len=%zu\n",
                i, fns[i].name, stage_str, fns[i].bitcode_len);

        uint8_t *rt_buf = NULL;
        size_t   rt_len = 0;
        lagfx_status_t rs = lagfx_bitcode_retarget_to_spirv(
            fns[i].bitcode, fns[i].bitcode_len, &rt_buf, &rt_len);
        if (rs != LAGFX_OK) {
            fprintf(stderr, "    retarget failed on %s: %d\n",
                    fns[i].name, (int)rs);
            rc = 2;
            continue;
        }

        char out_path[1024];
        snprintf(out_path, sizeof(out_path), "%s/%s.bc",
                 out_dir, fns[i].name);
        if (write_all(out_path, rt_buf, rt_len) != 0) {
            free(rt_buf);
            rc = 2;
            continue;
        }
        fprintf(stdout, "    wrote %zu bytes to %s\n", rt_len, out_path);
        free(rt_buf);
    }

    free(buf);
    return rc;
}
