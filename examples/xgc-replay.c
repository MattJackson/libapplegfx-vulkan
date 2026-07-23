/*
 * libapplegfx-vulkan — Xgc login-material offline replay harness
 * examples/xgc-replay.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * KICKOFF-fragment-survival (2026-07-23): the Xgc material rasterizes
 * full-coverage in the VM but lands pure black RGB from its real inputs.
 * This harness replays the exact draw OFFLINE on lavapipe: the real
 * translated VfxXgb vertex + Xgc fragment, the real captured 64 KiB
 * uniform windows (LAGFX_DUMP_BINDS), and the real texture set — then
 * bisects inputs per run (seconds per cycle vs 25-minute boots).
 *
 * usage: xgc-replay <vert.spv> <frag.spv> <binds-dir> [forest.ppm]
 *          [--ones=16,17,...]   fill those merged-binding buffers with 1.0f
 *          [--zero=16,17,...]   zero-fill those merged-binding buffers
 *          [--zrange=16:0:64]   zero bytes [lo,hi) of one buffer (repeatable)
 *          [--poke=16:12:ff]    set one byte of one buffer (repeatable)
 *          [--solid=24:1,0,0,1] override a texture slot with a 64x64 solid
 *                               RGBA color (repeatable, panel-truth bisector)
 *          [--gray-feedback]    feedback texture mid-gray instead of black
 *          [--out=path.ppm]     write the rendered frame (default out.ppm)
 *
 * Binding model (merged set 0, mirrors production):
 *   vertex module: raw bindings 0-4 (binding 0 is declared by several
 *     aliased views — one layout entry) ← binds-dir/pipe0x31-bind{0..4}.bin
 *   fragment module: raw bindings shifted +16 here (production's
 *     lagfx_spv_offset_bindings contract) →
 *     buffers 16-22,31 ← pipe0x31-bind{16..22,31}.bin
 *     sampled images 23-30, 32-36 (23=feedback 1280x1024, 30=32x32
 *       colorful, rest=1x1 black; live TEXBIND says 24 is usually the
 *       1x1 dummy 0x33 / sometimes the 256x2 gamma LUT 0x55 — NOT the
 *       forest; the forest 0x54 never binds into pipe 0x31, it feeds
 *       the 0x17-writing blur pipes. The [forest.ppm] arg therefore
 *       populates a slot the material ignores in the captured configs.)
 *     samplers 37-39 (linear)
 *
 * Exit 0 = rendered (stats printed); 77 = no ICD; 1 = fail.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <vulkan/vulkan.h>

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

/* Patch every OpDecorate ... Binding N to N+16 (production's fragment
 * binding shift, applied at the SPIR-V word level so the harness needs no
 * lib linkage). OpDecorate = opcode 71; Decoration Binding = 33. */
static void shift_fragment_bindings(uint8_t *spv, size_t len) {
    uint32_t *w = (uint32_t *)spv;
    size_t n = len / 4, i = 5;                    /* header = 5 words */
    while (i < n) {
        uint32_t inst = w[i];
        uint32_t wc = inst >> 16, op = inst & 0xffffu;
        if (wc == 0) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33) /* OpDecorate %id Binding N */
            w[i + 3] += 16u;
        i += wc;
    }
}

static uint32_t pick_memtype(VkPhysicalDevice phys, uint32_t bits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want)
            return i;
    die("no memory type"); return 0;
}

/* ---- tiny fp32→fp16 (round-to-nearest-even not needed for diagnostics) */
static uint16_t f2h(float f) {
    uint32_t u; memcpy(&u, &f, 4);
    uint32_t s = (u >> 16) & 0x8000u;
    int32_t  e = (int32_t)((u >> 23) & 0xffu) - 127 + 15;
    uint32_t m = u & 0x7fffffu;
    if (e <= 0) return (uint16_t)s;
    if (e >= 31) return (uint16_t)(s | 0x7bffu);
    return (uint16_t)(s | ((uint32_t)e << 10) | (m >> 13));
}

typedef struct {
    VkDevice dev; VkPhysicalDevice phys; VkQueue q;
    VkCommandPool pool;
} Ctx;

static void one_shot(Ctx *c, void (*rec)(VkCommandBuffer, void *), void *ud) {
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = c->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb; vkAllocateCommandBuffers(c->dev, &cai, &cb);
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    vkBeginCommandBuffer(cb, &bi);
    rec(cb, ud);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cb };
    vkQueueSubmit(c->q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(c->dev ? c->q : NULL);
    vkFreeCommandBuffers(c->dev, c->pool, 1, &cb);
}

/* upload helper: staging buffer → image */
typedef struct { VkBuffer sb; VkImage img; uint32_t w, h; } UploadUD;
static void rec_upload(VkCommandBuffer cb, void *p) {
    UploadUD *u = p;
    VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0, .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = u->img, .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { u->w, u->h, 1 } };
    vkCmdCopyBufferToImage(cb, u->sb, u->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
}

static Ctx g;

static VkImageView make_texture(uint32_t w, uint32_t h, VkFormat fmt,
                                const void *pixels, size_t bytes) {
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = fmt, .extent = { w, h, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img; if (vkCreateImage(g.dev, &ici, NULL, &img) != VK_SUCCESS) die("tex image");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(g.dev, img, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = pick_memtype(g.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory mem; if (vkAllocateMemory(g.dev, &mai, NULL, &mem) != VK_SUCCESS) die("tex mem");
    vkBindImageMemory(g.dev, img, mem, 0);

    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT };
    VkBuffer sb; vkCreateBuffer(g.dev, &bci, NULL, &sb);
    VkMemoryRequirements bmr; vkGetBufferMemoryRequirements(g.dev, sb, &bmr);
    VkMemoryAllocateInfo bmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = bmr.size,
        .memoryTypeIndex = pick_memtype(g.phys, bmr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory smem; vkAllocateMemory(g.dev, &bmai, NULL, &smem);
    vkBindBufferMemory(g.dev, sb, smem, 0);
    void *map; vkMapMemory(g.dev, smem, 0, bytes, 0, &map);
    memcpy(map, pixels, bytes);
    vkUnmapMemory(g.dev, smem);

    UploadUD ud = { sb, img, w, h };
    one_shot(&g, rec_upload, &ud);

    VkImageViewCreateInfo ivci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = fmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView v; if (vkCreateImageView(g.dev, &ivci, NULL, &v) != VK_SUCCESS) die("tex view");
    return v;
}

static VkBuffer make_ssbo(const void *data, size_t bytes) {
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = bytes, .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    VkBuffer b; if (vkCreateBuffer(g.dev, &bci, NULL, &b) != VK_SUCCESS) die("ssbo");
    VkMemoryRequirements mr; vkGetBufferMemoryRequirements(g.dev, b, &mr);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mr.size,
        .memoryTypeIndex = pick_memtype(g.phys, mr.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory mem; vkAllocateMemory(g.dev, &mai, NULL, &mem);
    vkBindBufferMemory(g.dev, b, mem, 0);
    void *map; vkMapMemory(g.dev, mem, 0, bytes, 0, &map);
    memcpy(map, data, bytes);
    vkUnmapMemory(g.dev, mem);
    return b;
}

/* trivial PPM (P6) loader */
static uint8_t *load_ppm(const char *path, uint32_t *w, uint32_t *h) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char magic[3] = {0}; unsigned W = 0, H = 0, mx = 0;
    if (fscanf(f, "%2s %u %u %u", magic, &W, &H, &mx) != 4 || strcmp(magic, "P6")) { fclose(f); return NULL; }
    fgetc(f);
    uint8_t *d = malloc((size_t)W * H * 3);
    if (fread(d, 1, (size_t)W * H * 3, f) != (size_t)W * H * 3) { free(d); fclose(f); return NULL; }
    fclose(f); *w = W; *h = H; return d;
}

#define NBUF_V 5
static const uint32_t vbufs[NBUF_V] = { 0, 1, 2, 3, 4 };
#define NBUF_F 8
static const uint32_t fbufs[NBUF_F] = { 16, 17, 18, 19, 20, 21, 22, 31 };
#define NTEX 13
static const uint32_t timgs[NTEX] = { 23, 24, 25, 26, 27, 28, 29, 30, 32, 33, 34, 35, 36 };
#define NSAMP 3
static const uint32_t tsamps[NSAMP] = { 37, 38, 39 };

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: %s vert.spv frag.spv binds-dir [forest.ppm] "
                        "[--ones=16,17] [--gray-feedback] [--out=out.ppm]\n", argv[0]);
        return 2;
    }
    const char *vpath = argv[1], *fpath = argv[2], *bdir = argv[3];
    const char *forest_ppm = NULL, *outpath = "out.ppm";
    int gray_feedback = 0, wide = 0, f16rt = 0;
    char ones[512] = {0}, zeros[512] = {0};
    struct { uint32_t bind; float rgba[4]; } solids[16]; int nsolid = 0;
    struct { uint32_t bind, lo, hi; } zrs[32]; int nzr = 0;
    struct { uint32_t bind, off, val; } pokes[32]; int npoke = 0;
    for (int a = 4; a < argc; a++) {
        if (!strncmp(argv[a], "--ones=", 7)) snprintf(ones, sizeof(ones), ",%s,", argv[a] + 7);
        else if (!strncmp(argv[a], "--zero=", 7)) snprintf(zeros, sizeof(zeros), ",%s,", argv[a] + 7);
        else if (!strncmp(argv[a], "--solid=", 8) && nsolid < 16) {
            unsigned b = 0; float r = 0, gcol = 0, bl = 0, al = 1;
            if (sscanf(argv[a] + 8, "%u:%f,%f,%f,%f", &b, &r, &gcol, &bl, &al) >= 4) {
                solids[nsolid].bind = b;
                solids[nsolid].rgba[0] = r; solids[nsolid].rgba[1] = gcol;
                solids[nsolid].rgba[2] = bl; solids[nsolid].rgba[3] = al;
                nsolid++;
            }
        }
        else if (!strncmp(argv[a], "--zrange=", 9) && nzr < 32) {
            unsigned b = 0, lo = 0, hi = 0;
            if (sscanf(argv[a] + 9, "%u:%u:%u", &b, &lo, &hi) == 3 && hi <= 65536 && lo < hi) {
                zrs[nzr].bind = b; zrs[nzr].lo = lo; zrs[nzr].hi = hi; nzr++;
            }
        }
        else if (!strncmp(argv[a], "--poke=", 7) && npoke < 32) {
            unsigned b = 0, off = 0, val = 0;
            if (sscanf(argv[a] + 7, "%u:%u:%x", &b, &off, &val) == 3 && off < 65536) {
                pokes[npoke].bind = b; pokes[npoke].off = off; pokes[npoke].val = val; npoke++;
            }
        }
        else if (!strcmp(argv[a], "--gray-feedback")) gray_feedback = 1;
        else if (!strcmp(argv[a], "--wide")) wide = 1;
        else if (!strcmp(argv[a], "--f16rt")) f16rt = 1;
        else if (!strncmp(argv[a], "--out=", 6)) outpath = argv[a] + 6;
        else forest_ppm = argv[a];
    }

    size_t vlen = 0, flen = 0;
    uint8_t *vspv = slurp(vpath, &vlen);
    uint8_t *fspv = slurp(fpath, &flen);
    if (!vspv || !fspv) die("cannot read SPV");
    shift_fragment_bindings(fspv, flen);   /* production's +16 contract */

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "xgc-replay", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai };
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) { fprintf(stderr, "no ICD\n"); return 77; }
    uint32_t nd = 0; vkEnumeratePhysicalDevices(inst, &nd, NULL);
    if (!nd) { fprintf(stderr, "no devices\n"); return 77; }
    VkPhysicalDevice pds[8]; if (nd > 8) nd = 8;
    vkEnumeratePhysicalDevices(inst, &nd, pds);
    g.phys = pds[0];
    for (uint32_t i = 0; i < nd; i++) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { g.phys = pds[i]; break; }
    }
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dq = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio };
    /* VariablePointersStorageBuffer: the translated helpers pass device
     * pointers as function args (production enables the same feature). */
    VkPhysicalDeviceVariablePointersFeatures vpf = {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES,
        .variablePointersStorageBuffer = VK_TRUE };
    VkDeviceCreateInfo dc = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &vpf,
        .queueCreateInfoCount = 1, .pQueueCreateInfos = &dq };
    if (vkCreateDevice(g.phys, &dc, NULL, &g.dev) != VK_SUCCESS) die("device");
    vkGetDeviceQueue(g.dev, 0, 0, &g.q);
    VkCommandPoolCreateInfo cpc = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0 };
    vkCreateCommandPool(g.dev, &cpc, NULL, &g.pool);

    /* ---- render target 640x512 BGRA8 ------------------------------- */
    const uint32_t W = 640, H = 512;
    /* --f16rt: float RT preserves NaN/negative/extended values verbatim —
     * discriminates "fragment computed 0" from "fragment emitted the NaN
     * typed-undef placeholder" (NaN clamps to 0 in a UNORM RT). */
    const VkFormat rfmt = f16rt ? VK_FORMAT_R16G16B16A16_SFLOAT
                                : VK_FORMAT_B8G8R8A8_UNORM;
    VkImageCreateInfo rci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = rfmt, .extent = { W, H, 1 },
        .mipLevels = 1, .arrayLayers = 1, .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage rt; vkCreateImage(g.dev, &rci, NULL, &rt);
    VkMemoryRequirements rmr; vkGetImageMemoryRequirements(g.dev, rt, &rmr);
    VkMemoryAllocateInfo rmai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rmr.size,
        .memoryTypeIndex = pick_memtype(g.phys, rmr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory rmem; vkAllocateMemory(g.dev, &rmai, NULL, &rmem);
    vkBindImageMemory(g.dev, rt, rmem, 0);
    VkImageViewCreateInfo rvci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = rt, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = rfmt,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VkImageView rtv; vkCreateImageView(g.dev, &rvci, NULL, &rtv);

    /* ---- descriptor layout: 13 SSBOs + 13 images + 3 samplers ------- */
    VkDescriptorSetLayoutBinding lb[NBUF_V + NBUF_F + NTEX + NSAMP];
    uint32_t nlb = 0;
    for (uint32_t i = 0; i < NBUF_V; i++)
        lb[nlb++] = (VkDescriptorSetLayoutBinding){ vbufs[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                    VK_SHADER_STAGE_VERTEX_BIT, NULL };
    for (uint32_t i = 0; i < NBUF_F; i++)
        lb[nlb++] = (VkDescriptorSetLayoutBinding){ fbufs[i], VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 0; i < NTEX; i++)
        lb[nlb++] = (VkDescriptorSetLayoutBinding){ timgs[i], VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1,
                                                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    for (uint32_t i = 0; i < NSAMP; i++)
        lb[nlb++] = (VkDescriptorSetLayoutBinding){ tsamps[i], VK_DESCRIPTOR_TYPE_SAMPLER, 1,
                                                    VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo dlc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = nlb, .pBindings = lb };
    VkDescriptorSetLayout dsl; if (vkCreateDescriptorSetLayout(g.dev, &dlc, NULL, &dsl) != VK_SUCCESS) die("dsl");

    VkDescriptorPoolSize psz[3] = {
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NBUF_V + NBUF_F },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX },
        { VK_DESCRIPTOR_TYPE_SAMPLER, NSAMP },
    };
    VkDescriptorPoolCreateInfo dpc = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1, .poolSizeCount = 3, .pPoolSizes = psz };
    VkDescriptorPool dpool; vkCreateDescriptorPool(g.dev, &dpc, NULL, &dpool);
    VkDescriptorSetAllocateInfo dsa = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = 1, .pSetLayouts = &dsl };
    VkDescriptorSet ds; if (vkAllocateDescriptorSets(g.dev, &dsa, &ds) != VK_SUCCESS) die("dset");

    /* ---- buffers from binds-dir (or 1.0-filled when in --ones) ------ */
    uint8_t *filedata = malloc(65536);
    for (uint32_t i = 0; i < NBUF_V + NBUF_F; i++) {
        uint32_t bnum = i < NBUF_V ? vbufs[i] : fbufs[i - NBUF_V];
        char key[16]; snprintf(key, sizeof(key), ",%u,", bnum);
        int force_ones = ones[0] && strstr(ones, key) != NULL;
        int force_zero = zeros[0] && strstr(zeros, key) != NULL;
        char path[512]; snprintf(path, sizeof(path), "%s/pipe0x31-bind%u.bin", bdir, bnum);
        size_t got = 0; uint8_t *fd = slurp(path, &got);
        if (force_zero) {
            memset(filedata, 0, 65536);
            printf("bind%u: ZEROED\n", bnum);
        } else if (fd && got >= 65536 && !force_ones) {
            memcpy(filedata, fd, 65536);
        } else if (force_ones) {
            float one = 1.0f;
            for (int q = 0; q < 65536 / 4; q++) memcpy(filedata + q * 4, &one, 4);
            printf("bind%u: FILLED with 1.0f\n", bnum);
        } else {
            memset(filedata, 0, 65536);
            printf("bind%u: MISSING file %s — zero\n", bnum, path);
        }
        free(fd);
        for (int z = 0; z < nzr; z++)
            if (zrs[z].bind == bnum)
                memset(filedata + zrs[z].lo, 0, zrs[z].hi - zrs[z].lo);
        for (int p = 0; p < npoke; p++)
            if (pokes[p].bind == bnum)
                filedata[pokes[p].off] = (uint8_t)pokes[p].val;
        VkBuffer b = make_ssbo(filedata, 65536);
        VkDescriptorBufferInfo bi = { b, 0, VK_WHOLE_SIZE };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = bnum, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi };
        vkUpdateDescriptorSets(g.dev, 1, &wr, 0, NULL);
    }
    free(filedata);

    /* ---- textures --------------------------------------------------- */
    /* feedback 1280x1024 (merged 23): black or mid-gray */
    uint32_t fbw = 1280, fbh = 1024;
    uint8_t *fbpx = calloc(1, (size_t)fbw * fbh * 4);
    if (gray_feedback)
        for (size_t o = 0; o < (size_t)fbw * fbh * 4; o += 4) {
            fbpx[o] = fbpx[o+1] = fbpx[o+2] = 0x80; fbpx[o+3] = 0xff;
        }
    else
        for (size_t o = 3; o < (size_t)fbw * fbh * 4; o += 4) fbpx[o] = 0xff; /* opaque black */
    VkImageView v_feedback = make_texture(fbw, fbh, VK_FORMAT_B8G8R8A8_UNORM, fbpx, (size_t)fbw * fbh * 4);
    free(fbpx);

    /* forest RGBA16F (merged 24) from PPM (a=1.0), else 2x2 mid-green */
    VkImageView v_forest;
    if (forest_ppm) {
        uint32_t pw, ph; uint8_t *ppm = load_ppm(forest_ppm, &pw, &ph);
        if (!ppm) die("forest ppm load");
        uint16_t *hx = malloc((size_t)pw * ph * 8);
        for (size_t i = 0; i < (size_t)pw * ph; i++) {
            hx[i*4+0] = f2h(ppm[i*3+0] / 255.0f);
            hx[i*4+1] = f2h(ppm[i*3+1] / 255.0f);
            hx[i*4+2] = f2h(ppm[i*3+2] / 255.0f);
            hx[i*4+3] = f2h(1.0f);
        }
        v_forest = make_texture(pw, ph, VK_FORMAT_R16G16B16A16_SFLOAT, hx, (size_t)pw * ph * 8);
        free(hx); free(ppm);
        printf("forest: %ux%u from %s\n", pw, ph, forest_ppm);
    } else {
        uint16_t px[4 * 4];
        for (int i = 0; i < 4; i++) { px[i*4+0]=f2h(0.2f); px[i*4+1]=f2h(0.6f); px[i*4+2]=f2h(0.3f); px[i*4+3]=f2h(1.0f); }
        v_forest = make_texture(2, 2, VK_FORMAT_R16G16B16A16_SFLOAT, px, sizeof(px));
        printf("forest: synthetic 2x2 green\n");
    }

    /* 1x1 opaque black (dummies), 32x32 colorful (merged 30) */
    uint8_t black1[4] = { 0, 0, 0, 0 };            /* the guest's real dummy: zero incl. alpha */
    VkImageView v_dummy = make_texture(1, 1, VK_FORMAT_B8G8R8A8_UNORM, black1, 4);
    uint8_t c32[32 * 32 * 4];
    for (int y = 0; y < 32; y++) for (int x = 0; x < 32; x++) {
        uint8_t *p = &c32[(y * 32 + x) * 4];
        p[0] = (uint8_t)(x * 8); p[1] = (uint8_t)(y * 8); p[2] = 0x80; p[3] = 0xff;
    }
    VkImageView v_c32 = make_texture(32, 32, VK_FORMAT_B8G8R8A8_UNORM, c32, sizeof(c32));

    for (uint32_t i = 0; i < NTEX; i++) {
        VkImageView v = v_dummy;
        if (timgs[i] == 23) v = v_feedback;
        else if (timgs[i] == 24) v = v_forest;
        else if (timgs[i] == 30) v = v_c32;
        for (int s = 0; s < nsolid; s++) {
            if (solids[s].bind != timgs[i]) continue;
            uint8_t spx[64 * 64 * 4];
            for (int q = 0; q < 64 * 64; q++) {    /* BGRA byte order */
                spx[q*4+0] = (uint8_t)(solids[s].rgba[2] * 255.0f);
                spx[q*4+1] = (uint8_t)(solids[s].rgba[1] * 255.0f);
                spx[q*4+2] = (uint8_t)(solids[s].rgba[0] * 255.0f);
                spx[q*4+3] = (uint8_t)(solids[s].rgba[3] * 255.0f);
            }
            v = make_texture(64, 64, VK_FORMAT_B8G8R8A8_UNORM, spx, sizeof(spx));
            printf("tex%u: SOLID %.2f,%.2f,%.2f,%.2f\n", timgs[i],
                   solids[s].rgba[0], solids[s].rgba[1], solids[s].rgba[2], solids[s].rgba[3]);
        }
        VkDescriptorImageInfo ii = { VK_NULL_HANDLE, v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = timgs[i], .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii };
        vkUpdateDescriptorSets(g.dev, 1, &wr, 0, NULL);
    }
    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler smp; vkCreateSampler(g.dev, &sci, NULL, &smp);
    for (uint32_t i = 0; i < NSAMP; i++) {
        VkDescriptorImageInfo ii = { smp, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
        VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds, .dstBinding = tsamps[i], .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii };
        vkUpdateDescriptorSets(g.dev, 1, &wr, 0, NULL);
    }

    /* ---- pipeline ---------------------------------------------------- */
    VkShaderModuleCreateInfo smc = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = vlen, .pCode = (const uint32_t *)vspv };
    VkShaderModule vs; if (vkCreateShaderModule(g.dev, &smc, NULL, &vs) != VK_SUCCESS) die("vs module");
    smc.codeSize = flen; smc.pCode = (const uint32_t *)fspv;
    VkShaderModule fs; if (vkCreateShaderModule(g.dev, &smc, NULL, &fs) != VK_SUCCESS) die("fs module");

    VkPipelineLayoutCreateInfo plc = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl };
    VkPipelineLayout pl; vkCreatePipelineLayout(g.dev, &plc, NULL, &pl);

    VkAttachmentDescription att = { .format = rfmt, .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR, .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE, .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED, .finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    VkAttachmentReference ar = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &ar };
    VkRenderPassCreateInfo rpc = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sp };
    VkRenderPass rp; vkCreateRenderPass(g.dev, &rpc, NULL, &rp);
    VkFramebufferCreateInfo fbc = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &rtv,
        .width = W, .height = H, .layers = 1 };
    VkFramebuffer fb; vkCreateFramebuffer(g.dev, &fbc, NULL, &fb);

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vin = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    /* --wide: map NDC [-4,+4] onto the RT instead of [-1,+1] so geometry
     * that lands outside the normal clip volume (e.g. a parked-band rect at
     * guest x>=1280 -> NDC x>=1.0) becomes VISIBLE — locates the quad. */
    VkViewport vp = wide
        ? (VkViewport){ -1.5f * W, -1.5f * H, 4.0f * W, 4.0f * H, 0, 1 }
        : (VkViewport){ 0, 0, (float)W, (float)H, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { W, H } };
    VkPipelineViewportStateCreateInfo vps = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState ba = { .blendEnable = VK_FALSE,
        .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &ba };
    VkGraphicsPipelineCreateInfo gpc = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages, .pVertexInputState = &vin,
        .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pColorBlendState = &cb, .layout = pl,
        .renderPass = rp, .subpass = 0 };
    VkPipeline pipe;
    if (vkCreateGraphicsPipelines(g.dev, VK_NULL_HANDLE, 1, &gpc, NULL, &pipe) != VK_SUCCESS)
        die("pipeline");

    /* ---- draw + readback --------------------------------------------- */
    struct DrawUD { VkRenderPass rp; VkFramebuffer fb; VkPipeline pipe;
                    VkPipelineLayout pl; VkDescriptorSet ds; uint32_t W, H; } dud =
        { rp, fb, pipe, pl, ds, W, H };
    {
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cbuf; vkAllocateCommandBuffers(g.dev, &cai, &cbuf);
        VkCommandBufferBeginInfo bi2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cbuf, &bi2);
        VkClearValue clr = { .color = { .float32 = { 0.10f, 0.10f, 0.50f, 1.0f } } };  /* non-black clear: rasterized black fragments show as a silhouette */
        VkRenderPassBeginInfo rbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = dud.rp, .framebuffer = dud.fb,
            .renderArea = { { 0, 0 }, { dud.W, dud.H } },
            .clearValueCount = 1, .pClearValues = &clr };
        vkCmdBeginRenderPass(cbuf, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dud.pipe);
        vkCmdBindDescriptorSets(cbuf, VK_PIPELINE_BIND_POINT_GRAPHICS, dud.pl, 0, 1, &dud.ds, 0, NULL);
        vkCmdDraw(cbuf, 6, 1, 0, 0);   /* the real draw: 6 verts, no vertex input */
        vkCmdEndRenderPass(cbuf);
        vkEndCommandBuffer(cbuf);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cbuf };
        vkQueueSubmit(g.q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(g.q);
        vkFreeCommandBuffers(g.dev, g.pool, 1, &cbuf);
    }

    /* readback */
    size_t need = (size_t)W * H * (f16rt ? 8 : 4);
    VkBufferCreateInfo rbc = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = need, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    VkBuffer rbuf; vkCreateBuffer(g.dev, &rbc, NULL, &rbuf);
    VkMemoryRequirements rbm; vkGetBufferMemoryRequirements(g.dev, rbuf, &rbm);
    VkMemoryAllocateInfo rba = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = rbm.size,
        .memoryTypeIndex = pick_memtype(g.phys, rbm.memoryTypeBits,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory rbmem; vkAllocateMemory(g.dev, &rba, NULL, &rbmem);
    vkBindBufferMemory(g.dev, rbuf, rbmem, 0);
    {
        VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = g.pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
        VkCommandBuffer cbuf; vkAllocateCommandBuffers(g.dev, &cai, &cbuf);
        VkCommandBufferBeginInfo bi2 = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
        vkBeginCommandBuffer(cbuf, &bi2);
        VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { W, H, 1 } };
        vkCmdCopyImageToBuffer(cbuf, rt, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf, 1, &r);
        vkEndCommandBuffer(cbuf);
        VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .commandBufferCount = 1, .pCommandBuffers = &cbuf };
        vkQueueSubmit(g.q, 1, &si, VK_NULL_HANDLE);
        vkQueueWaitIdle(g.q);
        vkFreeCommandBuffers(g.dev, g.pool, 1, &cbuf);
    }
    uint8_t *px; vkMapMemory(g.dev, rbmem, 0, need, 0, (void **)&px);

    if (f16rt) {
        uint32_t nan = 0, zero = 0, other = 0, neg = 0;
        uint16_t sample[4] = {0};
        for (size_t i = 0; i < (size_t)W * H; i++) {
            int all0 = 1, isnan = 0, isneg = 0;
            for (int c = 0; c < 4; c++) {
                uint16_t hv = (uint16_t)(px[i*8+c*2] | (px[i*8+c*2+1] << 8));
                if (hv != 0) all0 = 0;
                if ((hv & 0x7fffu) > 0x7c00u) isnan = 1;
                if (hv & 0x8000u) isneg = 1;
            }
            if (i == (size_t)W * H / 2 + W / 2)
                for (int c = 0; c < 4; c++)
                    sample[c] = (uint16_t)(px[i*8+c*2] | (px[i*8+c*2+1] << 8));
            if (all0) zero++; else if (isnan) nan++; else other++;
            if (isneg) neg++;
        }
        printf("F16RT %ux%u: zero=%.2f%% nan=%.2f%% other=%.2f%% neg=%.2f%% center=[%04x %04x %04x %04x]\n",
               W, H, 100.0 * zero / (W * H), 100.0 * nan / (W * H),
               100.0 * other / (W * H), 100.0 * neg / (W * H),
               sample[0], sample[1], sample[2], sample[3]);
        return 0;
    }

    uint32_t nb = 0, cf = 0, an = 0;
    for (size_t i = 0; i < (size_t)W * H; i++) {
        uint8_t b = px[i*4], gg = px[i*4+1], r = px[i*4+2], a = px[i*4+3];
        uint8_t mx = r > gg ? (r > b ? r : b) : (gg > b ? gg : b);
        uint8_t mn = r < gg ? (r < b ? r : b) : (gg < b ? gg : b);
        if (r > 8 || gg > 8 || b > 8) nb++;
        if (mx - mn > 12) cf++;
        if (a > 8) an++;
    }
    size_t ci = ((size_t)H / 2) * W + W / 2;
    printf("RESULT %ux%u: nonblack=%.2f%% colorful=%.2f%% alpha-nonzero=%.2f%% "
           "center=rgba(%u,%u,%u,%u)\n",
           W, H, 100.0 * nb / (W * H), 100.0 * cf / (W * H), 100.0 * an / (W * H),
           px[ci*4+2], px[ci*4+1], px[ci*4], px[ci*4+3]);
    FILE *of = fopen(outpath, "wb");
    if (of) {
        fprintf(of, "P6\n%u %u\n255\n", W, H);
        for (size_t i = 0; i < (size_t)W * H; i++) {
            uint8_t rgb[3] = { px[i*4+2], px[i*4+1], px[i*4] };
            fwrite(rgb, 1, 3, of);
        }
        fclose(of);
        printf("wrote %s\n", outpath);
    }
    return 0;
}
