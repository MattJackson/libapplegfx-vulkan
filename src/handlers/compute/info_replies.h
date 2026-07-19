/*
 * libapplegfx-vulkan — InfoDecoder reply handlers (inner opcodes 0x1c2..0x1d0)
 * src/handlers/compute/info_replies.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * See info_replies.c for full design rationale and RE refs.
 */

#ifndef LAGFX_HANDLERS_COMPUTE_INFO_REPLIES_H
#define LAGFX_HANDLERS_COMPUTE_INFO_REPLIES_H

#include "../handlers.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Translator callback type — supplied by exec_cmdbuf.c so we can share
 * its local-static per-task radix-tree walker without exposing the
 * non-built protocol/translate.c API. Returns true on success and
 * populates *out_gpa with the GPA for dev_addr. Reply sizes are <= 32B
 * and naturally aligned; we never cross a page in practice. */
typedef bool (*lagfx_info_translate_fn)(lagfx_protocol_t *p,
                                         const lagfx_task_entry_t *task,
                                         uint64_t dev_addr,
                                         uint64_t *out_gpa);

/* Forward declaration of the radix walker — promoted from static in exec_cmdbuf.c. */
bool lagfx_task_translate(lagfx_protocol_t *p,
                          const lagfx_task_entry_t *task,
                          uint64_t dev_addr,
                          uint64_t *out_gpa);

/* Dispatch one inner-PGCmdHeader on the InfoDecoder path.
 *
 *   inner_opcode    = PGCmdHeader.opcode (already masked to low 32 bits
 *                     by the caller; only [0x1c2..0x1d0] reach this).
 *   body            = pointer to payload bytes (after the 8-byte
 *                     PGCmdHeader). May span pages — caller has read up
 *                     to a 4 KiB window.
 *   body_len        = length of the body payload in bytes.
 *   outer_resources = pointer to the outer-payload resource_table base
 *                     (hdr->payload + 12 + 24 * descriptor_count). Each
 *                     entry is 16 bytes: {u64 host_gpu_addr, u32 length,
 *                     u32 pad}. Used to resolve buffer_id → target GPA.
 *   resource_count  = number of entries in outer_resources.
 *   task            = resolved task (NULL = no radix tree available).
 *   translate       = task-VA → GPA translator (must be non-NULL when
 *                     task is non-NULL).
 *
 * The function is observation-friendly: it logs the dispatch line
 * (named opcode) and the reply size, then writes the reply via the
 * shell.write_memory callback. On any translation/range failure it
 * logs LAGFX_WARN and skips the reply (no abort).
 */
void lagfx_info_dispatch(lagfx_protocol_t *p,
                          uint32_t inner_opcode,
                          const uint8_t *body,
                          size_t body_len,
                          const uint8_t *outer_resources,
                          uint32_t resource_count,
                          const lagfx_task_entry_t *task,
                          lagfx_info_translate_fn translate);

#endif /* LAGFX_HANDLERS_COMPUTE_INFO_REPLIES_H */
