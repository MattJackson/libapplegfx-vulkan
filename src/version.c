/*
 * libapplegfx-vulkan — version/build-info getters
 * src/version.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Values come from -D preprocessor flags set by meson.build
 * (LAGFX_VERSION_MAJOR/MINOR/PATCH and LAGFX_BUILD_INFO). Defaults
 * allow compiling ad-hoc without meson.
 */

#include "libapplegfx-vulkan.h"

#ifndef LAGFX_VERSION_MAJOR
#define LAGFX_VERSION_MAJOR 0
#endif
#ifndef LAGFX_VERSION_MINOR
#define LAGFX_VERSION_MINOR 0
#endif
#ifndef LAGFX_VERSION_PATCH
#define LAGFX_VERSION_PATCH 1
#endif
#ifndef LAGFX_BUILD_INFO
#define LAGFX_BUILD_INFO "libapplegfx-vulkan (phase-1.A.1, unknown build)"
#endif

int lagfx_version_major(void) { return LAGFX_VERSION_MAJOR; }
int lagfx_version_minor(void) { return LAGFX_VERSION_MINOR; }
int lagfx_version_patch(void) { return LAGFX_VERSION_PATCH; }

const char *lagfx_build_info(void) { return LAGFX_BUILD_INFO; }
