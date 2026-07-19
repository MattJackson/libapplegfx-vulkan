/*
 * libapplegfx-vulkan — Vulkan instance + device + queue init (Phase 1.B)
 * src/vulkan/instance.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
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
 *
 * === Vulkan validation layers ===================================
 *
 * Validation layers are disabled by default (no perf cost in steady-state).
 * Set LAGFX_VK_VALIDATION=1 environment variable to opt-in for debugging
 * Stage 80+ issues. Layers enabled: VK_LAYER_KHRONOS_validation.
 * Debug messenger installed with VERBOSE+WARNING+ERROR severity.
 */

#include "instance.h"
#include "command.h"
#include "pipeline.h"
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

/* Debug message callback for validation layers — prints VUIDs and warnings. */
static VkBool32 VKAPI_PTR lagfx_debug_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData) {
    (void)pUserData;
    (void)messageType;
    if (messageSeverity == VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT) {
        return VK_FALSE;
    }
    LAGFX_WARN("validation: %s", pCallbackData->pMessage);  /* stderr is dropped in-container */
    return VK_FALSE;
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

    /* === Validation layers (optional, gated by env var) ========== */
    const char *validation_env = getenv("LAGFX_VK_VALIDATION");
    bool enable_validation = validation_env && strcmp(validation_env, "1") == 0;

    if (enable_validation) {
        LAGFX_LOG("vk_init: enabling Vulkan validation layers (LAGFX_VK_VALIDATION=1)");
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

    /* === Validation layers (optional) ============================ */
    const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *all_layers[2] = {0};
    uint32_t layer_count = 0;
    
    if (enable_validation) {
        /* Check if validation layer is available */
        uint32_t check_n = 0;
        vkEnumerateInstanceLayerProperties(&check_n, NULL);
        VkLayerProperties *layers = NULL;
        bool found = false;
        
        if (check_n > 0) {
            layers = calloc(check_n, sizeof(*layers));
            if (layers) {
                vkEnumerateInstanceLayerProperties(&check_n, layers);
                for (uint32_t i = 0; i < check_n; ++i) {
                    if (strcmp(layers[i].layerName, "VK_LAYER_KHRONOS_validation") == 0) {
                        found = true;
                        break;
                    }
                }
                free(layers);
            }
        }
        
        if (!found) {
            LAGFX_WARN("vk_init: LAGFX_VK_VALIDATION=1 but VK_LAYER_KHRONOS_validation not available — skipping");
            enable_validation = false;
        } else {
            all_layers[0] = validation_layers[0];
            layer_count = 1;
            LAGFX_LOG("vk_init: validation layer 'VK_LAYER_KHRONOS_validation' enabled");
        }
    }

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
        .enabledLayerCount       = layer_count,
        .ppEnabledLayerNames     = layer_count > 0 ? all_layers : NULL,
        .enabledExtensionCount   = use_inst_n,
        .ppEnabledExtensionNames = use_inst_n ? use_inst_exts : NULL,
    };
    VkResult vr = vkCreateInstance(&ici, NULL, &s->instance);
    if (vr != VK_SUCCESS) {
        LAGFX_ERR("vk_init: vkCreateInstance failed (VkResult=%d)", (int)vr);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* === Debug messenger (optional, gated by env var) ========== */
    if (enable_validation) {
        s->create_debug_messenger_fn = (PFN_vkCreateDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkCreateDebugUtilsMessengerEXT");
        s->destroy_debug_messenger_fn = (PFN_vkDestroyDebugUtilsMessengerEXT)
            vkGetInstanceProcAddr(s->instance, "vkDestroyDebugUtilsMessengerEXT");

        if (s->create_debug_messenger_fn) {
            VkDebugUtilsMessengerCreateInfoEXT mc = {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                                 | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                             | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = lagfx_debug_callback,
            };

            vr = s->create_debug_messenger_fn(s->instance, &mc, NULL, &s->debug_messenger);
            if (vr != VK_SUCCESS) {
                LAGFX_WARN("vk_init: vkCreateDebugUtilsMessengerEXT failed (%d) — validation messages will not be captured", (int)vr);
                s->debug_messenger = VK_NULL_HANDLE;
            } else {
                LAGFX_LOG("vk_init: debug messenger installed");
            }
        } else {
            LAGFX_WARN("vk_init: vkCreateDebugUtilsMessengerEXT not found — validation messages will not be captured");
        }
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
     *
     * Audit fix (#18): query the ICD's actual feature support BEFORE
     * requesting features in vkCreateDevice. Today lavapipe always
     * advertises 1.3 with dynamicRendering=VK_TRUE so the previous
     * unconditional request worked; on a Vulkan 1.2 ICD or any
     * adapter that ships these as optional, vkCreateDevice fails
     * with a cryptic VK_ERROR_FEATURE_NOT_PRESENT. Probe + downgrade.
     *
     * Promoted-to-core features (1.3): dynamicRendering, synchronization2.
     * If the ICD reports these as supported, request them via the
     * VkPhysicalDeviceVulkan13Features chain. If not (Vulkan 1.2 ICD),
     * fall back to the VK_KHR_dynamic_rendering / KHR_synchronization2
     * extension chain — same shape on the wire, different sType.
     * ------------------------------------------------------------- */
    VkPhysicalDeviceVulkan13Features probe13 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
    };
    VkPhysicalDeviceVulkan12Features probe12 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .pNext = &probe13,
    };
    VkPhysicalDeviceShaderObjectFeaturesEXT probe_so = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .pNext = &probe12,
    };
    VkPhysicalDeviceFeatures2 probe2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = want_shader_object ? (void *)&probe_so : (void *)&probe12,
    };
    vkGetPhysicalDeviceFeatures2(s->phys_device, &probe2);

    /* Re-check ICD-advertised support. The "we'd like" set is fixed;
     * if the ICD says no, we still try to create the device with the
     * remaining features (vkCreateDevice will report which one
     * actually fails). */
    bool have_dyn_rendering_feature   = (probe13.dynamicRendering   == VK_TRUE);
    bool have_synchronization2        = (probe13.synchronization2   == VK_TRUE);
    bool have_timeline_semaphore_feat = (probe12.timelineSemaphore  == VK_TRUE);
    bool have_descriptor_indexing     = (probe12.descriptorIndexing == VK_TRUE);
    /* M2 host-flattening: bufferDeviceAddress lets the host hand the shader
     * real device addresses for the resources a Metal argument buffer points
     * to (via SPIR-V PhysicalStorageBuffer), so arg-buffer [[id(n)]] pointer
     * members can be dereferenced. Core 1.2; lavapipe advertises it. */
    bool have_buffer_device_address   = (probe12.bufferDeviceAddress == VK_TRUE);

    if (!have_dyn_rendering_feature) {
        LAGFX_WARN("vk_init: dynamicRendering feature not advertised by ICD; "
                   "draw paths that depend on it will fail. Vulkan 1.3 core "
                   "promotion missing — falling back to KHR_dynamic_rendering "
                   "extension if present (want_dyn_rendering=%d).",
                   (int)want_dyn_rendering);
    }
    if (!have_synchronization2) {
        LAGFX_WARN("vk_init: synchronization2 feature not advertised; "
                   "fence/timeline paths may need pre-1.3 fallback.");
    }
    if (!have_timeline_semaphore_feat) {
        LAGFX_WARN("vk_init: timelineSemaphore feature not advertised; "
                   "deferring downgrade — vkCreateDevice will report.");
    }
    if (!have_descriptor_indexing) {
        LAGFX_WARN("vk_init: descriptorIndexing feature not advertised; "
                   "bindless paths will need rewiring.");
    }
    if (!have_buffer_device_address) {
        LAGFX_WARN("vk_init: bufferDeviceAddress not advertised; M2 arg-buffer "
                   "host-flattening (PhysicalStorageBuffer) will be unavailable.");
    }

    VkPhysicalDeviceShaderObjectFeaturesEXT feat_so = {
        .sType     = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT,
        .shaderObject = VK_TRUE,
    };
    VkPhysicalDeviceVulkan13Features feat13 = {
        .sType             = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES,
        .dynamicRendering  = have_dyn_rendering_feature  ? VK_TRUE : VK_FALSE,
        .synchronization2  = have_synchronization2       ? VK_TRUE : VK_FALSE,
        .pNext             = want_shader_object ? &feat_so : NULL,
    };
    VkPhysicalDeviceVulkan12Features feat12 = {
        .sType                                      = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
        .timelineSemaphore                          = have_timeline_semaphore_feat ? VK_TRUE : VK_FALSE,
        .descriptorIndexing                         = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .runtimeDescriptorArray                     = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .shaderSampledImageArrayNonUniformIndexing  = have_descriptor_indexing     ? VK_TRUE : VK_FALSE,
        .bufferDeviceAddress                        = have_buffer_device_address   ? VK_TRUE : VK_FALSE,
        .pNext                                      = &feat13,
    };
    /* shaderInt64: our AIR→SPIR-V translator emits OpCapability Int64
     * (i64 GEP indices / lifetime sizes) on essentially every shader, so
     * the device MUST enable shaderInt64 or vkCreate{ShaderModule,Graphics
     * Pipelines} rejects the module (VUID-...-pCode-08740 → -13). lavapipe
     * advertises it; enable it when the ICD does. */
    bool have_shader_int64 = (probe2.features.shaderInt64 == VK_TRUE);
    if (!have_shader_int64) {
        LAGFX_WARN("vk_init: shaderInt64 not advertised by ICD; AIR shaders "
                   "that declare OpCapability Int64 will fail pipeline build.");
    }
    VkPhysicalDeviceFeatures2 feat2 = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .features = { .shaderInt64 = have_shader_int64 ? VK_TRUE : VK_FALSE },
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

    s->have_dynamic_rendering      = have_dyn_rendering_feature;
    s->have_synchronization2       = have_synchronization2;
    s->have_timeline_semaphore     = have_timeline_semaphore_feat;
    s->have_descriptor_indexing    = have_descriptor_indexing;
    s->have_shader_object          = want_shader_object;
    s->have_extended_dynamic_state3= want_eds3;
    s->have_buffer_device_address  = have_buffer_device_address;
    LAGFX_LOG("vk_init: bufferDeviceAddress=%d (M2 host-flattening %s)",
              (int)have_buffer_device_address,
              have_buffer_device_address ? "available" : "UNAVAILABLE");

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

    /* Phase 1.B.2: create the command pool AFTER the device + queue are
     * live. Failure here unwinds the whole Vulkan state — a device
     * without a command pool can't make progress in Phase 2. */
    lagfx_status_t cp_st = lagfx_vk_command_pool_create(s);
    if (cp_st != LAGFX_OK) {
        LAGFX_ERR("vk_init: lagfx_vk_command_pool_create failed (status=%d)",
                  (int)cp_st);
        vkDestroyDevice(s->device, NULL);
        vkDestroyInstance(s->instance, NULL);
        free(s);
        return LAGFX_ERR_VULKAN_INIT;
    }

    /* Phase 3.E: create the passthrough pipeline, frame image, and
     * dummy vertex buffer. Failure is non-fatal for device init but
     * draw calls will lack a real pipeline. */
    lagfx_status_t pp_st = lagfx_vk_pipeline_init(s);
    if (pp_st != LAGFX_OK) {
        LAGFX_WARN("vk_init: lagfx_vk_pipeline_init failed (status=%d) "
                   "— draw calls will lack passthrough pipeline",
                   (int)pp_st);
    }

    /* Stage 65d Option 3 — create the shared empty pipeline layout for
     * substitute triangle pipelines. Vulkan rejects layout=VK_NULL_HANDLE
     * in VkGraphicsPipelineCreateInfo, so even shaders with zero
     * descriptor bindings need a valid (empty) layout. */
    {
        VkPipelineLayoutCreateInfo el_plci = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        };
        VkResult lr = vkCreatePipelineLayout(s->device, &el_plci, NULL,
                                             &s->empty_layout);
        if (lr != VK_SUCCESS) {
            LAGFX_WARN("vk_init: vkCreatePipelineLayout(empty_layout) failed "
                       "(%d) — Stage 65d Option 3 pipeline builds will fail",
                       (int)lr);
            s->empty_layout = VK_NULL_HANDLE;
        }
    }

    *out = s;
    return LAGFX_OK;
}

void lagfx_vk_shutdown(struct lagfx_vk_state *state) {
    if (!state) {
        return;
    }
    if (state->frame_in_progress) {
        vkEndCommandBuffer(state->frame_cmdbuf);
        state->frame_in_progress = false;
    }
    if (state->frame_fence != VK_NULL_HANDLE && state->device != VK_NULL_HANDLE) {
        vkDestroyFence(state->device, state->frame_fence, NULL);
        state->frame_fence = VK_NULL_HANDLE;
    }
    if (state->empty_layout != VK_NULL_HANDLE && state->device != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(state->device, state->empty_layout, NULL);
        state->empty_layout = VK_NULL_HANDLE;
    }
    lagfx_vk_pipeline_shutdown(state);
    lagfx_vk_command_pool_destroy(state);
    if (state->device != VK_NULL_HANDLE) {
        vkDestroyDevice(state->device, NULL);
    }
    
    /* Destroy debug messenger before instance */
    if (state->instance != VK_NULL_HANDLE && state->debug_messenger != VK_NULL_HANDLE
        && state->destroy_debug_messenger_fn) {
        state->destroy_debug_messenger_fn(state->instance, state->debug_messenger, NULL);
        state->debug_messenger = VK_NULL_HANDLE;
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
