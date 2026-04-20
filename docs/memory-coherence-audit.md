# Memory coherence audit — `lagfx_task_map_host_memory`

**Date:** 2026-04-20
**Auditor:** Phase 1.C coherence audit (Phase 2 gate per
`/Users/mjackson/mos/paravirt-re/phase-2-first-pixel-plan.md` §8 item 4
and §R7).
**File audited:** `src/memory/task.c` — function
`lagfx_task_map_host_memory` (lines 141–190 as of commit ab1070d).

---

## 1. Executive summary

**Verdict: COPY-ON-MAP (incorrect).**

The current implementation allocates fresh memfd-backed pages into the
task's reserved VA range and then performs a one-shot
`memcpy(target, host_addr, len)` from the QEMU-owned host pointer.
The task VA and the host_ptr backing refer to **two distinct
physical page sets**. Writes by the guest (via its own mapping of the
same RAMBlock) propagate only to the host_ptr's backing; the task VA
view holds a stale snapshot.

This is adequate for Phase 1 (metal-no-op: empty cmdbuf, no mapped
guest-writable DMA) but is a **hard blocker for Phase 2 first-pixel**,
because the guest's indirect command buffer for
`CmdExecIndirect2` is written into a mapped range *after* the map
callback returns and re-read from the host by the decoder at submit
time. Under copy-on-map the decoder sees zeros (or whatever the guest
had placed there before the map call), not the actual command stream.

---

## 2. Evidence

Current implementation (quoted from
`src/memory/task.c:141-190`):

```c
bool lagfx_task_map_host_memory(lagfx_task_t *task, uint64_t vm_offset,
                                 void *host_addr, uint64_t len,
                                 bool read_only) {
    if (!task || vm_offset + len > task->reserved_size || len == 0) {
        return false;
    }

    /* Create or reuse memfd backing for this mapping. */
    if (task->memfd < 0) {
        task->memfd = task_create_memfd(task->reserved_size);
        if (task->memfd < 0) {
            return false;
        }
    }

    /* Target address in the reserved range. */
    void *target = (char *)task->reserved_base + vm_offset;

    /* Map protection flags. */
    int prot = PROT_READ;
    if (!read_only) {
        prot |= PROT_WRITE;
    }

    /* Map the memfd into the task's reserved range at the fixed offset. */
    void *mapped = mmap(target, (size_t)len, prot,
                         MAP_FIXED | MAP_SHARED, task->memfd,
                         (off_t)vm_offset);
    if (mapped == MAP_FAILED) {
        ...
        return false;
    }

    /* If host_addr is provided (e.g., QEMU's guest RAM pointer),
     * copy its contents into the newly mapped range.
     * Future: This could be optimized with page-table tricks to avoid
     * the copy, but for MVP we err on the side of correctness. */
    if (host_addr) {
        memcpy(target, host_addr, (size_t)len);                 /* <-- */
    }

    return true;
}
```

The load-bearing line is the `memcpy` at `src/memory/task.c:186`. The
`mmap(... task->memfd ...)` at line 172 backs the task VA with
**private** memfd pages owned by this library, not the pages behind
`host_addr`. `MAP_SHARED` here only shares within-process mappings of
`task->memfd` itself — it does not alias QEMU's RAMBlock.

The header's own design note calls this out at `src/memory/task.h:41`:

> "We do NOT mmap() those directly (we don't own them). [...] For
> now: guest DMA ranges allocate fresh memfd-backed storage, separate
> from QEMU's RAMBlock."

And the QEMU shell comment at
`qemu-mos15/hw/display/apple-gfx-common-linux.c:187` flags the same:

> "the current library implementation copies the host-addr contents
> into the memfd on map; a future optimisation may share pages
> directly [...] verified only for read-mostly ranges in Phase 1.A.1"

The audit confirms that flag. Phase 2 is the first consumer that
cannot tolerate it.

---

## 3. Fix options considered

### Option A — Public API change: QEMU passes its memfd fd

Change `lagfx_shell_callbacks_t::map_memory` signature (or add a new
callback) so QEMU hands the library `(memfd_fd, offset_in_memfd,
length)` instead of `(host_ptr, length)`. The library then
`mmap(target, length, MAP_FIXED|MAP_SHARED, memfd_fd, offset)` —
zero-copy true aliasing.

- **Pros:** Cleanest semantics; no guessing at the backing fd;
  portable across any MAP_SHARED backing (memfd, shm_open, hugetlbfs,
  file-backed).
- **Cons:** Public header change. Requires coordinated landing in
  `qemu-mos15/hw/display/apple-gfx-common-linux.c` (shell) and
  `libapplegfx-vulkan/include/libapplegfx-vulkan.h` (API). Blocked by
  agent write-set fence — cannot land from this audit wave.

**Verdict:** correct long-term fix; defer to a follow-up wave that
owns the public API.

### Option B — `/proc/self/map_files` introspection

On Linux, `/proc/self/map_files/<start>-<end>` gives an openable link
back to the file backing a VMA. From `host_ptr` we scan
`/proc/self/maps` to find the containing VMA, then open the
corresponding entry under `map_files`, then `mmap(MAP_FIXED|MAP_SHARED,
that_fd, offset)`.

- **Pros:** No public API change; no QEMU-side cooperation.
- **Cons:** Linux-specific (not just Linux-preferred); requires
  `/proc` access (may be denied in some sandboxes); VMA split/merge
  behaviour could make lookups racy; reading `/proc/self/maps` on a
  large process each map is O(n) in mappings; fragile under THP.

**Verdict:** works, but the complexity-per-gain ratio is worse than
Option C given Option C needs no FD at all.

### Option C — `mremap(old_addr=host_ptr, old_size=0, new_size=len, MREMAP_FIXED|MREMAP_MAYMOVE, new_addr=target)`

On Linux 4.7+, `mremap()` with `old_size == 0` against a `MAP_SHARED`
source creates a **duplicate** mapping pointing at the same underlying
pages, rather than moving the source. QEMU's
`memory_region_get_ram_ptr()` returns a pointer into a RAMBlock backed
by `mmap(MAP_SHARED, memfd, ...)` (or file-backed with `-mem-path`),
which meets the MAP_SHARED precondition for the duplicate-mapping
semantics.

- **Pros:** No public API change. No FD plumbing. Works for any
  MAP_SHARED backing. Single syscall per map.
- **Cons:** Linux-only (Darwin dev host has no `mremap`). Semantics of
  `old_size=0` are documented but less well-known, so the code needs a
  load-bearing comment. Requires Linux 4.7+ (MREMAP_DONTUNMAP was
  added later and is unrelated; basic MREMAP_FIXED has been there
  forever). Fails with EINVAL if the source VMA is `MAP_PRIVATE` —
  we must verify QEMU's RAMBlock backing is MAP_SHARED (it is: see
  `qemu/system/physmem.c:qemu_ram_mmap()` which uses MAP_SHARED for
  memfd-backed, file-backed, and some anonymous allocations).
- **Edge case:** If the guest RAM backing is `MAP_PRIVATE` anonymous
  (QEMU without `-mem-path` and without memfd-backed RAM), `mremap
  MREMAP_MAYMOVE|MREMAP_FIXED` with `old_size=0` returns EINVAL. We
  fall back to the copy path with a `LAGFX_LOG` warning so the
  operator can reconfigure QEMU to use memfd-backed RAM. The
  mos-qemu production path explicitly uses memfd-backed RAM (per
  `docker-macos/` compose configs and `qemu-mos15` device setup), so
  this fallback is a safety net, not a production path.

**Verdict:** chosen. Implementable in this audit wave with no public
API change.

### Rejected: double-mmap via shared VMA discovery without /proc

There is no portable POSIX way to "clone" an existing `MAP_SHARED`
mapping without knowing its fd. Options would be:
- `mprotect + mmap(MAP_FIXED)` with the same fd — requires the fd.
- `process_vm_readv` — copies, same problem.
- `memfd_create` + guest-side cooperation — that's Option A.

Only `mremap` (Linux) or Mach `vm_remap` (Darwin) give VMA-level
duplication without the fd. Hence Option C.

---

## 4. Chosen fix — Option C (Linux: mremap; Darwin: retain copy)

Implementation summary (see `src/memory/task.c`):

1. Reserve VA range unchanged (`mmap PROT_NONE MAP_PRIVATE|MAP_ANONYMOUS`).
2. `lagfx_task_map_host_memory`:
   - On Linux with `host_addr != NULL`, first `munmap(target, len)` to
     release the PROT_NONE reservation at that sub-range (required
     because `mremap MREMAP_FIXED` refuses to overlay an existing
     mapping unless the destination is unmapped — the old MAP_FIXED
     mmap-the-memfd pattern worked because `MAP_FIXED` *replaces*, but
     `mremap MREMAP_FIXED` *requires* a clear destination).
   - Call
     `mremap(host_addr, 0, len, MREMAP_MAYMOVE|MREMAP_FIXED, target)`.
   - On success: task VA at `target` now aliases `host_addr`'s pages.
     Writes by guest (which writes via its own mapping of the same
     RAMBlock) are immediately visible at `target`. Writes by the
     library at `target` are immediately visible to the guest.
   - On failure (EINVAL — source not MAP_SHARED — or ENOMEM): fall
     back to the legacy copy path with a `LAGFX_LOG` warning
     identifying the degraded coherence mode.
3. On Darwin (dev host only — not production): retain the legacy
   memfd + memcpy path; log an audit-trail warning so nobody is
   surprised when coherence tests fail on a dev build.
4. `lagfx_task_unmap` unchanged: replace with PROT_NONE pages.
5. `lagfx_task_destroy` unchanged: `munmap` the whole reservation;
   the mremap-duplicated VMA is cleaned up by the munmap.

**Why not `mmap(MAP_FIXED)` from the QEMU memfd directly (Option A
without the API change)?** Because we don't have the memfd. We only
have the host-virtual pointer `host_addr`. Option C's trick is to use
`mremap` to reach the underlying pages *through* the existing VMA
without ever learning what fd backs it.

---

## 5. Test

`tests/memory-coherence.c` (new) exercises the post-map write
coherence that copy-on-map fails:

1. Allocate a host-visible "RAMBlock analog" via
   `memfd_create + ftruncate + mmap(MAP_SHARED)` — this models
   QEMU's guest RAM backing.
2. Write sentinel A (0xAA) into the whole region.
3. `lagfx_task_create` + `lagfx_task_map_host_memory(host_ptr, ...)`.
4. Verify task VA shows sentinel A (catches broken first-shot maps).
5. Overwrite the host RAM via the *original host_ptr* with sentinel B
   (0xBB). If the task VA aliases, it sees 0xBB immediately.
6. Assert task VA[0] == 0xBB — this is the load-bearing assertion.
7. Overwrite task VA with sentinel C (0xCC). If aliasing, host_ptr
   sees 0xCC.
8. Assert host_ptr[0] == 0xCC — the bidirectional assertion.

Copy-on-map behaviour: step 6 fails (task VA still 0xAA) and step 8
fails (host_ptr still 0xBB).
Alias behaviour: both pass.

The test is registered in `tests/meson.build`; on Linux it must
pass post-fix. On Darwin it is built but skipped (the Darwin code
path keeps the copy and the test would fail — skip is correct
because Darwin isn't the production target).

---

## 6. Residual risks after fix

- **QEMU uses private anonymous RAM** (no `-mem-path`, no
  memory-backend-memfd): `mremap` returns EINVAL, we fall back to
  copy, Phase 2 pixels go missing. Mitigation: document the QEMU
  config requirement (memory-backend-memfd) in the M4 runbook; the
  EINVAL fallback logs loudly so operators notice in triage.
- **THP interactions with mremap**: if THP promotes 4K pages to 2M
  hugepages on the source VMA after our duplicate, the alias still
  points at the same physical pages (mremap does not copy), so there
  is no correctness risk, only a potential TLB-shootdown pulse on
  promotion. Not Phase 2 territory.
- **Guest writes through its own mapping of the RAMBlock** while the
  library is mid-read: that's ordinary DMA race territory. Synchronised
  at submit via the existing FIFO doorbell handshake — unchanged by
  this audit.
- **Unmap semantics**: `lagfx_task_unmap` still replaces with PROT_NONE
  pages via `mmap MAP_FIXED|MAP_PRIVATE|MAP_ANONYMOUS`. This cleanly
  breaks the alias without affecting the source VMA. Verified by
  follow-on assertions in the coherence test.

---

## 7. Follow-up items

- **Option A (API change) is still the right long-term fix.** Option C
  works but layers Linux-specific trickery under a public API that
  should really carry the backing fd. Queue an RFC to add
  `lagfx_shell_map_fd_t` to `lagfx_shell_callbacks_t` as the
  preferred path; keep Option C's `mremap` path as the
  `host_ptr`-only fallback when QEMU hasn't migrated.
- **QEMU config requirement**: document that
  `-object memory-backend-memfd,id=ram,size=...` (or
  memory-backend-file with share=on) is required for Phase 2
  coherence. The default `-m 4G` path on some QEMU builds uses
  anonymous private mappings which trigger the copy fallback.
- **Update
  `qemu-mos15/hw/display/apple-gfx-common-linux.c:187-193`**
  comment once the fix lands to reflect the new mremap-alias
  semantics and the fallback condition. (Out of this wave's
  write-set; flag for shell-side agent.)

---

**End of audit.**
