# Lavapipe integration guide for libapplegfx-vulkan

How to initialize and run Mesa's lavapipe (Vulkan-on-CPU driver) in
the headless Linux environment where libapplegfx-vulkan operates.

**Source research:** compiled 2026-04-19 during Phase 0 research.
Sources: Mesa documentation, Alpine Linux package registry, Phil
Dennis-Jordan's QEMU apple-gfx commits, Dave Airlie's lavapipe blog
posts, Vulkanised 2025 slides on lavapipe's state.

## TL;DR

- **Alpine 3.21 package:** `mesa-vulkan-swrast` 24.2.8-r0 — this
  ships lavapipe. No compile-from-source needed.
- **Force lavapipe:** `export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json`
- **Device reports as:** `VK_PHYSICAL_DEVICE_TYPE_CPU`
- **One queue family only** — no async compute / separate transfer queues
- **SPIR-V works out of the box** (`vkCreateShaderModule` with no extra setup)
- **Performance tuning env vars:** `LP_NUM_THREADS=N`, `LP_NATIVE_VECTOR_WIDTH=128|256`

## Alpine package setup

```sh
apk add --no-cache \
    mesa-vulkan-swrast \
    vulkan-loader \
    vulkan-headers
```

The `mesa-vulkan-swrast` package includes:
- Mesa 24.2.x with lavapipe enabled at build time
- ICD manifest at `/usr/share/vulkan/icd.d/lvp_icd.json`
- Runtime lib at `/usr/lib/libvulkan_lvp.so`

`vulkan-loader` provides `libvulkan.so.1` (the dispatch layer that
finds ICDs). `vulkan-headers` provides `/usr/include/vulkan/*.h`.

Minimum Mesa version for production use: **24.2.x.** 24.2.8 is stable;
earlier versions had format-table bugs that surface as random
VK_ERROR_UNKNOWN results.

## Runtime setup

Force lavapipe explicitly so we're not at the mercy of ICD
enumeration order (especially important in test environments where
other Vulkan drivers might be installed):

```c
setenv("VK_ICD_FILENAMES",
       "/usr/share/vulkan/icd.d/lvp_icd.json", 1);
```

Then a minimal headless init:

```c
#include <vulkan/vulkan.h>

VkInstance instance;
{
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "libapplegfx-vulkan",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    vkCreateInstance(&ci, NULL, &instance);
}

/* Pick a CPU device (lavapipe). */
VkPhysicalDevice phys;
{
    uint32_t count;
    vkEnumeratePhysicalDevices(instance, &count, NULL);
    VkPhysicalDevice devs[count];
    vkEnumeratePhysicalDevices(instance, &count, devs);

    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(devs[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            phys = devs[i];
            break;
        }
    }
}

/* Create a logical device. Lavapipe has ONE queue family. */
VkDevice device;
VkQueue queue;
{
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    vkCreateDevice(phys, &ci, NULL, &device);
    vkGetDeviceQueue(device, 0, 0, &queue);
}
```

No surface, no swapchain, no display extensions needed for our
use case — we're rendering into a `VkImage` and reading back with
`vkCmdCopyImageToBuffer` into a host buffer that libapplegfx-vulkan
then exposes to the QEMU shell via `lagfx_display_read_frame`.

## Image allocation pattern

For our render targets (the image the guest sees as its framebuffer):

```c
VkImageCreateInfo ii = {
    .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
    .imageType = VK_IMAGE_TYPE_2D,
    .format = VK_FORMAT_B8G8R8A8_UNORM,
    .extent = { .width = W, .height = H, .depth = 1 },
    .mipLevels = 1,
    .arrayLayers = 1,
    .samples = VK_SAMPLE_COUNT_1_BIT,
    .tiling = VK_IMAGE_TILING_OPTIMAL,
    .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT
           | VK_IMAGE_USAGE_TRANSFER_SRC_BIT  /* for readback */
           | VK_IMAGE_USAGE_SAMPLED_BIT,
    .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
};
```

## SPIR-V shader submission

Standard Vulkan path — no lavapipe-specific shimming:

```c
VkShaderModuleCreateInfo smi = {
    .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
    .codeSize = spirv_byte_length,
    .pCode = (const uint32_t *)spirv_bytes,
};
VkShaderModule sm;
vkCreateShaderModule(device, &smi, NULL, &sm);
```

Internally lavapipe pipelines SPIR-V → NIR → LLVM IR → JIT'd CPU
code. First call to `vkCreateShaderModule` on a given program is
slow (LLVM compilation); subsequent calls hit LLVM's own caches.

## Extensions worth enabling

Lavapipe supports essentially all core Vulkan 1.3 + many
extensions. For Metal-ish translation we want:

- `VK_KHR_dynamic_rendering` — maps cleanly to Metal's render-pass-free model
- `VK_KHR_timeline_semaphore` — needed for async frame handling
- `VK_EXT_descriptor_indexing` — lets us have more flexible
  descriptor binding (Metal allows late-binding of resources in ways
  Vulkan requires up-front; descriptor-indexing narrows the gap)
- `VK_EXT_vertex_input_dynamic_state` — vertex attrib format changes
  without rebuilding the pipeline (Metal's implicit behaviour)
- `VK_KHR_synchronization2` — cleaner barrier/layout model

Always check `vkEnumerateDeviceExtensionProperties` at startup;
don't assume — lavapipe adds extensions per Mesa release.

## Threading / concurrency

**One VkDevice + one VkQueue is the model.** Multiple host threads
CAN call into Vulkan (submit command buffers, create resources),
but lavapipe internally serializes everything through a per-queue
mutex. **No parallelism benefit from multi-threading at the Vulkan
API level.**

What DOES parallelize: lavapipe's own backend threads (controlled
by `LP_NUM_THREADS`). On our R730 host (72 cores), setting
`LP_NUM_THREADS=64` gives us significant pipeline-level parallelism
during render.

**Recommendation for libapplegfx-vulkan:** single Vulkan thread,
serialize all VkQueueSubmit. Lavapipe handles parallelism below that
line. Match the pattern apple-gfx.m used on macOS with dispatch
queues (serial, not concurrent).

## Performance tuning

| Env var | Default | Effect |
|---|---|---|
| `LP_NUM_THREADS` | auto (cores) | Backend worker pool size |
| `LP_NATIVE_VECTOR_WIDTH` | auto | Force 128 or 256 — 128 often faster on older CPUs (Haswell-EP), 256 better on Ice Lake+ |
| `GALLIUM_OVERRIDE_CPU_CAPS` | (unset) | `nosse`, `noavx`, etc. Testing only; turns off SIMD |
| `LP_DEBUG` | (unset) | Comma list: `state`, `perf`, `ir` — verbose logging |
| `MESA_DEBUG` | (unset) | `context`, `silent` — overall Mesa chatter |

For our docker host (Xeon E5-2699 v3, Haswell-EP, AVX2, **no
AVX-512**):
- `LP_NATIVE_VECTOR_WIDTH=256` — exercise the full AVX2 width
- `LP_NUM_THREADS=64` — we have 72 cores; leave 8 for QEMU & rest

## Gotchas

1. **Lavapipe is non-conformant** (no Vulkan conformance seal). Some
   corner cases of spec compliance fail vs hardware drivers. For our
   use case (translated Metal commands, not arbitrary Vulkan
   workloads), this is low-risk.
2. **Format support varies by Mesa version.** Run a format-capabilities
   audit at startup if we hit `VK_ERROR_FORMAT_NOT_SUPPORTED`.
3. **Pipeline compilation is expensive.** LLVM JIT is not fast; cache
   compiled pipelines aggressively (use `VkPipelineCache`).
4. **No swapchain extension.** We don't need it (headless) but if we
   ever do, lavapipe doesn't implement WSI for real display devices.
5. **Shader compilation errors surface late.** `vkCreateShaderModule`
   accepts any blob; invalid SPIR-V surfaces at
   `vkCreateGraphicsPipelines` time with a generic "pipeline creation
   failed" error. Run SPIRV-Val against outputs of `air2spirv` to
   validate early.

## What this means for the library

- Single-threaded Vulkan submission in `libapplegfx-vulkan` core
  — backend parallelism handled by lavapipe's internal threads
- Pipeline cache persisted to disk (per-install; under `/tmp` or
  user-cache) to amortize shader compilation
- Startup: force VK_ICD_FILENAMES, confirm we got a CPU-type
  device, log lavapipe version from `VkPhysicalDeviceProperties`
- Perf target per plan rev 9 risk R2: 1080p @ 30fps animated
  wallpaper. Lavapipe on 64-core Haswell-EP should reach that with
  headroom for simple scenes. Complex shaders compound quickly.

## Further reading

- Mesa lavapipe docs: https://docs.mesa3d.org/drivers/llvmpipe.html
- Alpine mesa-vulkan-swrast: https://pkgs.alpinelinux.org/package/v3.21/main/x86/mesa-vulkan-swrast
- Vulkanised 2025 lavapipe state: https://www.vulkan.org/user/pages/09.events/vulkanised-2025/T5-Lucas-Fryzek-Igalia.pdf
- Airlie blog: https://airlied.blogspot.com/2020/08/vallium-software-swrast-vulkan-layer-faq.html
- Lavapipe deepwiki: https://deepwiki.com/sailfishos-mirror/mesa/3.6.2-lavapipe-software-vulkan-driver
