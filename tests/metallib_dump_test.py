#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""metallib_dump unit tests.

Tests the metallib_dump CLI tool against triangle.metallib fixture.
Test cases:
1. --list exits 0, stdout has >= 1 line per function
2. --extract triangle_vertex exits 0, stdout >= 100 bytes starting with BC C0 DE
3. --extract bogus_name exits 4, stderr contains "not found"
4. Bad usage (no args or unknown flag) exits 2
5. Bad metallib (/dev/null or empty file) exits 3

NOTE: metallib_reader MVP currently has a known bug where function names
are parsed with a leading character stripped (e.g., "riangle_vertex").
Tests are adjusted to be permissive on cases that depend on correct name
parsing. See mos/paravirt-re/library/stage50-metallib-reader-scoping.md.
"""

import subprocess
import sys
import tempfile
import os

METALLIB_DUMP = sys.argv[1] if len(sys.argv) > 1 else "builddir/tools/metallib_dump"
FIXTURE = "tests/fixtures/triangle.metallib"


def run_cmd(args, cwd=None):
    """Run metallib_dump with args, return (returncode, stdout, stderr)."""
    cmd = [METALLIB_DUMP] + args
    result = subprocess.run(
        cmd, capture_output=True, text=False, cwd=cwd
    )
    return result.returncode, result.stdout, result.stderr


def test_list():
    """--list exits 0 and lists triangle_vertex + triangle_fragment."""
    rc, stdout, stderr = run_cmd([FIXTURE, "--list"])

    if rc != 0:
        print(f"FAIL: --list returned {rc}, expected 0")
        print(f"stderr: {stderr.decode('utf-8', errors='replace')}")
        return False

    lines = [l for l in stdout.strip().split(b"\n") if l]
    if len(lines) != 2:
        print(f"FAIL: --list returned {len(lines)} lines, expected exactly 2")
        return False

    names = []
    for line in lines:
        parts = line.split(b"\t")
        if len(parts) != 2:
            print(f"FAIL: malformed list output: {line}")
            return False
        name, _type = parts
        names.append(name.decode("utf-8"))

    if names != ["triangle_vertex", "triangle_fragment"]:
        print(f"FAIL: expected ['triangle_vertex', 'triangle_fragment'], got {names}")
        return False

    print(f"PASS: --list found both expected functions")
    return True


def test_extract():
    """--extract triangle_vertex exits 0 with bitcode bytes starting BC C0 DE."""
    rc, stdout, stderr = run_cmd([FIXTURE, "--extract", "triangle_vertex"])
    if rc != 0:
        print(f"FAIL: --extract returned {rc}, expected 0")
        print(f"stderr: {stderr.decode('utf-8', errors='replace')}")
        return False
    if len(stdout) < 100:
        print(f"FAIL: bitcode too short ({len(stdout)} bytes)")
        return False
    if stdout[:4] != bytes([0x42, 0x43, 0xC0, 0xDE]):
        print(f"FAIL: bitcode magic mismatch, got {stdout[:4].hex()}")
        return False
    print(f"PASS: --extract triangle_vertex returned {len(stdout)} bytes with valid magic")
    return True


def test_extract_not_found():
    """--extract bogus_name exits 4 with 'not found' in stderr."""
    rc, stdout, stderr = run_cmd([FIXTURE, "--extract", "bogus_name_that_does_not_exist"])
    
    if rc != 4:
        print(f"FAIL: --extract bogus returned {rc}, expected 4")
        return False
    
    stderr_str = stderr.decode("utf-8", errors="replace").lower()
    if "not found" not in stderr_str:
        print(f"FAIL: stderr missing 'not found': {stderr}")
        return False
    
    print("PASS: --extract bogus returned 4 with 'not found'")
    return True


def test_bad_usage():
    """Bad usage (no args or unknown flag) exits 2."""
    # No args
    rc, stdout, stderr = run_cmd([])
    if rc != 2:
        print(f"FAIL: no args returned {rc}, expected 2")
        return False
    
    # Unknown flag
    rc, stdout, stderr = run_cmd([FIXTURE, "--unknown-flag"])
    if rc != 2:
        print(f"FAIL: unknown flag returned {rc}, expected 2")
        return False
    
    print("PASS: bad usage returns 2")
    return True


def test_bad_metallib():
    """Bad metallib (/dev/null or empty file) exits 3."""
    # /dev/null
    rc, stdout, stderr = run_cmd(["/dev/null", "--list"])
    if rc != 3:
        print(f"FAIL: /dev/null returned {rc}, expected 3")
        return False
    
    # Create empty file
    with tempfile.NamedTemporaryFile(delete=False) as f:
        tmp_path = f.name
    
    try:
        rc, stdout, stderr = run_cmd([tmp_path, "--list"])
        if rc != 3:
            print(f"FAIL: empty file returned {rc}, expected 3")
            return False
        
        print("PASS: bad metallib returns 3")
        return True
    finally:
        os.unlink(tmp_path)


def main():
    tests = [
        test_list,
        test_extract,
        test_extract_not_found,
        test_bad_usage,
        test_bad_metallib,
    ]
    
    failed = 0
    for test in tests:
        try:
            if not test():
                failed += 1
        except Exception as e:
            print(f"FAIL: {test.__name__} raised {e}")
            failed += 1
    
    if failed > 0:
        print(f"\n{failed}/{len(tests)} tests failed")
        sys.exit(1)
    
    print(f"\nAll {len(tests)} tests passed")
    sys.exit(0)


if __name__ == "__main__":
    main()
