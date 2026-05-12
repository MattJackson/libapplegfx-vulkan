/*
 * libapplegfx-vulkan — Channel dispatcher registry implementation
 * src/dispatchers/registry.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Global registry maps channel IDs to dispatcher instances. Doorbell handler
 * looks up by channel ID and calls polymorphic dispatch.
 */

#include "base.h"
#include "../protocol/state.h"
#include "compute_dispatcher.h"
#include "display_vchan_dispatcher.h"
#include "unknown_dispatcher.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Registry: 1 dispatcher per channel, hardcoded ===================== */

static lagfx_dispatcher_base_t *g_dispatchers[LAGFX_MAX_CHANNELS] = {0};

void lagfx_dispatcher_registry_init(void) {
    g_dispatchers[1] = (lagfx_dispatcher_base_t *)compute_dispatcher_new();
    g_dispatchers[2] = g_dispatchers[1];  /* Channel 2 shares with compute */
    g_dispatchers[3] = g_dispatchers[1];  /* Channel 3 shares with compute */
    g_dispatchers[4] = g_dispatchers[1];  /* Channel 4 shares with compute */
    
    lagfx_dispatcher_base_t *display_disp = (lagfx_dispatcher_base_t *)display_vchan_dispatcher_new();
    for (int i = 5; i < LAGFX_MAX_CHANNELS; i++) {
        g_dispatchers[i] = display_disp;
    }
    
    LAGFX_LOG("Dispatcher registry: %d channels", LAGFX_MAX_CHANNELS);
     LAGFX_LOG("  - Channels 1-4 -> ComputeDispatcher");
     LAGFX_LOG("  - Channels 5+   -> DisplayVchanDispatcher");
     LAGFX_LOG("  - Unregistered (0, etc.) -> unknown_dispatcher fallback");
}

lagfx_dispatcher_base_t *lagfx_dispatcher_lookup(uint8_t ch) {
    return (ch < LAGFX_MAX_CHANNELS && g_dispatchers[ch]) ? g_dispatchers[ch] : NULL;
}
