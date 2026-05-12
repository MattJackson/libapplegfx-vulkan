/*
 * libapplegfx-vulkan — CmdExecIndirect2 payload parser tests
 * tests/handler-cmdexecindirect2-parse.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Small testable unit: validates outer-payload parsing for opcode 0x37
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

static int test_task_id_parsing(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[8] = {0};
    payload[0] = 5u;  /* task_id = 5 */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x37,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_compute_exec_cmdbuf(p, &hdr);
    free(p);
    printf("%s: task_id parsing\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_query_mode(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[8] = {0};  /* Minimal query: task_id only */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x37,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_compute_exec_cmdbuf(p, &hdr);
    free(p);
    printf("%s: query mode (minimal payload)\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_empty_completion(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[20] = {0};
    ((uint32_t*)payload)[0] = 0;   /* task_id */
    ((uint32_t*)payload)[1] = 0;   /* reserved */
    ((uint32_t*)payload)[2] = 0;   /* descriptor_count = 0 */
    ((uint32_t*)payload)[3] = 0;   /* resource_count = 0 */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x37,
        .length = 32u,
        .stamp = 1u,
        .payload_size = 20u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_compute_exec_cmdbuf(p, &hdr);
    free(p);
    printf("%s: empty completion (no descriptors/resources)\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

int main(void) {
    int failures = 0;
    if (test_task_id_parsing()) failures++;
    if (test_query_mode()) failures++;
    if (test_empty_completion()) failures++;
    printf("\n%d failure(s)\n", failures);
    return failures;
}
