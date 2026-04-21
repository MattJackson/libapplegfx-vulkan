/*
 * libapplegfx-vulkan — triangle E2E: SPV entry-point rewriter tool
 * examples/triangle/spv_rewrite.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Post-processor for the SPV blobs LLVM's SPIR-V backend produces
 * from Apple's retargeted bitcode. The `llc -mtriple=spirv-...`
 * step emits OpenCL/Kernel-calling-convention SPIR-V (no
 * OpEntryPoint, CPacked decorations, OpCapability Linkage) that
 * vkCreateShaderModule either rejects or accepts-but-won't-pipeline.
 * This tool runs the post-processor in src/air2spirv/
 * spv_entrypoint_rewrite.c on each Apple-sourced .spv to produce a
 * Vulkan-shaped variant.
 *
 * Usage:
 *   triangle-spv-rewrite <in.spv> <out.spv> <entry-name> <stage>
 *   stage ∈ {vertex, fragment}
 *
 * Exit codes:
 *   0  — rewrite succeeded, out.spv written.
 *   1  — usage / I/O error.
 *   2  — library reported an error.
 *
 * Invoked by examples/triangle/build_spirv.sh after the llc step.
 */

#include "libapplegfx-vulkan.h"
#include "air2spirv/spv_entrypoint_rewrite.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
    if (argc != 5) {
        fprintf(stderr,
                "usage: %s <in.spv> <out.spv> <entry-name> "
                "<stage:vertex|fragment>\n"
                "  build info: %s\n",
                argv[0], lagfx_build_info());
        return 1;
    }
    const char *in_path    = argv[1];
    const char *out_path   = argv[2];
    const char *entry_name = argv[3];
    const char *stage_str  = argv[4];

    lagfx_spv_stage_t stage;
    if (strcmp(stage_str, "vertex") == 0) {
        stage = LAGFX_SPV_STAGE_VERTEX;
    } else if (strcmp(stage_str, "fragment") == 0) {
        stage = LAGFX_SPV_STAGE_FRAGMENT;
    } else {
        fprintf(stderr, "unknown stage '%s' (expected vertex|fragment)\n",
                stage_str);
        return 1;
    }

    size_t in_len = 0;
    uint8_t *in_buf = slurp(in_path, &in_len);
    if (!in_buf) {
        fprintf(stderr, "cannot read %s\n", in_path);
        return 1;
    }
    if (!lagfx_spv_has_magic(in_buf, in_len)) {
        fprintf(stderr, "%s: not a SPIR-V blob (magic missing)\n", in_path);
        free(in_buf);
        return 2;
    }
    fprintf(stdout, "loaded %zu bytes from %s\n", in_len, in_path);

    uint8_t *out_buf = NULL;
    size_t   out_len = 0;
    lagfx_status_t st = lagfx_spv_rewrite_entry_point(
        in_buf, in_len, entry_name, stage, &out_buf, &out_len);
    if (st != LAGFX_OK) {
        fprintf(stderr, "lagfx_spv_rewrite_entry_point failed: %d\n",
                (int)st);
        free(in_buf);
        return 2;
    }

    if (write_all(out_path, out_buf, out_len) != 0) {
        free(in_buf);
        free(out_buf);
        return 1;
    }
    fprintf(stdout, "wrote %zu bytes to %s (entry='%s' stage=%s)\n",
            out_len, out_path, entry_name, stage_str);

    free(in_buf);
    free(out_buf);
    return 0;
}
