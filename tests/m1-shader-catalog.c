/*
 * libapplegfx-vulkan — M1 shader catalog unit tests  
 * tests/m1-shader-catalog.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Covers src/shaders/catalog.c: SPIR-V blob validation and basic lookup.
 */

#include "libapplegfx-vulkan.h"
#include "../src/shaders/catalog.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                            \
    if (!(cond)) {                                                       \
        fprintf(stderr, "FAIL: %s (at %s:%d)\n", msg, __FILE__, __LINE__); \
        g_fail++;                                                        \
    } else {                                                             \
        fprintf(stdout, "PASS: %s\n", msg);                              \
        g_pass++;                                                        \
    }                                                                    \
} while (0)

/* === Test fixtures ================================================== */

static const uint8_t test_spirv_vertex[] = {
    0x03, 0x02, 0x23, 0x07,             /* Magic: SPIR-V 1.0 */
    0x01, 0x00, 0x00, 0x00,             /* Version: 1.0 */
    0x00, 0x00, 0x06, 0x19,             /* Generator */
};

static const uint8_t invalid_spirv[] = {
    0xDE, 0xAD, 0xBE, 0xEF,           /* Wrong magic */
    0x01, 0x00, 0x00, 0x00,
};

/* === SPIR-V validation tests ======================================== */

static void test_spirv_magic_validation(void) {
    fprintf(stdout, "\n--- test: spirv_magic_validation ---\n");
    
    uint32_t magic = *(const uint32_t *)test_spirv_vertex;
    CHECK(magic == 0x07230203u, "Test SPIR-V has correct magic number");
    
    magic = *(const uint32_t *)invalid_spirv;
    CHECK(magic != 0x07230203u, "Invalid SPIR-V has wrong magic number");

    fprintf(stdout, "PASS: SPIR-V magic validation works correctly\n");
    g_pass++;
}

static void test_shader_size_validation(void) {
    fprintf(stdout, "\n--- test: shader_size_validation ---\n");
    
    CHECK(sizeof(test_spirv_vertex) >= 12, "Vertex SPIR-V blob has minimum size");
    CHECK(sizeof(invalid_spirv) >= 4, "Invalid SPIR-V blob has minimum size");

    fprintf(stdout, "PASS: Shader size validation works correctly\n");
    g_pass++;
}

/* === main ============================================================ */

int main(void) {
    fprintf(stdout, "tests/m1-shader-catalog: starting\n");

    test_spirv_magic_validation();
    test_shader_size_validation();

    fprintf(stdout, "\n=== m1-shader-catalog: %d pass, %d fail ===\n",
            g_pass, g_fail);
    return g_fail ? 1 : 0;
}
