/*
 * libapplegfx-vulkan — Phase 4 emit_texture_sample unit test
 * tests/air2spv-texture-sample-emit-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Validates the Pattern F (separate image + sampler texture-sample)
 * reference emitter at src/air2spv/emit_texture_sample.c.
 */

#include "air2spv/emit_texture_sample.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPV_MAGIC 0x07230203u

static int test_magic_header(void) {
    uint8_t *blob = NULL;
    size_t   sz   = 0u;
    if (lagfx_air2spv_emit_texture_sample_stub(&blob, &sz) != 0 || !blob) {
        printf("FAIL: emit failed\n"); return 1;
    }
    if (sz < 20u) { printf("FAIL: blob %zu\n", sz); free(blob); return 1; }
    uint32_t magic; memcpy(&magic, blob, sizeof(magic));
    if (magic != SPV_MAGIC) { printf("FAIL: magic 0x%08x\n", magic); free(blob); return 1; }
    uint32_t bound; memcpy(&bound, blob + 12, sizeof(bound));
    if (bound < 2u) { printf("FAIL: bound %u\n", bound); free(blob); return 1; }
    if (sz % 4u != 0u) { printf("FAIL: size %zu\n", sz); free(blob); return 1; }
    printf("PASS: emit_texture_sample header (size=%zu, bound=%u)\n", sz, bound);
    free(blob);
    return 0;
}

static int test_spirv_val(void) {
    uint8_t *blob = NULL;
    size_t   sz   = 0u;
    if (lagfx_air2spv_emit_texture_sample_stub(&blob, &sz) != 0 || !blob) {
        printf("FAIL: emit failed in spirv-val test\n"); return 1;
    }
    const char *candidates[] = {
        "/opt/homebrew/bin/spirv-val",
        "/usr/local/bin/spirv-val",
        NULL,
    };
    const char *spirv_val = NULL;
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], X_OK) == 0) { spirv_val = candidates[i]; break; }
    }
    if (!spirv_val) {
        printf("SKIP: spirv-val not available\n"); free(blob); return 0;
    }
    char tmpl[] = "/tmp/lagfx_air2spv_texsamp_XXXXXX.spv";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) { printf("FAIL: mkstemps\n"); free(blob); return 1; }
    if ((size_t)write(fd, blob, sz) != sz) {
        printf("FAIL: write\n"); close(fd); unlink(tmpl); free(blob); return 1;
    }
    close(fd);
    pid_t pid = fork();
    if (pid < 0) { printf("FAIL: fork\n"); unlink(tmpl); free(blob); return 1; }
    if (pid == 0) {
        execl(spirv_val, "spirv-val",
              "--target-env", "vulkan1.0",
              tmpl, (char *)NULL);
        _exit(127);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    unlink(tmpl);
    free(blob);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("FAIL: spirv-val rejected (exit=%d)\n",
               WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return 1;
    }
    printf("PASS: spirv-val accepted the module (%s, vulkan1.0)\n", spirv_val);
    return 0;
}

int main(void) {
    int rc = 0;
    rc |= test_magic_header();
    rc |= test_spirv_val();
    return rc ? 1 : 0;
}
