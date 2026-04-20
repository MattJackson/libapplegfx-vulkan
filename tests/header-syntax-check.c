/* Compile-check only: verify libapplegfx-vulkan.h is a clean C header. */
#include "libapplegfx-vulkan.h"

int main(void)
{
    (void)lagfx_version_major();
    (void)lagfx_version_minor();
    (void)lagfx_version_patch();
    (void)lagfx_build_info();

    /* Touch the descriptor's thread_count field so a missing field
     * breaks the build. Plumbs gpu_cores from QEMU -> LP_NUM_THREADS;
     * see paravirt-re/gpu-cores-implementation-spec.md. */
    lagfx_device_descriptor_t desc = { 0 };
    desc.thread_count = 8;
    (void)desc.thread_count;
    return 0;
}
