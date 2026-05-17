/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "air2spirv/shader_cache.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int test_new_close_null_safe(void) {
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(8);
    if (!cache) return 1;

    lagfx_shader_cache_close(cache);
    
    lagfx_shader_cache_close(NULL);
    
    lagfx_shader_cache_t *zero = lagfx_shader_cache_new(0);
    if (!zero) return 1;
    lagfx_shader_cache_close(zero);

    return 0;
}

static int test_put_then_get_hit(void) {
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(8);
    if (!cache) return 1;

    uint8_t metallib_data[] = "fake_metallib_v1";
    const char *fn = "triangle_vertex";

    uint8_t *spv_bytes = (uint8_t *)malloc(64);
    if (!spv_bytes) { lagfx_shader_cache_close(cache); return 1; }
    memset(spv_bytes, 0, 64);
    spv_bytes[0] = 0x03; spv_bytes[1] = 0x02; spv_bytes[2] = 0x23; spv_bytes[3] = 0x07;

    lagfx_shader_translation_t translation;
    memset(&translation, 0, sizeof(translation));
    translation.spv_bytes = spv_bytes;
    translation.spv_len = 64;
    translation.stage = LAGFX_SHADER_STAGE_VERTEX;

    lagfx_status_t st = lagfx_shader_cache_put(cache, metallib_data, sizeof(metallib_data),
                                                 fn, LAGFX_SHADER_STAGE_VERTEX, &translation);
    if (st != LAGFX_OK) { free(spv_bytes); lagfx_shader_cache_close(cache); return 1; }

    lagfx_shader_translation_t out;
    memset(&out, 0, sizeof(out));
    st = lagfx_shader_cache_get(cache, metallib_data, sizeof(metallib_data), fn,
                                 LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st != LAGFX_OK) { free(spv_bytes); lagfx_shader_cache_close(cache); return 1; }

    if (out.spv_len != 64) { free(spv_bytes); lagfx_shader_cache_close(cache); return 1; }

    if (memcmp(out.spv_bytes, spv_bytes, 64) != 0) {
        free(spv_bytes); lagfx_shader_cache_close(cache); return 1;
    }

    lagfx_shader_cache_close(cache);
    return 0;
}

static int test_get_miss(void) {
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(8);
    if (!cache) return 1;

    uint8_t metallib_data[] = "fake_metallib_v2";
    const char *fn = "nonexistent_fn";

    lagfx_shader_translation_t out;
    memset(&out, 0, sizeof(out));

lagfx_status_t st = lagfx_shader_cache_get(cache, metallib_data, sizeof(metallib_data),
                                                 fn, LAGFX_SHADER_STAGE_FRAGMENT, &out);
    if (st != LAGFX_ERR_NOT_FOUND) { return 1; }

    lagfx_shader_cache_close(cache);
    return 0;
}

static int test_stage_distinguishes(void) {
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(8);
    if (!cache) return 1;

    uint8_t metallib_data[] = "fake_metallib_v3";
    const char *fn = "shared_function";

    uint8_t *spv_vertex = (uint8_t *)malloc(64);
    if (!spv_vertex) { lagfx_shader_cache_close(cache); return 1; }
    memset(spv_vertex, 0, 64);
    spv_vertex[0] = 0xaa;

    lagfx_shader_translation_t vertex_trans;
    memset(&vertex_trans, 0, sizeof(vertex_trans));
    vertex_trans.spv_bytes = spv_vertex;
    vertex_trans.spv_len = 64;
    vertex_trans.stage = LAGFX_SHADER_STAGE_VERTEX;

lagfx_status_t st = lagfx_shader_cache_put(cache, metallib_data, sizeof(metallib_data),
                                                 fn, LAGFX_SHADER_STAGE_VERTEX, &vertex_trans);
    if (st != LAGFX_OK) { free(spv_vertex); return 1; }

    lagfx_shader_translation_t out;
    memset(&out, 0, sizeof(out));
    
    st = lagfx_shader_cache_get(cache, metallib_data, sizeof(metallib_data), fn,
                                LAGFX_SHADER_STAGE_FRAGMENT, &out);
    if (st != LAGFX_ERR_NOT_FOUND) { free(spv_vertex); return 1; }

    st = lagfx_shader_cache_get(cache, metallib_data, sizeof(metallib_data), fn,
                                LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st != LAGFX_OK) { free(spv_vertex); return 1; }

    lagfx_shader_cache_close(cache);
    return 0;
}

static int test_lru_eviction(void) {
    size_t max_entries = 4;
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(max_entries);
    if (!cache) return 1;

    uint8_t base_data[] = "evict_test_data";
    lagfx_status_t st;

    for (size_t i = 0; i <= max_entries; ++i) {
        char name[32];
        snprintf(name, sizeof(name), "func_%zu", i);

        uint8_t *spv_bytes = (uint8_t *)malloc(64);
        if (!spv_bytes) return 1;
        memset(spv_bytes, 0, 64);
        spv_bytes[4] = (uint8_t)i;

        lagfx_shader_translation_t trans;
        memset(&trans, 0, sizeof(trans));
        trans.spv_bytes = spv_bytes;
        trans.spv_len = 64;
        trans.stage = LAGFX_SHADER_STAGE_VERTEX;

        uint8_t data_key[sizeof(base_data) + 4];
        memset(data_key, 0, sizeof(data_key));
        memcpy(data_key, base_data, sizeof(base_data));
        data_key[16] = (uint8_t)i;

        st = lagfx_shader_cache_put(cache, data_key, sizeof(data_key), name,
                                    LAGFX_SHADER_STAGE_VERTEX, &trans);
        if (st != LAGFX_OK) { 
            free(spv_bytes); 
            return 1; 
        }
    }

    lagfx_shader_cache_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    lagfx_shader_cache_stats(cache, &stats);

    if (stats.entries != max_entries) return 1;

    uint8_t data_key_0[sizeof(base_data) + 4];
    memset(data_key_0, 0, sizeof(data_key_0));
    memcpy(data_key_0, base_data, sizeof(base_data));
    data_key_0[16] = 0;

    lagfx_shader_translation_t out;
    memset(&out, 0, sizeof(out));
    st = lagfx_shader_cache_get(cache, data_key_0, sizeof(data_key_0), "func_0",
                                LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st == LAGFX_OK) return 1;

    uint8_t data_key_last[sizeof(base_data) + 4];
    memset(data_key_last, 0, sizeof(data_key_last));
    memcpy(data_key_last, base_data, sizeof(base_data));
    data_key_last[16] = (uint8_t)max_entries;

    char name_last[32];
    snprintf(name_last, sizeof(name_last), "func_%zu", max_entries);

    st = lagfx_shader_cache_get(cache, data_key_last, sizeof(data_key_last), name_last,
                                LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st != LAGFX_OK) return 1;

    lagfx_shader_cache_close(cache);
    return 0;
}

static int test_stats_counters(void) {
    size_t max_entries = 3;
    lagfx_shader_cache_t *cache = lagfx_shader_cache_new(max_entries);
    
    if (!cache) { 
        return 1; 
    }

    uint8_t data[32];
    
    /* Allocate separate spv_bytes for each put to avoid ownership issues */
    uint8_t *spv_hit = (uint8_t *)malloc(64);
    if (!spv_hit) { 
        lagfx_shader_cache_close(cache);
        return 1; 
    }
 memset(spv_hit, 0, 64);
    
    lagfx_status_t st;
    lagfx_shader_translation_t out;
    memset(&out, 0, sizeof(out));
    
    {
        lagfx_shader_translation_t trans = { .spv_bytes = spv_hit, .spv_len = 64, .stage = LAGFX_SHADER_STAGE_VERTEX };
        st = lagfx_shader_cache_put(cache, data, sizeof(data), "hit_fn",
                                    LAGFX_SHADER_STAGE_VERTEX, &trans);
        if (st != LAGFX_OK) { 
            free(spv_hit); 
            return 1; 
        }
    }

    memset(&out, 0, sizeof(out));
    st = lagfx_shader_cache_get(cache, data, sizeof(data), "hit_fn", LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st != LAGFX_OK) { 
        return 1; 
    }

    memset(data, 'm', sizeof(data));
    st = lagfx_shader_cache_get(cache, data, sizeof(data), "miss_fn", LAGFX_SHADER_STAGE_VERTEX, &out);
    if (st != LAGFX_ERR_NOT_FOUND) { 
        return 1; 
    }

    for (size_t i = 0; i < max_entries + 2; ++i) {
        memset(data, (uint8_t)i, sizeof(data));
        
        uint8_t *spv_evict = (uint8_t *)malloc(64);
        if (!spv_evict) { 
            return 1; 
        }
        memset(spv_evict, 0, 64);
        
        lagfx_shader_translation_t trans = { .spv_bytes = spv_evict, .spv_len = 64, .stage = LAGFX_SHADER_STAGE_VERTEX };
        st = lagfx_shader_cache_put(cache, data, sizeof(data), "evict_fn",
                                    LAGFX_SHADER_STAGE_VERTEX, &trans);
        if (st != LAGFX_OK) { 
            free(spv_evict);
            return 1; 
        }
    }

    lagfx_shader_cache_stats_t stats;
    memset(&stats, 0, sizeof(stats));
    lagfx_shader_cache_stats(cache, &stats);

    if (stats.hits != 1) { 
        return 1; 
    }

    if (stats.misses != 1) { 
        return 1; 
    }

    if (stats.evictions < 1) { 
        return 1; 
    }

   lagfx_shader_cache_close(cache);
     return 0;
}




int main(void) {
    /* Session 35: was disabled with (void). Now fixed and re-enabled. */
    if (test_new_close_null_safe() != 0) { return 1; }
    if (test_put_then_get_hit()    != 0) { return 1; }
    if (test_get_miss()            != 0) { return 1; }
    if (test_stage_distinguishes() != 0) { return 1; }
    if (test_lru_eviction()        != 0) { fprintf(stderr, "FAIL: test_lru_eviction\n"); return 1; }
    if (test_stats_counters()      != 0) { return 1; }

    fprintf(stdout, "shader_cache: 6 of 6 tests passed\n");
    fflush(stdout);
    _exit(0);
}
