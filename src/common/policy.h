/*
 * libapplegfx-vulkan — feature policy
 * src/common/policy.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * The proven render-path features (arbitrated reads, multi-slot vertex
 * sourcing, per-pass routing + ordered scanout assembly, real-shader
 * translation, texture demux/fallbacks) are default-on. Each keeps a
 * dev-only kill-switch (LAGFX_DISABLE_<NAME>) so a live regression can be
 * bisected per-feature without a rebuild.
 */

#ifndef LAGFX_COMMON_POLICY_H
#define LAGFX_COMMON_POLICY_H

#include <stdlib.h>

/* True unless the feature's kill-switch env is set.
 * Usage: if (LAGFX_POLICY("M2_READARB")) { ... } */
#define LAGFX_POLICY(name) (getenv("LAGFX_DISABLE_" name) == NULL)

#endif /* LAGFX_COMMON_POLICY_H */
