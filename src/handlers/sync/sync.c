/*
 * libapplegfx-vulkan — Sync and Resource handler stubs (0x22-0x28)
 * src/handlers/sync/sync.c + src/handlers/resource/resource.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "../handlers/handlers.h"
#include "../common/log.h"

lagfx_handler_status_t lagfx_sync_synchronize_resources(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdSynchronizeResources CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_resource_set_placement(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdSetObjectAndPlacementList CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_resource_iosurface_create(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdCreateIOSurfaceBacking2 CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

lagfx_handler_status_t lagfx_resource_iosurface_lookup(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    (void)p;
    LAGFX_ERR("=== CmdLookupIOSurface CALLED ===");
    return LAGFX_HANDLER_OK; /* TODO: implement */
}

