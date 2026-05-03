# libapplegfx-vulkan

Clean-room reimplementation of Apple's `ParavirtualizedGraphics.framework`
that decodes the wire protocol from the QEMU paravirt GPU device and
translates Metal commands to Vulkan via lavapipe. The active wedge for
Metal-on-Linux in the **mos** project.

This repo is one of several mos satellites. **Project context, current
state, mental model, build/deploy, and don't-do-this list all live in
the parent project's CLAUDE.md and library:**

- Airport map + working mode + current state: `../mos/CLAUDE.md`
- Onboarding for fresh sessions: `../mos/ONBOARDING-PROMPT.md`
- Standing rules + closed-milestone facts: `../mos/memory/MEMORY.md`
- Wire protocol / class layouts / state machines / decoder tables:
  `../mos/paravirt-re/library/`

## Scope of this repo

The Vulkan-side host of the paravirt protocol:

- `src/protocol/protocol.c` — outer dispatch, doorbell, ring drain
- `src/protocol/opcodes.c` — outer opcode table
- `src/protocol/ops_device.c` — CmdMapMemory / CmdUnmapMemory (vchan)
- `src/protocol/ops_display.c` — display vchan setup, opcode 0x04
- `src/protocol/ops_display_vchan.c` — sub-channel PGFIFO drain, cursor
- `src/protocol/ops_cmdbuf.c` — CmdExecIndirect2 inner parsing
- `src/protocol/render_opcodes.c` — Stage 30 (top 5 done, 90+ remaining)
- `src/protocol/compute_opcodes.c`, `iosurface.c`, `shell.c`

QEMU-side PCI device is in `/Users/mjackson/Developer/qemu-mos15/`.
RE notes are in `/Users/mjackson/Developer/mos/paravirt-re/`.

## Conventions

- **`lagfx_*` in API**, "PG" / ParavirtualizedGraphics in repo text.
  Trademark caution for the binary, discoverability for docs. See
  `../mos/memory/feedback_naming_lagfx_vs_pg.md`.
- **No `Co-Authored-By: Claude` trailers** — strip on sight. See
  `../mos/memory/feedback_no_ai_attribution_in_commits.md`.
- For build/deploy and the unattended M5 working mode, see
  `../mos/CLAUDE.md`.
