/* Compile-check only: verify libapplegfx-vulkan.h is a clean C header. */
#include "libapplegfx-vulkan.h"

int main(void)
{
    (void)lagfx_version_major();
    (void)lagfx_version_minor();
    (void)lagfx_version_patch();
    (void)lagfx_build_info();
    return 0;
}
