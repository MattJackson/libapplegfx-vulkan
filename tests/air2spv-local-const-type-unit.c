/*
 * libapplegfx-vulkan — function-local-constant operand-type resolution
 * tests/air2spv-local-const-type-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Regression guard for the AIR-type-index-0-vs-unknown sentinel bug
 * (the vertex-shader cluster root cause). The translator used
 * `type_index == 0` as its "unknown" marker, but index 0 is a LEGITIMATE
 * type (it is `float` in the compositor shaders). So a float-typed
 * value (type index 0) was read as untyped → CMP/SELECT/BINOP
 * mis-dispatch. clear's vertex stage (`(vid==1) ? 3.0 : -1.0`) is the
 * exemplar: its ternary lowered to `OpSelect %uint` over float consts
 * (loud spirv-val reject), or — with a naive local-const branch — the
 * SELECT was DROPPED entirely (silent-wrong). The fix
 * (LAGFX_AIR_TYPE_NONE = UINT32_MAX sentinel + a range-correct local-
 * const branch in value_air_type_idx) makes it emit `OpSelect %float`.
 *
 * The fixture is clear.metal's vertex stage compiled with `xcrun metal`.
 * Asserts: translate succeeds, at least one OpSelect is PRESENT (the
 * construct must not be dropped), and spirv-val ACCEPTS (the select must
 * be correctly typed — a mis-typed `%uint` select would be rejected).
 * "OpSelect present AND spirv-val clean" together prove correct emission.
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
#define OP_SELECT 169u
_Static_assert(LAGFX_SPV_OP_SELECT == 169, "OpSelect must be 169");

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

/* 1 = accepted, 0 = spirv-val unavailable, -1 = rejected. */
static int spirv_val(const uint8_t *blob, size_t sz) {
    const char *cands[] = { "/opt/homebrew/bin/spirv-val", "/usr/local/bin/spirv-val", NULL };
    const char *path = NULL;
    for (int i = 0; cands[i]; i++) if (access(cands[i], X_OK) == 0) { path = cands[i]; break; }
    if (!path) return 0;
    char tmpl[] = "/tmp/lagfx_lconst_XXXXXX.spv";
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

static int test_local_const_select(void) {
    const char *cands[] = {
        "tests/fixtures/clear_vertex_cmp_select.air.bc",
        "../tests/fixtures/clear_vertex_cmp_select.air.bc",
        SRCDIR "/fixtures/clear_vertex_cmp_select.air.bc",
        NULL,
    };
    uint8_t *air = NULL;
    size_t air_len = 0;
    for (int i = 0; cands[i]; i++) { air = slurp(cands[i], &air_len); if (air) break; }
    if (!air) { printf("FAIL: clear_vertex_cmp_select.air.bc fixture not found\n"); return 1; }

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
    int n_select = count_opcode(spv, spv_sz, OP_SELECT);
    printf("OpSelect count = %d (%zu spv bytes)\n", n_select, spv_sz);
    if (n_select < 1) {
        printf("FAIL: expected >=1 OpSelect — the ternary was DROPPED "
               "(float-type-index-0 read as unknown → select handler bailed)\n");
        rc = 1;
    }
    int val = spirv_val(spv, spv_sz);
    if (val < 0) {
        printf("FAIL: spirv-val rejected — the select is mis-typed "
               "(e.g. OpSelect %%uint over float operands)\n");
        rc = 1;
    } else if (val == 0) {
        printf("NOTE: spirv-val unavailable; OpSelect presence still checked\n");
    } else {
        printf("OK: spirv-val accepted clear's vertex stage\n");
    }
    if (rc == 0)
        printf("PASS: local-const float type (index 0) resolved correctly; "
               "real compositor shader (clear vertex) emits a typed OpSelect\n");

    free(spv); lagfx_air_module_free(m); free(air);
    return rc;
}

int main(void) {
    return test_local_const_select();
}
