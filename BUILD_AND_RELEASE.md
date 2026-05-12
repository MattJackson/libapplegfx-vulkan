# Build & Release Guide — libapplegfx-vulkan

Complete guide for building, testing, and releasing this library with GitHub Actions CI.

## Quick Start (Local Development)

### Prerequisites

```bash
# macOS/Linux development environment
brew install meson ninja vulkan-loader  # macOS
sudo apt-get install meson ninja-build libvulkan-dev  # Ubuntu/Debian

# Optional: Vulkan validation layers and lavapipe for testing
brew install vulkan-validation-layers  # macOS
```

### Build Locally

```bash
cd ~/Developer/libapplegfx-vulkan

# Setup build directory with debug flags
meson setup build -Db_sanitize=address,undefined -Dtests=enabled

# Compile
ninja -C build

# Run tests (requires Vulkan/lavapipe)
ninja -C build test

# Build release version
meson setup build-rel -Dtests=enabled --reconfigure \
  -Dbuildtype=release -Db_sanitize=none

ninja -C build-rel
```

### Verify Build Works

```bash
# Check library exports
nm -g build/libapplegfx-vulkan.so.0.0.1 | grep " T lagfx_" | head -20

# Run a simple test binary
./build/tests/vulkan-init  # Should initialize Vulkan instance
```

## CI/CD Workflow (GitHub Actions)

### What Triggers CI?

Every push to `main` branch automatically triggers the `ci.yml` workflow with these jobs:

1. **clang-tidy** — Static analysis on public headers only
2. **meson (ubuntu-latest, release, glibc)** — Primary build verification
3. **meson (ubuntu-latest, debug, glibc)** — Debug build + sanitizers disabled
4. **meson (macos-latest)** — macOS compatibility check
5. **meson (alpine:3.21, musl)** — Musl libc compatibility
6. **sanitizers** — ASAN+UBSAN on Linux

### Fast CI Monitoring (Don't Sleep!)

Use GitHub API + jq to get real-time job status without waiting for entire run to complete:

```bash
# Get run ID from recent push
RUN_ID=$(gh run list --repo MattJackson/libapplegfx-vulkan --limit 1 -s in_progress \
  | awk 'NR==2 {print $NF}')

# Check job status immediately (works while run is still in progress)
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | "\(.name): \(.conclusion // "running")"'

# Get IDs of failed jobs for immediate log inspection
FAILED_JOB_IDS=$(gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | .id')

# Fetch logs for specific failed job (even if parent run isn't complete)
for JOB_ID in $FAILED_JOB_IDS; do
  echo "=== Job: $JOB_ID ===" 
  gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/$JOB_ID/logs \
    | tail -60
done
```

### Why This Is Faster Than Traditional Methods

| Approach | Time to First Failure Insight |
|----------|------------------------------|
| `gh run watch <id>` | Waits for entire run (~3-5 min) |
| `sleep 60 && gh run view --log-failed` | Wastes time sleeping, parses full log |
| **GitHub API + jq** | **<10 seconds after job completes** |

The GitHub Actions API returns real-time job status. Use `jq` to filter for failures immediately—no need to wait or parse entire logs.

## Common Build Failures & Fixes

### 1. Missing Source File in meson.build

```
ERROR: File mmio.c does not exist.
```

**Cause:** `.c` file deleted but still referenced in `meson.build`.

**Fix:** Update source list to match actual files on disk.

```bash
# Check what .c files actually exist
ls src/*.c src/**/*.c

# Update meson.build accordingly
git add src/meson.build
git commit -m "Fix: update meson.build to match actual source files"
git push origin main
```

### 2. Duplicate Symbol Errors

```
multiple definition of `lagfx_mmio_read`
first defined in device.c, multiple definition in mmio.c
```

**Cause:** Same function defined in two `.c` files.

**Fix:** Keep single source of truth, remove duplicate.

```bash
# Identify duplicates
git grep -l "^uint32_t lagfx_mmio_read" src/*.c

# Remove the duplicate file
rm src/mmio.c
git add src/mmio.c  # Mark for deletion
git commit -m "Remove duplicate mmio.c"
git push origin main
```

### 3. Undefined Reference Errors

```
undefined reference to `lagfx_protocol_dispatch_one`
undefined reference to `lagfx_protocol_mmio_write`
```

**Cause:** Function exists in source file but not included in build.

**Fix:** Add missing `.c` file to `meson.build`.

```bash
# Find where function is defined
git grep -l "^int lagfx_protocol_dispatch_one" src/**/*.c

# Add it to protocol/meson.build
cat >> src/protocol/meson.build << 'EOF'
  'protocol.c',     # dispatch_one, mmio_read/write functions
EOF

git add src/protocol/meson.build
git commit -m "Fix: include protocol.c in meson.build"
git push origin main
```

### 4. Missing Symbol Declarations

```
undefined reference to `lagfx_protocol_last_completed_stamp`
```

**Cause:** Function moved from external declaration to static inline, but header still declares it externally.

**Fix:** Move function implementation to internal header as `static inline`.

```bash
# Add to src/protocol/state.h (internal header)
cat >> src/protocol/state.h << 'EOF'
/* Accessor for last completed stamp — used by tests. */
static inline uint32_t lagfx_protocol_last_completed_stamp(const lagfx_protocol_t *p) {
    return lagfx_protocol_is_valid(p) ? p->last_completed_stamp : 0u;
}
EOF

# Remove declaration from public header (src/protocol/protocol.h)
git checkout -- src/protocol/protocol.h  # or manually edit
```

### 5. Compiler Warnings as Errors

```
error: unknown conversion type character 'v' in format [-Werror=format=]
```

**Cause:** Format string mismatch in `printf`-style functions.

**Fix:** Use correct format specifiers (`%lx` for `uint64_t`, `%u` for `uint32_t`).

```bash
# Find the warning source
gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/$JOB_ID/logs \
  | grep -B 5 "unknown conversion type"

# Fix format specifier
sed -i 's/%x/%lx/g' src/file.c  # For uint64_t values
git add src/file.c
git commit -m "Fix: correct format specifier for uint64_t"
git push origin main
```

## Release Process

### When to Release

Release when:
- ✅ All CI jobs green on `main` branch
- ✅ No regressions in test suite
- ✅ Breaking changes documented (if any)
- ✅ Version bump planned (see versioning below)

### Version Bump Strategy

This project uses **semantic versioning** (`MAJOR.MINOR.PATCH`).

```bash
# Update version in meson.build
sed -i "s/version : '0.0.1'/version : '0.1.0'/" meson.build

# Check git log for changelog-worthy changes
git log --oneline HEAD~3..HEAD

# Commit version bump
git add meson.build
git commit -m "Bump version to 0.1.0"

# Tag release (after CI passes)
git tag v0.1.0
git push origin main --tags
```

### Publishing the Library

The library is automatically built and published as:
- **GitHub Packages**: `ghcr.io/MattJackson/libapplegfx-vulkan`
- **pkg-config file**: Installed to `/usr/local/lib/pkgconfig/libapplegfx-vulkan.pc`

Downstream consumers (like `mos-qemu`) can use:

```bash
# In meson.build of dependent project
vulkan_dep = dependency('vulkan', required : false)
lagfx_dep = dependency('libapplegfx-vulkan', required : true, 
                       fallback : ['applegfx-vulkan', 'applegfx-vulkan_dep'])
```

## Debugging Failed CI Builds

### Step 1: Identify Failed Jobs

```bash
RUN_ID=$(gh run list --repo MattJackson/libapplegfx-vulkan --limit 1 -s completed \
  | awk 'NR==2 {print $NF}')

gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | "\(.name)\t\(.id)"'
```

### Step 2: Fetch Job Logs Immediately

```bash
FAILED_JOB=$(gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | .id' | head -1)

gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/$FAILED_JOB/logs > /tmp/job.log
tail -100 /tmp/job.log
```

### Step 3: Search for Specific Errors

```bash
# Linker errors (undefined references)
grep "undefined reference" /tmp/job.log | head -5

# Compiler errors (syntax/type issues)
grep "^error:" /tmp/job.log | head -10

# Missing file errors
grep "does not exist" /tmp/job.log

# Format string warnings
grep "format.*expects argument" /tmp/job.log
```

### Step 4: Reproduce Locally

```bash
# Recreate the failing build environment
meson setup build-fail -Db_sanitize=address,undefined -Dtests=enabled
ninja -C build-fail 2>&1 | tee /tmp/local-build.log

# Compare with CI logs
diff <(tail -50 /tmp/job.log) <(tail -50 /tmp/local-build.log)
```

## Build Architecture Overview

### Source File Organization

```
src/
├── device.c              # Device lifecycle, MMIO entry points
├── display.c             # Display attachment/detachment
├── doorbell.c            # BAR0 write dispatcher (registry-based)
├── version.c             # Version info + build metadata
│
├── common/               # Shared utilities
│   ├── log.h             # LAGFX_LOG / LAGFX_ERR macros
│   └── meson.build
│
├── protocol/             # Protocol decoder + state machine
│   ├── protocol.c        # Core dispatch logic (lagfx_protocol_dispatch_one)
│   ├── protocol.h        # Public API declarations
│   ├── state.h           # Internal state + static inline helpers
│   ├── lifecycle.c       # new/free/reset
│   ├── stamp.c           # Stamp completion & advancement
│   ├── resource_registry.c  # Resource mapping table
│   └── ops_misc.c        # NOP, Debug, GetDeviceInfo handlers
│
├── dispatchers/          # Per-channel doorbell handlers
│   ├── primary_ring_door_dispatcher.c    # BAR0+0x1008 (root channel)
│   ├── channel_door_dispatcher.c         # BAR0+0x1020 (channel selector)
│   └── channel_*_dispatcher.c            # Per-channel routing
│
├── handlers/             # Opcode execution handlers
│   ├── compute/exec_cmdbuf.c     # CmdExecCmdbuf implementation
│   ├── task/task.c               # Task management
│   ├── memory/memory.c           # Memory mapping
│   ├── display/display.c         # Display operations
│   └── sync/sync.c               # Synchronization primitives
│
├── vulkan/             # Vulkan backend (Phase 1.B)
│   └── instance.c      # vkCreateInstance + device setup
│
├── translate/          # Metal → Vulkan translation
│   └── render_encoder.c    # Render pass translation
│
└── air2spirv/          # AIR → SPIR-V shader compilation
```

### Build Pipeline (meson)

1. **Top-level `meson.build`**: Discovers subdirectories, sets up Vulkan dependency
2. **`src/meson.build`**: Core library sources (`device.c`, `display.c`, etc.)
3. **Subdirectory `meson.build`s**: Append to shared `lagfx_sources` list:
   - `protocol/meson.build` → protocol decoder + state machine
   - `dispatchers/meson.build` → per-channel routing logic
   - `handlers/meson.build` → opcode execution implementations
4. **Linking**: All sources linked into single shared library `libapplegfx-vulkan.so.0.0.1`

### Key Design Decisions

| Decision | Rationale |
|----------|-----------|
| **Single registry lookup for doorbells** | No hardcoded offsets; clean separation of concerns |
| **Static inline helpers in internal headers** | Zero runtime overhead, type-safe, no duplicate symbols |
| **Modular `meson.build` structure** | Easy to add/remove source files without touching top-level config |
| **Protocol.c as single dispatch entry point** | Centralized command routing; tests can call `lagfx_protocol_dispatch_one()` directly |

## Troubleshooting Checklist

Before reporting a CI failure, check:

- [ ] Can you reproduce the build locally with same flags?
- [ ] Did you update `meson.build` when adding/removing source files?
- [ ] Are all function declarations matched by implementations in `.c` files?
- [ ] Do static inline helpers stay in internal headers (`*.h` in `src/`)?
- [ ] Is the public API header (`include/libapplegfx-vulkan.h`) only declaring external symbols?
- [ ] Did you check for duplicate symbol definitions across `.c` files?

## CI Command Cheat Sheet

```bash
# List recent runs (filtered by status)
gh run list --repo MattJackson/libapplegfx-vulkan --limit 3 --status in_progress

# Get job status for specific run
RUN_ID=25763598904
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | "\(.name): \(.conclusion // "running")"'

# Get failed job logs immediately
FAILED_JOB=$(gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/$RUN_ID/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | .id' | head -1)

gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/$FAILED_JOB/logs > /tmp/fail.log
tail -60 /tmp/fail.log

# Find undefined reference errors
grep "undefined reference" /tmp/fail.log | head -5

# Reproduce locally
meson setup build-debug -Db_sanitize=address,undefined -Dtests=enabled && \
  ninja -C build-debug

# Check library exports after successful build
nm -g build/libapplegfx-vulkan.so.0.0.1 | grep " T lagfx_" | wc -l
```

---

**TL;DR:** Push → Use GitHub API + jq to monitor CI jobs in real-time → Fetch logs for failed jobs immediately by ID → Fix issues locally, commit → push → repeat. Don't sleep waiting for entire run to complete.
