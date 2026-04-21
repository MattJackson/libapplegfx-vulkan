// metal-clear-color.m — M4 exit criterion: first pixel.
//
// Runs INSIDE the macOS VM. Creates a 512x512 RGBA8 MTLTexture, encodes a
// single render pass with loadAction=Clear and clearColor=red (1,0,0,1),
// commits, waits, then reads pixel (0,0) back via
// [MTLTexture getBytes:bytesPerRow:fromRegion:mipmapLevel:]. Prints the
// read-back RGB and PASSes if R≈255, G≈0, B≈0.
//
// No CAMetalLayer / windowing — off-screen texture only, so runs fine over
// plain SSH with no GUI session.
//
// Build (on a Mac host — Metal headers required):
//   clang -fobjc-arc -framework Foundation -framework Metal \
//         metal-clear-color.m -o metal-clear-color
//
// Build INSIDE the guest is also possible (Command Line Tools ship Metal).
//
// Run on the VM:
//   /tmp/metal-clear-color
//
// Exit codes:
//   0 — cmdbuf completed AND pixel (0,0) is red within tolerance (PASS)
//   1 — MTLCreateSystemDefaultDevice returned null
//   2 — newCommandQueue failed
//   3 — texture creation failed
//   4 — commandBuffer / renderCommandEncoder creation failed
//   5 — waitUntilCompleted timed out or ended in Error status
//   6 — pixel readback returned wrong color (FAIL)

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

static const NSTimeInterval kCompletionTimeoutSec = 5.0;
static const NSUInteger kSize = 512;
static const int kTolerance = 4;  // per-channel tolerance (out of 255)

int main(int argc, const char *argv[]) {
    @autoreleasepool {
        printf("=== M4: metal-clear-color ===\n");

        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        if (!device) { printf("FAIL: MTLCreateSystemDefaultDevice null\n"); return 1; }
        printf("device: %s\n", [[device name] UTF8String]);

        id<MTLCommandQueue> queue = [device newCommandQueue];
        if (!queue) { printf("FAIL: newCommandQueue null\n"); return 2; }

        MTLTextureDescriptor *td = [MTLTextureDescriptor
            texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                         width:kSize
                                        height:kSize
                                     mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeManaged;  // required for getBytes:
        id<MTLTexture> tex = [device newTextureWithDescriptor:td];
        if (!tex) { printf("FAIL: texture creation\n"); return 3; }

        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = tex;
        rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(1.0, 0.0, 0.0, 1.0);

        id<MTLCommandBuffer> cmdbuf = [queue commandBuffer];
        if (!cmdbuf) { printf("FAIL: commandBuffer null\n"); return 4; }
        cmdbuf.label = @"m4-clear-red";

        id<MTLRenderCommandEncoder> enc = [cmdbuf renderCommandEncoderWithDescriptor:rpd];
        if (!enc) { printf("FAIL: renderCommandEncoder null\n"); return 4; }
        [enc endEncoding];

        // synchronizeResource is required before getBytes: on Managed textures
        id<MTLBlitCommandEncoder> blit = [cmdbuf blitCommandEncoder];
        [blit synchronizeResource:tex];
        [blit endEncoding];

        dispatch_semaphore_t sem = dispatch_semaphore_create(0);
        [cmdbuf addCompletedHandler:^(id<MTLCommandBuffer> cb) {
            dispatch_semaphore_signal(sem);
        }];

        printf("committing clear-red render pass...\n");
        [cmdbuf commit];

        long waited = dispatch_semaphore_wait(sem,
            dispatch_time(DISPATCH_TIME_NOW,
                          (int64_t)(kCompletionTimeoutSec * NSEC_PER_SEC)));
        if (waited != 0) { printf("FAIL: cmdbuf timed out\n"); return 5; }
        if (cmdbuf.status != MTLCommandBufferStatusCompleted) {
            printf("FAIL: cmdbuf status %ld (error: %s)\n", (long)cmdbuf.status,
                   cmdbuf.error ? [[cmdbuf.error localizedDescription] UTF8String] : "(none)");
            return 5;
        }
        printf("cmdbuf completed in %.3fs\n",
               (cmdbuf.GPUEndTime - cmdbuf.GPUStartTime));

        // Read pixel (0,0). RGBA8Unorm = 4 bytes/pixel.
        uint8_t px[4] = {0};
        MTLRegion region = MTLRegionMake2D(0, 0, 1, 1);
        [tex getBytes:px
          bytesPerRow:4
           fromRegion:region
          mipmapLevel:0];

        int r = px[0], g = px[1], b = px[2], a = px[3];
        printf("clear color read-back: R=%d G=%d B=%d A=%d\n", r, g, b, a);

        BOOL ok = (r >= 255 - kTolerance) &&
                  (g <= kTolerance) &&
                  (b <= kTolerance);
        if (!ok) {
            printf("FAIL: expected R>=%d G<=%d B<=%d (tolerance=%d)\n",
                   255 - kTolerance, kTolerance, kTolerance, kTolerance);
            return 6;
        }

        printf("\n=== PASS: M4 exit criterion met (first pixel is red) ===\n");
        return 0;
    }
}
