/*
 * libapplegfx-vulkan — Phase 4 reference SPIR-V emitter for storage-image
 * read/write (Pattern K). src/air2spv/emit_storage_image.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Produces a GLCompute shader that declares two storage images (Rgba8 format)
 * at DescriptorSet 0 Bindings 0 and 1, runs with LocalSize = (1, 1, 1), and
 * reads pixel (0, 0) from the input image and writes it to pixel (0, 0) of
 * the output image using OpImageRead / OpImageWrite.
 *
 * Pipeline shape:
 *   Execution model: GLCompute with LocalSize(1, 1, 1)
 *   Input storage image: set 0 binding 0 = Rgba8 format
 *   Output storage image: set 0 binding 1 = Rgba8 format
 *   Coords: int2 (0, 0) hardcoded for this reference emitter
 *
 * Validates clean under `spirv-val --target-env vulkan1.0`.
 */

#ifndef LIBAPPLEGFX_AIR2SPV_EMIT_STORAGE_IMAGE_H
#define LIBAPPLEGFX_AIR2SPV_EMIT_STORAGE_IMAGE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int lagfx_air2spv_emit_storage_image_stub(uint8_t **out_blob, size_t *out_size);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBAPPLEGFX_AIR2SPV_EMIT_STORAGE_IMAGE_H */
