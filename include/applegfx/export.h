/*
 * libapplegfx-vulkan — public symbol export annotation
 * include/applegfx/export.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * The library is built with default-hidden symbol visibility; only
 * declarations carrying LAGFX_EXPORT are part of the public ABI.
 */

#ifndef APPLEGFX_EXPORT_H
#define APPLEGFX_EXPORT_H

#if defined(__GNUC__) || defined(__clang__)
#define LAGFX_EXPORT __attribute__((visibility("default")))
#else
#define LAGFX_EXPORT
#endif

#endif /* APPLEGFX_EXPORT_H */
