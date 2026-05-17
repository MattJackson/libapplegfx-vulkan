/*
 * libapplegfx-vulkan — AIR to SPIR-V translation runner (Phase 3.C.2 M5)
 * src/air2spirv/shader_translate.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of shader_translate.h — glues the offline MSL→SPIR-V
 * pipeline (metallib_extract + bitcode_retarget + spv_signature_transform)
 * into a single in-process call suitable for the SetRenderPipelineState
 * (0x70) inner-op handler.
 *
 * This is Stage 65 of the M5 progress scale per
 * paravirt-re/library/offline-pipeline-map-2026-05-16.md §"Gap Analysis".
 * The underlying stages are:
 *
 *   1. metallib_extract — find named function's LLVM Bitcode in MTLB blob
 *      (lagfx_metallib_extract_functions at metallib_extract.h:92)
 *   2. bitcode_retarget — rewrite triple to spir64-unknown-vulkan1.3
 *      (lagfx_bitcode_retarget_to_spirv at bitcode_retarget.h:95)
 *   3. llc lowering — shell out to `llc` binary for stage 3, mirroring
 *      build_spirv.sh's subprocess invocation pattern.
 *   4. spv_signature_transform — Vulkan GLSL-style rewrite
 *      (lagfx_spv_signature_transform at spv_signature_transform.h:123)
 */

#define _DEFAULT_SOURCE
#include "shader_translate.h"
#include "air2spirv/metallib_extract.h"
#include "air2spirv/bitcode_retarget.h"
#include "air2spirv/spv_signature_transform.h"
#include "common/log.h"

#include <spawn.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* environ declaration for posix_spawn */
extern char **environ;

/* ---- Helper: find llc binary ----------------------------------- */

static char *find_llc_binary(void) {
    /* Prefer env LAGFX_LLC if set. */
    const char *env_llc = getenv("LAGFX_LLC");
    if (env_llc && env_llc[0]) {
        struct stat st;
        if (stat(env_llc, &st) == 0 && S_ISREG(st.st_mode)) {
            char *copy = strdup(env_llc);
            if (copy) return copy;
        }
    }

    /* llc resolution order:
     *   1. LAGFX_LLC env var (test override)
     *   2. /opt/homebrew/opt/llvm@20/bin/llc  (pinned to a working version)
     *   3. /opt/homebrew/opt/llvm/bin/llc      (fallback; may be newer & broken)
     *   4. `which llc` from $PATH
     * brew llvm 22.x SPIR-V backend rejects retargeted triangle bitcode
     * ("LLVM ERROR: Unable to meet SPIR-V requirements for this target");
     * llvm@20 is the Phase 3.C.2 reference version. See
     * paravirt-re/library/stage65-llvm-pinning-options-2026-05-17.md.
     */

    /* Try brew llvm@20 first (verified working for retargeted triangle BC). */
    static const char *pinned = "/opt/homebrew/opt/llvm@20/bin/llc";
    struct stat st;
    if (stat(pinned, &st) == 0 && S_ISREG(st.st_mode)) {
        return strdup(pinned);
    }

    /* Fallback to common brew path. */
    static const char *fallbacks[] = {
        "/opt/homebrew/opt/llvm/bin/llc",
        "/usr/bin/llc",
        NULL,
    };
    for (int i = 0; fallbacks[i]; ++i) {
        struct stat st;
        if (stat(fallbacks[i], &st) == 0 && S_ISREG(st.st_mode)) {
            return strdup(fallbacks[i]);
        }
    }

    /* Try system PATH via which(1). */
    FILE *f = popen("which llc 2>/dev/null", "r");
    if (f) {
        char buf[256];
        if (fgets(buf, sizeof(buf), f)) {
            size_t len = strlen(buf);
            while (len > 0 && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
                buf[--len] = '\0';
            }
            pclose(f);
            if (len > 0) {
                char *copy = strdup(buf);
                if (copy) return copy;
            }
        }
        pclose(f);
    }

    return NULL;
}

/* ---- Helper: run llc subprocess -------------------------------- */

static lagfx_status_t run_llc(const char *llc_path, const char *bc_in,
                              const char *spv_out) {
    pid_t pid;
    int status;
    char *argv[] = {
        (char *)llc_path,
        "-mtriple=spirv64-unknown-vulkan1.3",
        "-filetype=obj",
        (char *)bc_in,
        "-o",
        (char *)spv_out,
        NULL,
    };

    LAGFX_LOG("llc: %s -mtriple=spirv64-unknown-vulkan1.3 "
              "-filetype=obj %s -o %s",
              llc_path, bc_in, spv_out);

    /* Use posix_spawn for simplicity; no need for full execv setup. */
    int rc = posix_spawn(&pid, llc_path, NULL, NULL, argv, environ);
    if (rc != 0) {
        LAGFX_ERR("llc: posix_spawn failed: %s", strerror(rc));
        return LAGFX_ERR_PROTOCOL;
    }

    if (waitpid(pid, &status, 0) < 0) {
        LAGFX_ERR("llc: waitpid failed: %s", strerror(errno));
        return LAGFX_ERR_PROTOCOL;
    }

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        LAGFX_ERR("llc: non-zero exit (%d)", WEXITSTATUS(status));
        return LAGFX_ERR_PROTOCOL;
    }

    return LAGFX_OK;
}

/* ---- Public API ------------------------------------------------ */

lagfx_status_t lagfx_shader_translate_run(
    const uint8_t *metallib_data,
    size_t metallib_len,
    const char *function_name,
    lagfx_shader_stage_t stage_hint,
    lagfx_shader_translation_t *out) {

    if (!metallib_data || !function_name || !function_name[0] || !out) {
        LAGFX_ERR("shader_translate: NULL inputs or empty function name");
        out->spv_bytes = NULL;
        out->spv_len   = 0;
        out->stage     = LAGFX_SHADER_STAGE_VERTEX;
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Zero-initialize on all paths. */
    memset(out, 0, sizeof(*out));

    /* Step 1: metallib_extract — lagfx_metallib_extract_functions() at
     * metallib_extract.h:92–97 to find the named function's LLVM Bitcode.
     */
    lagfx_metallib_function_t funcs[32];
    memset(funcs, 0, sizeof(funcs));
    size_t n_funcs = 0;

    lagfx_status_t st = lagfx_metallib_extract_functions(
        metallib_data, metallib_len,
        funcs, sizeof(funcs) / sizeof(funcs[0]), &n_funcs);
    if (st != LAGFX_OK) {
        LAGFX_ERR("shader_translate: metallib_extract failed: %d", (int)st);
        return st;
    }

    /* Find the function by name. */
    size_t func_idx = SIZE_MAX;
    for (size_t i = 0; i < n_funcs; ++i) {
        if (strcmp(funcs[i].name, function_name) == 0) {
            func_idx = i;
            break;
        }
    }
    if (func_idx == SIZE_MAX) {
        LAGFX_ERR("shader_translate: function '%s' not found in metallib "
                  "(%zu functions available)",
                  function_name, n_funcs);
        return LAGFX_ERR_INVALID_ARG;
    }

    const lagfx_metallib_function_t *fn = &funcs[func_idx];
    if (!fn->bitcode || fn->bitcode_len == 0) {
        LAGFX_ERR("shader_translate: function '%s' has no bitcode",
                  function_name);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Step 2: bitcode_retarget — lagfx_bitcode_retarget_to_spirv() at
     * bitcode_retarget.h:95–99 to rewrite the AIR triple.
     */
    uint8_t *retargeted_bc = NULL;
    size_t retargeted_bc_len = 0;

    st = lagfx_bitcode_retarget_to_spirv(
        fn->bitcode, fn->bitcode_len,
        &retargeted_bc, &retargeted_bc_len);
    if (st != LAGFX_OK) {
        LAGFX_ERR("shader_translate: bitcode_retarget failed: %d", (int)st);
        return st;
    }

    /* Step 3: llc lowering — shell out to `llc` binary.
      * See find_llc_binary() for resolution order (llvm@20 preferred).
      * Write retargeted_bc to a tempfile, invoke llc, read tmpfile.spv.
      */
    char *llc_path = find_llc_binary();
    if (!llc_path) {
        LAGFX_ERR("shader_translate: llc binary not found; "
                  "set LAGFX_LLC or ensure 'llc' is in PATH");
        free(retargeted_bc);
        return LAGFX_ERR_BACKEND;
    }

    /* Create temp files. */
    char bc_temp[] = "/tmp/lagfx-translate-XXXXXX.bc";
    char spv_temp[] = "/tmp/lagfx-translate-XXXXXX.spv";
    int bc_fd = mkstemp(bc_temp);
    if (bc_fd < 0) {
        LAGFX_ERR("shader_translate: mkstemp(bc) failed: %s", strerror(errno));
        free(llc_path);
        free(retargeted_bc);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    ssize_t written = write(bc_fd, retargeted_bc, retargeted_bc_len);
    free(retargeted_bc);
    retargeted_bc = NULL;
    if ((size_t)written != retargeted_bc_len || written < 0) {
        LAGFX_ERR("shader_translate: short write to temp bc file");
        close(bc_fd);
        unlink(bc_temp);
        free(llc_path);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    if (close(bc_fd) != 0) {
        LAGFX_WARN("shader_translate: close(bc_temp) failed: %s", strerror(errno));
    }

    int spv_fd = mkstemp(spv_temp);
    if (spv_fd < 0) {
        LAGFX_ERR("shader_translate: mkstemp(spv) failed: %s", strerror(errno));
        unlink(bc_temp);
        free(llc_path);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* Run llc. */
    st = run_llc(llc_path, bc_temp, spv_temp);
    if (st != LAGFX_OK) {
        LAGFX_ERR("shader_translate: llc failed");
        close(spv_fd);
        unlink(bc_temp);
        unlink(spv_temp);
        free(llc_path);
        return st;
    }

    close(spv_fd);
    free(llc_path);

    /* Read tmpfile.spv into memory. */
    struct stat st_stat;
    if (stat(spv_temp, &st_stat) != 0) {
        LAGFX_ERR("shader_translate: fstat(tmp spv) failed");
        unlink(bc_temp);
        unlink(spv_temp);
        return LAGFX_ERR_PROTOCOL;
    }

    size_t spv_len = (size_t)st_stat.st_size;
    if (spv_len == 0) {
        LAGFX_ERR("shader_translate: llc produced empty output");
        unlink(bc_temp);
        unlink(spv_temp);
        return LAGFX_ERR_PROTOCOL;
    }

    FILE *f = fopen(spv_temp, "rb");
    if (!f) {
        LAGFX_ERR("shader_translate: fopen(tmp spv) failed");
        unlink(bc_temp);
        unlink(spv_temp);
        return LAGFX_ERR_PROTOCOL;
    }

    uint8_t *raw_spv = (uint8_t *)malloc(spv_len);
    if (!raw_spv) {
        LAGFX_ERR("shader_translate: malloc(raw_spv) failed");
        fclose(f);
        unlink(bc_temp);
        unlink(spv_temp);
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    size_t rd = fread(raw_spv, 1, spv_len, f);
    fclose(f);
    if (rd != spv_len) {
        LAGFX_ERR("shader_translate: short read from tmp spv");
        free(raw_spv);
        unlink(bc_temp);
        unlink(spv_temp);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Step 4: spv_signature_transform — lagfx_spv_signature_transform() at
     * spv_signature_transform.h:123–129 to produce Vulkan-ready SPIR-V.
     */
    uint8_t *final_spv = NULL;
    size_t final_spv_len = 0;

    st = lagfx_spv_signature_transform(
        raw_spv, spv_len, function_name,
        (lagfx_spv_stage_t)stage_hint,
        &final_spv, &final_spv_len);
    free(raw_spv);

    if (st != LAGFX_OK) {
        LAGFX_ERR("shader_translate: spv_signature_transform failed: %d",
                  (int)st);
        unlink(bc_temp);
        unlink(spv_temp);
        return st;
    }

    /* Step 5: Populate output. */
    out->spv_bytes = final_spv;
    out->spv_len   = final_spv_len;
    out->stage     = stage_hint;

    LAGFX_LOG("shader_translate: '%s' -> %zu bytes SPIR-V (stage=%d)",
              function_name, final_spv_len, (int)out->stage);

    /* Cleanup temp files. */
    unlink(bc_temp);
    unlink(spv_temp);

    return LAGFX_OK;
}

void lagfx_shader_translate_free(lagfx_shader_translation_t *translation) {
    if (!translation) return;
    free((void *)translation->spv_bytes);
    translation->spv_bytes = NULL;
    translation->spv_len   = 0;
    translation->stage     = LAGFX_SHADER_STAGE_VERTEX;
}
