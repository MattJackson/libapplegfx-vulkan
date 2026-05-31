/*
 * libapplegfx-vulkan — real SkyLight shader render over lavapipe
 * tests/skylight-lavapipe-render.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * "Beyond Stage 80" gate: proves a REAL Apple SkyLight compositor fragment
 * shader — translated by OUR clean-room AIR->SPIR-V pipeline (air2spv) —
 * builds a VkGraphicsPipeline, draws, and produces the CORRECT pixels on
 * lavapipe (the exact Mesa/Vulkan stack the production scanout->noVNC path
 * uses). Stage 80 proved this for the demo triangle; this proves it for a
 * shader pulled verbatim from macOS's SkyLightShaders.air64.metallib.
 *
 * Pipeline:
 *   - Vertex: a tiny reference shader (GLSL->SPIR-V) that emits a fullscreen
 *     triangle and feeds the fragment its stage_in: a constant colour at
 *     Location 1 and a dummy vec4 at Location 0 (the fragment's unused
 *     [[position]] input).
 *   - Fragment: OUR translated real SkyLight `SimpleColorFragment` (returns
 *     its interpolated `color` input -> the render target).
 *
 * Args: argv[1]=vertex.spv argv[2]=fragment.spv [argv[3]=RRGGBBAA expected]
 *       [argv[4]=--tex] — when --tex is given, bind a real texture+sampler
 *       descriptor set (set 0: binding 0 = sampled image, binding 1 =
 *       sampler) filled with the expected colour, so a texture-sampling
 *       SkyLight fragment (e.g. SimpleTextureFragment) samples it at the
 *       Location-1 tex coord (0.5,0.5) and returns it. This exercises the
 *       resource-binding path the live noVNC wiring needs (most SkyLight
 *       compositor shaders sample textures).
 * Default expected centre pixel: 00ff00ff (green) — the colour the vertex
 * feeds (passthrough mode) or the texture is filled with (--tex mode).
 *
 * Exit 0 = correct pixel rendered; 77 = no Vulkan ICD (skip); 1 = fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

typedef struct { uint8_t r, g, b, a; } RGBA;

static void die(const char *m) { fprintf(stderr, "FATAL: %s\n", m); exit(1); }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *b = malloc((size_t)sz);
    if (fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return NULL; }
    fclose(f); *len = (size_t)sz; return b;
}

static uint32_t pick_memtype(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    die("no memory type"); return 0;
}

static VkShaderModule make_module(VkDevice dev, const uint8_t *spv, size_t len) {
    VkShaderModuleCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = len, .pCode = (const uint32_t *)spv,
    };
    VkShaderModule sm = VK_NULL_HANDLE;
    if (vkCreateShaderModule(dev, &ci, NULL, &sm) != VK_SUCCESS) return VK_NULL_HANDLE;
    return sm;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: %s vert.spv frag.spv [RRGGBBAA]\n", argv[0]); return 2; }
    RGBA want = { 0x00, 0xff, 0x00, 0xff };
    if (argc >= 4) {
        unsigned v = (unsigned)strtoul(argv[3], NULL, 16);
        want.r = (v >> 24) & 0xff; want.g = (v >> 16) & 0xff;
        want.b = (v >> 8) & 0xff;  want.a = v & 0xff;
    }
    int use_tex = (argc >= 5 && strcmp(argv[4], "--tex") == 0);
    /* --buf: bind a StorageBuffer (set 0, binding 0) holding `want` as a
     * vec4 — matches what the translator emits for a `[[buffer]]` arg. The
     * real guest ColorFill fragment reads buffer[0][0] and returns it. */
    int use_buf = (argc >= 5 && strcmp(argv[4], "--buf") == 0);

    size_t vlen = 0, flen = 0;
    uint8_t *vspv = slurp(argv[1], &vlen);
    uint8_t *fspv = slurp(argv[2], &flen);
    if (!vspv || !fspv) die("cannot read shader SPV");

    if (getenv("LAGFX_FORCE_LAVAPIPE"))
        setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json", 1);

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "skylight-lavapipe-render",
                             .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &ai };
    VkInstance inst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) {
        fprintf(stderr, "no Vulkan ICD — skip\n"); return 77;
    }
    uint32_t ndev = 0; vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (!ndev) { fprintf(stderr, "no devices — skip\n"); return 77; }
    VkPhysicalDevice *pds = malloc(ndev * sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &ndev, pds);
    VkPhysicalDevice phys = pds[0];
    for (uint32_t i = 0; i < ndev; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { phys = pds[i]; break; }
    }
    free(pds);
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    fprintf(stdout, "  VkDevice: %s\n", pp.deviceName);

    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                     .queueFamilyIndex = 0, .queueCount = 1,
                                     .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &dqci };
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(phys, &dci, NULL, &dev) != VK_SUCCESS) die("vkCreateDevice");
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);

    const uint32_t W = 64, H = 64;
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;

    VkImageCreateInfo ici_img = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt,
        .extent = { W, H, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage image; if (vkCreateImage(dev, &ici_img, NULL, &image) != VK_SUCCESS) die("vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, image, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = pick_memtype(phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory image_mem; if (vkAllocateMemory(dev, &mai, NULL, &image_mem) != VK_SUCCESS) die("alloc image");
    vkBindImageMemory(dev, image, image_mem, 0);

    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = image, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView iview; if (vkCreateImageView(dev, &ivci, NULL, &iview) != VK_SUCCESS) die("imageview");

    VkAttachmentDescription att = { .format = fmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference att_ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription subpass = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &att_ref };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &subpass };
    VkRenderPass rp; if (vkCreateRenderPass(dev, &rpci, NULL, &rp) != VK_SUCCESS) die("renderpass");

    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &iview, .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; if (vkCreateFramebuffer(dev, &fbci, NULL, &fb) != VK_SUCCESS) die("framebuffer");

    VkShaderModule vs = make_module(dev, vspv, vlen);
    VkShaderModule fs = make_module(dev, fspv, flen);
    if (!vs) die("vkCreateShaderModule(vertex) — bad vertex SPV");
    if (!fs) { fprintf(stderr, "FAIL: vkCreateShaderModule(fragment) rejected the translated SkyLight SPV\n"); return 1; }

    /* --tex: bind a real texture (binding 0 = sampled image) + sampler
     * (binding 1) in descriptor set 0, filled with `want` — matches what
     * the translator emits for air.sample_texture_2d. */
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    if (use_tex) {
        const uint32_t TW = 4, TH = 4;
        VkImageCreateInfo tici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { TW, TH, 1 },
            .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
        VkImage tex; if (vkCreateImage(dev, &tici, NULL, &tex) != VK_SUCCESS) die("tex image");
        VkMemoryRequirements tmr; vkGetImageMemoryRequirements(dev, tex, &tmr);
        VkMemoryAllocateInfo tmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = tmr.size,
            .memoryTypeIndex = pick_memtype(phys, tmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        VkDeviceMemory tmem; if (vkAllocateMemory(dev, &tmai, NULL, &tmem) != VK_SUCCESS) die("tex mem");
        vkBindImageMemory(dev, tex, tmem, 0);

        /* Staging buffer filled with `want` for every texel. */
        VkBufferCreateInfo tbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = TW * TH * sizeof(RGBA), .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
        VkBuffer tbuf; if (vkCreateBuffer(dev, &tbci, NULL, &tbuf) != VK_SUCCESS) die("tex buf");
        VkMemoryRequirements tbmr; vkGetBufferMemoryRequirements(dev, tbuf, &tbmr);
        VkMemoryAllocateInfo tbmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = tbmr.size,
            .memoryTypeIndex = pick_memtype(phys, tbmr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory tbmem; if (vkAllocateMemory(dev, &tbmai, NULL, &tbmem) != VK_SUCCESS) die("tex buf mem");
        vkBindBufferMemory(dev, tbuf, tbmem, 0);
        void *tm = NULL; vkMapMemory(dev, tbmem, 0, VK_WHOLE_SIZE, 0, &tm);
        for (uint32_t i = 0; i < TW * TH; i++) ((RGBA *)tm)[i] = want;
        vkUnmapMemory(dev, tbmem);

        /* One-shot upload: UNDEFINED->TRANSFER_DST, copy, ->SHADER_READ_ONLY. */
        VkCommandPoolCreateInfo upci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
        VkCommandPool upool; vkCreateCommandPool(dev, &upci, NULL, &upool);
        VkCommandBufferAllocateInfo ucbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = upool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer ucmd; vkAllocateCommandBuffers(dev, &ucbai, &ucmd);
        VkCommandBufferBeginInfo ubbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkBeginCommandBuffer(ucmd, &ubbi);
        VkImageMemoryBarrier b1 = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = tex, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT };
        vkCmdPipelineBarrier(ucmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b1);
        VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { TW, TH, 1 } };
        vkCmdCopyBufferToImage(ucmd, tbuf, tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
        VkImageMemoryBarrier b2 = b1;
        b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b2.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(ucmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, NULL, 0, NULL, 1, &b2);
        vkEndCommandBuffer(ucmd);
        VkSubmitInfo usi = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &ucmd };
        vkQueueSubmit(queue, 1, &usi, VK_NULL_HANDLE); vkQueueWaitIdle(queue);

        VkImageViewCreateInfo tivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = tex, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VkImageView tview; if (vkCreateImageView(dev, &tivci, NULL, &tview) != VK_SUCCESS) die("tex view");
        VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST, .minFilter = VK_FILTER_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
        if (vkCreateSampler(dev, &sci, NULL, &sampler) != VK_SUCCESS) die("sampler");

        /* Descriptor set layout: binding 0 = sampled image, binding 1 = sampler
         * (separate, as the translator emits OpTypeImage + OpTypeSampler). */
        VkDescriptorSetLayoutBinding binds[2] = {
            { .binding = 0, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
            { .binding = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .descriptorCount = 1,
              .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT },
        };
        VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 2, .pBindings = binds };
        if (vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl) != VK_SUCCESS) die("dsl");

        VkDescriptorPoolSize psz[2] = {
            { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1 }, { VK_DESCRIPTOR_TYPE_SAMPLER, 1 } };
        VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = psz };
        VkDescriptorPool dpool; if (vkCreateDescriptorPool(dev, &dpci, NULL, &dpool) != VK_SUCCESS) die("dpool");
        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        if (vkAllocateDescriptorSets(dev, &dsai, &dset) != VK_SUCCESS) die("dset");
        VkDescriptorImageInfo img_info = { .imageView = tview,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkDescriptorImageInfo samp_info = { .sampler = sampler };
        VkWriteDescriptorSet writes[2] = {
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 0,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &img_info },
            { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset, .dstBinding = 1,
              .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &samp_info },
        };
        vkUpdateDescriptorSets(dev, 2, writes, 0, NULL);
        fprintf(stdout, "  --tex: bound texture+sampler (set 0, bindings 0/1) = %02x%02x%02x%02x\n",
                want.r, want.g, want.b, want.a);
    }
    if (use_buf) {
        /* StorageBuffer (set 0, binding 0) holding `want` as a vec4. */
        float rgba[4] = { want.r / 255.0f, want.g / 255.0f, want.b / 255.0f, want.a / 255.0f };
        VkBufferCreateInfo sbci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = sizeof(rgba), .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
        VkBuffer sbuf; if (vkCreateBuffer(dev, &sbci, NULL, &sbuf) != VK_SUCCESS) die("storage buf");
        VkMemoryRequirements sbmr; vkGetBufferMemoryRequirements(dev, sbuf, &sbmr);
        VkMemoryAllocateInfo sbmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = sbmr.size,
            .memoryTypeIndex = pick_memtype(phys, sbmr.memoryTypeBits,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
        VkDeviceMemory sbmem; if (vkAllocateMemory(dev, &sbmai, NULL, &sbmem) != VK_SUCCESS) die("storage mem");
        vkBindBufferMemory(dev, sbuf, sbmem, 0);
        void *sm = NULL; vkMapMemory(dev, sbmem, 0, VK_WHOLE_SIZE, 0, &sm);
        memcpy(sm, rgba, sizeof(rgba)); vkUnmapMemory(dev, sbmem);

        VkDescriptorSetLayoutBinding b0 = { .binding = 0,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_VERTEX_BIT };
        VkDescriptorSetLayoutCreateInfo dslci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .bindingCount = 1, .pBindings = &b0 };
        if (vkCreateDescriptorSetLayout(dev, &dslci, NULL, &dsl) != VK_SUCCESS) die("buf dsl");
        VkDescriptorPoolSize psz = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
        VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &psz };
        VkDescriptorPool dpool; if (vkCreateDescriptorPool(dev, &dpci, NULL, &dpool) != VK_SUCCESS) die("buf dpool");
        VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
        if (vkAllocateDescriptorSets(dev, &dsai, &dset) != VK_SUCCESS) die("buf dset");
        VkDescriptorBufferInfo binfo = { .buffer = sbuf, .offset = 0, .range = VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = dset,
            .dstBinding = 0, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &binfo };
        vkUpdateDescriptorSets(dev, 1, &wr, 0, NULL);
        fprintf(stdout, "  --buf: bound StorageBuffer (set 0, binding 0) = %02x%02x%02x%02x\n",
                want.r, want.g, want.b, want.a);
    }

    int use_desc = use_tex || use_buf;
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = use_desc ? 1u : 0u, .pSetLayouts = use_desc ? &dsl : NULL };
    VkPipelineLayout pl; if (vkCreatePipelineLayout(dev, &plci, NULL, &pl) != VK_SUCCESS) die("layout");

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkViewport vp = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    VkRect2D sc = { {0,0}, {W,H} };
    VkPipelineViewportStateCreateInfo vpst = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rsst = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo msst = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba };
    VkGraphicsPipelineCreateInfo gpci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin, .pInputAssemblyState = &ia,
        .pViewportState = &vpst, .pRasterizationState = &rsst, .pMultisampleState = &msst,
        .pColorBlendState = &cb, .layout = pl, .renderPass = rp, .subpass = 0 };
    VkPipeline pipe = VK_NULL_HANDLE;
    VkResult pr = vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpci, NULL, &pipe);
    if (pr != VK_SUCCESS) {
        fprintf(stderr, "FAIL: vkCreateGraphicsPipelines(real SkyLight frag) = %d\n", (int)pr);
        return 1;
    }
    fprintf(stdout, "  PASS: pipeline built from translated SkyLight fragment\n");

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .queueFamilyIndex = 0 };
    VkCommandPool cpool; if (vkCreateCommandPool(dev, &cpci, NULL, &cpool) != VK_SUCCESS) die("cmdpool");
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cmd; vkAllocateCommandBuffers(dev, &cbai, &cmd);
    VkCommandBufferBeginInfo cbbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    vkBeginCommandBuffer(cmd, &cbbi);
    /* Clear to RED so a passed-through GREEN proves the fragment wrote it. */
    VkClearValue clear = { .color = { .float32 = { 1, 0, 0, 1 } } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb, .renderArea = { {0,0}, {W,H} },
        .clearValueCount = 1, .pClearValues = &clear };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    if (use_desc)
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &dset, 0, NULL);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = W * H * sizeof(RGBA), .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer staging; if (vkCreateBuffer(dev, &bci, NULL, &staging) != VK_SUCCESS) die("buffer");
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(dev, staging, &bmr);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = bmr.size,
        .memoryTypeIndex = pick_memtype(phys, bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory bmem; if (vkAllocateMemory(dev, &bmai, NULL, &bmem) != VK_SUCCESS) die("alloc staging");
    vkBindBufferMemory(dev, staging, bmem, 0);

    VkCommandBuffer cmd2; vkAllocateCommandBuffers(dev, &cbai, &cmd2);
    vkBeginCommandBuffer(cmd2, &cbbi);
    VkBufferImageCopy copy = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { W, H, 1 } };
    vkCmdCopyImageToBuffer(cmd2, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging, 1, &copy);
    vkEndCommandBuffer(cmd2);
    si.pCommandBuffers = &cmd2; vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE); vkQueueWaitIdle(queue);

    void *mapped = NULL; vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &mapped);
    RGBA *px = (RGBA *)mapped;
    RGBA c = px[(H/2)*W + (W/2)];
    fprintf(stdout, "  centre pixel R=%02x G=%02x B=%02x A=%02x (want %02x%02x%02x%02x)\n",
            c.r, c.g, c.b, c.a, want.r, want.g, want.b, want.a);
    int ok = (c.r == want.r && c.g == want.g && c.b == want.b && c.a == want.a);
    vkUnmapMemory(dev, bmem);

    if (ok) fprintf(stdout, "PASS: real SkyLight fragment rendered the correct colour on lavapipe\n");
    else    fprintf(stderr, "FAIL: centre pixel mismatch\n");

    free(vspv); free(fspv);
    return ok ? 0 : 1;
}
