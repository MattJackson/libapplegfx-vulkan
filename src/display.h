/*
 * libapplegfx-vulkan — internal display state
 * src/display.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 */

#ifndef LIBAPPLEGFX_DISPLAY_INTERNAL_H
#define LIBAPPLEGFX_DISPLAY_INTERNAL_H

#include "libapplegfx-vulkan.h"
#include "device.h"
#include "vulkan/render_target.h"

/* Default scanout geometry if the descriptor carries no modes. Phase 2
 * first-pixel baseline per
 * mos/paravirt-re/phase-2-first-pixel-plan.md §2.D. Single source of
 * truth — the handler-side shared-state page setup (handlers/display/
 * display.c vchan_setup_shared_state) and the device's fallback
 * rt_create geometry both reach for these instead of redeclaring
 * literal 1920/1080. */
#define LAGFX_DISPLAY_DEFAULT_W 1920u
#define LAGFX_DISPLAY_DEFAULT_H 1080u
#define LAGFX_DISPLAY_DEFAULT_BYTES_PER_PIXEL 4u

#include <pthread.h>
#include <stdint.h>

#ifdef LAGFX_HAVE_VULKAN
#  include <vulkan/vulkan.h>
#endif

/* === Per-display staging-buffer ring ============================
 *
 * lagfx_display_submit_rendered_frame previously created + bound +
 * fence-waited + destroyed a VkBuffer / VkDeviceMemory / VkFence
 * trio on every present. At a 30 Hz Stage-30 target that's 30 large
 * allocations per second through Mesa's allocator — a real
 * throughput ceiling for lavapipe. Two persistent staging buffers
 * (double-buffered with explicit fences) lets the present path
 * reuse mappings across frames.
 *
 * Two slots is enough: at most one frame in-flight on the GPU
 * (lavapipe drains synchronously today, so even one is a soft
 * minimum), one being assembled. More slots would help only if the
 * present path were pipelined past the fence-wait.
 *
 * Allocated lazily on first present at the current rt size; freed
 * (and re-allocated) if rt resizes. */
#define LAGFX_DISPLAY_STAGING_SLOTS 2u
#ifdef LAGFX_HAVE_VULKAN
typedef struct {
    VkBuffer        buffer;
    VkDeviceMemory  memory;
    VkFence         fence;
    void           *mapped;        /* persistent map */
    size_t          size;
    bool            in_flight;     /* fence pending */
} lagfx_display_staging_slot_t;
#endif

/* Thread safety: rt_* fields are accessed from multiple threads.
 * - VBlank tick runs on QEMU_CLOCK_VIRTUAL thread (QEMU side)
 * - Doorbell drain may run in MMIO context (different thread)
 * - display_rt_create/destroy called during realize (initialization thread)
 *
 * All accesses to rt, rt_ready, rt_width, rt_height must be guarded by
 * this mutex. No-vulkan builds don't use Vulkan resources but still need
 * the mutex for rt_ready and dimension fields. */
#define LAGFX_DISPLAY_RT_LOCK(disp) pthread_mutex_lock(&(disp)->rt_lock)
#define LAGFX_DISPLAY_RT_UNLOCK(disp) pthread_mutex_unlock(&(disp)->rt_lock)

struct lagfx_display {
    uint32_t magic;                     /* LAGFX_DISPLAY_MAGIC */
    lagfx_device_t *device;             /* owning device (not owned) */
    lagfx_display_descriptor_t desc;    /* copied */
    uint32_t port;
    uint32_t serial_num;

    /* Current cursor position. Updated by Phase 1.A.2 MMIO handlers;
     * for now stays at (0,0). */
    lagfx_coord_t cursor_pos;

    /* Phase 2.B render target: a VkImage+view+memory quartet sized
     * width_px × height_px in BGRA8Unorm. Populated by lagfx_display_new
     * when Vulkan is initialised; torn down in lagfx_display_free. In
     * no-vulkan builds rt is a placeholder and rt_ready stays false. */
    lagfx_vk_render_target_t rt;
    bool rt_ready;
    uint32_t rt_width;
    uint32_t rt_height;

    /* Thread safety: protects rt, rt_ready, rt_width, rt_height from
     * concurrent access (VBlank tick + doorbell drain). */
    pthread_mutex_t rt_lock;

    /* Latched "a new frame has rendered" flag. Set by the protocol
     * decoder (ops_display.c) when a clear-colour transaction has been
     * submitted + waited-on. Consumed + cleared by
     * lagfx_display_read_frame so the shell only calls dpy_gfx_update
     * once per rendered frame. */
    bool new_frame_ready;

    /* Phase 1.A.1 legacy: true once a frame has rendered (retained for
     * observability; superseded by new_frame_ready semantically). */
    int has_frame;

   /* Fallback path for when CmdDisplaySwapMapping never fires:
      * store rendered pixels here until shell.read_memory pulls them.
      * This enables noVNC display even without scanout buffer registration. */
    uint8_t *fallback_pixels;
    size_t fallback_stride;
    size_t fallback_bytes;

    /* Scanout buffer info from CmdDisplaySwapMapping (opcode 0x12).
      * The kext may send arg2=0 in vchan_display_submit if it expects
      * us to use the last-seen swap mapping. Fields are populated when
      * CmdDisplaySwapMapping fires, used as fallback when arg2==0. */
    uint64_t scanout_gpa;
    uint64_t scanout_length;
    uint32_t scanout_width;
    uint32_t scanout_height;

    /* GOAL-M2z HOLD-FRAME (anti-flicker): the compositor legitimately paints
     * black mid-recomposite, so the presented stream alternates content/black
     * (user-visible flashing). Keep the most content-rich recent readback and
     * serve it whenever the live readback is near-black. Kill-switch:
     * LAGFX_DISABLE_HOLDFRAME. */
    uint8_t *held_pixels;
    size_t   held_bytes;
    size_t   held_stride;
    uint32_t held_score;
    uint32_t held_age;
    bool     scanout_valid;

#ifdef LAGFX_HAVE_VULKAN
    /* Persistent staging-buffer ring — see doc-block above the
     * lagfx_display_staging_slot_t typedef. Allocated lazily on
     * first present; rebuilt on rt resize.
     *
     * staging_size==0 means uninitialised. staging_slot_idx is the
     * next slot to write into. Both protected by rt_lock (same
     * lifetime as the render target). */
    lagfx_display_staging_slot_t staging[LAGFX_DISPLAY_STAGING_SLOTS];
    size_t                       staging_size;
    uint32_t                     staging_slot_idx;
#endif
};

/* Internal accessor: called by ops_display.c when a clear-colour
 * transaction lands. Renders the clear into the display's render
 * target, reads the pixels back, and sets new_frame_ready. On
 * no-vulkan builds this records the clear colour + flips the flag
 * but does no Vulkan work — read_frame returns NO_FRAME in that
 * configuration anyway.
 *
 * rgba may be NULL in which case a (0,0,0,1) black clear is used;
 * Phase 2 callers always pass the transaction's last_clear_rgba.
 *
 * scanout_gpa / scanout_length are optional. When non-zero (and the
 * device shell supplied a write_memory callback), the rendered pixels
 * are DMA'd back to the guest's scanout buffer at that GPA after the
 * clear fence-waits. This closes M4 GAP #1: CmdDisplayTransaction3's
 * rendered output previously stopped at the shell's staging buffer;
 * now it lands in the guest-visible scanout VA captured by the prior
 * CmdDisplaySwapMapping (ops_display.c:213ff). Pass (0, 0) to skip
 * the writeback (legacy / unit-test callers that only care about the
 * flag state). */
lagfx_status_t lagfx_display_submit_clear_color(lagfx_display_t *display,
                                                const float rgba[4],
                                                uint64_t scanout_gpa,
                                                uint64_t scanout_length);

lagfx_status_t lagfx_display_submit_rendered_frame(
    lagfx_display_t *display,
    uint64_t scanout_gpa,
    uint64_t scanout_length);

/* Stage 65d Option 3 — flag the display as "frame ready" so QEMU's
 * display-tick callback will pull pixels via lagfx_display_read_frame.
 * Called after each substitute-triangle draw because the kext's normal
 * vchan_display_submit signal only fires during boot (compositor-driven
 * scanout isn't wired yet). Internal API, no-op on NULL display. */
void lagfx_display_signal_frame_ready(lagfx_display_t *display);

/* Alpha-blend the captured guest cursor glyph over the composited rt
 * (loadOp=LOAD). No-op without a captured glyph / visible state. */
lagfx_status_t lagfx_display_overlay_cursor(lagfx_display_t *display);

#endif /* LIBAPPLEGFX_DISPLAY_INTERNAL_H */
