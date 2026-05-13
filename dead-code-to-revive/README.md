# dead-code-to-revive/

Pre-refactor code that's NOT in any current `meson.build` but contains
material we'll need for upcoming stages (M5 stages 25/30+, M6, etc.).

Files here are **not compiled**. They're a forwarding address for
future agents who need to port a specific opcode or handler back into
the live tree. **Do not import these into meson without first checking
struct/signature compatibility against current `src/protocol/state.h`**
— the dispatcher refactor (`b8d1166`) renamed several state-machine
types (e.g. `lagfx_display_entry_t` → `lagfx_display_mode_t`) and
restructured per-task storage. Naive re-add will fail to compile.

The reference architecture is documented in:
- `~/Developer/mos/memory/reference_lagfx_mmio_handler.md`
- `~/Developer/mos/paravirt-re/library/README.md`
- `~/Developer/mos/paravirt-re/library/PROTOCOL.md`

## Inventory

See `protocol/README.md` and per-file `.README.md` for details.
