/*
 * libapplegfx-vulkan — Channel dispatcher registry header
 * src/dispatchers/registry.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Global registry maps channel IDs to dispatcher instances. Doorbell handler
 * looks up by channel ID and calls polymorphic dispatch.
 */

#ifndef LAGFX_DISPATCHER_REGISTRY_H
#define LAGFX_DISPATCHER_REGISTRY_H

#include "base.h"

/** Initialize the dispatcher registry (called once on first protocol creation) */
void lagfx_dispatcher_registry_init(void);

/** Look up dispatcher for a given channel ID — returns NULL if unregistered */
lagfx_dispatcher_base_t *lagfx_dispatcher_lookup(uint8_t ch);

#endif /* LAGFX_DISPATCHER_REGISTRY_H */
