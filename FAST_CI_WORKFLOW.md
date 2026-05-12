# Fast CI Workflow — libapplegfx-vulkan

Quick reference for monitoring and debugging GitHub Actions builds without waiting or using slow CLI commands.

## Quick Status Checks

```bash
# List recent runs (filtered by status)
gh run list --repo MattJackson/libapplegfx-vulkan --limit 3 --status in_progress

# Get jobs from a specific run with their conclusions
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
  | jq -r '.jobs[] | "\(.name): \(.conclusion // "running")"'
```

## Find Failed Jobs Immediately (While Run is Still In Progress)

```bash
# Get IDs of failed jobs from in-progress run
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | .id'

# Fetch logs for a specific failed job (works even if parent run isn't complete)
gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/<JOB_ID>/logs \
  | tail -60
```

## Common Failure Patterns & Quick Fixes

### Duplicate Symbol Errors (`lagfx_mmio_read/write`)
**Cause:** Both `device.c` and `mmio.c` define the same functions.
**Fix:** Remove `mmio.c` (keep `device.c` as single source of truth). Update `src/meson.build`.

```bash
git add src/mmio.c  # mark for deletion
git rm --cached src/mmio.c  # if still in index
git commit -m "Remove duplicate mmio.c"
git push origin main
```

### Missing Symbol Errors (`lagfx_protocol_last_completed_stamp`)
**Cause:** Function declared in `protocol.h` but implementation moved to static inline in `state.h`.
**Fix:** Remove declaration from public header, keep only in internal state header.

```bash
# Move function to src/protocol/state.h as static inline
git add src/protocol/state.h src/protocol/protocol.h
git commit -m "Fix: move lagfx_protocol_last_completed_stamp to static inline"
git push origin main
```

### Build System Errors (`File mmio.c does not exist`)
**Cause:** `src/meson.build` still references deleted `.c` file.
**Fix:** Update source list in `src/meson.build`.

```bash
# In src/meson.build, replace 'mmio.c' with 'doorbell.c'
git add src/meson.build
git commit -m "Fix: replace mmio.c with doorbell.c in build"
git push origin main
```

## Monitoring Workflow (Speed-Optimized)

1. **Push → Check for new run:**
   ```bash
   gh run list --repo MattJackson/libapplegfx-vulkan --limit 2 --status in_progress
   ```

2. **Wait ~5 seconds, check job status:**
   ```bash
   gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
     | jq -r '.jobs[] | select(.conclusion != null) | "\(.name): \(.conclusion)"'
   ```

3. **If failures detected, fetch logs immediately:**
   ```bash
   FAILED_JOB=$(gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
     | jq -r '.jobs[] | select(.conclusion == "failure") | .id' | head -1)
   
   gh api repos/MattJackson/libapplegfx-vulkan/actions/jobs/$FAILED_JOB/logs \
     | tail -50
   ```

4. **Fix + commit → push → repeat** (no `sleep` needed between steps)

## Why This Is Faster Than `gh run watch` or `sleep` Commands

| Approach | Time to First Failure Insight |
|----------|------------------------------|
| `gh run watch <id>` | Waits for entire run to complete (~2-5 min) |
| `sleep 60 && gh run view --log-failed` | Wastes time sleeping, then parses full log |
| **GitHub API + jq** | **<10 seconds after job completes** |

The GitHub Actions API returns real-time job status. Use `jq` to filter for failures immediately — no need to wait or parse entire logs.

## Key Commands Cheat Sheet

```bash
# Get run ID from recent pushes
gh run list --repo MattJackson/libapplegfx-vulkan --limit 1 -s in_progress \
  | awk 'NR==2 {print $NF}'

# One-liner: check if any jobs failed
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
  | jq -r '[.jobs[] | select(.conclusion == "failure")] | length > 0'

# Get all failure reasons in one command
gh api repos/MattJackson/libapplegfx-vulkan/actions/runs/<RUN_ID>/jobs \
  | jq -r '.jobs[] | select(.conclusion == "failure") | "\(.name) -> ID: \(.id)"'
```

## Post-Fix Verification Checklist

- [ ] `git status` clean (no uncommitted changes)
- [ ] All `.c` files referenced in `src/meson.build` exist on disk
- [ ] No duplicate symbol definitions across translation units
- [ ] Static inline helpers stay in internal headers (`protocol/state.h`)
- [ ] Public API header (`protocol/protocol.h`) only declares external symbols
- [ ] `git push origin main` triggers fresh CI build

---

**TL;DR:** Don't sleep. Use GitHub API + jq to get job-level status immediately after each commit. Fetch logs for failed jobs by ID — don't wait for entire run to complete.
