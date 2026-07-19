/*
 * libapplegfx-vulkan — IOSurface outer-FIFO opcode handlers
 * src/handlers/iosurface/iosurface.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Outer-FIFO opcodes 0x26..0x2a (CmdDeleteIOSurfaceBacking2 /
 * CmdCreateIOSurfaceBacking2 / CmdLookupIOSurface /
 * CmdImportIOSurfaceMachPort / CmdUnmapIOSurface). The kext / dylib
 * emit these on the root channel (ch 0) during IOSurface lifecycle.
 *
 * RE: paravirt-re/library/state-machines/PGFIFO-sub-channel-opcode-table.md
 *     re-followup-spec-gaps.md §14.5,
 *     pre-refactor src/protocol/ops_iosurface.c at b652199~1
 *     (stranded in dead-code-to-revive/protocol/ since 2026-05-12).
 *
 * Private to src/handlers/iosurface/. Not installed.
 */

#ifndef LAGFX_HANDLERS_IOSURFACE_IOSURFACE_H
#define LAGFX_HANDLERS_IOSURFACE_IOSURFACE_H

#include "protocol/opcodes.h"
#include "protocol/state.h"

lagfx_handler_status_t lagfx_iosurface_delete_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_iosurface_create_backing2(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_iosurface_lookup(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_iosurface_import_mach_port(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);
lagfx_handler_status_t lagfx_iosurface_unmap(
    lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr);

#endif /* LAGFX_HANDLERS_IOSURFACE_IOSURFACE_H */
