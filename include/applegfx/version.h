/*
 * libapplegfx-vulkan — compile-time version macros
 * include/applegfx/version.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Semver of the public ABI. The build system may override via -D
 * (single source of truth is meson.build's project version); the
 * defaults here track the release the headers shipped with so
 * standalone consumers see the right numbers.
 */

#ifndef APPLEGFX_VERSION_H
#define APPLEGFX_VERSION_H

#ifndef LAGFX_VERSION_MAJOR
#define LAGFX_VERSION_MAJOR 0
#endif
#ifndef LAGFX_VERSION_MINOR
#define LAGFX_VERSION_MINOR 1
#endif
#ifndef LAGFX_VERSION_PATCH
#define LAGFX_VERSION_PATCH 0
#endif

#define LAGFX_VERSION_STRING "0.1.0"

/* Encoded as MMmmpp for numeric comparison. */
#define LAGFX_VERSION \
    (LAGFX_VERSION_MAJOR * 10000 + LAGFX_VERSION_MINOR * 100 + LAGFX_VERSION_PATCH)

#endif /* APPLEGFX_VERSION_H */
