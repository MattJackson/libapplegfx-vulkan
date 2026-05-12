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
#include "channel_0_dispatcher.h"
#include "channel_1_dispatcher.h"
#include "channel_2_dispatcher.h"
#include "channel_3_dispatcher.h"
#include "channel_4_dispatcher.h"
#include "display_vchan_dispatcher.h"
#include "unknown_dispatcher.h"
#include "../common/log.h"

#include <stdlib.h>
#include <string.h>

/* === Registry: 1 dispatcher per channel, hardcoded ===================== */

static lagfx_dispatcher_base_t *g_dispatchers[LAGFX_MAX_CHANNELS] = {0};

void lagfx_dispatcher_registry_init(void) {
    /* Channel 0: Primary ring dispatcher */
    g_dispatchers[0] = (lagfx_dispatcher_base_t *)channel_0_dispatcher_new();
    
    /* Channels 1-4: Individual compute/render dispatchers (per-channel stamp tracking) */
    g_dispatchers[1] = (lagfx_dispatcher_base_t *)channel_1_dispatcher_new();
    g_dispatchers[2] = (lagfx_dispatcher_base_t *)channel_2_dispatcher_new();
    g_dispatchers[3] = (lagfx_dispatcher_base_t *)channel_3_dispatcher_new();
    g_dispatchers[4] = (lagfx_dispatcher_base_t *)channel_4_dispatcher_new();
    
    /* Channels 5+: Display vchan dispatchers */
    lagfx_dispatcher_base_t *display_disp = (lagfx_dispatcher_base_t *)display_vchan_dispatcher_new();
    for (int i = 5; i < LAGFX_MAX_CHANNELS; i++) {
        g_dispatchers[i] = display_disp;
    }
    
    LAGFX_LOG("Dispatcher registry: %d channels", LAGFX_MAX_CHANNELS);
     LAGFX_LOG("  - Channel 0   -> Channel0Dispatcher (primary ring)");
     LAGFX_LOG("  - Channel 1   -> Channel1Dispatcher (compute/render)");
     LAGFX_LOG("  - Channel 2   -> Channel2Dispatcher (compute/render)");
     LAGFX_LOG("  - Channel 3   -> Channel3Dispatcher (compute/render)");
     LAGFX_LOG("  - Channel 4   -> Channel4Dispatcher (compute/render)");
     LAGFX_LOG("  - Channels 5+ -> DisplayVchanDispatcher");
     LAGFX_LOG("  - Unregistered → unknown_dispatcher fallback");
}

lagfx_dispatcher_base_t *lagfx_dispatcher_lookup(uint8_t ch) {
    return (ch < LAGFX_MAX_CHANNELS && g_dispatchers[ch]) ? g_dispatchers[ch] : NULL;
}
