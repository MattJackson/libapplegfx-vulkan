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

    @unittest.skipUnless(
        shutil.which("llvm-dis"),
        "llvm-dis not installed"
    )
    def test_per_function_pipeline_with_metallib_dump(self):
        """Test per-function pipeline with metallib_dump --list and --extract.

        Assert CSV has rows where first_seen_function is triangle_vertex OR
        triangle_fragment (not <mvp:whole-module>).
        """
        if not FIXTURE.exists():
            self.skipTest(f"no fixture at {FIXTURE}")

        # Find metallib_dump in builddir or PATH
        dump_path = shutil.which("metallib_dump")
        if dump_path is None:
            relative_path = REPO_ROOT / "builddir" / "tools" / "metallib_dump"
            if relative_path.exists():
                dump_path = str(relative_path)

        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as f:
            out_path = f.name
        try:
            r = run([str(FIXTURE), "--out", out_path, "--metallib-dump", dump_path])
            # Exit 0 means per-function pipeline succeeded
            self.assertEqual(r.returncode, 0, f"stderr={r.stderr}, stdout={r.stdout}")

            with open(out_path, newline="") as f:
                reader = csv.DictReader(f)
                rows = list(reader)
                first_seen_funcs = set(row["first_seen_function"] for row in rows if row.get("first_seen_function"))

                # Should have actual function names, not <mvp:whole-module>
                self.assertNotIn("<mvp:whole-module>", first_seen_funcs,
                                 "Expected per-function pipeline to use actual function names")
                self.assertTrue(any(fn in ("triangle_vertex", "triangle_fragment")
                                   for fn in first_seen_funcs),
                            f"Expected triangle_vertex or triangle_fragment in {first_seen_funcs}")
        finally:
            if os.path.exists(out_path):
                os.unlink(out_path)

    @unittest.skipUnless(shutil.which("llvm-dis"), "llvm-dis not installed")
    def test_per_function_pipeline_fallback(self):
        """Test fallback to whole-module pipeline when metallib_dump is missing.

        Point at a metallib with --metallib-dump /nonexistent/path. Tool should fall back
        to whole-module pipeline with a stderr WARN. Assert tool exits 0 or 2 (llvm-dis
        missing acceptable) AND stderr contains "fallback" or "whole-module".
        """
        if not FIXTURE.exists():
            self.skipTest(f"no fixture at {FIXTURE}")

        with tempfile.NamedTemporaryFile(suffix=".csv", delete=False) as f:
            out_path = f.name
        try:
            r = run([str(FIXTURE), "--out", out_path, "--metallib-dump", "/nonexistent/path"])
            # Should exit 0 (success via whole-module fallback) or 2 (llvm-dis missing)
            self.assertIn(r.returncode, (0, 2), f"Expected 0 or 2, got {r.returncode}")

            # Should have stderr warning about fallback
            self.assertTrue(
                "fallback" in r.stderr.lower() or "whole-module" in r.stderr.lower(),
                f"Expected 'fallback' or 'whole-module' in stderr: {r.stderr}"
            )
        finally:
            if os.path.exists(out_path):
                os.unlink(out_path)


if __name__ == "__main__":
    unittest.main()
