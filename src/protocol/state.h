/*
 * libapplegfx-vulkan — Protocol state machine definition
 * src/protocol/state.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif
#define LAGFX_MAX_FIFOS        32u     /* Max child FIFOs */
/* Per-dispatcher scratch buffer size — large enough to hold any single
 * ring command (header + payload). Matches the legacy
 * LAGFX_*_MAX_CMD_BYTES values that lived in each dispatcher and the
 * exec_cmdbuf static \`buf[4096]\`. */
#define LAGFX_MAX_RING_READ    4096u
/* LAGFX_MAX_DISPLAYS (defined in device.h) is 4 in the live build —
 * the framebuffer + display-pipe / paravirt-event matched-class
 * counts published by the kext top out around 4 attached displays
 * before the test image hits its `ioreg` cap. Channels here cover
 * root (0) + compute (1-4) + display vchans (5..15); per
 * state-machines/FIFORingDescriptor.md the kext can provision up
 * to 8 displays (5..12) with hot-plug headroom. 16 channels keeps
 * the per-channel array dense without burning more than ~256 KiB
 * of scratch. */
#define LAGFX_MAX_CHANNELS     16     /* Root + 4 compute + up to 11 display vchans */

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

/* === Render pass description (per-task) ==========================
 *
 * Stores the parsed PGCmdDescribeRenderPass (0x1a, 584 B) payload.
 * Populated when op_render_describe_render_pass fires; consumed by
 * Stage 70c to construct VkRenderingInfo at vkCmdBeginRendering.
 */
typedef struct {
    bool       valid;           /* True if RenderDescribeRenderPass parsed */
    /* VkFormat values stored as raw u32 so the struct is visible
     * across translation units that don't pull in <vulkan/vulkan.h>.
     * Cast to VkFormat at use site (under LAGFX_HAVE_VULKAN). */
    uint32_t   color_format;    /* VkFormat: first attachment format */
    uint32_t   depth_format;    /* VkFormat: depth/stencil, VK_FORMAT_UNDEFINED if none */
    float      clear_color[4];  /* RGBA clear values (0..1) */
    float      clear_depth;     /* Depth clear value (0..1) */
    uint32_t   render_area_x;   /* Render area origin x */
    uint32_t   render_area_y;   /* Render area origin y */
    uint32_t   render_area_w;   /* Render area extent width */
    uint32_t   render_area_h;   /* Render area extent height */
    uint32_t   view_count;      /* Number of color attachments (usually 1) */
    /* M2 per-pass RT: the IOSurface/object ref this pass renders INTO (the color
     * attachment target), parsed from the 0x1a attachment descriptor. 0 = the
     * default scanout (display->rt). A non-zero target means the pass renders an
     * intermediate layer (e.g. the wallpaper) into that surface, which a later
     * composite samples — so we route the pass's draws into that surface's
     * VkImage instead of the single scanout. */
    uint32_t   target_ref;
} lagfx_render_pass_desc_t;

/* === Pending draw description (per-task) =========================
 *
 * Stores the parsed Draw family opcode payload for per-task state.
 * Populated by op_draw_* handlers when they fire; consumed later at
 * vkCmdDraw/vkCmdDrawIndexed time to bind primitive type, counts, etc.
 *
 * MTLPrimitiveType → VkPrimitiveTopology mapping (for Stage 70f):
 *   MTLPrimitiveType=0 (Point)        → VK_PRIMITIVE_TOPOLOGY_POINT_LIST
 *   MTLPrimitiveType=1 (Line)         → VK_PRIMITIVE_TOPOLOGY_LINE_LIST
 *   MTLPrimitiveType=2 (LineStrip)    → VK_PRIMITIVE_TOPOLOGY_LINE_STRIP
 *   MTLPrimitiveType=3 (Triangle)     → VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST
 *   MTLPrimitiveType=4 (TriangleStrip)→ VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP
 * Cites: Apple Metal documentation; values from MTLPrimitiveType enum.
 *
 * Wire format variants (from render-decoder-handlers.md):
 *   - DrawPrimitives16 (0x01, line 55): (vertex_start, vertex_count) — u32×2 = 8 B
 *     Encoder: drawPrimitives:vertexStart:vertexCount:
 *   - DrawInstancedPrimitives16 (0x03, line 57): (field0, field1) — u32×2 = 8 B
 *     Encoder: drawPrimitives:vertexStart:vertexCount:instanceCount:
 *     Note: Metal selector has 4 args but payload only fits 2 u32; wire format
 *     may pack differently than documented C++ struct field names.
 */
typedef struct {
    bool       valid;           /* True if a Draw opcode was parsed for this task */
    uint32_t   primitive_type;  /* MTLPrimitiveType (0=Point,1=Line,2=LineStrip,3=Triangle,4=TriangleStrip) */
    uint32_t   index_count;     /* or vertex_count for unindexed draws */
    uint32_t   instance_count;  /* Instance count (default 1 if not specified) */
    int32_t    base_vertex;     /* Vertex offset (signed, default 0) */
    uint32_t   first_instance;  /* First instance index (default 0) */
    uint32_t   index_buffer_ref;/* Index buffer reference (0 if unindexed draw) */
    bool       indexed;         /* 1 = use index_buffer_ref, 0 = vertex-only draw */
} lagfx_pending_draw_t;

/* === Per-task binding slots (Stage 70d) ==========================
 *
 * Apple's binding model: "set N at index I" where I is a per-stage slot (vertex/fragment 0..31).
 * Vulkan needs descriptor sets — Stage 70d just CAPTURES the bindings, not descriptor-set wiring.
 *
 * Binding opcodes (from render-decoder-handlers.md):
 *   - SetVertexBufferOffset (0x7e, line 133): PGCmdSetBufferOffset (12 B) → updates offset only
 *     Wire: [offset:u64][padding:u32][index:u32] — payload at offsets 0,8,12 per spec
 *   - SetVertexBuffers (0x7d, line 132): PGCmdSetBuffers + N×PGCmdSetBufferEntry → updates ref+offset
 *     Wire head: [count:u32][firstIndex:u32]; Entry: [ref:u32][offset:u64] = 12 B each
 *   - SetFragmentBufferOffset (0x6f, line 108): PGCmdSetBufferOffset (12 B) → updates offset only
 *     Wire: same as 0x7e but for fragment stage
 *   - SetFragmentBuffers (0x6e, line 107): PGCmdSetBuffers + N×PGCmdSetBufferEntry → updates ref+offset
 *     Wire head: [count:u32][firstIndex:u32]; Entry: same as vertex buffers
 *   - SetFragmentTextures (0x72, line 111): PGCmdSetTextures + N×u32 ref → updates texture slot only
 *     Wire head: [count:u32][firstIndex:u32]; Entry: [ref:u32] = 4 B each
 */
#define LAGFX_MAX_BINDING_SLOTS 32

typedef struct {
    uint32_t ref;           /* Resource registry reference (0 = unbound) */
    uint64_t offset;        /* SetXBufferOffset payload — byte offset into the buffer */
    bool     valid;         /* True if slot is bound (ref != 0) */
} lagfx_binding_slot_t;

typedef struct {
    lagfx_binding_slot_t vertex_buffers[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t fragment_buffers[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t vertex_textures[LAGFX_MAX_BINDING_SLOTS];
    lagfx_binding_slot_t fragment_textures[LAGFX_MAX_BINDING_SLOTS];
} lagfx_bindings_t;

/* Stage 65d Option 3: shader modules selected for this task.
 * Currently always copied from device's bundled triangle SPVs
 * — pending proper metallib capture. VkShaderModule handles are
 * stored as uintptr_t so this struct stays visible across translation
 * units that don't pull in <vulkan/vulkan.h>. Cast at use site. */
typedef struct {
    bool       valid;
    uintptr_t  vertex_shader;    /* VkShaderModule, NULL if not set */
    uintptr_t  fragment_shader;  /* VkShaderModule, NULL if not set */
    uint32_t   reference;        /* SetRenderPipelineState ref value (debug) */
    /* Phase 6a: when this pipeline came from translated AIR (vs the
     * substitute triangle SPVs), the entry-point names differ. Our
     * translator emits "main"; the substitute SPVs use
     * "triangle_vertex" / "triangle_fragment". `translated == true`
     * routes the draw site to the right pName, and signals that the
     * shader_module handles need vkDestroyShaderModule on overwrite
     * (substitute modules live on the device and must NOT be freed). */
    bool       translated;
    /* Phase 6a descriptor layout: when the translated shaders declare
     * [[buffer]]/texture descriptors (DescriptorSet 0), these hold the
     * VkDescriptorSetLayout + VkPipelineLayout built from SPIR-V reflection
     * (lagfx_build_pipeline_layout_from_spv). Both NULL for a resource-free
     * pipeline (use the device empty layout) or the substitute path. Stored
     * as uintptr_t to keep this struct free of <vulkan/vulkan.h>. */
    uintptr_t  descriptor_set_layout; /* VkDescriptorSetLayout, NULL if none */
    uintptr_t  pipeline_layout;       /* VkPipelineLayout, NULL if none */
    /* Stage 85b: reflected set-0 bindings so the draw site can populate a
     * matching descriptor set from the guest's bound resources. kind is
     * lagfx_spv_binding_kind_t (1=sampled image, 2=sampler, 3=storage buffer).
     * n_spv_bindings==0 ⇒ resource-free. */
    uint8_t    spv_binding_no[16];
    uint8_t    spv_binding_kind[16];
    uint8_t    n_spv_bindings;
    /* Vertex stage-in attributes reflected from the translated VERTEX shader
     * (Location-decorated Inputs). The host builds a non-empty vertex-input
     * state + binds the guest's vertex buffer from these; without them the
     * vertex shader reads unbound attributes → 0 positions → degenerate draws.
     * vtx_in_comp[i] = component count (1..4) for Location i; n_vtx_inputs = count. */
    uint8_t    vtx_in_loc[8];
    uint8_t    vtx_in_comp[8];
    uint8_t    n_vtx_inputs;
    /* B1 (M0): cached VkPipeline built from this pending_pipeline + the RT
     * formats it was built for. Reused across draws (was rebuilt + LEAKED every
     * draw → OOM on long runs). Destroyed when the shaders change (op_0x74
     * overwrite) or the task is torn down. */
    uintptr_t  cached_pipeline;       /* VkPipeline, NULL if none */
    uint32_t   cached_color_fmt;      /* render_pass_desc formats the cache was built for */
    uint32_t   cached_depth_fmt;
} lagfx_pending_pipeline_t;

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
    
    /* Render pass description from PGCmdDescribeRenderPass (0x1a).
     * Per-task state: each task has its own render pass descriptor. */
    lagfx_render_pass_desc_t render_pass_desc;

    /* Pending draw description — populated by Draw opcode handlers (0x01, 0x03, 0x06, 0x07).
     * Per-task state: each task has its own pending draw descriptor. */
    lagfx_pending_draw_t pending_draw;

    /* Descriptor-set bindings from SetVertexBuffer/FragmentBuffer/Texture opcodes (Stage 70d).
     * Stores per-stage binding slots indexed by slot number (0..31). Populated by binding handlers:
     *   - 0x7e SetVertexBufferOffset, 0x7d SetVertexBuffers → vertex_buffers[]
     *   - 0x6f SetFragmentBufferOffset, 0x6e SetFragmentBuffers → fragment_buffers[]
     *   - 0x81 SetVertexTextures, 0x72 SetFragmentTextures → texture arrays (TODO: add handlers) */
    lagfx_bindings_t bindings;

#ifdef LAGFX_HAVE_VULKAN
    /* Stage 65d Option 3: shader modules selected for this task. */
    lagfx_pending_pipeline_t pending_pipeline;
#endif

    /* Phase 6b — active-object registry populated by 0x25
     * CmdSetObjectAndPlacementList. Per the live wire data captured
     * 2026-05-28 (see paravirt-re/library/apv-object-entry-parser-
     * scoping-2026-05-17.md), 0x25's payload is 8 bytes:
     * `{count: u32, objectId: u32}`. The kext publishes which
     * objectIds are in the active set; op_0x74 SetRenderPipelineState
     * later resolves these IDs to pipeline-state metallib bytes via
     * the existing Phase B heap-VA resolver.
     *
     * Small bounded array — Apple boot path publishes ~10s of unique
     * objectIds total per task in observed traffic. */
    struct {
        uint32_t object_ids[64];   /* dedup'd set of active objectIds */
        uint32_t count;
    } active_objects;
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


    /* M2c: 0x3b per-ref backing/residency updates (wire: {u32 seq, u32 ref,
     * u32 5, u32 6, u64 addr}). Latest addr per ref; consulted (gated
     * LAGFX_M2_BACKUPD) by the vertex-upload resolve in preference to the
     * (stale for some refs) placement-table PFN walk. Per-protocol state
     * (standing rule: no file-statics in handlers). */
    struct {
        uint32_t ref;
        uint64_t addr;
        uint8_t  valid;
    } backing_update[64];
    uint32_t backing_update_n;

    /* M2c WHITEFB: lazily-created opaque-white fallback texture
     * (lagfx_vk_iosurface_t*, stored as uintptr_t — vulkan.h-free header).
     * Bound for unresolved SAMPLED_IMAGEs under LAGFX_M2_WHITEFB so the
     * translated composite fragments' alpha-discard never fires. */
    uintptr_t white_fb_ios;

    /* Per-frame scanout-assembly blit queue. Draws into per-pass surfaces
     * enqueue their surface (draw order, dedup); the display present blits
     * the queue in order into display->rt and clears it, so a later
     * near-empty surface can never wipe an earlier content-bearing one.
     * Entries are lagfx_vk_iosurface_t* as uintptr_t (vulkan.h-free). */
    uintptr_t frame_blit_queue[16];
    uint32_t  frame_blit_n;

    /* One-shot / capped diagnostic flags and counters (protocol-owned so
     * handlers stay free of file-static state). */
    struct {
        uint32_t unknown_blit_ops;
        uint32_t unknown_render_ops;
        uint8_t  first_draw_dumped;
        uint8_t  resource_buf_dumped;
        uint8_t  desc_table_dumped;
        uint32_t submit_probe_count;
    } diag;

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
