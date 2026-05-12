/*
 * libapplegfx-vulkan — Memory handler tests
 * tests/handler-memory.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
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

static bool mock_read(void *op, uint64_t gpa, uint64_t len, void *out) {
    (void)op; (void)gpa; (void)len; (void)out;
    return true;
}

struct lagfx_device mock_dev = { .desc.shell.read_memory = mock_read };

static int test_map_memory2(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 32-byte payload for CmdMapMemory2 */
    uint8_t payload[32] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x02,
        .length = 44u,
        .stamp = 1u,
        .payload_size = 32u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_memory_map_memory2(p, &hdr);
    free(p);
    printf("%s: memory_map_memory2 stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_unmap_memory(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};  /* task_id */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x03,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_memory_unmap_memory(p, &hdr);
    free(p);
    printf("%s: memory_unmap_memory stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_define_child_fifo(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};  /* fifo_id */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x04,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_memory_define_child_fifo(p, &hdr);
    free(p);
    printf("%s: memory_define_child_fifo stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_map_memory_immediate(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 20-byte trailer: task_id(4) + vaBase(8) + vaLength(8) */
    uint8_t payload[20] = {0};
    ((uint32_t*)payload)[0] = 1u;   /* task_id */
    ((uint64_t*)payload)[1][0] = 0x2000ull;  /* vaBase */
    ((uint64_t*)payload)[1][1] = 0x1000ull;  /* vaLength */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x39,
        .length = 32u,
        .stamp = 1u,
        .payload_size = 20u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_memory_map_memory_immediate(p, &hdr);
    free(p);
    printf("%s: memory_map_memory_immediate parses trailer\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

int main(void) {
    int failures = 0;
    if (test_map_memory2()) failures++;
    if (test_unmap_memory()) failures++;
    if (test_define_child_fifo()) failures++;
    if (test_map_memory_immediate()) failures++;
    printf("\n%d failure(s)\n", failures);
    return failures;
}
