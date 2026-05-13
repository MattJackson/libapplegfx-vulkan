/*
 * libapplegfx-vulkan — Linux implementation of Apple's
 * ParavirtualizedGraphics framework, Vulkan backend.
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * === Status: Phase 1.A.1 — no-op scaffolding ===================
 *
 * This header is the C API consumed by the Linux port of QEMU's
 * apple-gfx-pci.m device (which lives in the mos-qemu tree at
 * hw/display/apple-gfx-pci-linux.c). It replaces the macOS-only
 * #import <ParavirtualizedGraphics/...> header that upstream QEMU
 * uses on macOS hosts.
 *
 * Shape mirrors apple-gfx.m's known callback patterns but
 * expressed in C instead of Objective-C blocks + @protocol. Will
 * be refined as reverse-engineering proceeds; Phase 1.A.1 is
 * shaped to be API-stable with room to add fields (fields are
 * grouped semantically with versioning in mind; future callback
 * additions append).
 *
 * === Architecture ==============================================
 *
 *  Guest macOS
 *     └─ AppleParavirtGPU.kext
 *           │ MMIO + DMA
 *           ▼
 *  mos-qemu apple-gfx-pci-linux.c
 *           │ calls lagfx_* functions
 *           ▼
 *  libapplegfx-vulkan (THIS LIBRARY)
 *           │ Vulkan API
 *           ▼
 *  Mesa lavapipe — CPU Vulkan driver
 *           │ rendered VkImage
 *           ▼
 *  callback back up → QEMU DisplaySurface
 */

#ifndef LIBAPPLEGFX_VULKAN_H
#define LIBAPPLEGFX_VULKAN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declaration for timer API (defined fully in src/device.h). */
typedef struct lagfx_device lagfx_device_t;

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque handles. Implementation details hidden from consumers. */
typedef struct lagfx_device      lagfx_device_t;
typedef struct lagfx_display     lagfx_display_t;
typedef struct lagfx_task        lagfx_task_t;

/* Return status for operations that can fail. */
typedef enum {
    LAGFX_OK = 0,
    LAGFX_ERR_INVALID_ARG   = -1,
    LAGFX_ERR_OUT_OF_MEMORY = -2,
    LAGFX_ERR_VULKAN_INIT   = -3,
    LAGFX_ERR_PROTOCOL      = -4,  /* unknown / malformed paravirt traffic */
    LAGFX_ERR_INTERNAL      = -5,
    LAGFX_ERR_NO_FRAME      = -6,  /* no rendered frame available yet */
    LAGFX_ERR_BACKEND       = -7,  /* rendering backend (Vulkan) init failed */
} lagfx_status_t;

/* === Memory model callbacks ===================================
 *
 * Mirrors Apple's PGDeviceDescriptor memory callbacks. The Linux
 * implementation of these lives in the QEMU device shell (uses
 * QEMU's memory_region_* / dma_memory_read APIs); libapplegfx-vulkan
 * calls BACK into the shell via these function pointers when it
 * needs to read guest memory or manage task-virtual ranges.
 *
 * The "task" concept mirrors Apple's PGTask_t: a reserved virtual
 * address range in the host (created via createTask) into which
 * guest-physical ranges get mapped (via mapMemory). Guest-addressed
 * DMA buffers are then accessible in that range.
 *
 * On Linux, implementations typically use memfd_create +
 * mmap(MAP_FIXED) to mirror Darwin's mach_vm_remap semantics. See
 * src/memory/ for the reference impl, exposed via
 * lagfx_task_create/destroy/map_host_memory/unmap below.
 * ------------------------------------------------------------- */

/* Single contiguous range of guest physical memory. */
typedef struct {
    uint64_t guest_physical_address;
    uint64_t length;
} lagfx_physical_range_t;

/* Memory-model callbacks provided BY the QEMU shell TO libapplegfx-vulkan. */
typedef struct {
    void *opaque;  /* passed back in every callback (typically AppleGFXState*) */

    /* Reserve a contiguous virtual-address range of `vm_size` bytes
     * in the QEMU process. Returns a task handle (opaque to us) and
     * writes the reserved base address to *base_address_out. */
    lagfx_task_t *(*create_task)(void *opaque, uint64_t vm_size,
                                 void **base_address_out);

    /* Release the task and its backing VA range. */
    void (*destroy_task)(void *opaque, lagfx_task_t *task);

    /* Map guest-physical ranges into the task's reserved virtual range,
     * starting at `virtual_offset` into the task. `read_only` hints the
     * expected mapping protection. Multiple ranges can be mapped in one
     * call (guest sometimes submits scatter-gather). */
    bool (*map_memory)(void *opaque, lagfx_task_t *task,
                       uint64_t virtual_offset,
                       const lagfx_physical_range_t *ranges,
                       size_t range_count, bool read_only);

    /* Replace a mapped range with fresh zero pages. */
    bool (*unmap_memory)(void *opaque, lagfx_task_t *task,
                         uint64_t virtual_offset, uint64_t length);

    /* One-shot DMA read from guest RAM into host-allocated dst buffer.
     * Used for small control-plane reads that don't justify mapping. */
    bool (*read_memory)(void *opaque, uint64_t guest_physical_address,
                        uint64_t length, void *dst);

    /* One-shot DMA write from host buffer `src` into guest RAM at GPA.
     * Used for control-plane response writebacks (e.g. CmdGetDeviceInfo's
     * single-u32 answer) that don't justify a full task/map cycle. */
    bool (*write_memory)(void *opaque, uint64_t guest_physical_address,
                         uint64_t length, const void *src);

    /* Raise an MSI-X interrupt on `vector` back to the guest. */
    void (*raise_interrupt)(void *opaque, uint32_t vector);
} lagfx_shell_callbacks_t;

/* === Direct task memory API ===================================
 *
 * These are the in-tree reference implementations of task memory.
 * Shells may use these directly (and wire them into their
 * lagfx_shell_callbacks_t) instead of rolling their own. See
 * src/memory/task.c for the memfd_create + mmap(MAP_FIXED)
 * implementation.
 *
 * Note the layering: lagfx_shell_callbacks_t is the boundary we
 * call back across; lagfx_task_* are the primitives a shell can
 * use to satisfy those callbacks. QEMU may well wrap these
 * primitives with its own per-task bookkeeping.
 * ------------------------------------------------------------- */

/* Reserve a contiguous VA range of `vm_size` bytes. On success
 * writes the reserved base to *base_out and returns an opaque task
 * handle. On failure returns NULL. The reserved range is initially
 * PROT_NONE. */
lagfx_task_t *lagfx_task_create(size_t vm_size, void **base_out);

/* Destroy the task and release the reserved VA range. Safe on NULL. */
void lagfx_task_destroy(lagfx_task_t *task);

/* Map host-process memory into the task's reserved range at
 * `vm_offset`. `host_addr` may be NULL (leaves mapping zeroed);
 * non-NULL contents are copied in. `read_only` requests PROT_READ
 * mapping. Returns true on success. */
bool lagfx_task_map_host_memory(lagfx_task_t *task, uint64_t vm_offset,
                                 void *host_addr, uint64_t len,
                                 bool read_only);

/* Unmap a range (replaces it with fresh PROT_NONE pages). */
bool lagfx_task_unmap(lagfx_task_t *task, uint64_t vm_offset,
                      uint64_t len);

/* Return the base host-virtual address of the task's reserved VA window.
 * Returns NULL if the task is NULL or has no reservation. This is the
 * pointer that map_memory aliases guest pages into; consumers can
 * compute (base + virtual_offset) to get a host pointer to any
 * mapped guest range. */
void *lagfx_task_get_base_ptr(const lagfx_task_t *task);

/* === Display descriptor =======================================
 * Per-display configuration + callbacks for display-plane events
 * (mode change, cursor movement, etc.)
 * ------------------------------------------------------------- */

typedef struct {
    uint32_t width_px;
    uint32_t height_px;
    uint32_t refresh_rate_hz;
} lagfx_display_mode_t;

typedef struct {
    uint16_t x;
    uint16_t y;
} lagfx_coord_t;

typedef struct {
    void *opaque;  /* identifies the display for callback routing */

    /* Display has produced a new frame; shell should mark its
     * DisplaySurface dirty and schedule a read-out. */
    void (*frame_ready)(void *opaque);

    /* Guest requested a mode change; shell should resize its
     * DisplaySurface to (width_px, height_px). */
    void (*mode_changed)(void *opaque,
                          uint32_t width_px, uint32_t height_px);

    /* Cursor glyph changed. Bitmap is 32-bpp BGRA, (width, height)
     * pixels, hotspot in glyph coordinates. */
    void (*cursor_glyph)(void *opaque,
                          const uint8_t *bgra_pixels,
                          uint32_t width, uint32_t height,
                          lagfx_coord_t hotspot);

    /* Cursor moved; read current position via get_cursor_position(). */
    void (*cursor_moved)(void *opaque);

    /* Cursor show/hide. */
    void (*cursor_show)(void *opaque, bool show);
} lagfx_display_callbacks_t;

typedef struct {
    const char *name;               /* display name, e.g. "QEMU display" */
    uint32_t size_mm_width;          /* for DPI calc; 400 = 20" landscape */
    uint32_t size_mm_height;

    const lagfx_display_mode_t *modes;
    uint32_t mode_count;

    lagfx_display_callbacks_t callbacks;
} lagfx_display_descriptor_t;

/* === Device descriptor ========================================
 * Parameters for creating a paravirt GPU device.
 * ------------------------------------------------------------- */

typedef struct {
    /* Callbacks into the QEMU device shell. */
    lagfx_shell_callbacks_t shell;

    /* Optional: hint about MMIO register range size. If 0, library
     * uses its default (Apple: 16 KB total, registers at 0x1000+). */
    size_t mmio_region_size;

    /* Optional: Vulkan instance handle if shell already created one;
     * if NULL, library creates its own targeting lavapipe. Advanced
     * use only — typical callers pass NULL. */
    void /* VkInstance */ *shell_vulkan_instance;

    /* Requested lavapipe worker-thread count.
     *   0 = unset; let lavapipe pick its default (host core count).
     *   N > 0 = apply via setenv("LP_NUM_THREADS", "N", 1) before the
     *           first Vulkan call. Values above Mesa's internal cap
     *           (historically LP_MAX_THREADS = 32) are accepted and
     *           silently clamped by the ICD.
     *
     * End-to-end path: QEMU `-device apple-gfx-pci,gpu_cores=N` -> this
     * field -> LP_NUM_THREADS. Must be consumed BEFORE vkCreateInstance;
     * lagfx_device_new does this inside the library.
     *
     * LP_NUM_THREADS is process-global: if multiple lagfx devices are
     * created in one process, the first thread_count value wins for
     * the lavapipe worker pool (env var is read once at ICD init).
     * See libapplegfx-vulkan/README.md + gpu-cores spec. */
    uint32_t thread_count;

    /* Reserved for future expansion — must be NULL for now. */
    void *reserved[3];
} lagfx_device_descriptor_t;

/* === Device lifecycle ========================================= */

/* Create a new paravirt GPU device. Shell provides callbacks via
 * the descriptor. Returns opaque handle or NULL on failure (in which
 * case *errp_out is set to a malloc'd string if non-NULL; caller
 * must free()).
 *
 * In Phase 1.A.1 this is a no-op object: allocates state, copies in
 * descriptor fields, marks it live. Vulkan init and protocol state
 * come in 1.A.2+. */
lagfx_device_t *lagfx_device_new(const lagfx_device_descriptor_t *desc,
                                  char **errp_out);

/* Destroy the device and all associated displays. Safe on NULL. */
void lagfx_device_free(lagfx_device_t *device);

/* Reset device state — clear in-flight command buffers, reset
 * internal state, preserving the configured callbacks. */
void lagfx_device_reset(lagfx_device_t *device);

/* === MMIO dispatch ============================================
 * The QEMU shell calls these on every guest MMIO access to the
 * device's BAR. Offsets are byte offsets within the BAR; Apple's
 * layout has MSI-X vectors at 0x0-0xfff and registers at 0x1000+.
 * The library interprets reads/writes according to its internal
 * protocol state machine (per Phase 0 spec).
 *
 * In Phase 1.A.1 read returns 0 and write is ack'd; both log via
 * LAGFX_LOG to stderr. Real dispatch lives in src/protocol/ when
 * Phase 1.A.2 wires it.
 * ------------------------------------------------------------- */

/* 4-byte read (Apple's protocol is 4-byte aligned per their docs). */
uint32_t lagfx_mmio_read(lagfx_device_t *device, uint64_t offset);

/* 4-byte write. */
void lagfx_mmio_write(lagfx_device_t *device, uint64_t offset,
                       uint32_t value);

/* === Display ================================================== */

/* Attach a display to a device. Returns handle or NULL (with
 * *errp_out set to a malloc'd string if non-NULL). */
lagfx_display_t *lagfx_display_new(lagfx_device_t *device,
                                    const lagfx_display_descriptor_t *desc,
                                    uint32_t port, uint32_t serial_num,
                                    char **errp_out);

/* Detach + free. Safe on NULL. */
void lagfx_display_free(lagfx_display_t *display);

/* Shell queries current cursor position. */
lagfx_coord_t lagfx_display_cursor_position(lagfx_display_t *display);

/* Read the current display surface into a caller-provided buffer.
 * Returns pixels in BGRA8 (matches QEMU's DisplaySurface format by
 * default) and writes the stride into *stride_out.
 *
 * Typical usage: shell's graphic-update callback calls this to get
 * the latest rendered frame into its DisplaySurface.
 *
 * Returns LAGFX_ERR_NO_FRAME if no new frame is available yet
 * (normal during Phase 1.A.1 because no rendering is wired).
 * new_frame_out, stride_out may be NULL. */
lagfx_status_t lagfx_display_read_frame(lagfx_display_t *display,
                                         void *dst,
                                         size_t dst_size_bytes,
                                         size_t *stride_out,
                                         bool *new_frame_out);

/* Advance the display shared-state vblank counter and DMA-write it
 * to the guest. The shell should call this at ~60 Hz from a timer.
 * Returns false if the shared state hasn't been installed yet. */
bool lagfx_timer_tick_vblank(
    lagfx_device_t *dev,
    void *shell_opaque,
    bool (*write_memory)(void *, uint64_t, uint64_t, const void *),
    bool (*read_memory)(void *, uint64_t, uint64_t, void *));

/* === Shader catalog (Phase 3.C scaffold) ======================
 *
 * Enumerates the stock shaders libapplegfx-vulkan ships pre-built
 * for the paravirt compositor fast path. Each kind maps to a
 * vertex + fragment SPIR-V blob embedded in the library at build
 * time from src/shaders/msl/<name>.metal (Apple-side source of
 * truth) and src/shaders/glsl/<name>.{vert,frag} (the GLSL twin
 * compiled to SPIR-V).
 *
 * The catalog is keyed on this enum at Phase 3.C scaffold time;
 * Phase 3.C.2 flips the primary key to SHA-256-truncated-64 of
 * the raw AIR bytes submitted by the guest (see
 * paravirt-re/shader-catalog-plan.md §5). The enum then becomes
 * a secondary/debug index.
 *
 * Enum values are 32-bit so they can be exchanged across
 * in-process boundaries (tests, future FFI). Values are
 * append-only — never reorder or delete. */
typedef enum {
    LAGFX_SHADER_BLIT           = 1,  /* texture-sample blit  */
    LAGFX_SHADER_CLEAR          = 2,  /* fullscreen colour fill */
    LAGFX_SHADER_COMPOSITE_OVER = 3,  /* Porter-Duff OVER    */
    LAGFX_SHADER_CURSOR         = 4,  /* cursor glyph quad   */
    LAGFX_SHADER_COLOR_FILL     = 5,  /* solid-colour region */
} lagfx_shader_kind_t;

/* Pre-register the 5-shader catalog into the device's internal
 * table. Called from lagfx_device_new; exposed for tests that
 * want to simulate a fresh device without going through the full
 * lifecycle. */
lagfx_status_t lagfx_device_register_shader_catalog(lagfx_device_t *device);

/* === Capability / introspection =============================== */

int lagfx_version_major(void);
int lagfx_version_minor(void);
int lagfx_version_patch(void);
const char *lagfx_build_info(void);  /* short build/ident string */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_VULKAN_H */
