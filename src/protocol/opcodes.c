/*
 * libapplegfx-vulkan — opcode descriptor table (Phase 1.A.2)
 * src/protocol/opcodes.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * All 36 named opcodes from command-buffer-format.md §3 are recognized
 * by name. Handlers are wired to the real implementations in
 * ops_misc.c (NOP, Debug) and stubs in ops_device.c / ops_queue.c /
 * ops_cmdbuf.c for P0/P1 that the next agent will fill in. P2 entries
 * have handler=NULL; the dispatcher falls through to the default
 * log+ack handler, matching the dylib's fail-open semantics
 * (command-buffer-format.md §6).
 *
 * min_payload for the three gap-closed opcodes (0x04, 0x0a, 0x22) was
 * corrected against re-followup-spec-gaps.md:
 *   0x04 CmdDefineChildFIFO   — 4 bytes (§3.3); was 16 in the scaffold
 *   0x0a CmdGetDeviceInfo     — 12 bytes (§2.2)
 *   0x22 CmdSynchronizeResources — 8 bytes minimum (empty list) (§4.3)
 */

#include "opcodes.h"
#include "ops_display.h"
#include "ops_iosurface.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

#include <stddef.h>
#include <stdio.h>

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
      LAGFX_PRIO_P0, 4,  4,  lagfx_op_define_child_fifo },
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
      LAGFX_PRIO_P0, 12, 0, lagfx_op_get_device_info },
    { LAGFX_OP_GET_COMPUTE_INFO,     "CmdGetComputeInfo",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELAY,                "CmdDelay",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DEBUG,                "CmdDebug",
      LAGFX_PRIO_P1, 0, 0, lagfx_op_debug },

    /* --- NOP ------------------------------------------------- */
    { LAGFX_OP_NOP,                  "CmdNOP",
      LAGFX_PRIO_P0, 0, 0, lagfx_op_nop },

    /* --- Display (0x10-0x1a) — Phase 2.A promotes 0x10/0x12/0x16 to
     *     real handlers with PARTIAL-confidence layouts. Remaining
     *     display ops stay P2 log+ack stubs until Phase 3/4. -------- */
    { LAGFX_OP_DISPLAY_ACK,              "CmdDisplayAck",
      LAGFX_PRIO_P1, 8, 8, lagfx_op_display_ack },
    { LAGFX_OP_DISPLAY_SET_PROPERTIES,   "CmdDisplaySetProperties",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SWAP_MAPPING,     "CmdDisplaySwapMapping",
      LAGFX_PRIO_P1, 40, 40, lagfx_op_display_swap_mapping },
    { LAGFX_OP_DISPLAY_CURSOR_SHOW,      "CmdDisplayCursorShow",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_CURSOR_GLYPH,     "CmdDisplayCursorGlyph",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_TRANSACTION2_DEP, "CmdDisplayTransaction2_DEPRECATED",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_TRANSACTION3,     "CmdDisplayTransaction3",
      LAGFX_PRIO_P1, 12, 0, lagfx_op_display_transaction3 },
    { LAGFX_OP_DISPLAY_SET_SHARED_PAGE,  "CmdDisplaySetSharedStatePage",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_SLEEP_STATE,      "CmdDisplaySleepState",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS,"CmdDisplayCompositorParameters",
      LAGFX_PRIO_P2, 0, 0, lagfx_op_display_compositor_params },
    { LAGFX_OP_DISPLAY_SET_ICC_PROFILE,  "CmdDisplaySetGuestICCProfile",
      LAGFX_PRIO_P2, 0, 0, lagfx_op_display_set_icc_profile },

    /* --- Display-adjacent extended (0x1e) ------------------
     * A2 kext disasm (re-followup-spec-gaps.md §13.5):
     * "unknown display-adjacent". Log+ack until the dispatch
     * callsite is fully disassembled. */
    { LAGFX_OP_DISPLAY_EXT_1E,           "Unknown(0x1e)",
      LAGFX_PRIO_P2, 0, 0, NULL },

    /* --- Execution / Sync (0x20-0x26) ----------------------- */
    { LAGFX_OP_EXEC_INDIRECT2,        "CmdExecIndirect2",
      LAGFX_PRIO_P1, 0, 0, lagfx_op_exec_indirect2 },
    { LAGFX_OP_EXEC_INDIRECT3,        "CmdExecIndirect3",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SYNCHRONIZE_RESOURCES, "CmdSynchronizeResources",
      LAGFX_PRIO_P0, 8, 0, lagfx_op_synchronize_resources },
    { LAGFX_OP_SYNCHRONIZE_DISCARD,   "CmdSynchronizeAndDiscardResources",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SET_OBJECT_LIST,       "CmdSetObjectList",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_SET_OBJECT_PLACEMENT,  "CmdSetObjectAndPlacementList",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELETE_IOSURFACE_BACKING, "CmdDeleteIOSurfaceBacking2",
      LAGFX_PRIO_P2, 0, 0, NULL },

    /* --- IOSurface family (0x27-0x29) ----------------------
     * Conjectured opcode numbers + payload layouts (§14.5 /
     * phase-4-iosurface-videotoolbox-plan §3.3). Log+ack only
     * at M6; Phase 4 promotes to real VkImage-backed
     * lifecycle. Min/max payloads are left at 0 so short
     * captures still hit the handlers (the handlers
     * opportunistically decode whatever bytes arrive — fail-
     * open is the right answer during the §14.8 instrumentation
     * pass; we prefer "log whatever arrived" over refusing the
     * command). Note: 0x28 previously carried the unused
     * Unknown(0x28) entry (A2 kext disasm §13.5); §14.5
     * reclaims it for CmdIOSurfaceCreate. */
    { LAGFX_OP_DELETE_IOSURFACE,         "CmdDeleteIOSurface",
      LAGFX_PRIO_P2, 0, 0, lagfx_op_iosurface_delete },
    { LAGFX_OP_IOSURFACE_CREATE,         "CmdIOSurfaceCreate",
      LAGFX_PRIO_P2, 0, 0, lagfx_op_iosurface_create },
    { LAGFX_OP_IOSURFACE_UPDATE,         "CmdIOSurfaceUpdate",
      LAGFX_PRIO_P2, 0, 0, lagfx_op_iosurface_update },

    /* --- Heap / Resource (0x80-0x82) ------------------------ */
    { LAGFX_OP_HEAP_TEX_SIZE_ALIGN,     "CmdHeapTextureSizeAndAlign",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_RESET_RASTERIZATION_RATE,"CmdResetRasterizationRateMap",
      LAGFX_PRIO_P2, 0, 0, NULL },
    { LAGFX_OP_DELETE_SHARED_TEX_BACK,  "CmdDeleteSharedTextureBacking",
      LAGFX_PRIO_P2, 0, 0, NULL },

    /* --- M2+ extended range (0x30-0x41) — kext-only opcodes.
     *     Init-phase P0 — 0x30/0x33/0x38/0x3a must ack for
     *     `registerService()` to fire (prerequisite for
     *     MTLCreateSystemDefaultDevice).
     *     0x30/0x33/0x38 use the default stamp+log path (handler=NULL).
     *     0x3a fills the guest-supplied response page with a minimum
     *     viable device-info tuple stream. See paravirt-re §13.
     *
     *     The additional extended opcodes (0x31, 0x34..0x37, 0x39,
     *     0x3b, 0x3c, 0x41) are P2: A2's disasm enumerated their
     *     callsites (re-followup-spec-gaps.md §13.5) but payload
     *     shapes are unknown, so min/max_payload=0 and handler=NULL
     *     (default stamp+log path). Names are per §13.5 where
     *     inferred, Unknown(0xNN) otherwise. */
    { LAGFX_OP_DEFINE_CHILD_CHANNEL,     "CmdDefineChildChannel",
      LAGFX_PRIO_P0, 4,  4,  NULL },
    { LAGFX_OP_FREE_VIRTUAL_CHANNEL,     "VirtualChannelFree",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_SET_RESOURCE_HEAP,        "CmdSetResourceHeap",
      LAGFX_PRIO_P0, 12, 12, NULL },
    { LAGFX_OP_CHANNEL_EVENT_34,         "ChannelEvent34",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_CHANNEL_EVENT_35,         "ChannelEvent35",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_CHANNEL_EVENT_36,         "ChannelEvent36",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_CHANNEL_EVENT_37,         "ChannelEvent37",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_DEFINE_HOST_TASK,         "CmdDefineHostTask",
      LAGFX_PRIO_P0, 16, 16, NULL },
    { LAGFX_OP_ROOT_CHANNEL_INVALIDATE,  "RootChannelInvalidateTask",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_GET_DEVICE_INFO_2,        "CmdGetDeviceInfo2",
      LAGFX_PRIO_P0, 12, 12, lagfx_op_get_device_info_2 },
    { LAGFX_OP_UNKNOWN_3B,               "Unknown(0x3b)",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_UNKNOWN_3C,               "Unknown(0x3c)",
      LAGFX_PRIO_P2, 0,  0,  NULL },
    { LAGFX_OP_EXEC_INDIRECT_EXT_41,     "ExecIndirectExt41",
      LAGFX_PRIO_P2, 0,  0,  NULL },
};

static const size_t g_op_table_count =
    sizeof(g_op_table) / sizeof(g_op_table[0]);

/* Compile-time assert: descriptor table matches the header constant. */
_Static_assert(sizeof(g_op_table) / sizeof(g_op_table[0]) ==
               LAGFX_OPCODE_COUNT,
               "opcode descriptor table size mismatch");

const lagfx_op_descriptor_t *lagfx_opcode_lookup(uint16_t opcode) {
    for (size_t i = 0; i < g_op_table_count; ++i) {
        if (g_op_table[i].opcode == opcode) {
            return &g_op_table[i];
        }
    }
    return NULL;
}

const char *lagfx_opcode_name(uint16_t opcode) {
    const lagfx_op_descriptor_t *d = lagfx_opcode_lookup(opcode);
    if (d) {
        return d->name;
    }
    /* Phase 1.A.2 is single-threaded (see protocol.h header comment),
     * so a file-static buffer is safe. */
    static char unknown_buf[24];
    snprintf(unknown_buf, sizeof(unknown_buf), "Unknown(0x%04x)", opcode);
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
    LAGFX_WARN("dispatch: unrecognized opcode 0x%04x (stamp 0x%08x, "
               "length %u) — log+ack fallback",
               hdr->opcode, hdr->stamp, (unsigned)hdr->length);
    return LAGFX_HANDLER_OK;
}
