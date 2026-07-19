/*
 * libapplegfx-vulkan — guarded-loop translation regression guard
 * tests/air2spv-guarded-loop-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * REGRESSION GUARD for the 4-block "guarded rotated loop" — the shape
 * `xcrun metal` emits for a `while (cond) {...}` (or a for-loop with a
 * runtime trip-count guard) whose FIRST iteration can be skipped.
 *
 * THE GAP: detect_simple_loop() only matched the PLAIN rotated loop
 * (entry ends in an UNCONDITIONAL br; the merge block has no phis).
 * A guarded loop has (a) an ENTRY GUARD — the entry block ends in a
 * CONDITIONAL br into {header, merge} — and (b) LOOP-CLOSING PHIs in the
 * merge block (a value that differs depending on whether the loop ran).
 * Those two features made the plain-loop detector reject it, so it fell
 * through to the linear single-block path, which dropped the BR/PHI
 * terminators and emitted loop-body math that referenced unbound phi
 * value-ids → spirv-val "FMul operand ... must be of Result Type".
 *
 * THE FIX: detect_guarded_loop() + translate_body_guarded_loop() emit
 * STRUCTURED SPIR-V — the entry guard becomes OpSelectionMerge +
 * OpBranchConditional, the loop becomes OpLoopMerge with a dedicated
 * continue + loop-exit landing pad, and the after-loop block's
 * loop-closing phis become OpPhi with predecessors {entry guard-skip
 * edge, loop-exit edge}.
 *
 * THE FIXTURE: tests/fixtures/while_fragment.air.bc, compiled with
 * `xcrun metal`:
 *     float v = in.uv.x; int n = 0;
 *     while (v > 0.01 && n < 16) { v *= 0.5; n++; }
 *     return float4(float(n)/16.0, v, 0.0, 1.0);
 * Apple lowers this to a guarded rotated loop: entry guard br on
 * `v > 0.01`, a header block with two PHIs (v + n) and a self back-edge,
 * and a merge block with two LOOP-CLOSING PHIs (the post-loop v and n).
 *
 * THE ASSERT (toolchain-independent — walks the SPIR-V word stream):
 *   - >=1 OpLoopMerge       (the loop was structured)
 *   - >=1 OpSelectionMerge  (the ENTRY GUARD — distinguishes the guarded
 *                            loop from the plain rotated loop, whose entry
 *                            br is unconditional → no OpSelectionMerge)
 *   - >=4 OpPhi             (2 header-carried + 2 loop-closing merge phis)
 *   - >=2 OpBranchConditional (the entry guard + the latch test)
 *   - spirv-val, if present, must accept the module.
 *
 * RED ON REVERT: with detect_guarded_loop() removed (or forced to fail),
 * the fixture falls to the linear path: OpLoopMerge/OpSelectionMerge drop
 * to 0, the merge phis vanish, and spirv-val rejects the module (the
 * dropped-terminator / unbound-phi bug).
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
_Static_assert(LAGFX_SPV_OP_SELECTION_MERGE == 247, "OpSelectionMerge must be 247");
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

    char tmpl[] = "/tmp/lagfx_gloop_XXXXXX.spv";
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

static int test_guarded_loop(void) {
    const char *candidates[] = {
        "tests/fixtures/while_fragment.air.bc",
        "../tests/fixtures/while_fragment.air.bc",
        SRCDIR "/fixtures/while_fragment.air.bc",
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
        printf("FAIL: while_fragment.air.bc fixture not found\n");
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
    int n_sel    = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_SELECTION_MERGE);
    int n_phi    = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_PHI);
    int n_brcond = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_BRANCH_CONDITIONAL);
    printf("OPCODES: OpLoopMerge=%d OpSelectionMerge=%d OpPhi=%d "
           "OpBranchConditional=%d (spv %zu bytes)\n",
           n_loop, n_sel, n_phi, n_brcond, spv_sz);

    int rc = 0;

    if (n_loop < 1) {
        printf("FAIL: expected >=1 OpLoopMerge; got %d. The guarded loop "
               "was not structured (fell to the linear path).\n", n_loop);
        rc = 1;
    }
    if (n_sel < 1) {
        printf("FAIL: expected >=1 OpSelectionMerge (the ENTRY GUARD); got "
               "%d. The conditional entry guard was not emitted — the "
               "guarded-loop classifier regressed.\n", n_sel);
        rc = 1;
    }
    if (n_phi < 4) {
        printf("FAIL: expected >=4 OpPhi (2 header-carried + 2 loop-closing "
               "merge phis); got %d. The merge-block PHIs were dropped.\n",
               n_phi);
        rc = 1;
    }
    if (n_brcond < 2) {
        printf("FAIL: expected >=2 OpBranchConditional (entry guard + latch "
               "test); got %d.\n", n_brcond);
        rc = 1;
    }

    int val = spirv_val(spv, spv_sz);
    if (val < 0) {
        printf("FAIL: spirv-val rejected the translated module (guarded loop "
               "fell to the linear path → unbound merge-phi value-ids)\n");
        rc = 1;
    } else if (val == 0) {
        printf("NOTE: spirv-val unavailable; structural validation skipped "
               "(opcode assertions still enforced)\n");
    } else {
        printf("OK: spirv-val accepted the guarded-loop module\n");
    }

    if (rc == 0) {
        printf("PASS: guarded while-loop → OpSelectionMerge guard + "
               "OpLoopMerge + 4x OpPhi (incl. merge-closing) + 2x "
               "OpBranchConditional; spirv-val clean\n");
    }

    free(spv);
    lagfx_air_module_free(m);
    free(air);
    return rc;
}

int main(void) {
    return test_guarded_loop();
}
