/* SPDX-License-Identifier: MIT */
#ifndef LIBAPPLEGFX_AIR2SPIRV_SHADER_CACHE_H
#define LIBAPPLEGFX_AIR2SPIRV_SHADER_CACHE_H

#include "libapplegfx-vulkan.h"
#include "air2spirv/shader_translate.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lagfx_shader_cache lagfx_shader_cache_t;

/**
 * Create a shader cache with an LRU eviction bound.
 * @param max_entries  cache eviction threshold (LRU when full)
 * @return  handle, or NULL on alloc failure.
 */
lagfx_shader_cache_t *lagfx_shader_cache_new(size_t max_entries);

/**
 * Look up a cached SPIR-V translation by content key.
 * Key = (sha256 of metallib_data) || function_name || stage.
 *
 * @return LAGFX_OK if found (out populated), LAGFX_ERR_NOT_FOUND if miss.
 *         On hit, out->spv_bytes remains valid until the entry is evicted
 *         or the cache is closed.
 */
lagfx_status_t lagfx_shader_cache_get(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      lagfx_shader_translation_t *out);

/**
 * Insert a translation result. Takes ownership of translation->spv_bytes
 * (will free on eviction or close).
 *
 * If the key already exists, the existing entry is replaced (old bytes freed).
 */
lagfx_status_t lagfx_shader_cache_put(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      const lagfx_shader_translation_t *translation);

/** Free all entries. Safe with NULL. */
void lagfx_shader_cache_close(lagfx_shader_cache_t *cache);

/** Diagnostic stats. */
typedef struct {
    size_t entries;
    size_t hits;
    size_t misses;
    size_t evictions;
} lagfx_shader_cache_stats_t;

void lagfx_shader_cache_stats(const lagfx_shader_cache_t *cache,
                              lagfx_shader_cache_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* LIBAPPLEGFX_AIR2SPIRV_SHADER_CACHE_H */
