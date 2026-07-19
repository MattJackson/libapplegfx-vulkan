/*
 * libapplegfx-vulkan — Compute channel dispatcher (ch 1-4)
 * src/dispatchers/channel_compute_dispatcher.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Per-channel ring drain for compute vchans. Each child channel
 * (chan_id 1..4) has its own command ring registered via
 * FIFORingDescriptor at:
 *
 *   shared_control + 0x400 + 20 * (chan_id - 1)
 *
 * where shared_control is the page registered at BAR0+0x101c
 * (ring_shared_page_pfn). See PROTOCOL.md §3, §5 and
 * paravirt-re/library/state-machines/FIFORingDescriptor.md (the
 * authoritative reference verified 2026-04-25 against a live xp dump).
 *
 * Wire format and read semantics on each ring mirror the root-channel
 * drainer (channel_0_dispatcher.c). The descriptor's read_ptr is
 * authoritative — host advances it after draining.
 */

#include "channel_compute_dispatcher.h"
#include "ring_common.h"
#include "../doorbell.h"
#include "../device.h"
#include "../common/le.h"
#include "../common/log.h"
#include "protocol/opcodes.h"
#include "protocol/state.h"
#include "protocol/object_resolver.h"
#include "handlers/handlers.h"
#include "handlers/iosurface/iosurface.h"

#include <stddef.h>
#include <stdlib.h>

/* Phase 6b: Wire data captured 2026-05-28 showed 0x25
 * CmdSetObjectAndPlacementList payload is `{count: u32, objectId: u32}`
 * (see paravirt-re/library/apv-object-entry-parser-scoping-2026-05-17.md).
 * Insert the objectId into the per-task active-objects registry so
 * op_0x74 SetRenderPipelineState can query it before falling back to
 * the heap-VA lookup. Idempotent — duplicate objectIds are a no-op. */
static void
register_active_object(lagfx_task_entry_t *task, uint32_t object_id) {
    if (!task) return;
    /* Linear scan for dedup. The bounded set (64 entries) is fine for
     * Apple's observed traffic (~10s of unique objectIds per task). */
    for (uint32_t i = 0; i < task->active_objects.count; i++) {
        if (task->active_objects.object_ids[i] == object_id) return;
    }
    if (task->active_objects.count >= 64u) {
        LAGFX_WARN("register_active_object: task=%u registry full (64); dropping objectId=0x%x",
                   task->id, object_id);
        return;
    }
    task->active_objects.object_ids[task->active_objects.count++] = object_id;
}

/* Phase 6b reconnaissance: env-gated hex-dump of 0x24/0x25 payload
 * bytes. The current 0x24 (CmdSetObjectList) + 0x25
 * (CmdSetObjectAndPlacementList) handlers are log+ack stubs; the
 * wire format is OPEN (see paravirt-re/library/apv-object-entry-
 * parser-scoping-2026-05-17.md). This helper writes the payload as
 * hex to lagfx.log when LAGFX_PHASE_B2_CAPTURE is set, capped at 8
 * captures total. Consumed by senior-side wire RE. */
static void
capture_object_list_payload_b2(uint16_t opcode, const lagfx_cmd_header_t *hdr) {
    static uint32_t cap_count = 0u;
    if (cap_count >= 48u) return;  /* M2 RE: raised from 8 so 0x34/0x39 get captured too */
    const char *env = getenv("LAGFX_PHASE_B2_CAPTURE");
    if (!env || env[0] == '0' || env[0] == '\0') return;
    if (!hdr || !hdr->payload || hdr->payload_size == 0u) return;

    uint16_t n = hdr->payload_size;
    if (n > 256u) n = 256u;  /* clamp per-line; full payload may exceed */

    /* Emit a single LAGFX_LOG line per 16-byte chunk. */
    char hex[16 * 3 + 1];
    uint16_t off = 0;
    while (off < n) {
        uint16_t chunk = (uint16_t)((n - off) < 16u ? (n - off) : 16u);
        for (uint16_t i = 0; i < chunk; i++) {
            static const char H[] = "0123456789abcdef";
            uint8_t b = hdr->payload[off + i];
            hex[i * 3 + 0] = H[(b >> 4) & 0xFu];
            hex[i * 3 + 1] = H[b & 0xFu];
            hex[i * 3 + 2] = ' ';
        }
        hex[chunk * 3] = '\0';
        LAGFX_LOG("B2_CAPTURE op=0x%02x stamp=0x%08x +0x%03x: %s",
                  opcode, hdr->stamp, off, hex);
        off += chunk;
    }
    if (hdr->payload_size > n) {
        LAGFX_LOG("B2_CAPTURE op=0x%02x stamp=0x%08x truncated (full %u)",
                  opcode, hdr->stamp, (unsigned)hdr->payload_size);
    }
    cap_count++;
}

#define LAGFX_COMPUTE_DRAIN_MAX_CMDS 128u
#define LAGFX_COMPUTE_MAX_CMD_BYTES  4096u

/* VirtualChannel ring size per FIFORingDescriptor.md (§"ring size"):
 * 0x10000 (64 KiB). See ring_common.h for the descriptor read +
 * PFN-array resolve helpers.
 */
#define LAGFX_COMPUTE_RING_SIZE  0x10000u

/* Dispatch a single command to appropriate handler. */
static void dispatch_command(lagfx_protocol_t *p, const lagfx_cmd_header_t *hdr) {
    if (!p || !hdr) {
        return;
    }

    LAGFX_TRACE("compute dispatch: opcode=0x%04x stamp=0x%08x", hdr->opcode, hdr->stamp);

    switch (hdr->opcode) {
        /* CmdExecIndirect2 on a compute channel — the inner stream is
         * where Metal commands live. See render-decoder-handlers.md. */
        case LAGFX_OP_EXEC_INDIRECT2:  /* 0x20 */
            lagfx_compute_exec_cmdbuf(p, hdr);
            break;

        /* Kext-side per-channel exec variant per
         * M4-inner-opcode-implementation-guide.md §1.1 — same outer
         * payload as 0x20, kext emits this on the per-channel exec
         * rings (ch 1..4). Conflicts with LAGFX_OP_CHANNEL_EVENT_37
         * naming in opcodes.h — the kext-disasm pass classified 0x37
         * as "ChannelEventMachine-adjacent" before the M4 RE pass
         * identified it as the per-channel CmdExecIndirect2. The
         * payload's outer layout (12 + dc*24 + rc*16) distinguishes
         * the two at runtime; for now route 0x37 to exec_cmdbuf
         * which handles both empty (event-only) and populated (real
         * exec) payloads gracefully. */
        case LAGFX_OP_CHANNEL_EVENT_37:  /* 0x37 — see comment above */
            lagfx_compute_exec_cmdbuf(p, hdr);
            break;

        /* Channel-event / immediate-vchan opcodes fired by the kext
         * on the Immediate (ch=2) and Uploads/Downloads (ch=3/4)
         * channels. Log + ack — these don't carry render workloads
         * (no inner-PGCmdHeader stream); they're kext-internal lifecycle
         * events. Documented in paravirt-re/library/PROTOCOL.md §13
         * + journey/opcodes-0x35-0x36-0x39.md. */
        case LAGFX_OP_UNMAP_MEMORY_IMMEDIATE: /* 0x22 — CmdUnmapMemoryImmediate */
            LAGFX_LOG("compute: 0x22 CmdUnmapMemoryImmediate ch=%u stamp=0x%08x "
                      "payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;
        case LAGFX_OP_SET_OBJECT_LIST:        /* 0x24 CmdSetObjectList */
            /* Stub. Payload per command-buffer-format.md:254 is
             * `objectArray[], count`. Phase 6b will parse the
             * objectArray (APVObjectEntry records) to register
             * pipeline-state metallib bytes. Today: log+ack discards
             * the payload. With LAGFX_PHASE_B2_CAPTURE=1, the payload
             * is hex-dumped to lagfx.log for offline RE. */
            LAGFX_LOG("compute: 0x24 CmdSetObjectList ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            capture_object_list_payload_b2(0x24u, hdr);
            break;
        case LAGFX_OP_SET_OBJECT_PLACEMENT:   /* 0x25 CmdSetObjectAndPlacementList */
            /* Phase 6b — wire data captured 2026-05-28 confirmed
             * payload = `{count: u32, objectId: u32}` (NOT the dual-
             * array TLV ENTRY-007 anticipated). Decode the objectId
             * and register it in the per-task active-objects set so
             * op_0x74 can query active membership before doing the
             * heap-VA lookup. With LAGFX_PHASE_B2_CAPTURE=1, the
             * payload is also hex-dumped for further RE.
             *
             * Mislabeled enum kept for backwards-compatibility —
             * 0x25 carries object+placement together, not just
             * placement (command-buffer-format.md:255). */
            LAGFX_LOG("compute: 0x25 CmdSetObjectAndPlacementList ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            capture_object_list_payload_b2(0x25u, hdr);
            if (hdr->payload && hdr->payload_size >= 8u) {
                uint32_t count     = lagfx_le32(hdr->payload + 0);
                uint32_t object_id = lagfx_le32(hdr->payload + 4);
                /* Channel ID was set when the dispatcher entered the
                 * per-channel drain; we resolve task via the current
                 * task association on the channel (or fall back to
                 * task[0] if not yet bound, which matches the
                 * observed early-boot pattern). */
                lagfx_task_entry_t *task = NULL;
                for (uint32_t ti = 0; ti < LAGFX_MAX_TASKS; ti++) {
                    if (p->tasks[ti].live) { task = &p->tasks[ti]; break; }
                }
                if (task) {
                    register_active_object(task, object_id);
                    LAGFX_LOG("compute: 0x25 registered objectId=0x%x (count=%u) in task=%u (active=%u)",
                              object_id, count, task->id,
                              task->active_objects.count);
                    /* M2 CREATEBACK (LAGFX_M2_CREATEBACK): capture the view->backing
                     * relationship at OBJECT-CREATION time, while the backing is still
                     * live. The draw-time BACKREF resolve in compute_inner_ops misses
                     * because the guest EVICTS the backing before the view's draw; the
                     * 0x25 registration is the earliest host-visible signal. Read the
                     * object's descriptor: log its type, and for a type-0x05 VIEW scan
                     * its descriptor words for a backing object that resolves to a real
                     * texture, then persist {backing_obj -> pfn,sz,task} into the same
                     * m2_backing_cache the draw path consults. Falsifiable probe: the
                     * log shows whether render-target views (0x7/0x9/...) pass through
                     * 0x25 and whether their backing matches a SAMPLED ref. Gated;
                     * read-only + cache-populate, no render-path behavior change. */
                    if (getenv("LAGFX_M2_CREATEBACK")) {
                        uint8_t ot = 0; uint64_t ova = 0, ogpa = 0;
                        uint8_t od[64] = {0};
                        if (lagfx_resolve_object_data(p, task, object_id, &ot, &ova, &ogpa)
                            && ova != 0u
                            && lagfx_task_read_virtual(p, task, ova, sizeof(od), od)) {
                            uint64_t e0sz = lagfx_le64(od + 0);
                            uint64_t e0pf = lagfx_le64(od + 8) & 0xffffffffull;
                            LAGFX_LOG("compute: 0x25 CREATEBACK obj=0x%x type=0x%02x "
                                      "e0sz=%llu e0pfn=0x%llx", object_id, ot,
                                      (unsigned long long)e0sz, (unsigned long long)e0pf);
                            if (ot == 0x05u) {
                                for (int w = 0; w < 16; w++) {
                                    uint32_t cand = lagfx_le32(od + (size_t)w * 4u);
                                    if (cand == 0u || cand == object_id || cand > 0xffffu) continue;
                                    uint8_t ct = 0; uint64_t cva = 0, cgpa = 0;
                                    uint8_t cd[64] = {0};
                                    uint64_t cpfn = 0, csz = 0;
                                    if (lagfx_resolve_object_data(p, task, cand, &ct, &cva, &cgpa)
                                        && cva != 0u
                                        && lagfx_task_read_virtual(p, task, cva, sizeof(cd), cd)) {
                                        for (int e = 0; e < 4; e++) {
                                            uint64_t es = lagfx_le64(cd + (size_t)e * 16u);
                                            uint64_t ep = lagfx_le64(cd + (size_t)e * 16u + 8u) & 0xffffffffull;
                                            if (ep < 0x10u || ep > 0xfffffu || es < 4u) continue;
                                            cpfn = ep; csz = es; break;
                                        }
                                        bool is_back = (cpfn != 0u && csz >= 256u
                                            && csz <= 16u * 1024u * 1024u && (csz % 4u) == 0u
                                            && (ct == 0x01u || ct == 0x03u || ct == 0x04u));
                                        LAGFX_LOG("compute: 0x25 CREATEBACK view=0x%x word[%d]=0x%x "
                                                  "-> type=0x%02x pfn0x%llx sz=%llu backing=%d",
                                                  object_id, w, cand, ct,
                                                  (unsigned long long)cpfn,
                                                  (unsigned long long)csz, (int)is_back);
                                        if (is_back) {
                                            bool found = false;
                                            for (uint32_t k = 0; k < p->m2_backing_cache_n; k++)
                                                if (p->m2_backing_cache[k].obj_id == (uint16_t)cand) {
                                                    p->m2_backing_cache[k].pfn = (uint32_t)cpfn;
                                                    p->m2_backing_cache[k].sz = csz;
                                                    p->m2_backing_cache[k].task_id = (uint16_t)task->id;
                                                    p->m2_backing_cache[k].valid = 1u; found = true; break;
                                                }
                                            if (!found && p->m2_backing_cache_n < 64u) {
                                                uint32_t k = p->m2_backing_cache_n++;
                                                p->m2_backing_cache[k].obj_id = (uint16_t)cand;
                                                p->m2_backing_cache[k].pfn = (uint32_t)cpfn;
                                                p->m2_backing_cache[k].sz = csz;
                                                p->m2_backing_cache[k].task_id = (uint16_t)task->id;
                                                p->m2_backing_cache[k].valid = 1u;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                } else {
                    LAGFX_WARN("compute: 0x25 no live task to register objectId=0x%x", object_id);
                }
            }
            break;
        case LAGFX_OP_IOSURFACE_LOOKUP:       /* 0x28 */
            /* Live evidence shows 0x28 on compute channels too — route
             * to the real lookup handler so resource_registry stays
             * in sync with the cross-task IOSurface lifecycle. */
            lagfx_iosurface_lookup(p, hdr);
            break;
        case LAGFX_OP_CHANNEL_EVENT_34:       /* 0x34 */
        case LAGFX_OP_CHANNEL_EVENT_35:       /* 0x35 */
        case LAGFX_OP_CHANNEL_EVENT_36:       /* 0x36 */
            /* ChannelEventMachine-adjacent (kext-internal scheduler
             * pings). Log + ack. 0x37 is handled above as the
             * per-channel CmdExecIndirect2. */
            LAGFX_LOG("compute: channel_event 0x%02x ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)hdr->opcode, (unsigned)p->current_chan_id,
                      hdr->stamp, (unsigned)hdr->payload_size);
            /* M2 RE: 0x34 may be PAGE_BACKING (AppleParavirtResource::pageBacking
             * emits FIFO 0x34 carrying hostMappedBaseVA + a 3-record range
             * descriptor sequence). Capture its full payload to confirm whether
             * the compute-channel 0x34 publishes resource backing GPAs (the
             * missing data-GPA source) or is just a scheduler ping. */
            capture_object_list_payload_b2(hdr->opcode, hdr);
            break;
        case LAGFX_OP_MAP_MEMORY_IMMEDIATE:   /* 0x39 — CmdMapMemoryImmediate */
            /* M2 RE: capture the full 0x39 payload too — it builds the per-task
             * page table and is a candidate for the buffer data-GPA mapping. */
            capture_object_list_payload_b2(0x39u, hdr);
            LAGFX_LOG("compute: 0x39 CmdMapMemoryImmediate ch=%u stamp=0x%08x payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      (unsigned)hdr->payload_size);
            break;

        case LAGFX_OP_VCHAN_REPLACE_PHYSICAL: { /* 0x3c — per-resource replacePhysical on Immediate vchan */
            /* RE: paravirt-re — single kext emit-site at vaddr 0x14564494
             * in AppleParavirtResource::replacePhysical() (vaddr 0x145643f2,
             * mangled __ZN21AppleParavirtResource15replacePhysicalEv).
             * See paravirt-re/classes/AppleParavirtResource.md and the
             * caller annotated/AppleParavirtMemoryMap-commitIntoGPUPageTable.annotated.asm
             * (loop body at 0x1455dc4e calls this for each resource in
             * task[+0xa0] when [chan+0x114] re-binding flag is set).
             *
             * Wire trailer: 8 bytes `{u32 eventID/taskID, u32 counter}` —
             * same emit-helper family as 0x25/0x36 (single
             * apvgpu_cmd_builder_reserve(8) after channel lock, then
             * `[r12]=eventID; [r12+4]=[chan+0x180]=counter`). Total
             * command size = 12 (header) + 8 (trailer) = 20 bytes;
             * matches the live ring delta `(wp - rp) == 0x14` observed
             * in /tmp/lagfx.log compute-drain traces.
             *
             * Semantics: kext notifies host that a per-task Resource has
             * had its host backing pages relocated and any host-side
             * mapping caches should be invalidated. The downstream side-
             * table publishes (two `vt[+0x1b8]` calls at kext 0x14564598 /
             * 0x145645b6) update `[accel_shared+0x380]`'s per-task tracker
             * with the new task[+0x20] / task[+0x270] pointers — these
             * are kext-internal cleanup, not wire data we need to
             * consume.
             *
             * Log+ack correct because: until lagfx allocates real
             * VkImage/VkBuffer per-resource (Stage 30 work; the
             * pre-refactor real-translation paths in dead-code-to-revive/
             * have been ported as parse-and-trace stubs per
             * project_m5_inflight.md), there is no host-side mapping
             * cache to invalidate. The kext does NOT wait on a
             * 0x3c-specific stamp completion — `stamp` is the standard
             * per-channel monotonic that the universal complete_stamp
             * path already raises after dispatch.
             *
             * TODO (Stage 30): when per-resource Vulkan backing lands,
             * look up the resource by `eventID` (== task->id via
             * 0x14563376) and flush any host-side mapping cache. The
             * `counter` field is the per-channel emit counter and is
             * not load-bearing for host behaviour. */
            uint32_t eventID = 0, counter = 0;
            if (hdr->payload && hdr->payload_size >= 8) {
                eventID = lagfx_le32(hdr->payload + 0);
                counter = lagfx_le32(hdr->payload + 4);
            }
            LAGFX_LOG("compute: 0x3c VchanReplacePhysical ch=%u stamp=0x%08x "
                      "eventID=%u counter=%u payload_size=%u",
                      (unsigned)p->current_chan_id, hdr->stamp,
                      eventID, counter, (unsigned)hdr->payload_size);
            break;
        }

        case 0x3b: {
            /* M2c wire RE (2026-07-19): per-ref BACKING/RESIDENCY UPDATE.
             * Payload {u32 seq, u32 ref, u32 =5, u32 =6, u64 addr} (24 B).
             * Live refs 0x13/0x8/0xa/0xc/0xe with addr ~0x226e14. Was dropped
             * entirely before the ring-wrap fix; store latest addr per ref so
             * the resolve paths (gated LAGFX_M2_BACKUPD) can prefer it over
             * the stale placement-table PFN walk. */
            if (hdr->payload && hdr->payload_size >= 24u) {
                uint32_t bseq = lagfx_le32(hdr->payload + 0);
                uint32_t bref = lagfx_le32(hdr->payload + 4);
                uint64_t baddr = lagfx_le64(hdr->payload + 16);
                uint32_t sl = 0; bool found = false;
                for (; sl < p->backing_update_n && sl < 64u; sl++)
                    if (p->backing_update[sl].valid
                        && p->backing_update[sl].ref == bref) { found = true; break; }
                if (!found && p->backing_update_n < 64u)
                    sl = p->backing_update_n++;
                if (sl < 64u) {
                    p->backing_update[sl].ref = bref;
                    p->backing_update[sl].addr = baddr;
                    p->backing_update[sl].valid = 1u;
                }
                LAGFX_LOG("compute: 0x3b BackingUpdate ch=%u seq=%u ref=0x%x addr=0x%llx",
                          (unsigned)p->current_chan_id, bseq, bref,
                          (unsigned long long)baddr);
            }
            break;
        }

        default:
            LAGFX_WARN("compute dispatch: ch=%u unknown opcode 0x%04x stamp=0x%08x",
                       (unsigned)p->current_chan_id, hdr->opcode, hdr->stamp);
            /* M2c wire RE (LAGFX_DUMP_SPV): raw payload hex for unknown opcodes.
             * 0x3b flows now that the ring-wrap drop is fixed and is a suspect
             * for the slab-content upload that would fill the composite vertex
             * buffers we read as text/poison. */
            if (getenv("LAGFX_DUMP_SPV") && hdr->payload && hdr->payload_size) {
                char uh[200]; size_t un = 0;
                uint32_t cap = hdr->payload_size < 64u ? hdr->payload_size : 64u;
                for (uint32_t k = 0; k < cap && un + 3 < sizeof(uh); k++)
                    un += (size_t)snprintf(uh + un, sizeof(uh) - un, "%02x ",
                                           hdr->payload[k]);
                LAGFX_LOG("UNKOP 0x%04x len=%u: %s", hdr->opcode,
                          (unsigned)hdr->payload_size, uh);
            }
            p->unknown_opcode_count++;
            break;
    }
}

/* Drain ring buffer for compute channels. Returns number of commands processed. */
size_t channel_compute_dispatcher_drain(lagfx_protocol_t *p, uint32_t chan_id) {
    if (!lagfx_protocol_is_valid(p)) {
        return 0;
    }

    /* Look up the per-channel ring geometry from the FIFORingDescriptor.
     * Helper lives in ring_common.{c,h}; shared with display dispatcher. */
    uint64_t desc_gpa = 0;
    uint32_t write_ptr = 0;
    uint32_t read_ptr = 0;
    uint64_t page0_gpa = 0;
    if (!lagfx_ring_fifo_descriptor_read(p, chan_id, &desc_gpa, &write_ptr,
                                          &read_ptr, &page0_gpa)) {
        return 0;
    }

    uint32_t ring_size = LAGFX_COMPUTE_RING_SIZE;

    if (write_ptr == read_ptr) {
        LAGFX_TRACE("compute drain: ch=%u caught up rp=0x%x wp=0x%x",
                    chan_id, read_ptr, write_ptr);
        return 0;
    }

    LAGFX_LOG("compute drain: ch=%u desc_gpa=0x%llx page0_gpa=0x%llx "
              "ring_size=0x%x rp=0x%x wp=0x%x",
              chan_id,
              (unsigned long long)desc_gpa,
              (unsigned long long)page0_gpa,
              ring_size, read_ptr, write_ptr);

    lagfx_device_t *dev = (lagfx_device_t *)p->dev;
    if (!dev || !dev->desc.shell.read_memory) {
        LAGFX_WARN("compute drain: no shell.read_memory callback");
        return 0;
    }

    uint32_t rp = read_ptr;
    size_t cmds = 0;
    for (unsigned i = 0; i < LAGFX_COMPUTE_DRAIN_MAX_CMDS; ++i) {
        if (rp == write_ptr) break;

        /* Header read MUST be chunk-resolved: at rp=0xffc the 12-byte header
         * straddles the ring-page boundary and the ring's data pages are NOT
         * physically contiguous — the old flat read pulled the length dword
         * from the wrong physical page → length=0 → "bad length … stop" →
         * every later command in the batch silently dropped. */
        uint8_t hdr_buf[LAGFX_CMD_HEADER_BYTES];
        if (!lagfx_ring_read_bytes(p, page0_gpa, ring_size, rp,
                                    LAGFX_CMD_HEADER_BYTES, hdr_buf)) {
            LAGFX_WARN("compute drain: ch=%u header read failed at rp=0x%x",
                       chan_id, rp);
            break;
        }

        uint16_t opcode       = lagfx_le16(hdr_buf + 0);
        uint16_t arg_count_8b = lagfx_le16(hdr_buf + 2);
        uint32_t length       = lagfx_le32(hdr_buf + 4);
        uint32_t stamp        = lagfx_le32(hdr_buf + 8);

        if (length < LAGFX_CMD_HEADER_BYTES ||
            length > LAGFX_COMPUTE_MAX_CMD_BYTES ||
            length > ring_size) {
            LAGFX_WARN("compute drain: ch=%u bad length 0x%x at rp=0x%x "
                       "opcode=0x%04x first8=%02x%02x%02x%02x%02x%02x%02x%02x — stop",
                       chan_id, length, rp, opcode,
                       hdr_buf[0], hdr_buf[1], hdr_buf[2], hdr_buf[3],
                       hdr_buf[4], hdr_buf[5], hdr_buf[6], hdr_buf[7]);
            break;
        }

        /* Read body. For compute (64 KiB ring) a command can straddle
         * a page boundary within the same ring slot — resolve each
         * 4 KiB chunk independently because page0[i] may map to
         * non-contiguous physical pages.
         *
         * Body lands in the protocol's compute scratch (state.h's
         * "Per-dispatcher scratch buffers"). BQL-serialised. */
        uint8_t *cmd_buf = p->scratch_compute;
        bool body_ok = true;
        uint32_t bytes_read = 0;
        while (bytes_read < length) {
            uint64_t chunk_gpa = 0;
            if (!lagfx_ring_resolve_data_gpa(p, page0_gpa, ring_size,
                                        rp + bytes_read, &chunk_gpa)) {
                body_ok = false;
                break;
            }
            uint32_t chunk_off_in_page = (uint32_t)(chunk_gpa & 0xfffu);
            uint32_t chunk_len = 0x1000u - chunk_off_in_page;
            if (chunk_len > length - bytes_read) {
                chunk_len = length - bytes_read;
            }
            if (!dev->desc.shell.read_memory(dev->desc.shell.opaque,
                                              chunk_gpa, chunk_len,
                                              cmd_buf + bytes_read)) {
                LAGFX_WARN("compute drain: ch=%u body chunk DMA failed",
                           chan_id);
                body_ok = false;
                break;
            }
            bytes_read += chunk_len;
        }
        if (!body_ok) break;

        lagfx_cmd_header_t hdr;
        hdr.opcode       = opcode;
        hdr.arg_count_8b = arg_count_8b;
        hdr.length       = length;
        hdr.stamp        = stamp;
        hdr.payload_size = (uint16_t)((length > LAGFX_CMD_HEADER_BYTES)
                                       ? (length - LAGFX_CMD_HEADER_BYTES)
                                       : 0u);
        hdr.payload      = (hdr.payload_size > 0)
                              ? (cmd_buf + LAGFX_CMD_HEADER_BYTES)
                              : NULL;

        dispatch_command(p, &hdr);

        /* Per-channel stamp slot = chan_id (1..4 → SLOT_COMPUTE_1..4). */
        lagfx_protocol_complete_stamp_slot(p, chan_id, stamp);

        p->total_cmds_seen++;
        cmds++;
        rp = rp + length;  /* absolute, monotonic — see per-channel-ring-pfn-array.md */
    }

    /* Publish the drained position back to the descriptor so the
     * kext's watchdog observes progress (channel-progress-flag-RE.md). */
    lagfx_ring_publish_read_ptr(p, desc_gpa, rp);

    LAGFX_LOG("compute drain: ch=%u drained=%zu new rp=0x%x",
              chan_id, cmds, rp);
    return cmds;
}
