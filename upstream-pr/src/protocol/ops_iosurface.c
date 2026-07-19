/*
 * libapplegfx-vulkan — IOSurface-family opcode handlers (M6 log-only stubs)
 * src/protocol/ops_iosurface.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Implements log+ack handlers for the three conjectured IOSurface
 * opcodes WindowServer is expected to emit at M6 startup:
 *
 *   0x27 CmdDeleteIOSurface       (u32 surface_id)
 *   0x28 CmdIOSurfaceCreate       (u32 surface_id, u32 w, u32 h,
 *                                  u32 pixel_format, u32 bytes_per_row,
 *                                  u64 size)
 *   0x29 CmdIOSurfaceUpdate       (u32 surface_id, u32 flags, u64 size)
 *
 * See re-followup-spec-gaps.md §14.5 for the risk analysis:
 * silently dropping these opcodes causes the guest's IOSurface handle
 * table to accumulate stale entries and the next 0x20/0x21 that
 * references the surfaceID fails lookup → WindowServer renders black
 * instead of red-of-failure. The correct M6 behaviour is "log + ack"
 * so the stamp + IRQ still flow and the guest never blocks on a
 * missing completion.
 *
 * Real lifecycle management (VkImage allocation, refcounting,
 * cross-task import) lands in Phase 4. For now the handlers:
 *   1. validate payload_size against the minimum conjectured shape
 *      (fail-open on short payloads — still log + still signal stamp
 *      via the dispatcher),
 *   2. decode as many fields as the payload allows,
 *   3. capture the raw bytes into a per-opcode ring for the §14.8
 *      instrumentation pass,
 *   4. log at LAGFX_LOG so the verbose trace lets us confirm/refute
 *      the conjectured layouts once real WindowServer captures land.
 */

#include "opcodes.h"
#include "ops_iosurface.h"
#include "protocol.h"
#include "state.h"
#include "../common/log.h"

#include <stdint.h>
#include <string.h>

/* === Little-endian primitives (local copies — the project's policy
 *     is that each ops_*.c gets its own, to keep the units self-
 *     contained and avoid cross-file inline dependencies). ======== */

static inline uint32_t lagfx_iosurf_le32(const uint8_t *b) {
    return (uint32_t)b[0]
         | ((uint32_t)b[1] << 8)
         | ((uint32_t)b[2] << 16)
         | ((uint32_t)b[3] << 24);
}

static inline uint64_t lagfx_iosurf_le64(const uint8_t *b) {
    return (uint64_t)b[0]
         | ((uint64_t)b[1] << 8)
         | ((uint64_t)b[2] << 16)
         | ((uint64_t)b[3] << 24)
         | ((uint64_t)b[4] << 32)
         | ((uint64_t)b[5] << 40)
         | ((uint64_t)b[6] << 48)
         | ((uint64_t)b[7] << 56);
}

/* === Per-opcode capture state ============================= */

static lagfx_iosurface_capture_t g_cap_delete = {0};
static lagfx_iosurface_capture_t g_cap_create = {0};
static lagfx_iosurface_capture_t g_cap_update = {0};

const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_delete(void) {
    return &g_cap_delete;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_create(void) {
    return &g_cap_create;
}
const lagfx_iosurface_capture_t *lagfx_ops_iosurface_last_update(void) {
    return &g_cap_update;
}

void lagfx_ops_iosurface_reset(void) {
    memset(&g_cap_delete, 0, sizeof(g_cap_delete));
    memset(&g_cap_create, 0, sizeof(g_cap_create));
    memset(&g_cap_update, 0, sizeof(g_cap_update));
}

/* Copy the raw payload into the capture slot (up to MAX bytes). Caller
 * is responsible for setting the decoded-field members. */
static void lagfx_iosurf_capture_bytes(lagfx_iosurface_capture_t *c,
                                       const lagfx_cmd_header_t *hdr) {
    c->valid        = true;
    c->dispatch_count += 1;
    c->last_stamp   = hdr->stamp;
    c->payload_size = hdr->payload_size;
    uint32_t to_copy = hdr->payload_size;
    if (to_copy > LAGFX_IOSURFACE_CAPTURE_MAX_BYTES) {
        to_copy = LAGFX_IOSURFACE_CAPTURE_MAX_BYTES;
    }
    c->captured_len = to_copy;
    if (to_copy > 0 && hdr->payload != NULL) {
        memcpy(c->bytes, hdr->payload, to_copy);
    }
}

/* === 0x27 CmdDeleteIOSurface ============================== */

#define LAGFX_IOSURF_DELETE_MIN_PAYLOAD 4u

lagfx_handler_status_t lagfx_op_iosurface_delete(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    /* Zero the decode-only fields; we fill whichever we can below. */
    g_cap_delete.surface_id    = 0;
    g_cap_delete.width         = 0;
    g_cap_delete.height        = 0;
    g_cap_delete.pixel_format  = 0;
    g_cap_delete.bytes_per_row = 0;
    g_cap_delete.flags         = 0;
    g_cap_delete.size          = 0;
    lagfx_iosurf_capture_bytes(&g_cap_delete, hdr);

    if (!hdr->payload ||
        hdr->payload_size < LAGFX_IOSURF_DELETE_MIN_PAYLOAD) {
        LAGFX_WARN("CmdDeleteIOSurface: payload missing or too small "
                   "(size=%u, need %u) — log+ack anyway (fail-open)",
                   (unsigned)hdr->payload_size,
                   LAGFX_IOSURF_DELETE_MIN_PAYLOAD);
        return LAGFX_HANDLER_OK;
    }

    g_cap_delete.surface_id = lagfx_iosurf_le32(hdr->payload + 0);

    LAGFX_LOG("CmdDeleteIOSurface: surface_id=0x%x payload_size=%u "
              "stamp=0x%08x (Phase 3+ real lifecycle TODO)",
              g_cap_delete.surface_id,
              (unsigned)hdr->payload_size,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* === 0x28 CmdIOSurfaceCreate ============================== */

#define LAGFX_IOSURF_CREATE_MIN_PAYLOAD 28u  /* 5*u32 + u64 = 28 B */

lagfx_handler_status_t lagfx_op_iosurface_create(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    g_cap_create.surface_id    = 0;
    g_cap_create.width         = 0;
    g_cap_create.height        = 0;
    g_cap_create.pixel_format  = 0;
    g_cap_create.bytes_per_row = 0;
    g_cap_create.flags         = 0;
    g_cap_create.size          = 0;
    lagfx_iosurf_capture_bytes(&g_cap_create, hdr);

    /* Opportunistic decode: take whichever fields fit in payload_size.
     * We never fail the command on short payload — this is a log-only
     * stub, and a premature SIZE error would hide real captures during
     * the §14.8 instrumentation pass. */
    if (hdr->payload && hdr->payload_size >= 4) {
        g_cap_create.surface_id = lagfx_iosurf_le32(hdr->payload + 0);
    }
    if (hdr->payload && hdr->payload_size >= 8) {
        g_cap_create.width = lagfx_iosurf_le32(hdr->payload + 4);
    }
    if (hdr->payload && hdr->payload_size >= 12) {
        g_cap_create.height = lagfx_iosurf_le32(hdr->payload + 8);
    }
    if (hdr->payload && hdr->payload_size >= 16) {
        g_cap_create.pixel_format = lagfx_iosurf_le32(hdr->payload + 12);
    }
    if (hdr->payload && hdr->payload_size >= 20) {
        g_cap_create.bytes_per_row = lagfx_iosurf_le32(hdr->payload + 16);
    }
    if (hdr->payload && hdr->payload_size >= 28) {
        g_cap_create.size = lagfx_iosurf_le64(hdr->payload + 20);
    }

    if (hdr->payload_size < LAGFX_IOSURF_CREATE_MIN_PAYLOAD) {
        LAGFX_WARN("CmdIOSurfaceCreate: payload_size=%u < conjectured %u "
                   "— layout may differ from §14.5 / Phase 4 §3.3 guess "
                   "(captured for instrumentation); log+ack",
                   (unsigned)hdr->payload_size,
                   LAGFX_IOSURF_CREATE_MIN_PAYLOAD);
    }

    LAGFX_LOG("CmdIOSurfaceCreate: surface_id=0x%x %ux%u fmt=0x%x "
              "bpr=%u size=%llu payload_size=%u stamp=0x%08x "
              "(Phase 4 VkImage backing TODO)",
              g_cap_create.surface_id,
              g_cap_create.width, g_cap_create.height,
              g_cap_create.pixel_format,
              g_cap_create.bytes_per_row,
              (unsigned long long)g_cap_create.size,
              (unsigned)hdr->payload_size,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}

/* === 0x29 CmdIOSurfaceUpdate ============================== */

#define LAGFX_IOSURF_UPDATE_MIN_PAYLOAD 16u  /* 2*u32 + u64 = 16 B */

lagfx_handler_status_t lagfx_op_iosurface_update(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    if (!hdr) {
        return LAGFX_HANDLER_ERR_INTERNAL;
    }

    g_cap_update.surface_id    = 0;
    g_cap_update.width         = 0;
    g_cap_update.height        = 0;
    g_cap_update.pixel_format  = 0;
    g_cap_update.bytes_per_row = 0;
    g_cap_update.flags         = 0;
    g_cap_update.size          = 0;
    lagfx_iosurf_capture_bytes(&g_cap_update, hdr);

    if (hdr->payload && hdr->payload_size >= 4) {
        g_cap_update.surface_id = lagfx_iosurf_le32(hdr->payload + 0);
    }
    if (hdr->payload && hdr->payload_size >= 8) {
        g_cap_update.flags = lagfx_iosurf_le32(hdr->payload + 4);
    }
    if (hdr->payload && hdr->payload_size >= 16) {
        g_cap_update.size = lagfx_iosurf_le64(hdr->payload + 8);
    }

    if (hdr->payload_size < LAGFX_IOSURF_UPDATE_MIN_PAYLOAD) {
        LAGFX_WARN("CmdIOSurfaceUpdate: payload_size=%u < conjectured %u "
                   "— layout may differ from §14.5 guess "
                   "(captured for instrumentation); log+ack",
                   (unsigned)hdr->payload_size,
                   LAGFX_IOSURF_UPDATE_MIN_PAYLOAD);
    }

    LAGFX_LOG("CmdIOSurfaceUpdate: surface_id=0x%x flags=0x%x size=%llu "
              "payload_size=%u stamp=0x%08x "
              "(Phase 4 refcount/remap TODO)",
              g_cap_update.surface_id,
              g_cap_update.flags,
              (unsigned long long)g_cap_update.size,
              (unsigned)hdr->payload_size,
              hdr->stamp);
    return LAGFX_HANDLER_OK;
}
