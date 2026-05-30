/*
 * libapplegfx-vulkan — typed vertex stage-in attribute input
 * tests/air2spv-vertex-attribute-input-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression guard for the vertex-input ABI fix. The translator used to
 * HARDCODE vertex arg 0 = vertex_id and bind it to
 * `OpLoad %uint gl_VertexIndex`. Real vertex shaders take stage-in
 * ATTRIBUTES (vec2-float params), not just [[vertex_id]]: color_fill's
 * vertex stage feeds its attribute into an OpVectorShuffle, which then
 * had a SCALAR uint operand ("The type of Vector 1 must be a vector
 * type"). The fix classifies each vertex arg (integer → BuiltIn
 * VertexIndex; float/vector → Location-decorated Input; pointer/etc. →
 * skip) and emits a correctly-typed Input variable + OpLoad per arg.
 *
 * Fixture: color_fill.metal's vertex stage (1 vec2 attribute). Asserts:
 * translate succeeds, an OpVectorShuffle is present (the construct that
 * was failing), and spirv-val ACCEPTS — a mis-typed scalar attribute
 * would be rejected. "shuffle present AND spirv-val clean" proves the
 * attribute is emitted as a typed vector Input.
 */

#include "air2spv/translate.h"
#include "air2spv/spv_builder.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPV_MAGIC 0x07230203u
#define OP_VECTOR_SHUFFLE 79u
_Static_assert(LAGFX_SPV_OP_VECTOR_SHUFFLE == 79, "OpVectorShuffle must be 79");

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

static int spirv_val(const uint8_t *blob, size_t sz) {
    const char *cands[] = { "/opt/homebrew/bin/spirv-val", "/usr/local/bin/spirv-val", NULL };
    const char *path = NULL;
    for (int i = 0; cands[i]; i++) if (access(cands[i], X_OK) == 0) { path = cands[i]; break; }
    if (!path) return 0;
    char tmpl[] = "/tmp/lagfx_vattr_XXXXXX.spv";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) return -1;
    if ((size_t)write(fd, blob, sz) != sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);
    pid_t pid = fork();
    if (pid < 0) { unlink(tmpl); return -1; }
    if (pid == 0) { execl(path, "spirv-val", tmpl, (char *)NULL); _exit(127); }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);
    return (WIFEXITED(status) && WEXITSTATUS(status) == 0) ? 1 : -1;
}

static int count_opcode(const uint8_t *blob, size_t sz, uint16_t opcode) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u) return -1;
    size_t i = 5u;
    int count = 0;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        if (wc == 0u) break;
        if ((uint16_t)(header & 0xFFFFu) == opcode) count++;
        i += wc;
    }
    return count;
}

static int test_vertex_attribute_input(void) {
    const char *cands[] = {
        "tests/fixtures/colorfill_vertex_attrib.air.bc",
        "../tests/fixtures/colorfill_vertex_attrib.air.bc",
        SRCDIR "/fixtures/colorfill_vertex_attrib.air.bc",
        NULL,
    };
    uint8_t *air = NULL;
    size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: colorfill_vertex_attrib.air.bc fixture not found\n"); return 1; }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL: module open\n"); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    if (lagfx_air2spv_translate_module(m, &spv, &spv_sz) != LAGFX_OK || !spv) {
        printf("FAIL: translate\n"); lagfx_air_module_free(m); free(air); return 1;
    }
    uint32_t magic; memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad magic 0x%08x\n", magic);
        free(spv); lagfx_air_module_free(m); free(air); return 1;
    }

    int rc = 0;
    int n_shuf = count_opcode(spv, spv_sz, OP_VECTOR_SHUFFLE);
    printf("OpVectorShuffle count = %d (%zu spv bytes)\n", n_shuf, spv_sz);
    if (n_shuf < 1) {
        printf("FAIL: expected >=1 OpVectorShuffle (color_fill vertex builds "
               "position via shuffles over its attribute)\n");
        rc = 1;
    }
    int val = spirv_val(spv, spv_sz);
    if (val < 0) {
        printf("FAIL: spirv-val rejected — the stage-in attribute is mis-typed "
               "(e.g. a scalar uint vid where a vec2 attribute is needed)\n");
        rc = 1;
    } else if (val == 0) {
        printf("NOTE: spirv-val unavailable; OpVectorShuffle presence still checked\n");
    } else {
        printf("OK: spirv-val accepted color_fill's vertex stage\n");
    }
    if (rc == 0)
        printf("PASS: vertex stage-in attribute emitted as a typed Location "
               "Input; shuffle operates on a real vector\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_vertex_attribute_input();
}
