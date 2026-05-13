/*
 * libapplegfx-vulkan — Channel door dispatcher (BAR0+0x1020)
 * src/dispatchers/channel_door_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 */

#include "channel_door_dispatcher.h"
#include "channel_0_dispatcher.h"
#include "channel_compute_dispatcher.h"
#include "channel_display_dispatcher.h"
#include "../common/log.h"
#include "protocol/state.h"

void channel_door_dispatcher_dispatch(void *protocol_state, uint32_t data) {
    lagfx_protocol_t *p = (lagfx_protocol_t *)protocol_state;

    if (!lagfx_protocol_is_valid(p)) {
        LAGFX_WARN("channel_door: invalid protocol state");
        return;
    }

    /* Data contains channel ID written to BAR0+0x1020 */
    uint32_t chan_id = data & 0xFFu;

    if (chan_id >= LAGFX_MAX_CHANNELS) {
        LAGFX_TRACE("channel_door: chan_id=%u out of range", chan_id);
        return;
    }

    /* Update protocol state with channel ID */
    p->current_chan_id = (uint8_t)chan_id;

    LAGFX_LOG("channel_door: routing ch=%u → sub-dispatcher", chan_id);

    switch (chan_id) {
        case 0:
            /* Root/primary ring FIFO - trigger drain */
            channel_0_dispatcher_drain(p);
            break;

        case 1 ... 4:
            /* Compute channels (vchans) */
            channel_compute_dispatcher_drain(p, chan_id);
            break;

        case 5 ... 15:
            /* Display pipes (display_index + 5) */
            channel_display_dispatcher_drain(p, chan_id);
            break;

        default:
            LAGFX_WARN("channel_door: unknown channel %u", chan_id);
            break;
    }
}
