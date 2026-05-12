/*
 * libapplegfx-vulkan — Sync and Utility handler tests
 * tests/handler-sync-utility.c
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

static int test_sync_synchronize_resources(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 8-byte payload: stamp + reserved */
    uint8_t payload[8] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x22,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_sync_synchronize_resources(p, &hdr);
    free(p);
    printf("%s: sync_synchronize_resources stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_resource_set_placement(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 20-byte payload */
    uint8_t payload[20] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x26,
        .length = 32u,
        .stamp = 1u,
        .payload_size = 20u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_resource_set_placement(p, &hdr);
    free(p);
    printf("%s: resource_set_placement stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_resource_iosurface_create(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 32-byte payload */
    uint8_t payload[32] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x27,
        .length = 44u,
        .stamp = 1u,
        .payload_size = 32u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_resource_iosurface_create(p, &hdr);
    free(p);
    printf("%s: resource_iosurface_create stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_resource_iosurface_lookup(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    /* 8-byte payload: resource_id + flags */
    uint8_t payload[8] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x28,
        .length = 20u,
        .stamp = 1u,
        .payload_size = 8u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_resource_iosurface_lookup(p, &hdr);
    free(p);
    printf("%s: resource_iosurface_lookup stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_util_nop(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x29,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_util_nop(p, &hdr);
    free(p);
    printf("%s: util_nop returns OK\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

static int test_util_device_info(void) {
    struct lagfx_protocol *p = calloc(1, sizeof(*p));
    p->dev = &mock_dev;
    
    uint8_t payload[4] = {0};
    
    lagfx_cmd_header_t hdr = {
        .opcode = 0x2a,
        .length = 16u,
        .stamp = 1u,
        .payload_size = 4u,
        .payload = payload,
        .arg_count_8b = 0u,
    };
    
    int rc = lagfx_util_device_info(p, &hdr);
    free(p);
    printf("%s: util_device_info stub\n", rc == LAGFX_HANDLER_OK ? "PASS" : "FAIL");
    return rc != LAGFX_HANDLER_OK;
}

int main(void) {
    int failures = 0;
    if (test_sync_synchronize_resources()) failures++;
    if (test_resource_set_placement()) failures++;
    if (test_resource_iosurface_create()) failures++;
    if (test_resource_iosurface_lookup()) failures++;
    if (test_util_nop()) failures++;
    if (test_util_device_info()) failures++;
    printf("\n%d failure(s)\n", failures);
    return failures;
}
