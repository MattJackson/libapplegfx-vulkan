/*
 * libapplegfx-vulkan — login-screen SkyLight compositor shader corpus
 * tests/air2spv-login-corpus-unit.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Red-on-revert regression corpus of the REAL shaders the macOS 15.7.5
 * login screen (full SkyLight compositor) submits. Captured live from a
 * phase-4 guest (LAGFX_PHASE_C_CAPTURE) and reduced to the 7 unique AIR
 * functions across pipelines 0xd / 0x14 / 0x1d / 0x20 / 0x23 / 0x28 / 0x2c:
 *
 *   ViewportToNDC          (vertex)   — fullscreen-quad viewport->NDC
 *   UberCompositeVertex    (vertex)   — composite layer quad
 *   Vfx                    (vertex)   — effect-layer quad
 *   ColorFill              (fragment) — solid-fill layer
 *   TextureCopy            (fragment) — 1-texture blit (wallpaper tile)
 *   UberCompositeFragment  (fragment) — sample + fast_clamp tint (2 variants)
 *
 * The wallpaper/composite pipelines 0x2c / 0x23 / 0x28 decompose entirely
 * into {UberCompositeVertex|Vfx, TextureCopy} — so this corpus is exactly
 * what must translate correctly for the login wallpaper to render, not
 * garble, once the compositor routes it.
 *
 * Each shader must:
 *   (A) translate to SPIR-V (module_open + air2spv_translate_module),
 *   (B) carry the SPIR-V magic,
 *   (C) pass spirv-val when the validator is present (else skipped),
 *   (D) emit the semantically-load-bearing op for its role:
 *         - texture fragments -> >=1 OpTypeImage AND >=1
 *           OpImageSampleImplicitLod (a dropped sample renders black),
 *         - vertices          -> a BuiltIn Position decoration (a dropped
 *           position renders nothing / degenerate geometry).
 *
 * (D) is what a mere spirv-val-clean bar misses: a translator regression
 * that silently drops the texture sample or the position store still
 * validates but composites WRONG. This corpus pins the real login shaders
 * against exactly that.
 */

#include "air2spv/translate.h"
#include "air/bitcode_reader.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#define SPV_MAGIC                     0x07230203u
#define OP_DECORATE                   71u
#define OP_TYPE_IMAGE                 25u
#define OP_IMAGE_SAMPLE_IMPLICIT_LOD  87u
#define SPV_DECOR_BUILTIN             11u   /* SpvDecorationBuiltIn */
#define SPV_BUILTIN_POSITION          0u    /* SpvBuiltInPosition */

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
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
        "/usr/bin/spirv-val",
        NULL,
    };
    const char *path = NULL;
    for (int i = 0; candidates[i]; i++)
        if (access(candidates[i], X_OK) == 0) { path = candidates[i]; break; }
    if (!path) return 0;

    char tmpl[] = "/tmp/lagfx_login_XXXXXX.spv";
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
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) return -1;
    return 1;
}

static int count_opcode(const uint8_t *blob, size_t sz, uint16_t opcode) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u) return -1;
    int n = 0;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == opcode) n++;
        i += wc;
    }
    return n;
}

/* Scan for `OpDecorate <id> BuiltIn Position`: OpDecorate (op 71) with
 * decoration operand == BuiltIn (11) and builtin operand == Position (0). */
static int has_position_builtin(const uint8_t *blob, size_t sz) {
    const uint32_t *w = (const uint32_t *)(const void *)blob;
    size_t nwords = sz / 4u;
    if (nwords < 5u) return 0;
    size_t i = 5u;
    while (i < nwords) {
        uint32_t hdr = w[i];
        uint16_t wc = (uint16_t)(hdr >> 16);
        uint16_t op = (uint16_t)(hdr & 0xFFFFu);
        if (wc == 0u) break;
        if (op == OP_DECORATE && wc >= 4u &&
            w[i + 2] == SPV_DECOR_BUILTIN && w[i + 3] == SPV_BUILTIN_POSITION)
            return 1;
        i += wc;
    }
    return 0;
}

typedef enum { ROLE_VERT, ROLE_FRAG_TEX, ROLE_FRAG_OTHER } role_t;

typedef struct { const char *name; const char *file; role_t role; } shader_t;

static const shader_t g_corpus[] = {
    { "ViewportToNDC",           "login_viewporttondc_vert.air.bc",   ROLE_VERT },
    { "UberCompositeVertex",     "login_ubercomposite_vert.air.bc",   ROLE_VERT },
    { "Vfx",                     "login_vfx_vert.air.bc",             ROLE_VERT },
    { "ColorFill",               "login_colorfill_frag.air.bc",       ROLE_FRAG_OTHER },
    { "TextureCopy",             "login_texturecopy_frag.air.bc",     ROLE_FRAG_TEX },
    { "UberCompositeFragment_a", "login_ubercomposite_frag_a.air.bc", ROLE_FRAG_TEX },
    { "UberCompositeFragment_b", "login_ubercomposite_frag_b.air.bc", ROLE_FRAG_TEX },
    /* GOAL-M2z additions — the two shader classes behind the 22k
     * vkCreateGraphicsPipelines(-13) failures at the login screen:
     *   VfxU11:      GEP through a `device float4*` runtime-array buffer;
     *                the pointee walk descended into the vector (result
     *                pointer float vs loaded v4float — spirv-val reject).
     *   TvcmXc_Isrc: single-colour-output fragment returning a STRUCT
     *                wrapper (stored the struct into the v4float Output);
     *                also carried a `[0 x struct]` dead artifact array
     *                (OpTypeArray length 0). */
    { "VfxU11",                  "login_vfxu11_vert.air.bc",          ROLE_VERT },
    { "TvcmXc_Isrc",             "login_tvcmxc_isrc_frag.air.bc",     ROLE_FRAG_TEX },
    /* KICKOFF-pipe0x31 additions — the last two broken login shaders
     * (pipeline 0x31, the login panel material):
     *   VfxXgb: `device uchar*` blob BITCAST to several typed views
     *           (struct Uniforms / float4 array) — modelled as multiple
     *           aliased Block variables on ONE descriptor binding, plus
     *           member-typed INSERTVAL undef fallbacks.
     *   Xgc:    73 KB fragment with full control flow (474 basic blocks);
     *           validity fixes: no undef-POINTER binds on unknown-base
     *           GEPs, operand/result type guards (binop/cmp/insertelt/
     *           bitcast widths), bool<->float conversion lowerings
     *           (OpSelect / OpFOrdNotEqual) in both the CAST and the
     *           air.convert intrinsic paths. */
    { "VfxXgb",                  "login_vfxxgb_vert.air.bc",          ROLE_VERT },
    { "Xgc",                     "login_xgc_frag.air.bc",             ROLE_FRAG_TEX },
};

static uint8_t *find_fixture(const char *basename, size_t *len) {
    char p[512];
    const char *dirs[] = { "tests/fixtures", "../tests/fixtures",
                           SRCDIR "/fixtures", NULL };
    for (int i = 0; dirs[i]; i++) {
        snprintf(p, sizeof(p), "%s/%s", dirs[i], basename);
        uint8_t *b = slurp(p, len);
        if (b) return b;
    }
    return NULL;
}

static int test_one(const shader_t *s) {
    size_t air_len = 0;
    uint8_t *air = find_fixture(s->file, &air_len);
    if (!air) { printf("FAIL[%s]: fixture %s not found\n", s->name, s->file); return 1; }

    lagfx_air_module_t *m = NULL;
    if (lagfx_air_module_open(air, air_len, &m) != LAGFX_OK || !m) {
        printf("FAIL[%s]: module open\n", s->name); free(air); return 1;
    }
    uint8_t *spv = NULL; size_t spv_sz = 0u;
    lagfx_status_t st = lagfx_air2spv_translate_module(m, &spv, &spv_sz);
    lagfx_air_module_free(m);
    free(air);
    if (st != LAGFX_OK || !spv || spv_sz == 0u) {
        printf("FAIL[%s]: translate st=%d\n", s->name, (int)st);
        if (spv) free(spv);
        return 1;
    }

    int rc = 0;
    uint32_t magic; memcpy(&magic, spv, sizeof(magic));
    if (magic != SPV_MAGIC) { printf("FAIL[%s]: bad magic 0x%08x\n", s->name, magic); rc = 1; }

    int val = spirv_val(spv, spv_sz);
    if (val < 0) { printf("FAIL[%s]: spirv-val rejected the module\n", s->name); rc = 1; }

    const char *vs = val < 0 ? "REJECTED" : (val == 0 ? "n/a" : "clean");

    if (s->role == ROLE_FRAG_TEX) {
        int n_img = count_opcode(spv, spv_sz, OP_TYPE_IMAGE);
        int n_smp = count_opcode(spv, spv_sz, OP_IMAGE_SAMPLE_IMPLICIT_LOD);
        if (n_img < 1) { printf("FAIL[%s]: no OpTypeImage (texture arg dropped)\n", s->name); rc = 1; }
        if (n_smp < 1) { printf("FAIL[%s]: no OpImageSampleImplicitLod (sample dropped -> black)\n", s->name); rc = 1; }
        printf("  %-24s frag-tex  img=%d sample=%d  spirv-val=%s  (%zuB)\n",
               s->name, n_img, n_smp, vs, spv_sz);
    } else if (s->role == ROLE_VERT) {
        if (!has_position_builtin(spv, spv_sz)) {
            printf("FAIL[%s]: no BuiltIn Position (vertex position dropped)\n", s->name); rc = 1;
        }
        printf("  %-24s vertex    Position=%s  spirv-val=%s  (%zuB)\n",
               s->name, has_position_builtin(spv, spv_sz) ? "yes" : "NO", vs, spv_sz);
    } else {
        printf("  %-24s frag      spirv-val=%s  (%zuB)\n", s->name, vs, spv_sz);
    }

    free(spv);
    return rc;
}

int main(void) {
    int fail = 0;
    size_t n = sizeof(g_corpus) / sizeof(g_corpus[0]);
    printf("login-screen SkyLight shader corpus (%zu shaders):\n", n);
    for (size_t i = 0; i < n; i++)
        fail |= test_one(&g_corpus[i]);
    if (fail) { printf("FAIL: login corpus regressed\n"); return 1; }
    printf("PASS: all %zu login shaders translate valid + role-correct\n", n);
    return 0;
}
