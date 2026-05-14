/*
 * libapplegfx-vulkan — Protocol state machine definition
 * src/protocol/state.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Core protocol struct and shared types used by all dispatchers/handlers.
 */

#ifndef LAGFX_PROTOCOL_STATE_H
#define LAGFX_PROTOCOL_STATE_H

#include <stdatomic.h>
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

/* Resource registry must be defined before protocol struct uses it.
 * Included here to avoid circular deps when state.h is included by handlers. */
#include "resource_registry.h"


/* === Constants =================================================== */
#define LAGFX_MAX_TASKS        64u     /* Max concurrent tasks */
#define LAGFX_MAX_FIFOS        32u     /* Max child FIFOs */
/* Per-dispatcher scratch buffer size — large enough to hold any single
 * ring command (header + payload). Matches the legacy
 * LAGFX_*_MAX_CMD_BYTES values that lived in each dispatcher and the
 * exec_cmdbuf static \`buf[4096]\`. */
#define LAGFX_MAX_RING_READ    4096u
/* LAGFX_MAX_DISPLAYS defined in device.h; channels = root (0) + compute (1-4) + displays.
 * Per state-machines/FIFORingDescriptor.md the kext provisions chan_ids
 * 1..4 for compute + 5..12 for up to 8 displays. Allow up to 16 to
 * cover hot-plug + headroom. */
#define LAGFX_MAX_CHANNELS     16     /* Root + 4 compute + up to 11 displays */

/* Stamp slot mapping (per waitForStamp-mechanism-summary.md) */
enum {
    SLOT_ROOT_CHANNEL       = 0,   // ch 0 → root channel
    SLOT_COMPUTE_1          = 1,   // ch 1 → compute vchan
    SLOT_COMPUTE_2          = 2,   // ch 2 → compute vchan
    SLOT_COMPUTE_3          = 3,   // (unused by macOS)
    SLOT_COMPUTE_4          = 4,   // ch 4 → compute vchan
    SLOT_DISPLAY_PIPE_0     = 5,   // display pipe 0
};

/* Register shadow indices (BAR0+0x1000..0x1FFF → idx 0..15) */
enum {
    REG_STATUS_CONTROL      = 0,   // BAR0+0x1000 - ring_armed
    REG_RING_SIZE           = 1,   // BAR0+0x1004 - command ring size
    REG_WRITE_PTR           = 2,   // BAR0+0x1008 - doorbell (primary)
    REG_FIFO_FAULT_OFFSET   = 3,   // BAR0+0x100c - fault offset (read-only)
    REG_START_OFFSET        = 4,   // BAR0+0x1010 - ring start offset
    REG_SHARED_PAGE_PFN     = 5,   // BAR0+0x101c - shared page PFN
    REG_STAMP_BITMASK       = 7,   // BAR0+0x1020 - stamp bitmask (read)
    REG_BASE_PFN            = 8,   // BAR0+0x1030 - ring base PFN
};

/* === Wire Format Structures ====================================== */
/* lagfx_cmd_header_t defined in opcodes.h with derived fields.
 * The wire-format-only version is kept here for documentation. */

// 12-byte command header on wire (per-command-buffer-format.md §2.1)
typedef struct {
    uint16_t opcode;        // Outer opcode (0x00..0x44)
    uint32_t length;        // Total command size including this header
    uint32_t stamp;         // Stamp to ack on completion
    uint8_t  arg_count_8b;  // Number of 64-bit arguments after payload
    uint16_t payload_size;  // Payload bytes after 12-byte header
} __attribute__((packed)) lagfx_cmd_header_wire_t;

// Inner opcode (inside CmdExecIndirect2, 8-byte header)
typedef struct {
    uint32_t length;        // Total inner command size
    uint32_t stamp;         // Stamp to ack
    uint16_t opcode;        // Inner opcode (Render/Blit/Compute domain-specific)
} __attribute__((packed)) lagfx_inner_cmd_header_t;

/* === Task Entry ================================================== */
typedef struct {
    uint32_t id;                /* Task ID (from CmdDefineTask2 / CmdDefineHostTask) */
    uint64_t root_page_pfn;     // Root page PFN for VA→GPA translation
                                // (set by CmdDefineHostTask 0x38)
    uint64_t base_va;           // Guest VA or shell-returned ptr low bits
    uint32_t length;            // Task address space size in bytes
    void *shell_task;           // Opaque handle passed to shell (if any)
    /* Resource heap (set by CmdSetResourceHeap 0x33). Per kext disasm
     * the kext provides a (heap_pfn, heap_size) tuple per task; we
     * record it as a hint for later resource lookups. */
    uint32_t heap_pfn;
    uint32_t heap_size;
    bool live;                  /* Slot is in use */
} lagfx_task_entry_t;

/* === FIFO Entry ================================================== */
typedef struct {
    uint32_t id;                /* Child FIFO ID (from CmdDefineChildFIFO) */
    uint64_t ring_base_gpa;     // Ring base GPA for this child FIFO
    uint32_t ring_size;         // Ring size in bytes
    uint32_t entry_count;       // Number of entries in ring
    bool live;                  // Whether this FIFO is active
} lagfx_fifo_entry_t;

/* === Display Child Ring ========================================== */
typedef struct {
    uint64_t ring_base_gpa;     // Ring base GPA (separate from parent ring)
    uint32_t ring_size;         // Ring size
    uint32_t entry_count;       // Entry count
    bool live;                  // Active flag
} lagfx_display_child_ring_t;

/* === Handler Status ============================================== */
/* Defined in opcodes.h to avoid duplication when including state.h. */

/* === Protocol State Structure ==================================== */
typedef struct lagfx_protocol {
    uint32_t magic;             // LAGFX_PROTOCOL_MAGIC for validation

    /* Device reference */
    void *dev;                  // Back-reference to device (opaque here)

    /* Register shadow (BAR0+0x1000..0x1FFF → idx 0..15) */
    uint32_t reg[16];           // Shadowed register values

    /* Ring geometry (set by MMIO, preserved across resets) */
    uint64_t ring_base_pfn;     // Command ring base PFN (from BAR0+0x1030)
    uint64_t ring_base_gpa;     // Computed: (pfn << 12) + start_offset
    uint32_t ring_size;         // Ring size in bytes (BAR0+0x1004)
    uint32_t ring_start_offset; // Offset within base page (BAR0+0x1010)
    uint64_t ring_shared_page_pfn;  // Shared page PFN for descriptors
    uint32_t page_size;         // Page size (observed: 0x1000)

    /* Ring pointers (updated by doorbells) */
    uint32_t read_ptr;          // Last processed write pointer (root channel only)
    uint32_t write_ptr;         // Current write pointer from doorbell (BAR0+0x1008)

    /* Per-channel tracking */
    uint8_t current_chan_id;    // Set by channel_door_dispatcher on entry

    /* Ring armed flag (set by STATUS_CONTROL @ 0x1000) */
    bool ring_armed;            // Whether ring is enabled for processing

    /* === Stamp management ==========================================
     *
     * \`pending_stamps_bitmask\` and \`pending_displays_bitmask\` are
     * touched from two threads in the live deployment:
     *
     *   - The QEMU MMIO read thread, when macOS reads BAR0+0x1014
     *     (display) or BAR0+0x1018 (stamp). doorbell_handle_read
     *     does an xchg-and-clear: capture the current mask, return
     *     it to the guest, then zero it out so the next read sees
     *     only freshly-pending interrupts.
     *
     *   - The drain thread invoked from the doorbell dispatch path
     *     (channel_*_dispatcher), which does a bit-set via
     *     \`|=\` whenever it completes a command on a given slot
     *     before raising the IRQ.
     *
     * QEMU serialises both paths through the BQL today, so the data
     * race is latent — but the field shape was uint32_t, the
     * compiler is free to tear reads, and a future BQL-relaxed lane
     * would expose it. Switch to _Atomic uint32_t and update every
     * touch to atomic_* with the documented memory orderings:
     *
     *   bit-set on completion → memory_order_release (synchronises
     *     stamp-cell DMA write + IRQ raise with the reader)
     *   xchg-and-clear on read → memory_order_acquire (sees the
     *     drainer's stamp-cell DMA before returning the mask)
     *
     * last_completed_stamp stays non-atomic — it's pure diagnostics
     * (no consumer reads it cross-thread). */
    _Atomic uint32_t pending_stamps_bitmask;  // Bits set for slots needing IRQ
    uint32_t last_completed_stamp;             // Most recent stamp value acked (diag only)
    _Atomic uint32_t pending_displays_bitmask; // Display interrupt bitmask

    /* Task table (per CmdDefineTask2) */
    lagfx_task_entry_t tasks[LAGFX_MAX_TASKS];

    /* FIFO table (per CmdDefineChildFIFO) */
    lagfx_fifo_entry_t fifos[LAGFX_MAX_FIFOS];

    /* Display child rings */
    lagfx_display_child_ring_t display_child_rings[16];  // Up to 16 displays

    /* Resource registry (per CmdMapMemory2, CmdDeleteResource, etc.) */
    lagfx_resource_registry_t resources;

    /* Command counters (for diagnostics) */
    uint64_t total_cmds_seen;
    uint64_t total_cmds_completed;
    uint64_t unknown_opcode_count;

    /* === Per-dispatcher scratch buffers ============================
     *
     * Each dispatcher (root channel, compute vchan drain, display
     * vchan drain) and the inner exec walker need a 4 KiB scratch
     * to read one command (header + payload) before handing it to
     * the handler. The legacy layout used either a function-static
     * (\`exec_cmdbuf.c: static uint8_t buf[4096]\`) or a stack-local
     * (\`uint8_t cmd_buf[4096]\` in each drain loop). Static was
     * non-reentrant; stack burned 12 KiB on every BAR write.
     *
     * Single-threaded invariant: drain callbacks serialise through
     * QEMU's BQL today. The four scratches below are owned by the
     * named dispatcher and must never be used from another (incl.
     * cross-call recursion). If a future BQL-relaxed path lands,
     * scratches will need to move to thread-local — or be replaced
     * by per-channel mallocs in the CmdDefineChildChannel 0x30
     * handler. Document any such change here when made.
     */
    uint8_t scratch_ch0[LAGFX_MAX_RING_READ];
    uint8_t scratch_compute[LAGFX_MAX_RING_READ];
    uint8_t scratch_display[LAGFX_MAX_RING_READ];
    uint8_t scratch_exec[LAGFX_MAX_RING_READ];

} lagfx_protocol_t;

/* Validation macro */
#define LAGFX_PROTOCOL_MAGIC 0x4C414758u  /* "LAGX" */
static inline int lagfx_protocol_is_valid(const lagfx_protocol_t *p) {
    return p != NULL && p->magic == LAGFX_PROTOCOL_MAGIC;
}

/* Accessor for last completed stamp - used by tests. */
static inline uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}

/* Task table helpers — static inline for performance. */
static inline lagfx_task_entry_t* lagfx_protocol_find_task(lagfx_protocol_t *p, uint32_t task_id) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (p->tasks[i].live && p->tasks[i].id == task_id) {
            return &p->tasks[i];
        }
    }
    return NULL;
}

static inline lagfx_task_entry_t* lagfx_protocol_alloc_task_slot(lagfx_protocol_t *p) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_TASKS; ++i) {
        if (!p->tasks[i].live) {
            memset(&p->tasks[i], 0, sizeof(p->tasks[i]));
            p->tasks[i].live = false;
            return &p->tasks[i];
        }
    }
    return NULL;
}

/* FIFO table helpers. */
static inline lagfx_fifo_entry_t* lagfx_protocol_find_fifo(lagfx_protocol_t *p, uint32_t fifo_id) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_FIFOS; ++i) {
        if (p->fifos[i].live && p->fifos[i].id == fifo_id) {
            return &p->fifos[i];
        }
    }
    return NULL;
}

static inline lagfx_fifo_entry_t* lagfx_protocol_alloc_fifo_slot(lagfx_protocol_t *p) {
    if (!p || !lagfx_protocol_is_valid(p)) return NULL;
    for (uint32_t i = 0; i < LAGFX_MAX_FIFOS; ++i) {
        if (!p->fifos[i].live) {
            memset(&p->fifos[i], 0, sizeof(p->fifos[i]));
            p->fifos[i].live = false;
            return &p->fifos[i];
        }
    }
    return NULL;
}

/* Stamp slot helpers — exported for tests. */
extern void lagfx_protocol_complete_stamp_slot(lagfx_protocol_t *p, uint32_t slot, uint32_t stamp);

/* Back-compat wrapper — completes slot 0 with given stamp value (defined in stamp.c). */
extern void lagfx_protocol_complete_stamp(lagfx_protocol_t *p, uint32_t stamp);

#endif /* LAGFX_PROTOCOL_STATE_H */
