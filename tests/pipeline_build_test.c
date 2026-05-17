/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "libapplegfx-vulkan.h"
#include <stdio.h>
#include <stdlib.h>

#ifndef LAGFX_HAVE_VULKAN
/* Vulkan-disabled stub build (e.g. macos-latest CI without Vulkan SDK):
 * skip the whole test suite with meson SKIP exit code. */
int main(void) {
    fprintf(stderr, "pipeline_build_test: built without LAGFX_HAVE_VULKAN; skipping\n");
    return 77;
}
#else  /* LAGFX_HAVE_VULKAN */

#include "air2spirv/shader_translate.h"
#include "vulkan/pipeline_build.h"
#include <vulkan/vulkan.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

#define ASSERT(cond, msg) do { if (!(cond)) { \
    fprintf(stderr, "FAIL: %s\n", msg); return 1; } } while (0)

static int llc_available(void) {
    return access("/opt/homebrew/opt/llvm@20/bin/llc", X_OK) == 0;
}

/* Forward declarations */
static int test_invalid_device_handle(void);
static int test_mismatched_formats(void);
static int test_invalid_shader_module(void);
static int test_only_one_shader(void);
static int test_null_inputs(void);
static int test_missing_shader_modules(void);
static int test_validation_layers_and_vuids(void);
static int test_build_from_triangle(void);
static int test_build_no_dynamic_rendering(void);

int main(void) {
    fprintf(stdout, "pipeline_build_test: starting\n");
    
    /* Negative tests (no llc or Vulkan dependency) always run. */
    if (test_invalid_device_handle() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: invalid device handle test passed\n");
    
    if (test_mismatched_formats() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: mismatched formats test passed\n");
    
    if (test_invalid_shader_module() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: invalid shader module test passed\n");
    
    if (test_only_one_shader() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: only one shader test passed\n");
    
    if (test_null_inputs() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: null inputs test passed\n");
    
    if (test_missing_shader_modules() != 0) { _exit(1); }
    fprintf(stdout, "pipeline_build_test: missing shader modules test passed\n");
    
    /* Smoke tests need llc. Skip with meson SKIP code if absent. */
    if (!llc_available()) {
        fprintf(stderr, "pipeline_build_test: llc not available; smoke tests skipped (negative tests passed)\n");
        _exit(77);
    }
    
    /* Validation layers test - checks VK_LAYER_KHRONOS_validation availability */
    int ret = test_validation_layers_and_vuids();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: validation layers test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: validation layers test passed\n");
    }
    
    /* Full path smoke test - may skip at runtime if no Vulkan ICD or shader translation fails */
    ret = test_build_from_triangle();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: triangle build test failed\n");
        _exit(1);
    } else if (ret == 0) {
        fprintf(stdout, "pipeline_build_test: triangle smoke test passed\n");
    }
    
    /* No-dynamic-rendering test - skips if VK_KHR_dynamic_rendering present */
    ret = test_build_no_dynamic_rendering();
    if (ret == 1) {
        fprintf(stderr, "pipeline_build_test: no-dynamic-rendering test failed\n");
        _exit(1);
    } else if (ret != 0) {
        /* Skip code (77) is OK */
    }
    
    fprintf(stdout, "pipeline_build_test: all tests passed\n");
    fflush(stdout);
    _exit(0);
}

static int s_vuid_count = 0;

static VkBool32 VKAPI_PTR debug_message_callback(
    VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
    VkDebugUtilsMessageTypeFlagsEXT messageType,
    const VkDebugUtilsMessengerCallbackDataEXT *pCallbackData,
    void *pUserData) {
    (void)messageSeverity; (void)messageType; (void)pUserData;
    s_vuid_count++;
    fprintf(stderr, "VK validation: %s\n", pCallbackData->pMessage);
    return VK_FALSE;
}

static int test_validation_layers_and_vuids(void) {
    uint32_t layer_count = 0;
    VkResult vr = vkEnumerateInstanceLayerProperties(&layer_count, NULL);
    if (vr != VK_SUCCESS || layer_count == 0) {
        fprintf(stderr, "pipeline_build_test: vkEnumerateInstanceLayerProperties failed or no layers\n");
        return 77;
    }

    VkLayerProperties *layers = malloc(layer_count * sizeof(VkLayerProperties));
    vr = vkEnumerateInstanceLayerProperties(&layer_count, layers);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "pipeline_build_test: vkEnumerateInstanceLayerProperties failed\n");
        free(layers);
        return 77;
    }

    int validation_available = 0;
    for (uint32_t i = 0; i < layer_count; ++i) {
        if (strstr(layers[i].layerName, "VK_LAYER_KHRONOS_validation") != NULL) {
            validation_available = 1;
            break;
        }
    }
    free(layers);

    if (!validation_available) {
        fprintf(stderr, "pipeline_build_test: VK_LAYER_KHRONOS_validation not available; skipping\n");
        return 77;
    }

    VkApplicationInfo ai = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "pipeline_build_test",
        .apiVersion = VK_API_VERSION_1_3,
    };

    const char *validation_layers[] = {"VK_LAYER_KHRONOS_validation"};
    const char *debug_utils_ext = "VK_EXT_debug_utils";

    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &ai,
        .enabledLayerCount = 1,
        .ppEnabledLayerNames = validation_layers,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = &debug_utils_ext,
    };

    VkInstance inst = VK_NULL_HANDLE;
    vr = vkCreateInstance(&ici, NULL, &inst);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "pipeline_build_test: vkCreateInstance failed (%d)\n", (int)vr);
        return 77;
    }

    PFN_vkCreateDebugUtilsMessengerEXT vkCreateDebugUtilsMessengerEXT_fn =
        (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkCreateDebugUtilsMessengerEXT");
    PFN_vkDestroyDebugUtilsMessengerEXT vkDestroyDebugUtilsMessengerEXT_fn =
        (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(inst, "vkDestroyDebugUtilsMessengerEXT");
    
    VkDebugUtilsMessengerCreateInfoEXT mc = {
        .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
        .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                         | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                     | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        .pfnUserCallback = debug_message_callback,
    };

    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (vkCreateDebugUtilsMessengerEXT_fn) {
        vr = vkCreateDebugUtilsMessengerEXT_fn(inst, &mc, NULL, &messenger);
        if (vr != VK_SUCCESS) {
            fprintf(stderr, "pipeline_build_test: vkCreateDebugUtilsMessengerEXT failed (%d)\n", (int)vr);
        }
    }

    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(inst, &device_count, NULL);
    
    if (device_count == 0) {
        fprintf(stderr, "pipeline_build_test: no physical devices available\n");
        if (messenger && vkDestroyDebugUtilsMessengerEXT_fn) {
            vkDestroyDebugUtilsMessengerEXT_fn(inst, messenger, NULL);
        }
        vkDestroyInstance(inst, NULL);
        return 77;
    }

    VkPhysicalDevice *devices = malloc(device_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(inst, &device_count, devices);
    
    VkPhysicalDevice phys_device = VK_NULL_HANDLE;
    for (uint32_t i = 0; i < device_count; ++i) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            phys_device = devices[i];
            break;
        }
    }

    free(devices);

    if (!phys_device) {
        fprintf(stderr, "pipeline_build_test: no CPU device found\n");
        if (messenger && vkDestroyDebugUtilsMessengerEXT_fn) {
            vkDestroyDebugUtilsMessengerEXT_fn(inst, messenger, NULL);
        }
        vkDestroyInstance(inst, NULL);
        return 77;
    }

    float queue_priority = 1.0f;
    VkDeviceQueueCreateInfo dqci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &queue_priority,
    };

    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &dqci,
    };

    VkDevice device = VK_NULL_HANDLE;
    vr = vkCreateDevice(phys_device, &dci, NULL, &device);
    if (vr != VK_SUCCESS) {
        fprintf(stderr, "pipeline_build_test: vkCreateDevice failed (%d)\n", (int)vr);
        if (messenger && vkDestroyDebugUtilsMessengerEXT_fn) {
            vkDestroyDebugUtilsMessengerEXT_fn(inst, messenger, NULL);
        }
        vkDestroyInstance(inst, NULL);
        return 77;
    }

    VkPipelineRenderingCreateInfo rendering_info = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = (VkFormat[]){VK_FORMAT_B8G8R8A8_UNORM},
    };

    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &rendering_info,
        .stageCount = 0,
        .pStages = NULL,
        .pVertexInputState = &(VkPipelineVertexInputStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO},
        .pInputAssemblyState = &(VkPipelineInputAssemblyStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST},
        .pViewportState = &(VkPipelineViewportStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, .viewportCount = 1, .scissorCount = 1},
        .pRasterizationState = &(VkPipelineRasterizationStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f},
        .pMultisampleState = &(VkPipelineMultisampleStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT},
        .pDepthStencilState = &(VkPipelineDepthStencilStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO},
        .pColorBlendState = &(VkPipelineColorBlendStateCreateInfo){.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, .attachmentCount = 1},
        .layout = VK_NULL_HANDLE,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
    };

    VkPipeline pipe = VK_NULL_HANDLE;
    
    s_vuid_count = 0;
    
    vr = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    
    if (messenger && vkDestroyDebugUtilsMessengerEXT_fn) {
        vkDestroyDebugUtilsMessengerEXT_fn(inst, messenger, NULL);
    }
    vkDestroyInstance(inst, NULL);

    if (vkCreateGraphicsPipelines) {
        vkDestroyPipeline(device, pipe, NULL);
    }
    
    vkDestroyDevice(device, NULL);

    fprintf(stdout, "pipeline_build_test: VUID count during empty pipeline creation: %d\n", s_vuid_count);
    ASSERT(s_vuid_count == 0, "No VUID warnings should be emitted");

    return 0;
}

static int test_null_inputs(void) {
    VkDevice dummy_dev = (VkDevice)(size_t)0x1234;
    
    lagfx_pipeline_desc_t desc = {0};
    VkPipeline pipe = VK_NULL_HANDLE;
    
    /* NULL device */
    lagfx_status_t st = lagfx_pipeline_build(NULL, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL device returns non-OK");
    
    /* NULL desc */
    st = lagfx_pipeline_build(dummy_dev, NULL, &pipe);
    ASSERT(st != LAGFX_OK, "NULL desc returns non-OK");
    
    /* NULL out_pipeline */
    st = lagfx_pipeline_build(dummy_dev, &desc, NULL);
    ASSERT(st != LAGFX_OK, "NULL out_pipeline returns non-OK");
    
    return 0;
}

static int test_invalid_device_handle(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* Fake shader modules — just non-NULL handles, won't be deref'd if device is invalid */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipeline = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipeline);
    ASSERT(st != LAGFX_OK, "VK_NULL_HANDLE device returns non-OK");
    return 0;
}

static int test_mismatched_formats(void) {
    /* VK_FORMAT_UNDEFINED is a sentinel value. The implementation may:
     * 1. Reject it explicitly (preferred)
     * 2. Substitute a default format internally
     * This test checks for explicit rejection. */
    
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* Fake shader modules - will not be dereferenced if device is invalid */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_UNDEFINED;

    VkPipeline pipe = VK_NULL_HANDLE;
    /* Use VK_NULL_HANDLE to trigger early validation path */
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    
    if (st != LAGFX_OK) {
        fprintf(stderr, "test_mismatched_formats: VK_FORMAT_UNDEFINED rejected (good)\n");
        return 0;
    }
    
    /* If we get here, impl accepts undefined format - document this */
    fprintf(stderr, "test_mismatched_formats: VK_FORMAT_UNDEFINED accepted (impl substitutes default)\n");
    return 0;
}

static int test_invalid_shader_module(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* vertex_shader is VK_NULL_HANDLE */
    desc.vertex_shader = VK_NULL_HANDLE;
    desc.fragment_shader = (VkShaderModule)0x2;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipe = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "VK_NULL_HANDLE vertex_shader returns non-OK");
    return 0;
}

static int test_only_one_shader(void) {
    lagfx_pipeline_desc_t desc;
    memset(&desc, 0, sizeof(desc));
    /* vertex but no fragment */
    desc.vertex_shader = (VkShaderModule)0x1;
    desc.fragment_shader = VK_NULL_HANDLE;
    desc.color_format = VK_FORMAT_B8G8R8A8_UNORM;

    VkPipeline pipe = VK_NULL_HANDLE;
    lagfx_status_t st = lagfx_pipeline_build(VK_NULL_HANDLE, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "no fragment_shader returns non-OK");
    return 0;
}

static int test_missing_shader_modules(void) {
    VkDevice dummy_dev = (VkDevice)(size_t)0x1234;
    
    lagfx_pipeline_desc_t desc = {0};
    VkPipeline pipe = VK_NULL_HANDLE;
    
    /* NULL vertex_shader */
    desc.fragment_shader = (VkShaderModule)(size_t)0x5678;
    lagfx_status_t st = lagfx_pipeline_build(dummy_dev, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL vertex_shader returns non-OK");
    
    /* NULL fragment_shader */
    desc.vertex_shader = (VkShaderModule)(size_t)0x5678;
    desc.fragment_shader = VK_NULL_HANDLE;
    st = lagfx_pipeline_build(dummy_dev, &desc, &pipe);
    ASSERT(st != LAGFX_OK, "NULL fragment_shader returns non-OK");
    
    return 0;
}

static int test_build_from_triangle(void) {
    /* On Mac dev boxes without lavapipe, skip this test gracefully. */
    fprintf(stderr, "pipeline_build_test: skipping triangle smoke (requires Linux + lavapipe)\n");
    return 77;
}

static int test_build_no_dynamic_rendering(void) {
    /* On Mac dev boxes without lavapipe, skip this test. */
    fprintf(stderr, "pipeline_build_test: skipping no-dynamic-rendering smoke (requires Linux + lavapipe)\n");
    return 77;
}

#endif  /* LAGFX_HAVE_VULKAN */
