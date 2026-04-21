// metal-no-op.m — M3 exit criterion: empty Metal command buffer round-trip.
//
// Runs INSIDE the macOS VM. Obtains the system default MTLDevice, creates a
// command queue, commits an empty command buffer, and waits for completion.
// Proves the device/queue/commit path through AppleParavirtGPU.kext +
// libapplegfx-vulkan is wired end-to-end.
//
// Build (on a Mac host — Metal headers required):
//   clang -fobjc-arc -framework Foundation -framework Metal \
//         metal-no-op.m -o metal-no-op
//
// Build INSIDE the guest is also possible: macOS ships clang + Metal SDK as
// part of the Command Line Tools; same invocation works there.
//
// Run on the VM (headless SSH is fine — no window/drawable needed):
//   /tmp/metal-no-op
// or, if a GUI login session is needed for device publication:
//   sudo launchctl asuser 501 /tmp/metal-no-op
//
// Exit codes:
//   0 — device obtained, queue created, empty cmdbuf committed + completed
//   1 — MTLCreateSystemDefaultDevice returned null (M2 not yet met)
//   2 — newCommandQueue failed
//   3 — commandBuffer creation failed
//   4 — waitUntilCompleted timed out (5s)
//   5 — command buffer returned error / non-Completed status

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const NSTimeInterval kCompletionTimeoutSec = 5.0;

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        printf("=== M3: metal-no-op ===\n");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) {
            printf("FAIL: MTLCreateSystemDefaultDevice() returned null\n");
            printf("      M2 not met — no IOAccelerator-backed Metal device.\n");
            return 1;
        }
        printf("device:        %s\n", [[device name] UTF8String]);
        printf("registryID:    0x%llx\n", [device registryID]);
        printf("lowPower:      %d\n", [device isLowPower]);
        printf("headless:      %d\n", [device isHeadless]);

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) {
            printf("FAIL: [device newCommandQueue] returned null\n");
            return 2;
        }

        id<MTLCommandBuffer> cmdbuf = [queue commandBuffer];
        if (!cmdbuf) {
            printf("FAIL: [queue commandBuffer] returned null\n");
            return 3;
        }
        cmdbuf.label = @"m3-no-op";

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            dispatch_semaphore_signal(sem);
        }];

        printf("committing empty command buffer...\n");
        [cmdbuf commit];

        long waited = dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW,
                          (int64_t)(kCompletionTimeoutSec * NSEC_PER_SEC)));
        if (waited != 0) {
            printf("FAIL: waitUntilCompleted timed out after %.1fs\n",
                   kCompletionTimeoutSec);
            return 4;
        }

        if (cmdbuf.status != MTLCommandBufferStatusCompleted) {
            printf("FAIL: cmdbuf ended in status %ld (error: %s)\n",
                   (long)cmdbuf.status,
                   cmdbuf.error ? [[cmdbuf.error localizedDescription] UTF8String] : "(none)");
            return 5;
        }

        printf("\n=== PASS: M3 exit criterion met ===\n");
        printf("round-trip GPU time: %.3fs\n",
               (cmdbuf.GPUEndTime - cmdbuf.GPUStartTime));
        return 0;
    }
}
