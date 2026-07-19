/*
 * libapplegfx-vulkan — vertex float-plausibility scorer unit test
 * tests/vtx-plausibility-unit.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * The vertex-source selector scores a candidate buffer by the fraction
 * of nonzero dwords that read as sane finite floats. Text/poison slabs
 * (UTF-16 boot log, 0xFFFFFFFF fill) must score ~0; real float vertex
 * data must score ~100. Pins that discrimination.
 */

#include "handlers/compute/compute_draw_internal.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static void put_f32(uint8_t *b, float f) { memcpy(b, &f, 4); }
static void put_u32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }

int main(void) {
    uint8_t buf[256];

    /* Real float vertex data: pixel coords / NDC — all finite, sane. */
    memset(buf, 0, sizeof(buf));
    float reals[16] = {0,0, 1024,1024, 0,1024, 1280,0, 6,6, 23,29,
                       0.5f, 1.0f, 101.0f, -64.0f};
    for (int i = 0; i < 16; i++) put_f32(buf + i * 4, reals[i]);
    uint32_t s_real = lagfx_vtx_float_plausibility(buf, 16 * 4);
    assert(s_real == 100u);

    /* UTF-16LE ASCII text ("on was not...") — small ints as f32 = denormals. */
    memset(buf, 0, sizeof(buf));
    static const uint8_t text[] = {
        0x6f,0x00,0x6e,0x00, 0x20,0x00,0x77,0x00, 0x61,0x00,0x73,0x00,
        0x20,0x00,0x6e,0x00, 0x6f,0x00,0x74,0x00, 0x20,0x00,0x70,0x00,
    };
    memcpy(buf, text, sizeof(text));
    uint32_t s_text = lagfx_vtx_float_plausibility(buf, sizeof(text));
    assert(s_text <= 10u);

    /* 0xFFFFFFFF poison fill — reads as NaN. */
    memset(buf, 0xff, sizeof(buf));
    uint32_t s_poison = lagfx_vtx_float_plausibility(buf, 64);
    assert(s_poison == 0u);

    /* Denormal garbage (tiny nonzero ints) — implausible. */
    memset(buf, 0, sizeof(buf));
    for (int i = 0; i < 16; i++) put_u32(buf + i * 4, 0x00000660u + (uint32_t)i);
    uint32_t s_denorm = lagfx_vtx_float_plausibility(buf, 16 * 4);
    assert(s_denorm <= 10u);

    /* All zero → no nonzero samples → scored 0 (nothing to trust). */
    memset(buf, 0, sizeof(buf));
    assert(lagfx_vtx_float_plausibility(buf, 64) == 0u);

    printf("vtx-plausibility-unit: OK (real=%u text=%u poison=%u denorm=%u)\n",
           s_real, s_text, s_poison, s_denorm);
    return 0;
}
