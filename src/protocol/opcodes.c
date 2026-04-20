/*
 * libapplegfx-vulkan — opcode descriptor table (Phase 1.A.2)
 * src/protocol/opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * All 37 opcodes from command-buffer-format.md §3 are recognized by
 * name. Handlers are wired to the real implementations in ops_misc.c
 * (NOP, Debug) and stubs in ops_device.c / ops_queue.c / ops_cmdbuf.c
 * for P0/P1 that the next agent will fill in. P2 entries have
 * handler=NULL; the dispatcher falls through to the default log+ack
 * handler, matching the dylib's fail-open semantics
 * (command-buffer-format.md §6).
 */

#include "opcodes.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

#include <stddef.h>
#include <stdio.h>

/* Table is declared here; lookups use a small linear scan. 37 entries
 * fit easily in one cache line's worth of work — we're not on a hot
 * path yet. When the ring-drain is live, the inner loop likely wants
 * a 256-entry direct jump table keyed on opcode byte; that's a trivial
 * future refactor. */
static const lagfx_op_descriptor_t g_op_table[] = {
    /* --- Core / Task / Memory (0x00-0x0d) -------------------- */
    { LAGFX_OP_DEFINE_TASK2,         "CmdDefineTask2",
      LAGFX_PRIO_P0, 24, 24, lagfx_op_define_task2 },
    { LAGFX_OP_DELETE_TASK,          "CmdDeleteTask",
      LAGFX_PRIO_P0, 4,  4,  lagfx_op_delete_task },
    { LAGFX_OP_MAP_MEMORY2,          "CmdMapMemory2",
      LAGFX_PRIO_P1, 20, 0,  lagfx_op_map_memory2 },
    { LAGFX_OP_UNMAP_MEMORY,         "CmdUnmapMemory",
      LAGFX_PRIO_P1, 20, 20, lagfx_op_unmap_memory },
    { LAGFX_OP_DEFINE_CHILD_FIFO,    "CmdDefineChildFIFO",
      LAGFX_PRIO_P0, 16, 16, lagfx_op_define_child_fifo },
    { LAGFX_OP_DELETE_CHILD_FIFO,    "CmdDeleteChildFIFO",
      LAGFX_PRIO_P0, 4,  4,  lagfx_op_delete_child_fifo },
    { LAGFX_OP_INVALIDATE_RESOURCES, "CmdInvalidateResources",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISCARD_RESOURCES,    "CmdDiscardResources",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELETE_RESOURCE,      "CmdDeleteResource",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_REPLACE_PHYSICAL,     "CmdReplacePhysical",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_GET_DEVICE_INFO,      "CmdGetDeviceInfo",
      LAGFX_PRIO_P0, 0, 0, lagfx_op_get_device_info },
    { LAGFX_OP_GET_COMPUTE_INFO,     "CmdGetComputeInfo",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELAY,                "CmdDelay",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DEBUG,                "CmdDebug",
      LAGFX_PRIO_P1, 0, 0, lagfx_op_debug },

    /* --- NOP ------------------------------------------------- */
    { LAGFX_OP_NOP,                  "CmdNOP",
      LAGFX_PRIO_P0, 0, 0, lagfx_op_nop },

    /* --- Display (0x10-0x1a) — P2, deferred ------------------ */
    { LAGFX_OP_DISPLAY_ACK,              "CmdDisplayAck",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SET_PROPERTIES,   "CmdDisplaySetProperties",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SWAP_MAPPING,     "CmdDisplaySwapMapping",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_CURSOR_SHOW,      "CmdDisplayCursorShow",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_CURSOR_GLYPH,     "CmdDisplayCursorGlyph",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_TRANSACTION2_DEP, "CmdDisplayTransaction2_DEPRECATED",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_TRANSACTION3,     "CmdDisplayTransaction3",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SET_SHARED_PAGE,  "CmdDisplaySetSharedStatePage",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SLEEP_STATE,      "CmdDisplaySleepState",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS,"CmdDisplayCompositorParameters",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SET_ICC_PROFILE,  "CmdDisplaySetGuestICCProfile",
      LAGFX_PRIO_P2, 0, 0, NULL },

    /* --- Execution / Sync (0x20-0x26) ----------------------- */
    { LAGFX_OP_EXEC_INDIRECT2,        "CmdExecIndirect2",
      LAGFX_PRIO_P1, 0, 0, lagfx_op_exec_indirect2 },
    { LAGFX_OP_EXEC_INDIRECT3,        "CmdExecIndirect3",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SYNCHRONIZE_RESOURCES, "CmdSynchronizeResources",
      LAGFX_PRIO_P0, 0, 0, lagfx_op_synchronize_resources },
    { LAGFX_OP_SYNCHRONIZE_DISCARD,   "CmdSynchronizeAndDiscardResources",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SET_OBJECT_LIST,       "CmdSetObjectList",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SET_OBJECT_PLACEMENT,  "CmdSetObjectAndPlacementList",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELETE_IOSURFACE_BACKING, "CmdDeleteIOSurfaceBacking2",
      LAGFX_PRIO_P2, 0, 0, NULL },

    /* --- Heap / Resource (0x80-0x82) ------------------------ */
    { LAGFX_OP_HEAP_TEX_SIZE_ALIGN,     "CmdHeapTextureSizeAndAlign",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_RESET_RASTERIZATION_RATE,"CmdResetRasterizationRateMap",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELETE_SHARED_TEX_BACK,  "CmdDeleteSharedTextureBacking",
      LAGFX_PRIO_P2, 0, 0, NULL },
};

static const size_t g_op_table_count =
    sizeof(g_op_table) / sizeof(g_op_table[0]);

/* Compile-time assert: descriptor table matches the header constant. */
_Static_assert(sizeof(g_op_table) / sizeof(g_op_table[0]) ==
               LAGFX_OPCODE_COUNT,
               "opcode descriptor table size mismatch");

const lagfx_op_descriptor_t *lagfx_opcode_lookup(uint8_t opcode) {
    for (size_t i = 0; i < g_op_table_count; ++i) {
        if (g_op_table[i].opcode == opcode) {
            return &g_op_table[i];
        }
    }
    return NULL;
}

const char *lagfx_opcode_name(uint8_t opcode) {
    const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(opcode);
    if (d) {
        return d->name;
    }
    /* Phase 1.A.2 is single-threaded (see protocol.h header comment),
     * so a file-static buffer is safe. */
    static char unknown_buf[24];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%02x)", opcode);
    return unknown_buf;
}

size_t lagfx_opcode_table_size(void) {
    return g_op_table_count;
}

const lagfx_op_descriptor_t *lagfx_opcode_table_entry(size_t index) {
    if (index >= g_op_table_count) {
        return NULL;
    }
    return &g_op_table[index];
}

lagfx_handler_status_t lagfx_op_default_handler(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }
    /* Fail-open: log it, return success so completion stamps still
     * flow. Per command-buffer-format.md §6, Apple's dylib silently
     * drops unknown opcodes and never writes a stamp — but during
     * bring-up we prefer to ack so the guest doesn't hang waiting
     * for a completion that will never come. Re-RE this once real
     * guest traffic is observed. */
    LAGFX_WARN("dispatch: unrecognized opcode 0x%02x (stamp 0x%08x, "
               "length %u) — log+ack fallback",
               hdr->opcode, hdr->stamp, (unsigned)hdr->length);
    return LAGFX_HANDLER_OK;
}
