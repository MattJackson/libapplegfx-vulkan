# M4 Render Opcodes Test Implementation Plan
**Date:** 2026-05-04  
**Status:** Phase 1 Complete (begin/end render pass), Phase 2+ In Progress

---

## Current Progress

### ✅ Completed Tests

| File | Status | Coverage | Lines |
|------|--------|----------|-------|
| `tests/m3-stamp-helpers.c` | ✅ PASS | Stamp monotonicity, slot bitmasking | 403 LOC |
| `tests/m4-task-translate.c` | ✅ PASS | VA translation, radix tree | 487 LOC |
| `tests/m5-deadlock-detect.c` | ✅ PASS | Online event timing | ~200 LOC |
| `tests/m4-render-opcode-begin.c` | 🟡 WRITTEN | CmdBegin/EndRenderPass | 315 LOC |

### ⏳ Tests to Implement (Priority Order)

#### Phase 2: Core Render Opcodes (Blocker for M5)
**Must complete before any new development:**

| # | File | Opcode Coverage | Priority | Est. LOC |
|---|------|-----------------|----------|---------|
| 1 | `tests/m4-render-opcode-pipeline.c` | setPipelineState, setBlendConstants, setColorWriteMask | **CRITICAL** | ~200 |
| 2 | `tests/m4-render-opcode-draw.c` | drawPrimitives, drawIndexedPrimitives, drawInstanced | **CRITICAL** | ~250 |
| 3 | `tests/m4-render-opcode-state.c` | setViewport, setScissor, stencil, depthBias | HIGH | ~200 |
| 4 | `tests/m4-render-opcode-texture.c` | storeTexture, loadTexture, sampleTexture | HIGH | ~300 |

#### Phase 3: Compute/Blit Opcodes (M5 Path)
**Essential for display transactions:**

| # | File | Opcode Coverage | Priority | Est. LOC |
|---|------|-----------------|----------|---------|
| 5 | `tests/m4-compute-opcodes.c` | All 32 compute opcodes | HIGH | ~400 |
| 6 | `tests/m4-blit-opcodes.c` | All 24 blit opcodes | HIGH | ~350 |

#### Phase 4: Supporting Infrastructure (M1/M3)
**Foundational APIs:**

| # | File | Coverage | Priority | Est. LOC |
|---|------|----------|----------|---------|
| 7 | `tests/m1-shader-catalog.c` | LRU eviction, lookup, insertion | MEDIUM | ~250 |
| 8 | `tests/m1-resource-registry.c` | Resource lifecycle tracking | MEDIUM | ~200 |
| 9 | `tests/m3-vulkan-command.c` | Command buffer allocation/encoding | LOW | ~250 |

---

## Implementation Patterns

### Mock Shell Template (Reusable)
All tests follow the same pattern as `m3-stamp-helpers.c`:

```c
typedef struct {
    unsigned raise_irq_count;
    unsigned read_memory_count;
    unsigned write_memory_count;
    uint8_t heap[65536];
    uint64_t heap_gpa;
} m4_shell_t;

/* Create device with mock shell */
static lagfx_device_t *make_dev(m4_shell_t *shell) {
    lagfx_device_descriptor_t d = {0};
    d.shell.opaque          = shell;
    d.shell.create_task     = m4_create_task;
    d.shell.destroy_task    = m4_destroy_task;
    d.shell.map_memory      = m4_map;
    d.shell.unmap_memory    = m4_unmap;
    d.shell.read_memory     = m4_read;
    d.shell.write_memory    = m4_write;
    d.shell.raise_interrupt = m4_irq;
    
    char *err = NULL;
    lagfx_device_t *dev = lagfx_device_new(&d, &err);
    if (!dev) { free(err); exit(2); }
    return dev;
}

/* Header builder (reusable across all opcode tests) */
static size_t build_header(uint8_t *out, uint16_t opcode,
                           uint16_t arg_count_8b,
                           uint32_t total_length, uint32_t stamp);

/* LE32 writer (reusable) */
static void put_le32(uint8_t *b, uint32_t v);
```

### Test Structure Pattern
Each test follows this template:

```c
static void test_<scenario>(void) {
    fprintf(stdout, "\n--- test: <scenario> ---\n");
    
    m4_shell_t shell = {0};
    shell.heap_gpa = 0x10000ull;
    lagfx_device_t *dev = make_dev(&shell);
    lagfx_protocol_t *p = (lagfx_protocol_t *)dev->protocol_state;
    
    /* Build command payload */
    uint8_t cmd[SIZE];
    memset(cmd, 0, sizeof(cmd));
    build_header(cmd, OPCODE, arg_count, total_length, stamp);
    fill_payload(cmd);
    
    int rc = lagfx_protocol_dispatch_one(p, cmd, sizeof(cmd));
    
    /* Assertions */
    CHECK(rc >= 0, "success condition");
    CHECK(shell.raise_irq_count == 1, "IRQ raised");
    CHECK(lagfx_protocol_last_completed_stamp(p) == stamp, "stamp correct");
    
    lagfx_device_free(dev);
}
```

---

## Next Steps (Immediate Action Required)

### Step 1: Register New Test in meson.build
Add to `tests/meson.build` after line 200 (after m4-doorbell-drain):

```meson
# M4 render opcode tests - Phase 2.A.1
if fs.is_file(meson.current_source_dir() / 'm4-render-opcode-begin.c')
  m4_render_begin_test = executable('m4-render-opcode-begin',
    'm4-render-opcode-begin.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4 render opcode begin', m4_render_begin_test)
endif

# M4 render opcode tests - Phase 2.A.2 (pipeline/state)
if fs.is_file(meson.current_source_dir() / 'm4-render-opcode-pipeline.c')
  m4_render_pipeline_test = executable('m4-render-opcode-pipeline',
    'm4-render-opcode-pipeline.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4 render opcode pipeline', m4_render_pipeline_test)
endif

# M4 render opcode tests - Phase 2.A.3 (draw commands)
if fs.is_file(meson.current_source_dir() / 'm4-render-opcode-draw.c')
  m4_render_draw_test = executable('m4-render-opcode-draw',
    'm4-render-opcode-draw.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4 render opcode draw', m4_render_draw_test)
endif

# M4 compute/blit opcodes - Phase 2.B
if fs.is_file(meson.current_source_dir() / 'm4-compute-opcodes.c')
  m4_compute_opcodes_test = executable('m4-compute-opcodes',
    'm4-compute-opcodes.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4 compute opcodes', m4_compute_opcodes_test)
endif

if fs.is_file(meson.current_source_dir() / 'm4-blit-opcodes.c')
  m4_blit_opcodes_test = executable('m4-blit-opcodes',
    'm4-blit-opcodes.c',
    include_directories : [include_dir, internal_include_dir],
    dependencies : libapplegfx_vulkan_dep,
    build_by_default : true,
  )
  test('m4 blit opcodes', m4_blit_opcodes_test)
endif
```

### Step 2: Build & Test New Files
```bash
cd /Users/mjackson/Developer/libapplegfx-vulkan
meson compile -C builddir m4-render-opcode-begin
./builddir/tests/m4-render-opcode-begin
# Expect: all PASS, exit 0

# Then run full suite to verify no regressions
meson test -C builddir --print-errorlogs
```

### Step 3: Implement Remaining Tests
Create the remaining 6 critical tests in priority order:
1. `m4-render-opcode-pipeline.c` (setPipelineState, blend constants)
2. `m4-render-opcode-draw.c` (drawPrimitives family)
3. `m4-render-opcode-state.c` (viewport, scissor, stencil)
4. `m4-compute-opcodes.c` (all 32 compute opcodes)
5. `m4-blit-opcodes.c` (all 24 blit opcodes)
6. `tests/m1-shader-catalog.c` (shader lookup/eviction)

### Step 4: Verify Full Coverage
```bash
meson test -C builddir --print-errorlogs
# Expect: 20+ tests, all PASS

# Then deploy to Docker VM and verify M5 Stage 10% via noVNC
ssh docker 'cd /home/matthew/mos-docker && sudo docker compose up -d macos'
```

---

## Success Criteria

Before enabling any new development:
- ✅ All existing 16 tests still PASS (no regressions)
- ✅ 7+ new opcode tests added and passing
- ✅ Full regression suite runs in <30 seconds
- ✅ CLAUDE.md updated with test coverage status
- ✅ M5 visible pixels blocker resolved via display path testing

---

## References

- **Opcode definitions:** `src/protocol/opcodes.h`, `paravirt-re/library/state-machines/inner-opcodes.md`
- **Existing test patterns:** `tests/m3-stamp-helpers.c`, `tests/m4-task-translate.c`
- **M5 progress tracking:** `memory/project_m5_progress_scale_2026_04_26.md`
- **Render opcode implementation:** `src/protocol/render_opcodes.c` (104KB, ~95 opcodes)
