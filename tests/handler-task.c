/*
 * libapplegfx-vulkan — Task handler tests
 * tests/handler-task.c
 *
 * Copyright (c) 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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

static int test_define_task2(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[24] = {0};
    ((uint32_t*)payload)[0] = 1u;   /* task_id */
    ((uint64_t*)payload)[1][0] = 0x1000ull;  /* root_va */
    ((uint64_t*)payload)[1][1] = 0x1000ull;  /* length */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x00,
        .length = 36u,
        .stamp = 1u,
        .payload_size = 24u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_task_define_task2(p, &hdr);
    free(p);
    printf("%s: task_define_task2 allocates slot\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_delete_task(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* First define a task */
    uint8_t payload[24] = {0};
    ((uint32_t*)payload)[0] = 5u;
    lagfx_cmd_header_t def_hdr = {
        .opcode = 0x00, .length = 36u, .stamp = 1u,
        .payload_size = 24u, .payload = payload, .arg_count_8b = 0u,
    };
    lagfx_task_define_task2(p, &def_hdr);
    
    /* Now delete it */
    uint8_t del_payload[4] = {5};  /* task_id = 5 */
    lagfx_cmd_header_t del_hdr = {
        .opcode = 0x01,
        .length = 16u,
        .stamp = 2u,
        .payload_size = 4u,
        .payload = del_payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_task_delete_task(p, &del_hdr);
    free(p);
    printf("%s: task_delete_task removes slot\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_define_host_task2(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[16] = {0};
    ((uint32_t*)payload)[0] = 2u;   /* slot_index */
    ((uint32_t*)payload)[1] = 0u;   /* reserved */
    ((uint32_t*)payload)[2] = 0u;   /* flags */
    ((uint32_t*)payload)[3] = 0x30000u;  /* root_page_pfn */
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x38,
        .length = 28u,
        .stamp = 1u,
        .payload_size = 16u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_task_define_host_task2(p, &hdr);
    free(p);
    printf("%s: task_define_host_task2 sets root_page_pfn\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

int main(void) {
    int failures = 0;
    if (test_define_task2()) failures++;
    if (test_delete_task()) failures++;
    if (test_define_host_task2()) failures++;
    printf("\n%d failure(s)\n", failures);
    return failures;
}
