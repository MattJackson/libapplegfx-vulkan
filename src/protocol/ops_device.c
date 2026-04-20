/*
 * libapplegfx-vulkan — device-domain opcode stubs
 * src/protocol/ops_device.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Phase 1.A.2 scaffold. These handlers are wired into the opcode
 * dispatch table but not yet implemented. The next agent fills them
 * in; until then they return OK so the completion path fires and
 * the guest doesn't hang.
 *
 * Priorities from phase-1a2-decoder-plan.md §4:
 *
 *   CmdGetDeviceInfo    (0x0a) P0 — required for metal-no-op
 *   CmdDefineTask2      (0x00) P0 — required; calls shell.create_task
 *   CmdDeleteTask       (0x01) P0 — pair with DefineTask2
 *   CmdMapMemory2       (0x02) P1 — kext maps ring backing
 *   CmdUnmapMemory      (0x03) P1 — pair with MapMemory2
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

/* TODO(Phase-1.A.3): hardcode a sane caps reply per §4.1: version=0,
 * max_tasks=64, max_fifos=16, feature_mask=0. Need to decide where
 * the reply blob lives (inline? via task memory? MMIO readback?) —
 * §9.1 flags this as an open format gap. */
lagfx_handler_status_t lagfx_op_get_device_info(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdGetDeviceInfo: TODO(P0) stamp=0x%08x", hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 taskID, u64 rootVA, u64 length,
 * u32 reserved}; call shell.create_task; record {taskID, shell_task,
 * base_va, length} in p->tasks. See §4.1 and §7.1. */
lagfx_handler_status_t lagfx_op_define_task2(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdDefineTask2: TODO(P0) stamp=0x%08x", hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 taskID}; look up p->tasks; call
 * shell.destroy_task; mark entry !live. */
lagfx_handler_status_t lagfx_op_delete_task(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdDeleteTask: TODO(P0) stamp=0x%08x", hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 taskID, u64 vm_offset, u32 read_only,
 * u32 range_count, APVMemoryRange ranges[count]}; call
 * shell.map_memory. See §4.2 P1 + §7.1 and command-buffer-format.md
 * §4 for variable-length array layout. */
lagfx_handler_status_t lagfx_op_map_memory2(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdMapMemory2: TODO(P1) stamp=0x%08x", hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}

/* TODO(Phase-1.A.3): parse {u32 taskID, u64 vm_offset, u64 length};
 * call shell.unmap_memory. */
lagfx_handler_status_t lagfx_op_unmap_memory(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr) {
    (void)p; (void)hdr;
    LAGFX_LOG("CmdUnmapMemory: TODO(P1) stamp=0x%08x", hdr ? hdr->stamp : 0u);
    return LAGFX_HANDLER_OK;
}
