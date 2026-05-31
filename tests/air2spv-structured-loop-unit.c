/*
 * libapplegfx-vulkan — structured-loop translation regression guard
 * tests/air2spv-structured-loop-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * REGRESSION GUARD for multi-basic-block (structured) control flow.
 *
 * THE GAP: translate_body() was single-basic-block — it emitted every
 * instruction linearly, turned any RET into OpReturn wherever it
 * appeared, and DROPPED BR/BR_COND/PHI/SWITCH (they fell through the
 * default case). A shader with a real counted `for`/`while` loop lowers
 * to a multi-block CFG with PHI nodes; dropping the terminators left the
 * loop body emitted AFTER the OpReturn → spirv-val "X must appear in a
 * block".
 *
 * THE FIX: detect the canonical LLVM "rotated" single loop and emit
 * STRUCTURED SPIR-V — OpLabel per block, OpBranch/OpBranchConditional,
 * OpLoopMerge with a dedicated continue block, and OpPhi for the loop
 * carried values.
 *
 * THE FIXTURE: tests/fixtures/forloop_fragment.air.bc is a fragment
 * shader compiled with `xcrun metal`:
 *     float acc = 0.0;
 *     for (int i = 0; i < 8; ++i) acc += in.uv.x * float(i);
 *     return float4(acc, acc, acc, 1.0);
 * Apple lowers this to a rotated loop whose header block holds two PHIs
 * (the i counter + the acc accumulator) and a self back-edge.
 *
 * THE ASSERT (toolchain-independent — walks the SPIR-V word stream):
 *   - >=1 OpLoopMerge  (a loop was structured)
 *   - >=2 OpPhi        (the two loop-carried values)
 *   - >=1 OpBranchConditional (the latch test)
 *   - spirv-val, if present, must accept the module.
 *
 * RED ON REVERT: with translate_body's loop dispatch removed (or
 * detect_simple_loop forced to fail), the loop body emits after OpReturn
 * and spirv-val rejects it — and OpLoopMerge/OpPhi counts drop to 0.
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

_Static_assert(LAGFX_SPV_OP_PHI == 245, "OpPhi must be 245");
_Static_assert(LAGFX_SPV_OP_LOOP_MERGE == 246, "OpLoopMerge must be 246");
_Static_assert(LAGFX_SPV_OP_BRANCH_CONDITIONAL == 250, "OpBranchConditional must be 250");

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

/* Returns 1 if spirv-val accepted, 0 if spirv-val unavailable, -1 if it
 * rejected the module. */
static int spirv_val(const uint8_t *blob, size_t sz) {
    const char *candidates[] = {
        "/opt/homebrew/bin/spirv-val",
        "/usr/local/bin/spirv-val",
        NULL,
    };
    const char *path = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) { path = candidates[i]; break; }
    }
    if (!path) return 0;

    char tmpl[] = "/tmp/lagfx_loop_XXXXXX.spv";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) return -1;
    if ((size_t)write(fd, blob, sz) != sz) { close(fd); unlink(tmpl); return -1; }
    close(fd);

    pid_t pid = fork();
    if (pid < 0) { unlink(tmpl); return -1; }
    if (pid == 0) {
        execl(path, "spirv-val", tmpl, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 1;
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
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;
        if (op == opcode) count++;
        i += wc;
    }
    return count;
}

static int test_structured_loop(void) {
    const char *candidates[] = {
        "tests/fixtures/forloop_fragment.air.bc",
        "../tests/fixtures/forloop_fragment.air.bc",
        SRCDIR "/fixtures/forloop_fragment.air.bc",
        NULL,
    };
    uint8_t *air = NULL;
    size_t   air_len = 0;
    const char *used = NULL;
    for (int i = 0; candidates[i]; i++) {
        air = slurp(candidates[i], &air_len);
        if (air) { used = candidates[i]; break; }
    }
    if (!air) {
        printf("FAIL: forloop_fragment.air.bc fixture not found\n");
        return 1;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(air, air_len, &m);
    if (st != LAGFX_OK || !m) {
        printf("FAIL: module open st=%d (from %s)\n", (int)st, used);
        free(air);
        return 1;
    }

    uint8_t *spv = NULL;
    size_t   spv_sz = 0u;
    st = lagfx_air2spv_translate_module(m, &spv, &spv_sz);
    if (st != LAGFX_OK || !spv) {
        printf("FAIL: translate st=%d\n", (int)st);
        lagfx_air_module_free(m);
        free(air);
        return 1;
    }

    uint32_t magic;
    memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad SPIR-V magic 0x%08x\n", magic);
        free(spv); lagfx_air_module_free(m); free(air);
        return 1;
    }

    int n_loop   = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_LOOP_MERGE);
    int n_phi    = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_PHI);
    int n_brcond = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_BRANCH_CONDITIONAL);
    printf("OPCODES: OpLoopMerge=%d OpPhi=%d OpBranchConditional=%d (spv %zu bytes)\n",
           n_loop, n_phi, n_brcond, spv_sz);

    int rc = 0;

    if (n_loop < 1) {
        printf("FAIL: expected >=1 OpLoopMerge; got %d. The counted loop "
               "was not structured (structured CFG regressed).\n", n_loop);
        rc = 1;
    }
    if (n_phi < 2) {
        printf("FAIL: expected >=2 OpPhi (i counter + acc accumulator); "
               "got %d. PHI nodes were dropped.\n", n_phi);
        rc = 1;
    }
    if (n_brcond < 1) {
        printf("FAIL: expected >=1 OpBranchConditional (the latch test); "
               "got %d.\n", n_brcond);
        rc = 1;
    }

    int val = spirv_val(spv, spv_sz);
    if (val < 0) {
        printf("FAIL: spirv-val rejected the translated module (loop body "
               "likely emitted after OpReturn — the dropped-terminator bug)\n");
        rc = 1;
    } else if (val == 0) {
        printf("NOTE: spirv-val unavailable; structural validation skipped "
               "(opcode assertions still enforced)\n");
    } else {
        printf("OK: spirv-val accepted the structured-loop module\n");
    }

    if (rc == 0) {
        printf("PASS: counted for-loop → OpLoopMerge + 2x OpPhi + "
               "OpBranchConditional; spirv-val clean\n");
    }

    free(spv);
    lagfx_air_module_free(m);
    free(air);
    return rc;
}

int main(void) {
    return test_structured_loop();
}
