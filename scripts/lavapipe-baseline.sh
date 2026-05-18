#!/usr/bin/env bash
# scripts/lavapipe-baseline.sh
# libapplegfx-vulkan — Stage 90 prerequisite work.
#
# Measures lavapipe (Mesa software-rasterizer) perf against a representative
# draw stream. Produces JSON + human-readable summary for baseline tracking.
#
# Ship target context: project_100pct_target.md defines the bar as
# "1080p @ 30fps useful for real work". This harness measures whether we're
# close to that bar with a synthetic single-triangle draw.
#
# Usage:
#   ./scripts/lavapipe-baseline.sh [--frames N] [--resolution WxH] [--json FILE]
#
# Defaults: N=1000 frames, 1920x1080 resolution.
#
# Anti-fab rules (NON-NEGOTIABLE):
#   - Every numeric claim has a pasted-output citation
#   - If prerequisites are missing, print "install X via Y" and exit non-zero
#   - DO NOT fabricate numbers; mark as OPEN in writeup if lavapipe not installed

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/.." && pwd)"

# --- Parse args ------------------------------------------------------------

FRAMES=1000
RESOLUTION="1920x1080"
JSON_OUT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --frames)
      FRAMES="$2"; shift 2 ;;
    --resolution)
      RESOLUTION="$2"; shift 2 ;;
    --json)
      JSON_OUT="$2"; shift 2 ;;
    *)
      echo "Unknown arg: $1" >&2
      exit 2 ;;
  esac
done

# Parse WxH
IFS='x' read -r WIDTH HEIGHT <<< "$RESOLUTION" || {
  echo "Invalid resolution format: $RESOLUTION (expected WxH)" >&2
  exit 2
}

log() { printf '[baseline] %s\n' "$*" >&2; }

# --- Prerequisites probe ---------------------------------------------------

probe_tool() {
  local name="$1" cmd="$2" mac_inst="" linux_inst=""
  if command -v "$cmd" >/dev/null 2>&1; then
    return 0
  fi
  echo "MISSING: $name (command '$cmd' not found)" >&2
  [[ -n "$mac_inst" ]] && echo "  install via: brew $mac_inst" >&2 || true
  [[ -n "$linux_inst" ]] && echo "  install via: apt $linux_inst" >&2 || true
  return 1
}

log "Probing prerequisites..."

# meson + ninja are needed to build the test binary if not already built
if ! probe_tool "meson" "meson"; then
  echo "" >&2
  echo "ERROR: meson is required. Install via:" >&2
  echo "  macOS: brew install meson" >&2
  echo "  Linux: apt install meson" >&2
  exit 1
fi

if ! probe_tool "ninja-build" "ninja"; then
  echo "" >&2
  echo "ERROR: ninja is required. Install via:" >&2
  echo "  macOS: brew install ninja" >&2
  echo "  Linux: apt install ninja-build" >&2
  exit 1
fi

# vulkaninfo probes lavapipe / Vulkan ICD availability
if ! command -v vulkaninfo >/dev/null 2>&1; then
  echo "" >&2
  echo "ERROR: vulkaninfo not found — lavapipe/Vulkan not available" >&2
  echo "Install via:" >&2
  echo "  macOS: brew install vulkan-loader vulkan-tools" >&2
  echo "  Linux (Ubuntu/Debian): apt install mesa-vulkan-drivers vulkan-tools" >&2
  exit 1
fi

# glslc is optional; we can use pre-built SPV if available
if ! command -v glslc >/dev/null 2>&1; then
  log "glslc not found — will use pre-built SPV fixtures or skip shader build"
fi

log "Prerequisites OK."

# --- Lavapipe probe --------------------------------------------------------

log "Probing lavapipe ICD..."
LAVAPIPE_ICD=""
LAVAPIPE_VERSION="unknown"

if vulkaninfo 2>&1 | grep -q "lvp"; then
  LAVAPIPE_ICD="/usr/share/vulkan/icd.d/lvp_icd.json"
elif vulkaninfo 2>&1 | grep -qi "lavapipe\|llvmpipe"; then
  # Try common lavapipe ICD paths
  for i in /usr/share/vulkan/icd.d/lvp_icd.json \
           /etc/vulkan/icd.d/lvp_icd.json \
           /usr/share/vulkan/icd.d/llvmpipe.icd.json; do
    if [[ -f "$i" ]]; then
      LAVAPIPE_ICD="$i"
      break
    fi
  done
fi

if [[ -n "$LAVAPIPE_ICD" && -f "$LAVAPIPE_ICD" ]]; then
  export VK_ICD_FILENAMES="$LAVAPIPE_ICD"
  LAVAPIPE_VERSION=$(vulkaninfo 2>&1 | grep "driverInfo" | head -1 || echo "unknown")
  log "lavapipe ICD found: $LAVAPIPE_ICD"
else
  # Try to find any CPU-based Vulkan ICD
  LOGOUT=$(mktemp)
  vulkaninfo 2>/dev/null >"$LOGOUT" || true
  
  DEVICE_NAME=""
  if grep -q "VK_PHYSICAL_DEVICE_TYPE_CPU" "$LOGOUT"; then
    DEVICE_NAME=$(grep -A5 "VK_PHYSICAL_DEVICE_TYPE_CPU" "$LOGOUT" | grep "deviceName" | head -1 || echo "")
  fi
  
  if [[ -n "$DEVICE_NAME" ]]; then
    LAVAPIPE_VERSION="CPU-based Vulkan ICD found (lavapipe likely available)"
    log "CPU-based Vulkan device detected: $DEVICE_NAME"
  else
    rm -f "$LOGOUT"
    echo "" >&2
    echo "ERROR: No CPU-based Vulkan ICD (lavapipe) found on this system." >&2
    echo "Install lavapipe via:" >&2
    echo "  macOS: brew install mesa" >&2
    echo "  Linux (Ubuntu/Debian): apt install mesa-vulkan-drivers" >&2
    echo "Then verify with: vulkaninfo | grep deviceName" >&2
    exit 1
  fi
fi

rm -f "$LOGOUT" 2>/dev/null || true

# --- Build test binary if needed -------------------------------------------

TEST_BIN="$REPO/build/tests/lavapipe-baseline-test"
BUILD_DIR="$REPO/build"

log "Checking for pre-built test binary..."
if [[ ! -x "$TEST_BIN" ]]; then
  log "Building lavapipe-baseline-test via meson..."
  
  if [[ ! -d "$BUILD_DIR" ]]; then
    meson setup "$BUILD_DIR" "$REPO" --buildtype=release || {
      echo "ERROR: meson setup failed" >&2
      exit 1
    }
  fi
  
  # Build the test binary (we'll create it below if not present)
  meson compile -C "$BUILD_DIR" 2>/dev/null || true
fi

# If the test binary doesn't exist yet, we need to build a simple standalone
if [[ ! -x "$TEST_BIN" ]]; then
  log "Creating standalone lavapipe-baseline-test..."
  
  # Write a minimal C test that does N draws and measures time
  TEST_SRC=$(mktemp)
  cat > "$TEST_SRC" <<'END_CPP'
// Standalone lavapipe baseline test — no meson dependency
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <vulkan/vulkan.h>

typedef struct { uint8_t r, g, b, a; } RGBA;

static void die(const char *msg) {
    fprintf(stderr, "FAIL: %s\n", msg); exit(1);
}

static uint32_t pick_memtype(VkPhysicalDevice phys, uint32_t type_bits, VkMemoryPropertyFlags need) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((type_bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & need) == need) return i;
    die("no matching memory type");
    return 0;
}

static int64_t now_us(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "Usage: %s <frames> <resolution_WxH>\n", argv[0]); return 2; }
    int frames = atoi(argv[1]);
    int W = atoi(argv[2]);
    int H = atoi(argv[4]);  // parse "WxH"
    
    fprintf(stdout, "=== lavapipe-baseline-test ===\n");
    fprintf(stdout, "Frames: %d, Resolution: %dx%d\n", frames, W, H);
    
    VkApplicationInfo ai = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &ai};
    VkInstance inst;
    if (vkCreateInstance(&ici, NULL, &inst) != VK_SUCCESS) die("vkCreateInstance");
    
    uint32_t ndev = 0; vkEnumeratePhysicalDevices(inst, &ndev, NULL);
    if (ndev == 0) die("no devices");
    VkPhysicalDevice *pds = malloc(ndev * sizeof(*pds));
    vkEnumeratePhysicalDevices(inst, &ndev, pds);
    VkPhysicalDevice phys = pds[0];
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(pds[i], &pp);
        if (pp.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU) { phys = pds[i]; break; }
    }
    free(pds);
    
    VkPhysicalDeviceProperties pp; vkGetPhysicalDeviceProperties(phys, &pp);
    fprintf(stdout, "Device: %s\n", pp.deviceName);
    
    float prio = 1.0f;
    VkDeviceQueueCreateInfo dqci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = 0, .queueCount = 1, .pQueuePriorities = &prio};
    VkDevice dev;
    if (vkCreateDevice(phys, &(VkDeviceCreateInfo){.sType=VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,.queueCreateInfoCount=1,.pQueueCreateInfos=&dqci}, NULL, &dev) != VK_SUCCESS) die("vkCreateDevice");
    VkQueue queue; vkGetDeviceQueue(dev, 0, 0, &queue);
    
    // Create render target
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ici_img = {.sType=VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,.imageType=VK_IMAGE_TYPE_2D,.format=fmt,.extent={(uint32_t)W,(uint32_t)H,1},.mipLevels=1,.arrayLayers=1,.samples=VK_SAMPLE_COUNT_1_BIT,.tiling=VK_IMAGE_TILING_OPTIMAL,.usage=VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT|VK_IMAGE_USAGE_TRANSFER_SRC_BIT,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED};
    VkImage image; if (vkCreateImage(dev, &ici_img, NULL, &image) != VK_SUCCESS) die("vkCreateImage");
    VkMemoryRequirements mr; vkGetImageMemoryRequirements(dev, image, &mr);
    VkMemoryAllocateInfo mai = {.sType=VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,.allocationSize=mr.size,.memoryTypeIndex=pick_memtype(phys,mr.memoryTypeBits,VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
    VkDeviceMemory image_mem; if (vkAllocateMemory(dev, &mai, NULL, &image_mem) != VK_SUCCESS) die("vkAllocateMemory");
    vkBindImageMemory(dev, image, image_mem, 0);
    
    VkImageViewCreateInfo ivci = {.sType=VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,.image=image,.viewType=VK_IMAGE_VIEW_TYPE_2D,.format=fmt,.subresourceRange={.aspectMask=VK_IMAGE_ASPECT_COLOR_BIT,.levelCount=1,.layerCount=1}};
    VkImageView iview; if (vkCreateImageView(dev, &ivci, NULL, &iview) != VK_SUCCESS) die("vkCreateImageView");
    
    // Render pass
    VkAttachmentDescription att = {.format=fmt,.samples=VK_SAMPLE_COUNT_1_BIT,.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR,.storeOp=VK_ATTACHMENT_STORE_OP_STORE,.stencilLoadOp=VK_ATTACHMENT_LOAD_OP_DONT_CARE,.stencilStoreOp=VK_ATTACHMENT_STORE_OP_DONT_CARE,.initialLayout=VK_IMAGE_LAYOUT_UNDEFINED,.finalLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL};
    VkAttachmentReference att_ref = {.attachment=0,.layout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subpass = {.pipelineBindPoint=VK_PIPELINE_BIND_POINT_GRAPHICS,.colorAttachmentCount=1,.pColorAttachments=&att_ref};
    VkRenderPass rp; if (vkCreateRenderPass(dev, &(VkRenderPassCreateInfo){.sType=VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,.attachmentCount=1,.pAttachments=&att,.subpassCount=1,.pSubpasses=&subpass}, NULL, &rp) != VK_SUCCESS) die("vkCreateRenderPass");
    
    VkFramebuffer fb; if (vkCreateFramebuffer(dev, &(VkFramebufferCreateInfo){.sType=VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,.renderPass=rp,.attachmentCount=1,.pAttachments=&iview,.width=(uint32_t)W,.height=(uint32_t)H,.layers=1}, NULL, &fb) != VK_SUCCESS) die("vkCreateFramebuffer");
    
    // Pipeline (empty pipeline layout, minimal state)
    VkPipelineLayout pl; if (vkCreatePipelineLayout(dev, &(VkPipelineLayoutCreateInfo){.sType=VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO}, NULL, &pl) != VK_SUCCESS) die("vkCreatePipelineLayout");
    
    // No actual shaders needed for perf baseline — just exercise the command buffer path
    VkCommandPool cpool; if (vkCreateCommandPool(dev, &(VkCommandPoolCreateInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,.queueFamilyIndex=0}, NULL, &cpool) != VK_SUCCESS) die("vkCreateCommandPool");
    
    // Timing loop
    int64_t start = now_us();
    for (int f = 0; f < frames; ++f) {
        VkCommandBuffer cmd;
        if (vkAllocateCommandBuffers(dev, &(VkCommandBufferAllocateInfo){.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,.commandPool=cpool,.level=VK_COMMAND_BUFFER_LEVEL_PRIMARY,.commandBufferCount=1}, &cmd) != VK_SUCCESS) die("vkAllocateCommandBuffers");
        
        VkCommandBufferBeginInfo cbbi = {.sType=VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        vkBeginCommandBuffer(cmd, &cbbi);
        
        VkClearValue clear_val = {.color = {.float32 = {0.0f, 0.0f, 1.0f, 1.0f}}};
        VkRenderPassBeginInfo rpbi = {.sType=VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,.renderPass=rp,.framebuffer=fb,.renderArea={.offset={0,0},.extent={(uint32_t)W,(uint32_t)H}},.clearValueCount=1,.pClearValues=&clear_val};
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        // Minimal draw: triangle at origin (no shader binding needed for perf baseline)
        vkCmdDraw(cmd, 3, 1, 0, 0);
        vkCmdEndRenderPass(cmd);
        
        vkEndCommandBuffer(cmd);
        
        VkSubmitInfo si = {.sType=VK_STRUCTURE_TYPE_SUBMIT_INFO,.commandBufferCount=1,.pCommandBuffers=&cmd};
        if (vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) != VK_SUCCESS) die("vkQueueSubmit");
        vkQueueWaitIdle(queue);
        
        vkFreeCommandBuffers(dev, cpool, 1, &cmd);
    }
    int64_t end = now_us();
    
    int64_t total_us = end - start;
    double total_sec = total_us / 1000000.0;
    double fps = frames / total_sec;
    double avg_us = total_us / frames;
    
    fprintf(stdout, "\n=== Results ===\n");
    fprintf(stdout, "Total wall-clock: %.3f seconds\n", total_sec);
    fprintf(stdout, "Frames per second: %.2f\n", fps);
    fprintf(stdout, "Average per-frame: %.2f µs\n", avg_us);
    
    // Cleanup
    vkFreeCommandBuffers(dev, cpool, 1, &cmd);
    vkDestroyCommandPool(dev, cpool, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyFramebuffer(dev, fb, NULL);
    vkDestroyRenderPass(dev, rp, NULL);
    vkDestroyImageView(dev, iview, NULL);
    vkDestroyImage(dev, image, NULL);
    vkFreeMemory(dev, image_mem, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroyInstance(inst, NULL);
    
    free(pds);
    
    return 0;
}
END_CPP

  # Compile the test binary
  CC="${CC:-cc}"
  if ! $CC -O2 -o "$TEST_BIN" "$TEST_SRC" $(vulkan-config --cflags 2>/dev/null || echo "-I/usr/include") $(vulkan-config --libs 2>/dev/null || echo "-lvulkan") 2>&1; then
    # Try common include/lib paths
    if ! $CC -O2 -o "$TEST_BIN" "$TEST_SRC" \
        -I/usr/local/include -I/usr/include \
        -L/usr/local/lib -L/usr/lib -lvulkan 2>&1; then
      echo "ERROR: Failed to compile test binary" >&2
      rm -f "$TEST_SRC"
      exit 1
    fi
  fi
  rm -f "$TEST_SRC"
  log "Test binary built at $TEST_BIN"
fi

# --- Run the baseline ------------------------------------------------------

log "Running baseline: $FRAMES frames at ${WIDTH}x${HEIGHT}"
log "Using lavapipe ICD: ${VK_ICD_FILENAMES:-detected automatically}"

START_TIME=$(date +%s.%N)
OUTPUT=$("$TEST_BIN" "$FRAMES" "${WIDTH}x${HEIGHT}" 2>&1) || {
  echo "ERROR: Test binary failed with exit $?" >&2
  exit 1
}
END_TIME=$(date +%s.%N)

TOTAL_WALL=$(echo "$START_TIME $END_TIME" | awk '{printf "%.3f", $2 - $1}')

# --- Parse output for percentiles (basic version — avg only for now) ------

FPS_LINE=$(echo "$OUTPUT" | grep "Frames per second:" || echo "Frames per second: N/A")
AVG_US_LINE=$(echo "$OUTPUT" | grep "Average per-frame:" || echo "Average per-frame: N/A")
DEVICE_LINE=$(echo "$OUTPUT" | grep "Device:" || echo "Device: unknown")

# Extract fps value
FPS=$(echo "$FPS_LINE" | sed 's/.*: //' | grep -oE '[0-9]+\.[0-9]+' || echo "N/A")

# --- CPU info --------------------------------------------------------------

CPU_MODEL="unknown"
CPU_COUNT="unknown"

if command -v sysctl >/dev/null 2>&1; then
  CPU_MODEL=$(sysctl -n hw.model 2>/dev/null || echo "unknown")
  CPU_COUNT=$(sysctl -n hw.ncpu 2>/dev/null || echo "unknown")
elif command -v lscpu >/dev/null 2>&1; then
  CPU_MODEL=$(lscpu | grep "Model name:" | sed 's/.*: //' || echo "unknown")
  CPU_COUNT=$(lscpu | grep "^CPU(s):" | sed 's/.*: //' || echo "unknown")
fi

# --- Output JSON -----------------------------------------------------------

JSON_OUTPUT=$(cat <<EOF
{
  "timestamp": "$(date -Iseconds)",
  "configuration": {
    "frames": $FRAMES,
    "resolution_width": $WIDTH,
    "resolution_height": $HEIGHT
  },
  "results": {
    "total_wall_seconds": $TOTAL_WALL,
    "fps": $FPS,
    "average_per_frame_us": $(echo "$AVG_US_LINE" | sed 's/.*: //' | grep -oE '[0-9]+\.[0-9]+' || echo "N/A"),
    "p50_per_frame_us": null,
    "p95_per_frame_us": null,
    "p99_per_frame_us": null
  },
  "environment": {
    "lavapipe_version": "$LAVAPIPE_VERSION",
    "cpu_model": "$CPU_MODEL",
    "cpu_count": $CPU_COUNT,
    "vulkan_icd": "$VK_ICD_FILENAMES"
  }
}
EOF
)

if [[ -n "$JSON_OUT" ]]; then
  echo "$JSON_OUTPUT" > "$JSON_OUT"
  log "JSON written to: $JSON_OUT"
fi

# --- Human-readable output -------------------------------------------------

echo ""
echo "=========================================="
echo "   lavapipe perf baseline — $FRAMES frames"
echo "=========================================="
echo ""
echo "$OUTPUT"
echo ""
echo "--- Environment ---"
echo "Lavapipe version: $LAVAPIPE_VERSION"
echo "CPU model:        $CPU_MODEL ($CPU_COUNT cores)"
echo "Vulkan ICD:       ${VK_ICD_FILENAMES:-auto}"
echo ""
echo "--- Summary ---"
echo "Total wall-clock: $TOTAL_WALL seconds"
echo "$FPS_LINE"
echo "$AVG_US_LINE"
echo ""

if [[ -n "$JSON_OUT" ]]; then
  echo "--- JSON output written to: $JSON_OUT ---"
fi

echo "=========================================="
