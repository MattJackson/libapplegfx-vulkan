#!/usr/bin/env python3
"""
ci/assert-vulkan-tests.py — gate the CI build on real Vulkan execution.

A green `meson test` exit code is not enough. The vulkan-* tests are
gated at configure time on `vulkan_dep.found()`; when meson doesn't
detect Vulkan they're NOT REGISTERED, and the workflow still passes
because no tests actually failed. Inside the tests, an open-and-bail
on first `vkEnumeratePhysicalDevices` failure also produces exit 0
with all-SKIP output — a healthier failure but still falsely green.

This script reads `build/meson-logs/testlog.txt` and asserts every
required vulkan-* test entry:
  - Is REGISTERED
  - Exited 0
  - Produced at least one PASS line (didn't self-skip)

Per `feedback_validate_against_real_api.md` — validate against the
real API or it doesn't count.

Usage:
    python3 scripts/ci/assert-vulkan-tests.py <path/to/testlog.txt>
"""
import re
import sys


REQUIRED_TESTS = [
    "vulkan init",
    "vulkan command",
    "vulkan render",
    "translate render",
]


def main(logpath: str) -> int:
    with open(logpath) as f:
        log = f.read()

    blocks = re.split(r"={10,}\s+\d+/\d+\s+={10,}", log)
    info = {}
    for b in blocks:
        # `meson test` testlog format: "test:         <name>" then
        # "result:       <status>". Capture name = everything after the
        # `test:` whitespace, trimmed.
        m = re.search(r"^test:\s+(.+?)\s*$", b, re.M)
        r = re.search(r"^result:\s+(.+?)\s*$", b, re.M)
        if m and r:
            info[m.group(1).strip()] = (r.group(1).strip(), b)

    problems = []
    for name in REQUIRED_TESTS:
        entry = info.get(name)
        if entry is None:
            problems.append(
                f"{name}: NOT REGISTERED — Vulkan dep missing at configure time"
            )
            continue
        status, body = entry
        if status != "exit status 0":
            problems.append(f"{name}: {status}")
            continue
        pass_count = len(re.findall(r"^PASS:", body, re.M))
        skip_count = len(re.findall(r"^SKIP:", body, re.M))
        if pass_count == 0 and skip_count > 0:
            problems.append(
                f"{name}: exit 0 but 0 PASS + {skip_count} SKIP — "
                "test self-skipped (likely no loadable ICD at runtime)"
            )

    if problems:
        print("::error::required vulkan-* tests did not actually execute:")
        for p in problems:
            print(f"  - {p}")
        print("\nAll registered test results:")
        for k, (s, _) in info.items():
            print(f"  {k}: {s}")
        return 1

    print("All required vulkan-* tests executed and passed:")
    for name in REQUIRED_TESTS:
        s, body = info[name]
        pc = len(re.findall(r"^PASS:", body, re.M))
        sc = len(re.findall(r"^SKIP:", body, re.M))
        print(f"  {name}: {s} ({pc} PASS, {sc} SKIP)")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(main(sys.argv[1]))
