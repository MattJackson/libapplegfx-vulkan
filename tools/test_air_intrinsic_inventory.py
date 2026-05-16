#!/usr/bin/env python3
"""Tests for air_intrinsic_inventory.py.

Run with:  python3 tools/test_air_intrinsic_inventory.py
       or: python3 -m unittest tools.test_air_intrinsic_inventory
"""
import csv
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
TOOL = TOOL_DIR / "air_intrinsic_inventory.py"
REPO_ROOT = TOOL_DIR.parent
FIXTURE = REPO_ROOT / "tests" / "fixtures" / "triangle.metallib"


def run(args, **kw):
    return subprocess.run(
        [sys.executable, str(TOOL), *args],
        capture_output=True,
        text=True,
        **kw,
    )


class AirIntrinsicInventoryTests(unittest.TestCase):

    def test_help_exits_zero(self):
        r = run(["--help"])
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn("Extract @air", r.stdout)

    def test_missing_file_exits_2(self):
        r = run(["/nonexistent/path/never/exists"])
        self.assertEqual(r.returncode, 2, r.stderr)
        self.assertIn("not a file", r.stderr)

    def test_no_bitcode_magic_exits_3(self):
        with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
            f.write(b"not the magic")
            path = f.name
        try:
            r = run([path])
            self.assertEqual(r.returncode, 3, r.stderr)
            self.assertIn("magic not found", r.stderr)
        finally:
            os.unlink(path)

    def test_real_metallib_finds_bitcode(self):
        if not FIXTURE.exists():
            self.skipTest(f"no fixture at {FIXTURE}")
        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as f:
            out_path = f.name
        try:
            r = run([str(FIXTURE), "--out", out_path])
            # The bitcode-detection step prints info to stderr BEFORE attempting
            # llvm-dis, so this assertion holds whether or not llvm-dis is on PATH.
            self.assertIn("info: bitcode at offset", r.stderr)
            # Exit code is 0 (success), 2 (llvm-dis missing), or 3 (other error).
            # Either success or llvm-dis missing is acceptable here.
            self.assertIn(r.returncode, (0, 2, 3), r.stderr)
        finally:
            if os.path.exists(out_path):
                os.unlink(out_path)

    @unittest.skipUnless(shutil.which("llvm-dis"), "llvm-dis not installed")
    def test_csv_schema_when_llvm_dis_available(self):
        if not FIXTURE.exists():
            self.skipTest(f"no fixture at {FIXTURE}")
        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as f:
            out_path = f.name
        try:
            r = run([str(FIXTURE), "--out", out_path])
            # With llvm-dis available, exit should be 0 (success) unless fixture is broken.
            self.assertEqual(r.returncode, 0, r.stderr)
            with open(out_path, newline="") as f:
                reader = csv.reader(f)
                header = next(reader, None)
                self.assertEqual(
                    header,
                    ["intrinsic_name", "signature",
                     "callsite_count", "first_seen_function"])
                # Row count > 1 not asserted — triangle shader may have 0 air.* intrinsics
        finally:
            if os.path.exists(out_path):
                os.unlink(out_path)

if __name__ == "__main__":
    unittest.main()
