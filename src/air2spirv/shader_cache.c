/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "air2spirv/shader_cache.h"
#include "common/log.h"

#include <stdlib.h>
#include <string.h>

typedef struct {
    uint8_t key[32];
    lagfx_shader_translation_t value;
    uint64_t lru_counter;
    int used;
} cache_entry_t;

struct lagfx_shader_cache {
    size_t max_entries;
    size_t count;
    cache_entry_t *entries;
    uint64_t next_lru;
    size_t hits;
    size_t misses;
    size_t evictions;
};

static void sha256(const void *data, size_t len, uint8_t out[32]) {
    const uint8_t *buf = (const uint8_t *)data;
    
    if (len == 0) return;

    uint8_t block[64];
    memset(block, 0, sizeof(block));

    uint32_t h[8] = {
        0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
        0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19
    };

    static const uint32_t k[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab125f0c,
        0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,
        0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,
        0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,
        0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,
        0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,
        0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,
        0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };

#define ROTR(x,n) (((x)>>(n))|((x)<<(32-(n))))
#define CH(x,y,z) (((x)&(y))^(~(x)&(z)))
#define MAJ(x,y,z) (((x)&(y))^((x)&(z))^((y)&(z)))
#define EP0(x) (ROTR(x,2)^ROTR(x,13)^ROTR(x,22))
#define EP1(x) (ROTR(x,6)^ROTR(x,11)^ROTR(x,25))
#define SIG0(x) (ROTR(x,7)^ROTR(x,18)^((x)>>3))
#define SIG1(x) (ROTR(x,17)^ROTR(x,19)^((x)>>10))

    size_t pos = 0;
    while (pos < len) {
        size_t chunk_len = (len - pos < 64) ? (len - pos) : 64;
        memcpy(block, buf + pos, chunk_len);

        uint32_t w[64];
        size_t i;
        
        for (i = 0; i < 16; ++i) {
            if (i * 4 < chunk_len) {
                w[i] = ((uint32_t)block[i*4]<<24)|((uint32_t)block[i*4+1]<<16)|
                       ((uint32_t)block[i*4+2]<<8)|(uint32_t)block[i*4+3];
            } else {
                w[i] = 0;
            }
        }

        for (i = 16; i < 64; ++i) {
            uint32_t s0 = SIG0(w[i-15]);
            uint32_t s1 = SIG1(w[i-2]);
            w[i] = s0 + w[i-7] + s1 + w[i-16];
        }

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        uint32_t e = h[4], f = h[5], g = h[6], hh = h[7];

        for (i = 0; i < 64; ++i) {
            uint32_t t1 = hh + EP1(e) + CH(e,f,g) + k[i] + w[i&15];
            uint32_t t2 = EP0(a) + MAJ(a,b,c);
            hh = g; g = f; f = e; e = d + t1;
            d = c; c = b; b = a; a = t1 + t2;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
        h[4] += e; h[5] += f; h[6] += g; h[7] += hh;

        pos += 64;
    }

    out[0] = (h[0]>>24)&0xff; out[1] = (h[0]>>16)&0xff; out[2] = (h[0]>>8)&0xff; out[3] = h[0]&0xff;
    out[4] = (h[1]>>24)&0xff; out[5] = (h[1]>>16)&0xff; out[6] = (h[1]>>8)&0xff; out[7] = h[1]&0xff;
    out[8] = (h[2]>>24)&0xff; out[9] = (h[2]>>16)&0xff; out[10] = (h[2]>>8)&0xff; out[11] = h[2]&0xff;
    out[12] = (h[3]>>24)&0xff; out[13] = (h[3]>>16)&0xff; out[14] = (h[3]>>8)&0xff; out[15] = h[3]&0xff;
    out[16] = (h[4]>>24)&0xff; out[17] = (h[4]>>16)&0xff; out[18] = (h[4]>>8)&0xff; out[19] = h[4]&0xff;
    out[20] = (h[5]>>24)&0xff; out[21] = (h[5]>>16)&0xff; out[22] = (h[5]>>8)&0xff; out[23] = h[5]&0xff;
    out[24] = (h[6]>>24)&0xff; out[25] = (h[6]>>16)&0xff; out[26] = (h[6]>>8)&0xff; out[27] = h[6]&0xff;
    out[28] = (h[7]>>24)&0xff; out[29] = (h[7]>>16)&0xff; out[30] = (h[7]>>8)&0xff; out[31] = h[7]&0xff;
}

static void make_key(const uint8_t *metallib_data, size_t metallib_len,
                     const char *function_name, lagfx_shader_stage_t stage,
                     uint8_t key[32]) {
    if (metallib_len > 500) return;

    uint8_t tmp[512];
    size_t pos = 0;

    memcpy(tmp + pos, metallib_data, metallib_len);
    pos += metallib_len;

    const char *fn = function_name ? function_name : "";
    if (pos + strlen(fn) + 1 > sizeof(tmp)) return;
    memcpy(tmp + pos, fn, strlen(fn) + 1);
    pos += strlen(fn) + 1;

    tmp[pos++] = (uint8_t)stage;

    sha256(tmp, pos, key);
}

static int find_entry(lagfx_shader_cache_t *cache, const uint8_t *key) {
    size_t i;
    for (i = 0; i < cache->count; ++i) {
        if (memcmp(cache->entries[i].key, key, 32) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static size_t find_lru_entry(lagfx_shader_cache_t *cache) {
    size_t idx = 0;
    uint64_t min_lru = cache->entries[0].lru_counter;
    size_t i;
    for (i = 1; i < cache->count; ++i) {
        if (cache->entries[i].lru_counter < min_lru) {
            min_lru = cache->entries[i].lru_counter;
            idx = i;
        }
    }
    return idx;
}

lagfx_shader_cache_t *lagfx_shader_cache_new(size_t max_entries) {
    if (max_entries == 0) {
        max_entries = 64;
    }

    lagfx_shader_cache_t *cache = (lagfx_shader_cache_t *)calloc(1, sizeof(lagfx_shader_cache_t));
    if (!cache) {
        return NULL;
    }

    cache->entries = (cache_entry_t *)calloc(max_entries, sizeof(cache_entry_t));
    if (!cache->entries) {
        free(cache);
        return NULL;
    }

    cache->max_entries = max_entries;
    cache->count = 0;
    cache->next_lru = 1;

    return cache;
}

lagfx_status_t lagfx_shader_cache_get(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      lagfx_shader_translation_t *out) {
    if (!cache || !metallib_data || !function_name || !out) {
        return LAGFX_ERR_INVALID_ARG;
    }

    uint8_t key[32];
    make_key(metallib_data, metallib_len, function_name, stage, key);

    int idx = find_entry(cache, key);
    if (idx < 0) {
        cache->misses++;
        return LAGFX_ERR_NOT_FOUND;
    }

    cache->hits++;
    
    cache->entries[idx].used = 1;
    cache->entries[idx].lru_counter = cache->next_lru++;

    memcpy(out, &cache->entries[idx].value, sizeof(lagfx_shader_translation_t));
    return LAGFX_OK;
}

lagfx_status_t lagfx_shader_cache_put(lagfx_shader_cache_t *cache,
                                      const uint8_t *metallib_data,
                                      size_t metallib_len,
                                      const char *function_name,
                                      lagfx_shader_stage_t stage,
                                      const lagfx_shader_translation_t *translation) {
    if (!cache || !metallib_data || !function_name || !translation || !translation->spv_bytes) {
        return LAGFX_ERR_INVALID_ARG;
    }

    uint8_t key[32];
    make_key(metallib_data, metallib_len, function_name, stage, key);

    int existing = find_entry(cache, key);
    
    size_t lru_idx = 0;
    if (existing >= 0) {
        free((void *)cache->entries[existing].value.spv_bytes);
        cache->count--;
    } else if (cache->count >= cache->max_entries) {
        lru_idx = find_lru_entry(cache);
        free((void *)cache->entries[lru_idx].value.spv_bytes);
        cache->entries[lru_idx].value.spv_bytes = NULL;
        cache->evictions++;
        cache->count--;
    }

    size_t insert_idx;
    if (existing >= 0) {
        insert_idx = (size_t)existing;
    } else {
        insert_idx = lru_idx;
        cache->count++;
    }

    memcpy(cache->entries[insert_idx].key, key, 32);
    cache->entries[insert_idx].value.spv_bytes = translation->spv_bytes;
    cache->entries[insert_idx].value.spv_len = translation->spv_len;
    cache->entries[insert_idx].value.stage = translation->stage;
    cache->entries[insert_idx].lru_counter = cache->next_lru++;
    cache->entries[insert_idx].used = 1;

    return LAGFX_OK;
}

void lagfx_shader_cache_close(lagfx_shader_cache_t *cache) {
    if (!cache) {
        return;
    }

    size_t i;
    for (i = 0; i < cache->count; ++i) {
        free((void *)cache->entries[i].value.spv_bytes);
    }

    free(cache->entries);
    free(cache);
}

void lagfx_shader_cache_stats(const lagfx_shader_cache_t *cache,
                              lagfx_shader_cache_stats_t *out) {
    if (!cache || !out) {
        return;
    }

    out->entries = cache->count;
    out->hits = cache->hits;
    out->misses = cache->misses;
    out->evictions = cache->evictions;
}
