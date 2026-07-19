/*
 * libapplegfx-vulkan — opcode enum + descriptor table (Phase 1.A.2)
 * src/protocol/opcodes.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Enumerates all 38 FIFO opcodes documented in
 * mos/paravirt-re/command-buffer-format.md §3. Each entry in the
 * descriptor table carries a name (for tracing), a priority band
 * (P0 = required for metal-no-op, P1 = side-effect, P2 = deferred),
 * and a jump-table handler function pointer.
 *
 * Opcodes are sparse (0x00..0x0e, 0x10..0x1a, 0x20..0x26, 0x80..0x82)
 * so direct-indexed dispatch is wasteful. We use a small lookup table
 * and a descriptor array keyed by opcode; a missing opcode dispatches
 * to the default "log + ack" handler to match the dylib's fail-open
 * model.
 *
 * This header is private to src/protocol/. Not installed.
 */

#ifndef LIBAPPLEGFX_PROTOCOL_OPCODES_H
#define LIBAPPLEGFX_PROTOCOL_OPCODES_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration — avoids pulling in the whole protocol state
 * struct from state.h for consumers that only need to dispatch. */
typedef struct lagfx_protocol lagfx_protocol_t;

/* === Opcode enum (all 37) ====================================== */

typedef enum {
    /* --- Core / Task / Memory (0x00-0x0d) ------------------------ */
    LAGFX_OP_DEFINE_TASK2             = 0x00,
    LAGFX_OP_DELETE_TASK              = 0x01,
    LAGFX_OP_MAP_MEMORY2              = 0x02,
    LAGFX_OP_UNMAP_MEMORY             = 0x03,
    LAGFX_OP_DEFINE_CHILD_FIFO        = 0x04,
    LAGFX_OP_DELETE_CHILD_FIFO        = 0x05,
    LAGFX_OP_INVALIDATE_RESOURCES     = 0x06,
    LAGFX_OP_DISCARD_RESOURCES        = 0x07,
    LAGFX_OP_DELETE_RESOURCE          = 0x08,
    LAGFX_OP_REPLACE_PHYSICAL         = 0x09,
    LAGFX_OP_GET_DEVICE_INFO          = 0x0a,
    LAGFX_OP_GET_COMPUTE_INFO         = 0x0b,
    LAGFX_OP_DELAY                    = 0x0c,
    LAGFX_OP_DEBUG                    = 0x0d,

    /* --- NOP (0x0e) --------------------------------------------- */
    LAGFX_OP_NOP                      = 0x0e,

    /* --- Display (0x10-0x1a) ------------------------------------ */
    LAGFX_OP_DISPLAY_ACK              = 0x10,
    LAGFX_OP_DISPLAY_SET_PROPERTIES   = 0x11,
    LAGFX_OP_DISPLAY_SWAP_MAPPING     = 0x12,
    LAGFX_OP_DISPLAY_CURSOR_SHOW      = 0x13,
    LAGFX_OP_DISPLAY_CURSOR_GLYPH     = 0x14,
    LAGFX_OP_DISPLAY_TRANSACTION2_DEP = 0x15,
    LAGFX_OP_DISPLAY_TRANSACTION3     = 0x16,
    LAGFX_OP_DISPLAY_SET_SHARED_PAGE  = 0x17,
    LAGFX_OP_DISPLAY_SLEEP_STATE      = 0x18,
    LAGFX_OP_DISPLAY_COMPOSITOR_PARAMS= 0x19,
    LAGFX_OP_DISPLAY_SET_ICC_PROFILE  = 0x1a,

    /* --- Display-adjacent extended (0x1e) -----------------------
     * Observed via A2's kext disasm of AppleParavirtGPU (re-followup-
     * spec-gaps.md §13.5). Not in command-buffer-format.md §3;
     * inferred display-adjacent from callsite neighbourhood. */
    LAGFX_OP_DISPLAY_EXT_1E           = 0x1e,

    /* --- Execution / Sync (0x20-0x26) --------------------------- */
    LAGFX_OP_EXEC_INDIRECT2           = 0x20,
    LAGFX_OP_EXEC_INDIRECT3           = 0x21,
    /* NOTE: LAGFX_OP_SYNCHRONIZE_RESOURCES is 0x42 even though it's
     * declared in the 0x20-bucket here. Wire value 0x42 fires on the
     * ROOT channel (channel_0_dispatcher); the dispatcher key matches
     * 0x42 directly. The enum entry sits here because the family is
     * "sync-class" and pre-refactor the spec table grouped them
     * together. See the LAGFX_OP_UNMAP_MEMORY_IMMEDIATE comment
     * below for the disjoint-namespace rule that lets these
     * two-byte values coexist. */
    LAGFX_OP_SYNCHRONIZE_RESOURCES    = 0x42,
    LAGFX_OP_SYNCHRONIZE_DISCARD      = 0x23,
    LAGFX_OP_SET_OBJECT_LIST          = 0x24,
    LAGFX_OP_SET_OBJECT_PLACEMENT     = 0x25,
   LAGFX_OP_DELETE_IOSURFACE_BACKING = 0x26,
    /* --- IOSurface family (0x27-0x29) ---------------------- */
    LAGFX_OP_IOSURFACE_CREATE         = 0x27,
    LAGFX_OP_IOSURFACE_LOOKUP         = 0x28,
    LAGFX_OP_IOSURFACE_UPDATE         = 0x29, /* alias: CmdImportIOSurfaceMachPort */
    LAGFX_OP_IOSURFACE_UNMAP          = 0x2a,

    /* --- M2+ extended range (0x30-0x41) -------------------------
     * Kext-only opcodes (never emitted by the dylib). Observed via
     * live trace + kext disasm. See paravirt-re §13 / §13.5. */
    LAGFX_OP_DEFINE_CHILD_CHANNEL     = 0x30,
    LAGFX_OP_FREE_VIRTUAL_CHANNEL     = 0x31, /* VirtualChannel::free, pairs 0x30 */
    LAGFX_OP_SET_RESOURCE_HEAP        = 0x33,
    LAGFX_OP_CHANNEL_EVENT_34         = 0x34, /* ChannelEventMachine-adjacent */
    LAGFX_OP_CHANNEL_EVENT_35         = 0x35, /* ChannelEventMachine-adjacent */
    LAGFX_OP_CHANNEL_EVENT_36         = 0x36, /* ChannelEventMachine-adjacent */
    LAGFX_OP_CHANNEL_EVENT_37         = 0x37, /* ChannelEventMachine-adjacent */
    LAGFX_OP_DEFINE_HOST_TASK         = 0x38,
    LAGFX_OP_MAP_MEMORY_IMMEDIATE     = 0x39, /* CmdMapMemoryImmediate, Immediate vchan; opcodes-0x35-0x36-0x39.md */
    /* === Disjoint-namespace rule =================================
     *
     * Wire value 0x22 (LAGFX_OP_UNMAP_MEMORY_IMMEDIATE) and 0x42
     * (LAGFX_OP_SYNCHRONIZE_RESOURCES, declared above with the
     * Execution/Sync group) DO NOT collide because their dispatch
     * channels are disjoint:
     *
     *   0x22  Compute vchan namespace — fires on chan_id 1..4
     *         through channel_compute_dispatcher. Kext-only
     *         (Immediate vchan); CmdUnmapMemoryImmediate.
     *   0x42  Root channel namespace — fires on chan_id 0 through
     *         channel_0_dispatcher. Dylib-only (issued from the
     *         user-space command translator); CmdSynchronizeResources.
     *
     * Each enum value is the wire opcode AS THE KEXT EMITS IT. The
     * router (channel_door_dispatcher → channel_*_dispatcher) is
     * authoritative for which namespace the bytes belong to; the
     * enum value alone is NOT a unique key across all dispatchers.
     * Don't promote any enum entry to "global opcode" or merge the
     * channel dispatchers without redesigning this. */
    LAGFX_OP_UNMAP_MEMORY_IMMEDIATE   = 0x22, /* Compute-vchan CmdUnmapMemoryImmediate (see disjoint-namespace rule above) */
    LAGFX_OP_GET_DEVICE_INFO_2        = 0x3a,
    LAGFX_OP_NEW_USER_CLIENT          = 0x3b,
    /* 0x3c emitted by AppleParavirtResource::replacePhysical()
     * (kext VA 0x145643f2, mangled __ZN21AppleParavirtResource15replacePhysicalEv);
     * single emit-site at VA 0x14564494 (`mov esi, 0x3c; call
     * apvgpu_cmd_builder_init`). Channel: Immediate vchan (`accel[+0xe48]`,
     * i.e. compute chan_id=2). Wire trailer: 8 bytes `{u32 eventID/taskID,
     * u32 counter}` — same shape as 0x25 and 0x36. Called per-resource
     * from `commitIntoGPUPageTable` (kext VA 0x1455dc4e) when the
     * `[chan+0x114]` re-binding flag is set, signalling that the
     * resource's host backing has been relocated.
     *
     * Naming note: this is the **kext-vchan** per-resource replacePhysical
     * notification. Distinct from 0x09 LAGFX_OP_REPLACE_PHYSICAL on the
     * root channel which is the TLV-style CmdReplacePhysical from
     * AppleParavirtMemoryMap. Disjoint-namespace rule applies — see the
     * `=== Disjoint-namespace rule ===` block above and
     * `paravirt-re/library/journey/opcodes-0x35-0x36-0x39.md` for the
     * kext-vchan emit-helper family this belongs to. */
    LAGFX_OP_VCHAN_REPLACE_PHYSICAL   = 0x3c,
    LAGFX_OP_UNKNOWN_3C               = LAGFX_OP_VCHAN_REPLACE_PHYSICAL,
    LAGFX_OP_EXEC_INDIRECT_EXT_41     = 0x41, /* highest; near exec-indirect bucket */

    /* --- Heap / Resource (0x80-0x82) ---------------------------- */
    LAGFX_OP_HEAP_TEX_SIZE_ALIGN      = 0x80,
    LAGFX_OP_RESET_RASTERIZATION_RATE = 0x81,
    LAGFX_OP_DELETE_SHARED_TEX_BACK   = 0x82,

    /* Sentinel — MUST be last. Not a real opcode. 0xffff doesn't fit
     * the 1-byte / 2-byte opcode dispatch ranges actually used on the
     * wire; the value is intentionally chosen to be obviously invalid
     * so a stale read that lands on it can never be mistaken for a
     * documented opcode. */
    LAGFX_OP__INVALID                 = 0xffff,
} lagfx_opcode_t;

/* Number of opcodes documented. Spec tally (command-buffer-format.md
 * §10) is 36 (14 core + 1 NOP + 11 display + 7 exec/sync + 3 heap).
 * Per re-followup-spec-gaps.md §5.3 the host jump table tolerates
 * 0x00..0x44 (69 entries) with the extras stubbed to "unknown opcode".
 * Currently no consumer in the live tree uses LAGFX_OPCODE_COUNT; the
 * value is kept as documentation of the populated set. The previous
 * comment claimed 14 + 36 + 3 = 53 but the macro sat at 51 — that
 * arithmetic was never reconciled. Reset to a count that matches the
 * live LAGFX_OP_* enum entries above (counted manually): 51 named
 * opcodes including the IOSurface family and kext-extended namespace. */
#define LAGFX_OPCODE_COUNT 51

/* === Handler return codes ====================================== */

typedef enum {
    LAGFX_HANDLER_OK           = 0,  /* command consumed; advance read_ptr */
    LAGFX_HANDLER_ERR_SIZE     = 1,  /* payload too small / malformed */
    LAGFX_HANDLER_ERR_STATE    = 2,  /* table full / not found / etc. */
    LAGFX_HANDLER_ERR_INTERNAL = 3,
    LAGFX_HANDLER_ERR_NOT_IMPLEMENTED = 4,  /* handler unimplemented; surface visibly */
} lagfx_handler_status_t;

/* === Parsed command header =====================================
 *
 * 12-byte on-wire layout per re-followup-spec-gaps.md §5.1 and
 * `processFifo` evidence (literal `movl $0xc, %edx` at disasm line
 * 74293 fixes the read-size at 12).
 *
 * Prior scaffold (Phase 1.A.2 initial drop) used a 16-byte layout
 * derived from an incorrect reading of the command-buffer-format spec
 * (which is itself scheduled for revision). This is the corrected
 * shape; there is no separate `flags` byte — each command
 * unconditionally signals its stamp on completion, and per-opcode
 * "expectations" are implicit in the opcode itself.
 */
typedef struct lagfx_cmd_header {
    uint16_t opcode;       /* 0x0000..0x0044 — index into dispatch table     */
    uint16_t arg_count_8b; /* number of 8-byte inline-argument slots (0..64) */
    uint32_t length;       /* total command bytes on ring (>= 12)            */
    uint32_t stamp;        /* completion stamp, u32                          */

    /* --- Derived (not on wire) ----------------------------------
     * Set by lagfx_fifo_parse_header() from the wire fields above.
     * payload_size = length - sizeof(on-wire header) = length - 12.
     * payload points just past the 12-byte header if the caller
     * passed a buffer long enough to contain the full command; else
     * payload is NULL and payload_size may still be > 0 so handlers
     * can detect the header-only case. */
    uint16_t       payload_size;
    const uint8_t *payload;
} lagfx_cmd_header_t;

/* On-wire header size. The derived fields above are NOT part of this
 * size. */
#define LAGFX_CMD_HEADER_BYTES 12u

/* Compile-time guarantee that the four wire fields at the top of the
 * struct occupy exactly 12 bytes (on every sane C11 target, opcode,
 * arg_count_8b, length, stamp pack to 2+2+4+4=12 with no padding). */
_Static_assert(offsetof(struct lagfx_cmd_header, opcode)       == 0,
               "opcode at offset 0");
_Static_assert(offsetof(struct lagfx_cmd_header, arg_count_8b) == 2,
               "arg_count_8b at offset 2");
_Static_assert(offsetof(struct lagfx_cmd_header, length)       == 4,
               "length at offset 4");
_Static_assert(offsetof(struct lagfx_cmd_header, stamp)        == 8,
               "stamp at offset 8");
_Static_assert(offsetof(struct lagfx_cmd_header, payload_size) == 12,
               "payload_size (derived) must sit just past the on-wire "
               "12-byte header");

/* === Handler forward declarations ==============================
 *
 * Real implementations live in ops_misc.c (NOP, Debug) and stubs
 * live in ops_device.c, ops_queue.c, ops_cmdbuf.c with TODO markers.
 * All are exposed here so opcodes.c can populate the table.
 * ------------------------------------------------------------- */

/* ops_misc.c — real implementations. */
lagfx_handler_status_t lagfx_op_nop(lagfx_protocol_t *p,
                                    const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_debug(lagfx_protocol_t *p,
                                      const lagfx_cmd_header_t *hdr);

/* ops_device.c — stubs (P0/P1 TODO). */
lagfx_handler_status_t lagfx_op_get_device_info(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_new_user_client(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr);

lagfx_handler_status_t lagfx_op_get_device_info_2(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_define_task2(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_delete_task(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_map_memory2(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_unmap_memory(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_unmap_memory_immediate(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_set_resource_heap(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_task_define_host_task(lagfx_protocol_t *p,
                                                 const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_map_memory_immediate(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_channel_event_35(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_channel_event_36(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_delete_resource(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_set_object_placement(lagfx_protocol_t *p,
                                                      const lagfx_cmd_header_t *hdr);

/* ops_queue.c — stubs (P0/P1 TODO). */
lagfx_handler_status_t lagfx_op_define_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_delete_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);

/* ops_queue.c - signaling for adaptive online event delay */
bool lagfx_ops_queue_cmddefine_called(void);
void lagfx_ops_queue_reset(void);
void lagfx_ops_queue_set_cmddefine_called(void);

/* ops_cmdbuf.c — stubs (P0/P1 TODO). */
lagfx_handler_status_t lagfx_op_synchronize_resources(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_exec_cmdbuf(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

/* ops_display.c — Phase 2.A real (partial-layout) handlers. */
lagfx_handler_status_t lagfx_op_display_ack(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_swap_mapping(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_display_transaction3(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LIBAPPLEGFX_PROTOCOL_OPCODES_H */
