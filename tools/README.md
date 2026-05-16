# Tools

Standalone helpers for lagfx development.

## air_intrinsic_inventory.py

Extracts `@air.*` intrinsic inventory from a .metallib file. Pipes
LLVM bitcode through `llvm-dis` and emits a CSV.

**Dependencies:** Python 3.10+, `llvm-dis` on PATH (install with
`brew install llvm` on macOS, `apt install llvm` on Linux).

**Usage:**
```sh
python3 tools/air_intrinsic_inventory.py path/to/file.metallib --out inventory.csv
```

**Stage 50.5 gate** per `mos/memory/M5_progress_scale_5percent.md`:
the CSV must list every distinct `@air.*` intrinsic with name,
signature, callsite count, first-seen function. Expected inventory
size: ≥ 5, ≤ ~200 per metallib.

**MVP scope (this iteration):** treats the entire bitcode blob as
a single module. Per-function splitting is Stage 50.5 part 3 (needs
a `metallib_dump` C helper that calls `lagfx_metallib_get_bitcode`
per function).
