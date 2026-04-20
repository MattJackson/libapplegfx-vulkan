/*
 * libapplegfx-vulkan — Vulkan instance + device + queue init (Phase 1.B)
 * src/vulkan/instance.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * === Scope =====================================================
 *
 * Minimum-viable Vulkan init path: VkInstance, VkPhysicalDevice
 * selection (preferring lavapipe — VK_PHYSICAL_DEVICE_TYPE_CPU),
 * VkDevice with the feature set libapplegfx-vulkan relies on
 * (dynamicRendering, synchronization2, timelineSemaphore, descriptor
 * indexing), and retrieval of one graphics+compute+transfer queue.
 *
 * No rendering, no swapchain, no command buffers — that lands in
 * later Phase 1.B increments (command pool setup) and Phase 2
 * (actual render encoder plumbing).
 *
 * === Graceful degradation ======================================
 *
 * When the host lacks a Vulkan SDK at build time (meson's
 * dependency('vulkan', required:false) returns .found() == false),
 * this file compiles WITHOUT the LAGFX_HAVE_VULKAN define. In that
 * mode lagfx_vk_init is a no-op that succeeds and returns a tiny
 * state struct with initialized=false. This keeps the rest of the
 * library — lifecycle, protocol decode, MMIO dispatch — fully
 * functional so tests that don't actually render still pass.
 *
 * On Darwin dev hosts without a Vulkan loader (the typical case
 * when working on this codebase from a Mac) we take the no-Vulkan
 * path even when <vulkan/vulkan.h> is available via a third-party
 * SDK — meson's dependency() is the single source of truth.
 *
 * === LP_NUM_THREADS ordering ===================================
 *
 * Mesa lavapipe reads LP_NUM_THREADS at ICD init, which is
 * triggered by the first Vulkan call in the process (vkCreateInstance).
 * device.c calls lagfx_apply_thread_count_env BEFORE invoking
 * lagfx_vk_init, so by the time we hit vkCreateInstance below the
 * env var is in place. See the note in device.c near the setenv.
 */

#include "instance.h"
#include "common/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef LAGFX_HAVE_VULKAN

/* --- Helpers ---------------------------------------------------- */

/* Return true if `name` is in the extension-property array. */
static bool has_ext(const VkExtensionProperties *avail, uint32_t count,
                    const char *name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (strcmp(avail[i].extensionName, name) == 0) {
            return true;
        }
    }
    return false;
}

/* Pretty-print physical device type for logging. */
static const char *phys_type_str(VkPhysicalDeviceType t) {
    switch (t) {
    case VK_PHYSICAL_DEVICE_TYPE_OTHER:          return "other";
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:   return "discrete-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:    return "virtual-GPU";
    case VK_PHYSICAL_DEVICE_TYPE_CPU:            return "CPU (lavapipe-like)";
    default:                                     return "?";
    }
}

/* Pick a queue family with graphics+compute+transfer. Lavapipe
 * exposes one family with all three; hardware drivers typically
 * have at least one such family too. Returns UINT32_MAX on
 * miss. */
static uint32_t pick_queue_family(VkPhysicalDevice phys) {
    uint32_t n = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, NULL);
    if (n == 0) {
        return UINT32_MAX;
    }
    VkQueueFamilyProperties *props = calloc(n, sizeof(*props));
    if (!props) {
        return UINT32_MAX;
    }
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &n, props);

    const VkQueueFlags want = VK_QUEUE_GRAPHICS_BIT
                            | VK_QUEUE_COMPUTE_BIT
                            | VK_QUEUE_TRANSFER_BIT;
    uint32_t found = UINT32_MAX;
    for (uint32_t i = 0; i < n; ++i) {
        if ((props[i].queueFlags & want) == want) {
            found = i;
            break;
        }
    }
    /* Fallback: any graphics family. */
    if (found == UINT32_MAX) {
        for (uint32_t i = 0; i < n; ++i) {
            if (props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                found = i;
                break;
            }
        }
    }
    free(props);
    return found;
}

/* Choose physical device: prefer CPU-type (lavapipe) for our use
 * case — we're targeting headless-CPU rendering. Fall back to the
 * first device with a graphics queue family. */
static VkPhysicalDevice pick_phys_device(VkInstance inst,
                                         uint32_t *qfam_out) {
    uint32_t n = 0;
    if (vkEnumeratePhysicalDevices(inst, &n, NULL) != VK_SUCCESS || n == 0) {
        LAGFX_ERR("vk_init: no Vulkan physical devices enumerable");
        return VK_NULL_HANDLE;
    }
    VkPhysicalDevice *devs = calloc(n, sizeof(*devs));
    if (!devs) {
        return VK_NULL_HANDLE;
    }
    vkEnumeratePhysicalDevices(inst, &n, devs);

    VkPhysicalDevice chosen = VK_NULL_HANDLE;
    uint32_t         chosen_qf = UINT32_MAX;

    /* Pass 1: CPU-type. */
    for (uint32_t i = 0; i < n; ++i) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            uint32_t qf = pick_queue_family(devs[i]);
            if (qf != UINT32_MAX) {
                chosen = devs[i];
                chosen_qf = qf;
                break;
            }
        }
    }

    /* Pass 2: any device with a graphics queue. */
    if (chosen == VK_NULL_HANDLE) {
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t qf = pick_queue_family(devs[i]);
            if (qf != UINT32_MAX) {
                chosen = devs[i];
                chosen_qf = qf;
                break;
            }
        }
    }

    free(devs);
    *qfam_out = chosen_qf;
    return chosen;
}

/* --- Public entry points --------------------------------------- */

lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                             const lagfx_device_descriptor_t *desc) {
    if (!out) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out = NULL;

    if (desc && desc->shell_vulkan_instance) {
        /* Future: share an externally-provided VkInstance. Today we
         * just log and proceed to create our own. */
        LAGFX_LOG("vk_init: shell_vulkan_instance provided but reuse "
                  "not yet implemented; ignoring");
    }

    struct lagfx_vk_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }

    /* === Instance =================================================
     * Request portability-enumeration + debug-utils opportunistically;
     * omit any that the loader doesn't advertise rather than failing.
     * ------------------------------------------------------------- */
    const char *wanted_inst_exts[] = {
        "VK_KHR_portability_enumeration",
        "VK_EXT_debug_utils",
    };
    const size_t wanted_inst_n = sizeof(wanted_inst_exts)
                               / sizeof(wanted_inst_exts[0]);

    uint32_t avail_n = 0;
    vkEnumerateInstanceExtensionProperties(NULL, &avail_n, NULL);
    VkExtensionProperties *avail = NULL;
    if (avail_n > 0) {
        avail = calloc(avail_n, sizeof(*avail));
        if (!avail) {
            free(s);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        vkEnumerateInstanceExtensionProperties(NULL, &avail_n, avail);
    }

    const char *use_inst_exts[2] = {0};
    uint32_t    use_inst_n       = 0;
    VkInstanceCreateFlags inst_flags = 0;
    for (size_t i = 0; i < wanted_inst_n; ++i) {
        if (has_ext(avail, avail_n, wanted_inst_exts[i])) {
            use_inst_exts[use_inst_n++] = wanted_inst_exts[i];
            if (strcmp(wanted_inst_exts[i],
                       "VK_KHR_portability_enumeration") == 0) {
                inst_flags |= 0x00000001; /* VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR */
            }
        } else {
            LAGFX_LOG("vk_init: instance ext '%s' not present — skipping",
                      wanted_inst_exts[i]);
        }
    }
    free(avail);

    VkApplicationInfo app = {
        .sType              = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName   = "libapplegfx-vulkan",
        .applicationVersion = 0,
        .pEngineName        = "libapplegfx-vulkan",
        .engineVersion      = 0,
        /* 1.3 is safer than 1.4 for max ICD compatibility; our host
         * runs 1.4 but lots of deploy targets (older Mesa on older
         * distros) cap at 1.3. We don't need 1.4 features. */
        .apiVersion         = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType                   = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .flags                   = inst_flags,
        .pApplicationInfo        = &app,
        .enabledExtensionCount   = use_inst_n,
        .ppEnabledExtensionNames = use_inst_n ? use_inst_exts : NULL,
    };
    VkResult vr = vkCreateInstance(&ici, NULL, &s->instance);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("vk_init: vkCreateInstance failed (VkResult=%d)", (int)vr);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* === Physical device ========================================= */
    s->phys_device = pick_phys_device(s->instance,
                                      &s->graphics_queue_family);
    if (s->phys_device == VK_NULL_HANDLE) {
        LAGFX_ERR("vk_init: no suitable physical device");
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }
    vkGetPhysicalDeviceProperties(s->phys_device, &s->phys_props);
    LAGFX_LOG("vk_init: phys device '%s' type=%s api=%u.%u.%u qf=%u",
              s->phys_props.deviceName,
              phys_type_str(s->phys_props.deviceType),
              VK_VERSION_MAJOR(s->phys_props.apiVersion),
              VK_VERSION_MINOR(s->phys_props.apiVersion),
              VK_VERSION_PATCH(s->phys_props.apiVersion),
              s->graphics_queue_family);

    /* === Device extensions ======================================= */
    uint32_t dext_n = 0;
    vkEnumerateDeviceExtensionProperties(s->phys_device, NULL, &dext_n, NULL);
    VkExtensionProperties *dext = NULL;
    if (dext_n > 0) {
        dext = calloc(dext_n, sizeof(*dext));
        if (!dext) {
            vkDestroyInstance(s->instance, NULL);
            free(s);
            return LAGFX_ERR_OUT_OF_MEMORY;
        }
        vkEnumerateDeviceExtensionProperties(s->phys_device, NULL,
                                              &dext_n, dext);
    }

    /* Required (core-1.3 promoted but still enumerated as extensions
     * on some ICDs; ask explicitly for widest compatibility). */
    const char *wanted_dev_exts[8];
    uint32_t    wanted_dev_n = 0;
    bool want_dyn_rendering =
        has_ext(dext, dext_n, "VK_KHR_dynamic_rendering");
    bool want_shader_object =
        has_ext(dext, dext_n, "VK_EXT_shader_object");
    bool want_eds3 =
        has_ext(dext, dext_n, "VK_EXT_extended_dynamic_state3");

    if (want_dyn_rendering) {
        wanted_dev_exts[wanted_dev_n++] = "VK_KHR_dynamic_rendering";
    } else {
        LAGFX_LOG("vk_init: VK_KHR_dynamic_rendering missing — relying on "
                  "Vulkan 1.3 core promotion");
    }
    if (want_shader_object) {
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_shader_object";
    } else {
        LAGFX_LOG("vk_init: VK_EXT_shader_object missing — will use "
                  "pipeline fallback in later phases");
    }
    if (want_eds3) {
        wanted_dev_exts[wanted_dev_n++] = "VK_EXT_extended_dynamic_state3";
    } else {
        LAGFX_LOG("vk_init: VK_EXT_extended_dynamic_state3 missing — "
                  "later phases may need fallbacks");
    }
    free(dext);

    /* === Device features =========================================
     * Request via the standard Vulkan-1.3 feature chain. If any
     * requested feature isn't supported, we could either fail or
     * downgrade; for a minimum-viable init we log and continue —
     * vkCreateDevice will surface the actual error.
     * ------------------------------------------------------------- */
    VkPhysicalDeviceShaderObjectFeaturesEXT feat_so = {
        .sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .shaderObject = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering  = VK_TRUE,
        .synchronization2  = VK_TRUE,
        .pNext             = want_shader_object ? &feat_so : NULL,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType                                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore                          = VK_TRUE,
        .descriptorIndexing                         = VK_TRUE,
        .runtimeDescriptorArray                     = VK_TRUE,
        .shaderSampledImageArrayNonUniformIndexing  = VK_TRUE,
        .pNext                                      = &feat13,
    };
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &feat12,
    };

    /* Queue create info — one graphics+compute+transfer queue. */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = s->graphics_queue_family,
        .queueCount       = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType                   = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .pNext                   = &feat2,
        .queueCreateInfoCount    = 1,
        .pQueueCreateInfos       = &qci,
        .enabledExtensionCount   = wanted_dev_n,
        .ppEnabledExtensionNames = wanted_dev_n ? wanted_dev_exts : NULL,
    };
    vr = vkCreateDevice(s->phys_device, &dci, NULL, &s->device);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("vk_init: vkCreateDevice failed (VkResult=%d)", (int)vr);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    s->have_dynamic_rendering      = true;
    s->have_synchronization2       = true;
    s->have_timeline_semaphore     = true;
    s->have_descriptor_indexing    = true;
    s->have_shader_object          = want_shader_object;
    s->have_extended_dynamic_state3= want_eds3;

    vkGetDeviceQueue(s->device, s->graphics_queue_family, 0,
                     &s->graphics_queue);
    if (s->graphics_queue == VK_NULL_HANDLE) {
        LAGFX_ERR("vk_init: failed to retrieve queue");
        vkDestroyDevice(s->device, NULL);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    s->initialized = true;
    LAGFX_LOG("vk_init: device=%p queue=%p (fam=%u) shader_object=%d "
              "eds3=%d", (void *)s->device, (void *)s->graphics_queue,
              s->graphics_queue_family,
              (int)s->have_shader_object,
              (int)s->have_extended_dynamic_state3);

    *out = s;
    return LAGFX_OK;
}

void lagfx_vk_shutdown(struct lagfx_vk_state *state) {
    if (!state) {
        return;
    }
    if (state->device != VK_NULL_HANDLE) {
        vkDestroyDevice(state->device, NULL);
    }
    if (state->instance != VK_NULL_HANDLE) {
        vkDestroyInstance(state->instance, NULL);
    }
    memset(state, 0, sizeof(*state));
    free(state);
}

#else  /* !LAGFX_HAVE_VULKAN -------------------------------------- */

/* No-vulkan build: provide stubs that keep the rest of the library
 * functional for scaffolding + lifecycle tests. */

lagfx_status_t lagfx_vk_init(struct lagfx_vk_state **out,
                             const lagfx_device_descriptor_t *desc) {
    (void)desc;
    if (!out) {
        return LAGFX_ERR_INVALID_ARG;
    }
    struct lagfx_vk_state *s = calloc(1, sizeof(*s));
    if (!s) {
        return LAGFX_ERR_OUT_OF_MEMORY;
    }
    s->initialized = false;
    LAGFX_LOG("vk_init: built without Vulkan — init is a no-op");
    *out = s;
    return LAGFX_OK;
}

void lagfx_vk_shutdown(struct lagfx_vk_state *state) {
    if (!state) {
        return;
    }
    memset(state, 0, sizeof(*state));
    free(state);
}

#endif /* LAGFX_HAVE_VULKAN */
