/*
 * libapplegfx-vulkan — non-void CALL value-numbering regression (blocker A)
 * tests/air2spv-nonvoid-call-valuenumber-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * REGRESSION GUARD for "blocker A" (the Stage-80 translator bug).
 *
 * THE BUG: a non-void CALL (e.g. a texture sample, which returns a
 * struct{vec4<float>, i8} in Metal AIR) was NOT counted as a value
 * producer — `inst_produces_value()` hardcoded CALL → false. In LLVM's
 * relative value numbering a non-void call DOES consume a value-id, so
 * skipping it desynced every downstream relative operand reference. The
 * `c = tex.sample(...); return c + float4(...)` body then resolved the
 * FADD's operands to the WRONG value-ids, deduced an integer type, and
 * emitted `OpIAdd %uint` instead of `OpFAdd %v4float` — which spirv-val
 * rejects ("Expected int scalar or vector type as operand: IAdd").
 *
 * Additionally, the array `inst_result_air_type[]` was WRITTEN by
 * instruction index but READ by value-number offset; those two index
 * spaces diverge exactly when a non-producing inst sits between
 * producers. The fix re-keys both sides by value-number.
 *
 * THE FIXTURE: tests/fixtures/texsample_add_fmain.air.bc is a fragment
 * shader compiled with `xcrun metal` — `float4 c = tex.sample(s, uv);
 * return c + float4(0.5,0,0,0);`. Its only add is a FLOAT add on the
 * sample result, so the translated SPIR-V MUST contain OpFAdd and MUST
 * NOT contain OpIAdd. This is the only test in the suite that exercises
 * a non-void CALL (gapped value numbering); triangle/vmain are gapless
 * and pass identically with or without the fix, so they cannot catch a
 * regression here.
 *
 * THE ASSERT is toolchain-independent: it walks the emitted SPIR-V word
 * stream directly (no spirv-dis dependency), counting OpFAdd (129) and
 * OpIAdd (128). spirv-val, if present, must also accept the module.
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

/* Pin the two opcode numbers this test reasons about to their canonical
 * SPIR-V core values (spirv/unified1/spirv.h: SpvOpIAdd/SpvOpFAdd). If a
 * future edit perturbs spv_builder.h these _Static_asserts fail at build
 * time, exactly like tests/air2spv-relational-opcodes-unit.c. */
_Static_assert(LAGFX_SPV_OP_IADD == 128, "OpIAdd must be 128");
_Static_assert(LAGFX_SPV_OP_FADD == 129, "OpFAdd must be 129");

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

    char tmpl[] = "/tmp/lagfx_nvcall_XXXXXX.spv";
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

/* Walk the SPIR-V word stream and count instructions with the given
 * opcode (low 16 bits of each instruction header; high 16 bits = word
 * count, used to step to the next instruction). */
static int count_opcode(const uint8_t *blob, size_t sz, uint16_t opcode) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u) return -1;               /* too small for a header */
    size_t i = 5u;                            /* skip the 5-word module header */
    int count = 0;
    while (i < nwords) {
        uint32_t header = w[i];
        uint16_t wc = (uint16_t)(header >> 16);
        uint16_t op = (uint16_t)(header & 0xFFFFu);
        if (wc == 0u) break;                  /* malformed — avoid infinite loop */
        if (op == opcode) count++;
        i += wc;
    }
    return count;
}

static int test_nonvoid_call_emits_fadd(void) {
    /* Fixture lives in the tracked tests/fixtures dir. Try a couple of
     * paths so the test runs both from the build dir and the source dir. */
    const char *candidates[] = {
        "tests/fixtures/texsample_add_fmain.air.bc",
        "../tests/fixtures/texsample_add_fmain.air.bc",
        SRCDIR "/fixtures/texsample_add_fmain.air.bc",
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
        printf("FAIL: texsample_add_fmain.air.bc fixture not found\n");
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

    int n_fadd = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_FADD);
    int n_iadd = count_opcode(spv, spv_sz, (uint16_t)LAGFX_SPV_OP_IADD);
    printf("OPCODES: OpFAdd=%d OpIAdd=%d (spv %zu bytes)\n", n_fadd, n_iadd, spv_sz);

    int rc = 0;

    /* THE blocker-A assertion: the sample-then-add body must lower to a
     * FLOAT add, never an integer add. */
    if (n_fadd < 1) {
        printf("FAIL: expected >=1 OpFAdd (float add on sample result); got %d. "
               "Non-void CALL value-numbering regressed.\n", n_fadd);
        rc = 1;
    }
    if (n_iadd != 0) {
        printf("FAIL: expected 0 OpIAdd; got %d. The FADD operand types "
               "resolved wrong (blocker A): a non-void CALL is not being "
               "counted as a value producer.\n", n_iadd);
        rc = 1;
    }

    /* And the module must be structurally valid. */
    int val = spirv_val(spv, spv_sz);
    if (val < 0) {
        printf("FAIL: spirv-val rejected the translated module\n");
        rc = 1;
    } else if (val == 0) {
        printf("NOTE: spirv-val unavailable; structural validation skipped "
               "(opcode assertion still enforced)\n");
    } else {
        printf("OK: spirv-val accepted the translated module\n");
    }

    if (rc == 0) {
        printf("PASS: non-void CALL → OpFAdd %%v4float (blocker A fixed); "
               "spirv-val clean\n");
    }

    free(spv);
    lagfx_air_module_free(m);
    free(air);
    return rc;
}

int main(void) {
    return test_nonvoid_call_emits_fadd();
}
