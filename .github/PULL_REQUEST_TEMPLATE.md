## What this PR does

<!-- 1-2 sentences describing the change. -->

## Why

<!-- Context / motivation. Link any upstream Mesa, Vulkan, or MoltenVK
     discussion that motivated the change. -->

## Test plan

<!-- Reproducible steps a reviewer can run locally. Include
     `meson setup`, `meson compile`, and `meson test` invocations where
     relevant. Call out any non-default feature flags. -->

```
meson setup build
meson compile -C build
meson test -C build
```

## Verification

- [ ] I ran `meson test` locally and it passed.
- [ ] I ran the translation unit tests (`tests/translate-*`) where relevant.
- [ ] I checked that no existing shader fixtures were regenerated unintentionally.

## Linked issues

<!-- Fixes #..., Refs #... -->
