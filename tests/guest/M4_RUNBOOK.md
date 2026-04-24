# M4 Runbook — metal-clear-color

M4 exit criterion: render one Metal pass that clears a texture to red and
read pixel (0,0) back. Exit 0 = PASS.

Host = macOS dev box (arm64). Guest = macOS VM under QEMU, x86_64, reachable
via SSH alias `guest` (`matthew@10.1.7.20`).

## Rebuild the binary (on host)

Whenever `metal-clear-color.m` changes:

```sh
clang -arch x86_64 -fobjc-arc \
      -framework Metal -framework Foundation \
      <repo>/tests/guest/metal-clear-color.m \
      -o /tmp/metal-clear-color

file /tmp/metal-clear-color   # expect: Mach-O 64-bit executable x86_64
```

Ship to the guest (or stage locally if guest is down):

```sh
scp /tmp/metal-clear-color guest:/tmp/             # preferred
# or, if guest unreachable (M3 not green yet):
cp /tmp/metal-clear-color <host-staging>/
```

Note: `-arch x86_64` is mandatory — the host is Apple Silicon; the guest
is x86_64. A native arm64 binary will refuse to exec inside the VM.

## Run on the guest

One-liner from the host:

```sh
ssh guest /tmp/metal-clear-color; echo "exit=$?"
```

### Expected PASS output

```
=== M4: metal-clear-color ===
device: <some MTLDevice name>
committing clear-red render pass...
cmdbuf completed in 0.00Xs
clear color read-back: R=255 G=0 B=0 A=255

=== PASS: M4 exit criterion met (first pixel is red) ===
exit=0
```

Exit 0 is the only PASS. Any nonzero exit is a FAIL.

## Diagnosing FAIL

The binary prints a `FAIL:` line and returns a specific exit code. Map:

| Exit | Meaning | Likely cause |
|------|---------|--------------|
| 1 | `MTLCreateSystemDefaultDevice` returned null | No Metal device visible in guest — IOKit/GPU passthrough not wired, or Metal stack not loaded. Check kext load + `ioreg -c IOAccelerator` on guest. |
| 2 | `newCommandQueue` null | Device present but queue alloc failed — usually host-side dylib shim returning null. Check `libapplegfx-vulkan` logs on host. |
| 3 | Texture creation failed | Allocator / heap path broken. Check shim `newTextureWithDescriptor` impl. |
| 4 | commandBuffer or renderCommandEncoder null | Encoder plumbing broken. Check render pass descriptor translation in shim. |
| 5 | cmdbuf timed out or ended in Error status | GPU side never signalled completion. Most common real failure. Inspect `cmdbuf.error` (printed), then host-side Vulkan queue / fence logs. Raise `kCompletionTimeoutSec` only for debugging, not for a real PASS. |
| 6 | Pixel readback returned wrong color | Clear executed but colour wrong, or `synchronizeResource` / `getBytes:` returning stale memory. Print shows actual RGBA — compare against expected (255,0,0,255). Check blit-sync path and Managed-storage readback. |

### Quick triage checklist

1. `file /tmp/metal-clear-color` on the guest — must be `Mach-O 64-bit executable x86_64`. If it's arm64, rebuild with `-arch x86_64`.
2. `ssh guest 'ioreg -l | grep -i metal'` — sanity-check that the guest sees a Metal-capable device at all.
3. Re-run with `MTL_DEBUG_LAYER=1 MTL_SHADER_VALIDATION=1` env on the guest for extra diagnostics.
4. On exit 5/6, capture host-side shim logs for the same time window and diff against a known-good run.

## Files

- Source: `<repo>/tests/guest/metal-clear-color.m`
- Host build output: `/tmp/metal-clear-color`
- Local stage (fallback when guest is down): `<host-staging>/metal-clear-color`
- Guest install path: `/tmp/metal-clear-color`
