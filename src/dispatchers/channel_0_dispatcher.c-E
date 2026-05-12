/*
 * libapplegfx-vulkan — Primary ring dispatcher (Channel 0) implementation
 * src/dispatchers/channel_0_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Dispatcher for primary ring (channel 0). Full opcode namespace via switch-case,
 * with direct calls to ops_*.c handlers — NO global opcode table lookup.
 * All routing through dispatcher hierarchy enables clean debugging.
 */

#include "channel_0_dispatcher.h"
#include "../device.h"
#include "../protocol/state.h"
#include "../handlers/handlers.h"
#include "../common/log.h"
#include <stdlib.h>
#include <string.h>

/**
 * Channel 0 dispatcher for primary ring.
 * Switch-case routing → direct ops_*.c handler calls (no opcode table).
 */
lagfx_channel_0_dispatcher_t *channel_0_dispatcher_new(void) {
    lagfx_channel_0_dispatcher_t *d = calloc(1, sizeof(*d));
    if (!d) return NULL;

    d->base.name = "Channel0Dispatcher(primary ring)";
    d->base.channel_id_min = 0;
    d->base.channel_id_max = 0;

    LAGFX_LOG("Channel0Dispatcher: created for primary ring");
    return d;
}

/**
 * Ring dispatch for channel 0 (primary ring).
 * Full opcode namespace via switch-case with direct ops_*.c calls.
 */
void channel_0_dispatcher_ring_dispatch(lagfx_channel_0_dispatcher_t *d,
                                         struct lagfx_protocol *p,
                                         uint64_t descr_gpa,
                                         uint8_t ch_id) {
    (void)d;
    (void)ch_id;

    LAGFX_LOG("Channel0Dispatcher(ring): processing primary ring commands");
    
    if (!p->dev || !p->dev->desc.shell.read_memory) {
        LAGFX_ERR("Channel0Dispatcher: no device or read_memory callback");
        return;
    }

    uint8_t descr[20] = {0};
    if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                        descr_gpa, sizeof(descr), descr)) {
        LAGFX_ERR("Channel0Dispatcher: failed to read descriptor");
        return;
    }

    uint32_t write_ptr = (uint32_t)descr[0] | ((uint32_t)descr[1] << 8) |
                         ((uint32_t)descr[2] << 16) | ((uint32_t)descr[3] << 24);
    uint32_t read_ptr  = (uint32_t)descr[4] | ((uint32_t)descr[5] << 8) |
                         ((uint32_t)descr[6] << 16) | ((uint32_t)descr[7] << 24);

    LAGFX_LOG("Channel0Dispatcher: primary ring wr=%u rd=%u", write_ptr, read_ptr);

    if (write_ptr <= read_ptr) {
        LAGFX_TRACE("Channel0Dispatcher: empty ring");
        return;
    }

    uint32_t cur_rp = read_ptr;
    while (cur_rp + 12u <= write_ptr) {
        uint8_t hdr_bytes[12] = {0};
        bool ok = true;

        for (size_t i = 0; i < 12 && ok; i++) {
            if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                descr_gpa + cur_rp + i, 1, &hdr_bytes[i])) {
                LAGFX_WARN("Channel0Dispatcher: failed to read header byte %zu", i);
                ok = false;
            }
        }

        if (!ok) break;

        uint32_t opcode = (uint32_t)(hdr_bytes[0] | ((uint32_t)hdr_bytes[1] << 8));
        uint32_t length = (uint32_t)(hdr_bytes[4] | ((uint32_t)hdr_bytes[5] << 8) |
                                     ((uint32_t)hdr_bytes[6] << 16) | ((uint32_t)hdr_bytes[7] << 24));

        LAGFX_LOG("Channel0Dispatcher: opcode=0x%04x length=%u", opcode, length);

        p->current_chan_id = ch_id;
        
        /* Switch-case dispatch → direct handler calls (no opcode table lookup) */
        switch (opcode) {
        case 0x20:  /* CmdExecIndirect2 - Metal command buffer execution */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x20 (CmdExecIndirect2) → lagfx_compute_exec_indirect2");
            
            uint8_t *payload = malloc(length);
            if (!payload || length < 12u) {
                free(payload);
                break;
            }

            for (size_t i = 0; i < length && ok; i++) {
                if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                    descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                    LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                    ok = false;
                }
            }

               if (ok && length >= 12) {
                    lagfx_cmd_header_t hdr = {
                        .opcode = opcode,
                        .length = length,
                        .stamp = 0,
                        .payload_size = length - 12u,
                        .arg_count_8b = 0,
                        .payload = payload + 12u
                    };
                    lagfx_compute_exec_indirect2(p, &hdr);
                }

            free(payload);
            break;

        case 0x00:  /* CmdDefineTask2 */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x00 (CmdDefineTask2) → lagfx_task_define_task2");
            if (length >= 24) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_task_define_task2(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x02:  /* CmdMapMemory2 */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x02 (CmdMapMemory2) → lagfx_memory_map_memory2");  /* CmdMapMemory2 */
            LAGFX_LOG("Channel0Dispatcher: CmdMapMemory2");
            if (length >= 20u) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_memory_map_memory2(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x03:  /* CmdUnmapMemory */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x03 (CmdUnmapMemory) → lagfx_memory_unmap_memory");  /* CmdUnmapMemory */
            LAGFX_LOG("Channel0Dispatcher: CmdUnmapMemory");
            if (length >= 20u) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_memory_unmap_memory(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x04:  /* CmdDefineChildFIFO */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x04 (CmdDefineChildFIFO) → lagfx_memory_define_child_fifo");  /* CmdDefineChildFIFO */
            LAGFX_LOG("Channel0Dispatcher: CmdDefineChildFIFO");
            if (length >= 4) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_memory_define_child_fifo(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x0a:  /* CmdGetDeviceInfo */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x0a (CmdGetDeviceInfo) → lagfx_util_device_info");  /* CmdGetDeviceInfo */
            LAGFX_LOG("Channel0Dispatcher: CmdGetDeviceInfo");
            if (length >= 12) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_util_device_info(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x10:  /* CmdDisplayAck */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x10 (CmdDisplayAck) → lagfx_display_ack");  /* CmdDisplayAck */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplayAck");
            if (length >= 8) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_ack(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x13:  /* CmdDisplayCursorShow */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x13 (CmdDisplayCursorShow) → lagfx_display_cursor_show");  /* CmdDisplayCursorShow */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplayCursorShow");
            if (length >= 16) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_cursor_show(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x14:  /* CmdDisplayCursorGlyph */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x14 (CmdDisplayCursorGlyph) → lagfx_display_cursor_glyph");  /* CmdDisplayCursorGlyph */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplayCursorGlyph");
            if (length >= 32) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_cursor_glyph(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x17:  /* CmdDisplayTransaction3 */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x17 (CmdDisplayTransaction3) → lagfx_display_transaction3");  /* CmdDisplayTransaction3 */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplayTransaction3");
            if (length >= 12) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_transaction3(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x19:  /* CmdDisplayCompositorParameters */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x19 (CmdDisplayCompositorParameters) → lagfx_display_compositor_params");  /* CmdDisplayCompositorParameters */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplayCompositorParameters");
            if (length > 0) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_compositor_params(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x1a:  /* CmdDisplaySetGuestICCProfile */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x1a (CmdDisplaySetGuestICCProfile) → lagfx_display_icc_profile");  /* CmdDisplaySetGuestICCProfile */
            LAGFX_LOG("Channel0Dispatcher: CmdDisplaySetGuestICCProfile");
            if (length > 0) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_display_icc_profile(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x22:  /* CmdSynchronizeResources */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x22 (CmdSynchronizeResources) → lagfx_sync_synchronize_resources");  /* CmdSynchronizeResources */
            LAGFX_LOG("Channel0Dispatcher: CmdSynchronizeResources");
            if (length >= 8) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_sync_synchronize_resources(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x26:  /* CmdSetObjectAndPlacementList */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x26 (CmdSetObjectAndPlacementList) → lagfx_resource_set_placement");  /* CmdSetObjectAndPlacementList */
            LAGFX_LOG("Channel0Dispatcher: CmdSetObjectAndPlacementList");
            if (length > 0) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_resource_set_placement(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x27:  /* CmdCreateIOSurfaceBacking2 */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x27 (CmdCreateIOSurfaceBacking2) → lagfx_resource_iosurface_create");  /* CmdCreateIOSurfaceBacking2 */
            LAGFX_LOG("Channel0Dispatcher: CmdCreateIOSurfaceBacking2");
            if (length > 0) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_resource_iosurface_create(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x28:  /* CmdLookupIOSurface */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x28 (CmdLookupIOSurface) → lagfx_resource_iosurface_lookup");  /* CmdLookupIOSurface */
            LAGFX_LOG("Channel0Dispatcher: CmdLookupIOSurface");
            if (length > 0) {
                uint8_t *payload = malloc(length);
                if (payload && ok) {
                    for (size_t i = 0; i < length && ok; i++) {
                        if (!p->dev->desc.shell.read_memory(p->dev->desc.shell.opaque,
                                                            descr_gpa + cur_rp + 12u + i, 1, &payload[i])) {
                            LAGFX_WARN("Channel0Dispatcher: failed to read payload byte %zu", i);
                            ok = false;
                        }
                    }

                    if (ok) {
                        lagfx_cmd_header_t hdr = {
                            .opcode = opcode,
                            .length = length,
                            .stamp = 0,
                            .payload_size = length - 12u,
                            .arg_count_8b = 0,
                            .payload = payload + 12u
                        };
                        lagfx_resource_iosurface_lookup(p, &hdr);
                    }
                    free(payload);
                }
            }
            break;

        case 0x11:  /* CmdNOP */
            LAGFX_LOG("Channel0Dispatcher: routing opcode 0x11 (CmdNOP) → lagfx_util_nop");  /* CmdNOP */
            LAGFX_LOG("Channel0Dispatcher: CmdNOP");
            break;

        default:
            LAGFX_WARN("Channel0Dispatcher: unknown primary ring opcode 0x%04x", opcode);
            p->unknown_opcode_count++;
            break;
        }

        cur_rp += length;
    }

    LAGFX_LOG("Channel0Dispatcher: updated read_ptr=%u", cur_rp);
}

