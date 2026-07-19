/*
 * libapplegfx-vulkan — stock shader VkShaderModule smoke (Phase 3.C)
 * tests/stock-shaders.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Complement to tests/shader-catalog.c. That test proves the five
 * stock-shader SPIR-V blobs embedded at build time begin with the
 * right magic word + are at least header-sized; this one proves the
 * loader itself accepts them by round-tripping every (kind, stage)
 * pair through vkCreateShaderModule + vkDestroyShaderModule.
 *
 * Scope:
 *   - BLIT, COMPOSITE_OVER, COLOR_FILL, CURSOR — the four stock
 *     shaders the M4/M5 compositor consumes (per the Phase 3.C plan:
 *     texture-blit, alpha-over layer composite, solid fill, cursor
 *     glyph).
 *   - CLEAR rounds out the catalog and is tested too since it shares
 *     the same codepath.
 *
 * The test reuses lagfx_device_new so the Vulkan init logic (ICD
 * discovery, physical-device pick, logical-device + queue create)
 * is the same as the production path. On hosts with no loadable ICD
 * (typical Darwin dev machine), lagfx_device_new still returns a
 * device object but dev->vk->initialized is false — in that case we
 * emit SKIP rather than failing, matching tests/vulkan-render.c.
 *
 * Gated in tests/meson.build on vulkan_dep being present at
 * configure time.
 */

#include "libapplegfx-vulkan.h"

#include "device.h"
#include "shaders/catalog.h"
#include "vulkan/instance.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail    = 0;
static int g_skipped = 0;

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s\n", msg); \
        g_fail++; \
    } else { \
        fprintf(stdout, "PASS: %s\n", msg); \
    } \
} while (0)

#define SKIP(msg) do { \
    fprintf(stdout, "SKIP: %s\n", msg); \
    g_skipped++; \
} while (0)

/* Shell callback stubs — none should fire in a shader-module smoke. */
static lagfx_task_t *cb_create_task(void *o, uint64_t s, void **p) {
    (void)o; (void)s; (void)p; return NULL;
}
static void cb_destroy_task(void *o, lagfx_task_t *t) { (void)o; (void)t; }
static bool cb_map(void *a, lagfx_task_t *t, uint64_t o,
                   const lagfx_physical_range_t *r, size_t c, bool ro) {
    (void)a; (void)t; (void)o; (void)r; (void)c; (void)ro; return true;
}
static bool cb_unmap(void *a, lagfx_task_t *t, uint64_t o, uint64_t l) {
    (void)a; (void)t; (void)o; (void)l; return true;
}
static bool cb_read_memory(void *a, uint64_t g, uint64_t l, void *d) {
    (void)a; (void)g; (void)l; (void)d; return true;
}
static void cb_raise_irq(void *a, uint32_t v) { (void)a; (void)v; }

static lagfx_device_t *make_device(char **errp_out) {
    lagfx_device_descriptor_t ddesc;
    memset(&ddesc, 0, sizeof(ddesc));
    ddesc.shell.opaque          = (void *)0xfeedu;
    ddesc.shell.create_task     = cb_create_task;
    ddesc.shell.destroy_task    = cb_destroy_task;
    ddesc.shell.map_memory      = cb_map;
    ddesc.shell.unmap_memory    = cb_unmap;
    ddesc.shell.read_memory     = cb_read_memory;
    ddesc.shell.raise_interrupt = cb_raise_irq;
    ddesc.thread_count          = 2;
    return lagfx_device_new(&ddesc, errp_out);
}

#ifdef LAGFX_HAVE_VULKAN
/* Attempt vkCreateShaderModule on one catalog (kind, stage) pair.
 * Increments g_fail on any failure. Returns 0 on success. */
static int try_create_module(struct lagfx_vk_state *vk,
                             lagfx_shader_kind_t kind,
                             lagfx_shader_stage_t stage,
                             const char *label) {
    const lagfx_shader_blob_t *b = lagfx_shader_catalog_lookup_stage(kind, stage);
    char msg[192];

    snprintf(msg, sizeof(msg), "%s: catalog blob present", label);
    CHECK(b != NULL, msg);
    if (!b) {
        return 1;
    }

    /* SPIR-V word-size contract: byte length must be a multiple of 4.
     * vkCreateShaderModule will reject anything that isn't. */
    snprintf(msg, sizeof(msg), "%s: blob length is 4-byte aligned (%zu bytes)",
             label, b->spirv_len);
    CHECK((b->spirv_len % 4u) == 0u, msg);

    VkShaderModuleCreateInfo ci = {
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = b->spirv_len,
        .pCode    = (const uint32_t *)b->spirv_bytes,
    };
    VkShaderModule mod = VK_NULL_HANDLE;
    VkResult vr = vkCreateShaderModule(vk->device, &ci, NULL, &mod);

    snprintf(msg, sizeof(msg),
             "%s: vkCreateShaderModule returns VK_SUCCESS (got %d)",
             label, (int)vr);
    CHECK(vr == VK_SUCCESS, msg);

    snprintf(msg, sizeof(msg), "%s: VkShaderModule handle non-NULL", label);
    CHECK(mod != VK_NULL_HANDLE, msg);

    if (mod != VK_NULL_HANDLE) {
        vkDestroyShaderModule(vk->device, mod, NULL);
    }
    return (vr == VK_SUCCESS && mod != VK_NULL_HANDLE) ? 0 : 1;
}

/* Walk every catalog kind × {vertex, fragment} and round-trip the
 * blob through vkCreateShaderModule. */
static int exercise_catalog(struct lagfx_vk_state *vk) {
    struct kind_row {
        lagfx_shader_kind_t kind;
        const char          *label;
    };
    static const struct kind_row rows[] = {
        { LAGFX_SHADER_BLIT,           "BLIT"           },
        { LAGFX_SHADER_CLEAR,          "CLEAR"          },
        { LAGFX_SHADER_COMPOSITE_OVER, "COMPOSITE_OVER" },
        { LAGFX_SHADER_CURSOR,         "CURSOR"         },
        { LAGFX_SHADER_COLOR_FILL,     "COLOR_FILL"     },
    };
    const size_t n = sizeof(rows) / sizeof(rows[0]);

    for (size_t i = 0; i < n; ++i) {
        char vlabel[64];
        char flabel[64];
        snprintf(vlabel, sizeof(vlabel), "%s/vert", rows[i].label);
        snprintf(flabel, sizeof(flabel), "%s/frag", rows[i].label);
        (void)try_create_module(vk, rows[i].kind,
                                LAGFX_SHADER_STAGE_VERTEX, vlabel);
        (void)try_create_module(vk, rows[i].kind,
                                LAGFX_SHADER_STAGE_FRAGMENT, flabel);
    }
    return 0;
}
#endif /* LAGFX_HAVE_VULKAN */

int main(void) {
    fprintf(stdout, "=== libapplegfx-vulkan stock shader module smoke ===\n");
    fprintf(stdout, "build: %s\n", lagfx_build_info());

    char *err = NULL;
    lagfx_device_t *dev = make_device(&err);

    if (!dev) {
        fprintf(stdout, "device_new returned NULL (err=%s)\n",
                err ? err : "<none>");
        free(err);
        SKIP("no loadable Vulkan ICD on this host — "
             "stock-shaders runtime path unavailable");
        fprintf(stdout, "\n=== Summary: %d skipped, 0 failed ===\n",
                g_skipped);
        return 0;
    }

#ifdef LAGFX_HAVE_VULKAN
    if (dev->vk && dev->vk->initialized) {
        (void)exercise_catalog(dev->vk);
    } else {
        SKIP("LAGFX_HAVE_VULKAN set but dev->vk->initialized is false");
    }
#else
    SKIP("library built without Vulkan — shader module path is stubbed");
#endif

    lagfx_device_free(dev);
    CHECK(1, "teardown completes");

    fprintf(stdout, "\n=== Summary: %s (%d skipped) ===\n",
            g_fail ? "FAILURES" : "ALL GOOD", g_skipped);
    return g_fail ? 1 : 0;
}
