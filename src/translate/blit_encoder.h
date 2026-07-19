/*
 * libapplegfx-vulkan — Blit-encoder Vulkan translation (M5 phase 1)
 * src/translate/blit_encoder.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Public API for blit opcode Vulkan translation. Minimal implementation
 * targeting first visible pixels (FillTextureWithColor + CopyFromTextureToTexture).
 */

#ifndef LIBAPPLEGFX_TRANSLATE_BLIT_ENCODER_H
#define LIBAPPLEGFX_TRANSLATE_BLIT_ENCODER_H

#include "libapplegfx-vulkan.h"
#include <stdint.h>
#include <stdbool.h>
#include "../vulkan/instance.h"

/* MTL types copied from Metal headers for blit API compatibility */
typedef struct {
    uint32_t x;
    uint32_t y;
    uint32_t z;
} MTLOrigin;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t depth;
} MTLSize;

/* FillTextureWithColor (0x141) — clear texture region to solid color */
lagfx_status_t lagfx_translate_blit_fill_texture_color(void *vk,
                                                       void *texture_ref,
                                                       uint32_t level,
                                                       uint32_t slice,
                                                       const MTLOrigin *origin,
                                                       const MTLSize *size,
                                                       const double color[4],
                                                       uint32_t pixel_format);

/* CopyFromTextureToTexture (0x12f) — blit surface copy */
lagfx_status_t lagfx_translate_blit_copy_texture_to_texture(void *vk,
                                                            void *src_texture_ref,
                                                            void *dst_texture_ref,
                                                            const MTLOrigin *src_origin,
                                                            const MTLSize *src_size,
                                                            uint32_t dst_slice,
                                                            uint32_t dst_level);

#endif /* LIBAPPLEGFX_TRANSLATE_BLIT_ENCODER_H */
