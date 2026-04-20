/*
 * libapplegfx-vulkan — opcode enum + descriptor table (Phase 1.A.2)
 * src/protocol/opcodes.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Enumerates all 37 FIFO opcodes documented in
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

    /* --- Execution / Sync (0x20-0x26) --------------------------- */
    LAGFX_OP_EXEC_INDIRECT2           = 0x20,
    LAGFX_OP_EXEC_INDIRECT3           = 0x21,
    LAGFX_OP_SYNCHRONIZE_RESOURCES    = 0x22,
    LAGFX_OP_SYNCHRONIZE_DISCARD      = 0x23,
    LAGFX_OP_SET_OBJECT_LIST          = 0x24,
    LAGFX_OP_SET_OBJECT_PLACEMENT     = 0x25,
    LAGFX_OP_DELETE_IOSURFACE_BACKING = 0x26,

    /* --- Heap / Resource (0x80-0x82) ---------------------------- */
    LAGFX_OP_HEAP_TEX_SIZE_ALIGN      = 0x80,
    LAGFX_OP_RESET_RASTERIZATION_RATE = 0x81,
    LAGFX_OP_DELETE_SHARED_TEX_BACK   = 0x82,

    /* Sentinel — MUST be last. Not a real opcode. */
    LAGFX_OP__MAX                     = 0xff,
} lagfx_opcode_t;

/* Number of opcodes documented. The phase-1a2 brief says "all 37"; the
 * authoritative command-buffer-format.md §10 tally is 36 (14 core +
 * 1 NOP + 11 display + 7 exec/sync + 3 heap). We go with the spec's
 * self-count. If a 37th opcode surfaces during bring-up, add it to
 * the enum + descriptor table and bump this constant. */
#define LAGFX_OPCODE_COUNT 36

/* === Priority bands ============================================ */

typedef enum {
    LAGFX_PRIO_P0 = 0,  /* required for metal-no-op */
    LAGFX_PRIO_P1 = 1,  /* likely side-effect */
    LAGFX_PRIO_P2 = 2,  /* deferred; log + ack stub */
} lagfx_priority_t;

/* === Handler return codes ====================================== */

typedef enum {
    LAGFX_HANDLER_OK        = 0,  /* command consumed; advance read_ptr */
    LAGFX_HANDLER_ERR_SIZE  = 1,  /* payload too small / malformed */
    LAGFX_HANDLER_ERR_STATE = 2,  /* table full / not found / etc. */
    LAGFX_HANDLER_ERR_INTERNAL = 3,
} lagfx_handler_status_t;

/* === Parsed command header ===================================== */

/* Corresponds to struct PGCommand in command-buffer-format.md §2.
 * Populated by the FIFO dequeue path before the handler runs. */
typedef struct {
    uint8_t  opcode;
    uint8_t  flags;
    uint16_t length;        /* total command size including header */
    uint32_t stamp;
    uint32_t reserved;
    uint16_t payload_size;  /* length - 16 */
    uint16_t padding;
    const uint8_t *payload; /* points into guest-copy buffer; may be NULL */
} lagfx_cmd_header_t;

#define LAGFX_CMD_HEADER_BYTES 16u

/* Flag bit positions (see command-buffer-format.md §2). R6 in brief
 * notes bit-0 is inferred; narrow once confirmed against live guest. */
#define LAGFX_FLAG_COMPLETION_EXPECTED (1u << 0)
#define LAGFX_FLAG_PRIORITY_HIGH       (1u << 1)
#define LAGFX_FLAG_FLUSH_CACHE         (1u << 2)

/* === Handler signature + descriptor ============================ */

typedef lagfx_handler_status_t (*lagfx_op_handler_fn)(
    lagfx_protocol_t *p,
    const lagfx_cmd_header_t *hdr);

typedef struct {
    uint8_t            opcode;     /* LAGFX_OP_* value */
    const char        *name;       /* short, log-friendly */
    lagfx_priority_t   priority;
    uint16_t           min_payload;/* floor (inclusive); 0 = none */
    uint16_t           max_payload;/* 0 = unbounded */
    lagfx_op_handler_fn handler;   /* NULL => default log+ack handler */
} lagfx_op_descriptor_t;

/* Look up the descriptor for an opcode. Returns NULL if the opcode
 * is not in the table (caller falls back to default handler). */
const lagfx_op_descriptor_t *lagfx_opcode_lookup(uint8_t opcode);

/* Short human-readable name for tracing. Never returns NULL —
 * unknown opcodes yield "Unknown(0xNN)" into a static thread-local
 * (actually a shared static buffer in phase 1.A.2; single-threaded
 * per the header comment in protocol.h). */
const char *lagfx_opcode_name(uint8_t opcode);

/* Iterate the full descriptor table. For tests / stats. */
size_t lagfx_opcode_table_size(void);
const lagfx_op_descriptor_t *lagfx_opcode_table_entry(size_t index);

/* Default handler exposed so the dispatcher can fall through to it
 * explicitly when no descriptor is found. Logs + returns OK. */
lagfx_handler_status_t lagfx_op_default_handler(lagfx_protocol_t *p,
                                                const lagfx_cmd_header_t *hdr);

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
lagfx_handler_status_t lagfx_op_define_task2(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_delete_task(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_map_memory2(lagfx_protocol_t *p,
                                            const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_unmap_memory(lagfx_protocol_t *p,
                                             const lagfx_cmd_header_t *hdr);

/* ops_queue.c — stubs (P0/P1 TODO). */
lagfx_handler_status_t lagfx_op_define_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_delete_child_fifo(lagfx_protocol_t *p,
                                                  const lagfx_cmd_header_t *hdr);

/* ops_cmdbuf.c — stubs (P0/P1 TODO). */
lagfx_handler_status_t lagfx_op_synchronize_resources(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_op_exec_indirect2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LIBAPPLEGFX_PROTOCOL_OPCODES_H */
