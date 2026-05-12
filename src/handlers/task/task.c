/*
 * libapplegfx-vulkan — Task handler stubs (0x00, 0x01)
 * src/handlers/task/task.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../common/log.h"

lagfx_handler_status_t lagfx_task_define_task2(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDefineTask2 CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_task_delete_task(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdDeleteTask CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

