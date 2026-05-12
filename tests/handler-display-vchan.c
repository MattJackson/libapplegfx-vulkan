/*
 * libapplegfx-vulkan — M5 display vchan unit tests (NEW HANDLER STYLE)
 * tests/handler-display-vchan.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Tests for display virtual channel handlers (channels 5+)
 */

#include "libapplegfx-vulkan.h"
#include "../src/device.h"
#include "../src/protocol/state.h"
#include "../src/handlers/handlers.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline uint32_t vchan_le32(const uint8_t *b) {
    return (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
}

static inline void vchan_store_le32(uint8_t *b, uint32_t val) {
    b[0] = (uint8_t)(val & 0xff);
    b[1] = (uint8_t)((val >> 8) & 0xff);
    b[2] = (uint8_t)((val >> 16) & 0xff);
    b[3] = (uint8_t)((val >> 24) & 0xff);
}

static bool mock_read(void *op, uint64_t gpa, uint64_t len, void *out) {
    (void)op; (void)gpa; (void)len; (void)out;
    return true;
}

static bool mock_write(void *op, uint64_t gpa, uint64_t len, const void *in) {
    (void)op; (void)gpa; (void)len; (void)in;
    return true;
}

struct lagfx_device mock_dev = {
    .desc.shell.read_memory = mock_read,
    .desc.shell.write_memory = mock_write,
};

static int test_setup_shared_state(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[8] = {0};
    vchan_store_le32(payload + 0, 0u);   /* display_index */
    vchan_store_le32(payload + 4, 0x20000u);  /* ss_pfn */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x01,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_display_vchan_setup_shared_state(p, &hdr);
    free(p);
    printf("%s: vchan_setup_shared_state installs display\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_define_child_fifo(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[44] = {0};
    vchan_store_le32(payload + 24, 32u);  /* entry_count */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x04,
        .length = 56u,
        .stamp = 1u,
        .payload_size = 44u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_display_define_child_fifo(p, &hdr);
    free(p);
    printf("%s: display_define_child_fifo allocates ring\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_display_submit(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[8] = {0};
    vchan_store_le32(payload + 0, 0u);   /* display_index */
    vchan_store_le32(payload + 4, 0x10000u);  /* arg2 */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x02,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_display_vchan_display_submit(p, &hdr);
    free(p);
    printf("%s: vchan_display_submit probes framebuffer\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_cursor_show(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x13,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_display_cursor_show(p, &hdr);
    free(p);
    printf("%s: display_cursor_show stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_cursor_glyph(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x14,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_display_cursor_glyph(p, &hdr);
    free(p);
    printf("%s: display_cursor_glyph stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

int main(void) {
    int failures = 0;
    if (test_setup_shared_state()) failures++;
    if (test_define_child_fifo()) failures++;
    if (test_display_submit()) failures++;
    if (test_cursor_show()) failures++;
    if (test_cursor_glyph()) failures++;
    printf("\n%d failure(s)\n", failures);
    return failures;
}
