#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vulkan/vulkan.h>
#include <time.h>

typedef struct {
    uint8_t b, g, r, a;
} BGRA;

static void die(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg);
    exit(1);
}

int main(void) {
    clock_t start = clock();
    
    /* Force lavapipe */
    setenv("VK_ICD_FILENAMES", "/usr/share/vulkan/icd.d/lvp_icd.json", 1);
    
    /* 1. Create instance */
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "lavapipe-smoke-test",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance instance;
    if (vkCreateInstance(&ici, NULL, &instance) != VK_SUCCESS)
        die("vkCreateInstance");
    printf("PASS: Instance created\n");
    
    /* 2. Find CPU device (lavapipe) */
    uint32_t dev_count = 0;
    vkEnumeratePhysicalDevices(instance, &dev_count, NULL);
    if (dev_count == 0)
        die("No Vulkan devices found");
    
    VkPhysicalDevice *devices = malloc(dev_count * sizeof(VkPhysicalDevice));
    vkEnumeratePhysicalDevices(instance, &dev_count, devices);
    
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkPhysicalDeviceProperties props;
    for (uint32_t i = 0; i < dev_count; i++) {
        vkGetPhysicalDeviceProperties(devices[i], &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) {
            phys = devices[i];
            break;
        }
    }
    free(devices);
    
    if (phys == VK_NULL_HANDLE)
        die("No CPU-type physical device found");
    printf("PASS: CPU device found: %s\n", props.deviceName);
    
    /* 3. Create logical device */
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = 0,
        .queueCount = 1,
        .pQueuePriorities = &priority,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    VkDevice device;
    if (vkCreateDevice(phys, &dci, NULL, &device) != VK_SUCCESS)
        die("vkCreateDevice");
    printf("PASS: Device created\n");
    
    VkQueue queue;
    vkGetDeviceQueue(device, 0, 0, &queue);
    
    /* 4. Create VkImage (64x64, BGRA) */
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_B8G8R8A8_UNORM,
        .extent = {.width = 64, .height = 64, .depth = 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage image;
    if (vkCreateImage(device, &ii, NULL, &image) != VK_SUCCESS)
        die("vkCreateImage");
    printf("PASS: Image created\n");
    
    /* 5. Allocate and bind memory */
    VkMemoryRequirements mem_req;
    vkGetImageMemoryRequirements(device, image, &mem_req);
    
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(phys, &mem_props);
    
    uint32_t mem_type = 0;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            mem_type = i;
            break;
        }
    }
    
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = mem_req.size,
        .memoryTypeIndex = mem_type,
    };
    VkDeviceMemory image_mem;
    if (vkAllocateMemory(device, &mai, NULL, &image_mem) != VK_SUCCESS)
        die("vkAllocateMemory for image");
    
    vkBindImageMemory(device, image, image_mem, 0);
    printf("PASS: Image memory bound\n");
    
    /* 6. Create command pool and buffer */
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = 0,
    };
    VkCommandPool cmd_pool;
    if (vkCreateCommandPool(device, &pci, NULL, &cmd_pool) != VK_SUCCESS)
        die("vkCreateCommandPool");
    
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    if (vkAllocateCommandBuffers(device, &cbai, &cmd) != VK_SUCCESS)
        die("vkAllocateCommandBuffers");
    printf("PASS: Command buffer allocated\n");
    
    /* 7. Record command buffer: transition layout + clear */
    VkCommandBufferBeginInfo cbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
    };
    vkBeginCommandBuffer(cmd, &cbi);
    
    /* Transition to TRANSFER_DST for clear */
    VkImageMemoryBarrier imb = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = 0,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .image = image,
        .subresourceRange = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .levelCount = 1,
            .layerCount = 1,
        },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    
    /* Clear to red (1,0,0,1) in BGRA = (0xFF, 0x00, 0x00, 0xFF) */
    VkClearColorValue clear_color = {
        .uint32 = {0xFF0000FF}  /* BGRA format */
    };
    VkImageSubresourceRange range = {
        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
        .levelCount = 1,
        .layerCount = 1,
    };
    vkCmdClearColorImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         &clear_color, 1, &range);
    
    /* Transition to TRANSFER_SRC for readback */
    imb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    imb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    imb.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    imb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &imb);
    
    vkEndCommandBuffer(cmd);
    printf("PASS: Command buffer recorded\n");
    
    /* 8. Submit and wait */
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS)
        die("vkQueueSubmit");
    
    if (vkQueueWaitIdle(queue) != VK_SUCCESS)
        die("vkQueueWaitIdle");
    printf("PASS: Command buffer executed\n");
    
    /* 9. Create staging buffer for readback */
    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 64 * 64 * sizeof(BGRA),
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer staging_buf;
    if (vkCreateBuffer(device, &bci, NULL, &staging_buf) != VK_SUCCESS)
        die("vkCreateBuffer");
    
    vkGetBufferMemoryRequirements(device, staging_buf, &mem_req);
    
    uint32_t host_mem_type = 0;
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_req.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            host_mem_type = i;
            break;
        }
    }
    
    mai.allocationSize = mem_req.size;
    mai.memoryTypeIndex = host_mem_type;
    VkDeviceMemory staging_mem;
    if (vkAllocateMemory(device, &mai, NULL, &staging_mem) != VK_SUCCESS)
        die("vkAllocateMemory for staging");
    
    vkBindBufferMemory(device, staging_buf, staging_mem, 0);
    
    /* 10. Copy image to buffer */
    VkCommandBufferAllocateInfo cbai2 = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd2;
    vkAllocateCommandBuffers(device, &cbai2, &cmd2);
    
    vkBeginCommandBuffer(cmd2, &cbi);
    
    VkBufferImageCopy bic = {
        .bufferOffset = 0,
        .imageSubresource = {
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .layerCount = 1,
        },
        .imageExtent = {.width = 64, .height = 64, .depth = 1},
    };
    vkCmdCopyImageToBuffer(cmd2, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           staging_buf, 1, &bic);
    vkEndCommandBuffer(cmd2);
    
    si.pCommandBuffers = &cmd2;
    vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(queue);
    printf("PASS: Image copied to staging buffer\n");
    
    /* 11. Map and read back */
    void *mapped;
    if (vkMapMemory(device, staging_mem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS)
        die("vkMapMemory");
    
    BGRA *pixels = (BGRA *)mapped;
    BGRA first_pixel = pixels[0];
    
    vkUnmapMemory(device, staging_mem);
    
    /* 12. Verify first pixel is red */
    printf("First pixel: B=%02x G=%02x R=%02x A=%02x\n",
           first_pixel.b, first_pixel.g, first_pixel.r, first_pixel.a);
    
    if (first_pixel.b == 0xFF && first_pixel.g == 0x00 &&
        first_pixel.r == 0x00 && first_pixel.a == 0xFF) {
        printf("PASS: Pixel color matches red (BGRA)\n");
    } else {
        die("Pixel color mismatch");
    }
    
    /* Cleanup */
    vkFreeMemory(device, staging_mem, NULL);
    vkDestroyBuffer(device, staging_buf, NULL);
    vkFreeMemory(device, image_mem, NULL);
    vkDestroyImage(device, image, NULL);
    vkDestroyCommandPool(device, cmd_pool, NULL);
    vkDestroyDevice(device, NULL);
    vkDestroyInstance(instance, NULL);
    
    clock_t end = clock();
    double elapsed = (double)(end - start) / CLOCKS_PER_SEC;
    
    printf("\n=== LAVAPIPE SMOKE TEST PASSED ===\n");
    printf("Total time: %.3f seconds\n", elapsed);
    return 0;
}
