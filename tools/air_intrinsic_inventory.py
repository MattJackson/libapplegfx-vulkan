#!/usr/bin/env python3
"""air_intrinsic_inventory.py — extract @air.* intrinsic inventory
from a .metallib bitcode blob.

For each function bitcode in the metallib, pipe through llvm-dis,
grep for @air.* callsites, accumulate counts, emit CSV.

MVP scope: locate LLVM bitcode magic BC C0 DE in the metallib and
treat everything from that offset onward as a single bitcode
module. Per-function splitting is Stage 50.5 part 3 (needs a
metallib_dump helper).

Stage 50.5 gate per memory/M5_progress_scale_5percent.md:
emits scratch/air_intrinsics.csv with rows
(intrinsic_name, signature, callsite_count, first_seen_function)
sorted by callsite_count descending.
"""
import argparse
import csv
import os
import re
import subprocess
import sys
from collections import OrderedDict

# LLVM bitcode magic: 0x42 ('B'), 0x43 ('C'), 0xC0, 0xDE
LLVM_MAGIC = b"\x42\x43\xc0\xde"

INTRINSIC_RE = re.compile(r"call\s+\S+\s+@(air\.[A-Za-z0-9_\.]+)\s*\(([^)]*)\)")
DECLARE_RE   = re.compile(r"declare\s+(\S+)\s+@(air\.[A-Za-z0-9_\.]+)\s*\(([^)]*)\)")


def find_bitcode_start(data: bytes) -> int | None:
    idx = data.find(LLVM_MAGIC)
    return idx if idx >= 0 else None


def llvm_dis_text(bitcode: bytes) -> str:
    """Pipe bitcode through `llvm-dis -` to get textual LLVM-IR."""
    try:
        proc = subprocess.run(
            ["llvm-dis", "-", "-o", "-"],
            input=bitcode,
            capture_output=True,
            check=False,
        )
    except FileNotFoundError:
        print("error: llvm-dis not on PATH. Install with `brew install llvm` "
              "(macOS) or `apt install llvm` (Linux). See tools/README.md.",
              file=sys.stderr)
        sys.exit(2)
    if proc.returncode != 0:
        print(f"error: llvm-dis failed (rc={proc.returncode}): "
              f"{proc.stderr.decode(errors='replace')[:500]}",
              file=sys.stderr)
        sys.exit(1)
    return proc.stdout.decode("utf-8", errors="replace")


def scan_ir(ir_text: str, function_label: str,
            counts: OrderedDict[str, dict]) -> None:
    """Update counts in-place from IR text."""
    # First pass: pick up signatures from declare lines.
    for m in DECLARE_RE.finditer(ir_text):
        ret_ty, name, args = m.group(1), m.group(2), m.group(3)
        sig = f"({args}) -> {ret_ty}"
        entry = counts.setdefault(name, {
            "signature": sig,
            "callsite_count": 0,
            "first_seen_function": function_label,
        })
        if not entry["signature"]:
            entry["signature"] = sig
    # Second pass: count call sites.
    for m in INTRINSIC_RE.finditer(ir_text):
        name = m.group(1)
        entry = counts.setdefault(name, {
            "signature": "",
            "callsite_count": 0,
            "first_seen_function": function_label,
        })
        entry["callsite_count"] += 1


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Extract @air.* intrinsic inventory from .metallib")
    ap.add_argument("metallib", help="path to .metallib file")
    ap.add_argument("--out", default="air_intrinsics.csv",
                    help="output CSV (default: air_intrinsics.csv)")
    args = ap.parse_args()

    if not os.path.isfile(args.metallib):
        print(f"error: not a file: {args.metallib}", file=sys.stderr)
        return 2

    with open(args.metallib, "rb") as f:
        blob = f.read()

    start = find_bitcode_start(blob)
    if start is None:
        print(f"error: LLVM bitcode magic not found in {args.metallib}",
              file=sys.stderr)
        return 3

    print(f"info: bitcode at offset 0x{start:x}, {len(blob)-start} bytes",
          file=sys.stderr)

    ir = llvm_dis_text(blob[start:])
    counts: OrderedDict[str, dict] = OrderedDict()
    scan_ir(ir, function_label="<mvp:whole-module>", counts=counts)

    rows = sorted(counts.items(), key=lambda kv: -kv[1]["callsite_count"])
    with open(args.out, "w", newline="") as out:
        w = csv.writer(out)
        w.writerow(["intrinsic_name", "signature",
                    "callsite_count", "first_seen_function"])
        for name, e in rows:
            w.writerow([name, e["signature"],
                        e["callsite_count"], e["first_seen_function"]])

    print(f"info: wrote {len(rows)} intrinsics to {args.out}",
          file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
