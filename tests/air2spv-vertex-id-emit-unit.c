/*
 * libapplegfx-vulkan — Phase 4 reference emitter unit test (vertex_id)
 * tests/air2spv-vertex-id-emit-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Same shape as air2spv-position-emit-unit but for the vertex_id +
 * Position-output emitter. Confirms the spv_builder scaffolds for
 * input-side builtins (OpLoad / OpConvertUToF / OpCompositeConstruct)
 * also produce Vulkan-valid modules.
 */

#include "air2spv/emit_vertex_id.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#define SPV_MAGIC 0x07230203u

static int test_header_and_validity(void) {
    uint8_t *blob = NULL;
    size_t   sz   = 0u;
    if (lagfx_air2spv_emit_vertex_id_stub(&blob, &sz) != 0 || !blob) {
        printf("FAIL: emit failed\n");
        return 1;
    }
    if (sz < 20u) {
        printf("FAIL: blob too small (%zu < 20)\n", sz);
        free(blob);
        return 1;
    }
    uint32_t magic;
    memcpy(&magic, blob, sizeof(magic));
    if (magic != SPV_MAGIC) {
        printf("FAIL: bad magic 0x%08x\n", magic);
        free(blob);
        return 1;
    }
    printf("PASS: emit_vertex_id header (size=%zu)\n", sz);

    /* spirv-val if available. */
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
        printf("SKIP: spirv-val unavailable\n");
        free(blob);
        return 0;
    }

    char tmpl[] = "/tmp/lagfx_air2spv_vid_XXXXXX.spv";
    int fd = mkstemps(tmpl, 4);
    if (fd < 0) { printf("FAIL: mkstemps\n"); free(blob); return 1; }
    if ((size_t)write(fd, blob, sz) != sz) {
        printf("FAIL: write\n"); close(fd); unlink(tmpl); free(blob); return 1;
    }
    close(fd);
    pid_t pid = fork();
    if (pid < 0) { printf("FAIL: fork\n"); unlink(tmpl); free(blob); return 1; }
    if (pid == 0) {
        execl(spirv_val, "spirv-val", tmpl, (char *)NULL);
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
    printf("PASS: spirv-val accepted emit_vertex_id (%s)\n", spirv_val);
    return 0;
}

int main(void) {
    return test_header_and_validity() ? 1 : 0;
}
