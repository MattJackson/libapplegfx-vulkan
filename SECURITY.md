# Security Policy

## Reporting a Vulnerability

**Please do not open public GitHub issues for security problems.**

Report vulnerabilities privately via GitHub Security Advisories:

  https://github.com/MattJackson/libapplegfx-vulkan/security/advisories/new

If GitHub Advisories are unavailable to you, email
`matthew@pq.io` with a subject line starting `[libapplegfx-vulkan
security]` and we will route it to the advisory workflow.

You should receive an acknowledgement within **7 days**. If the
report is confirmed, we will coordinate a fix, a CVE (where
warranted), and a disclosure timeline with you before making any
details public.

## Supported Versions

`libapplegfx-vulkan` is pre-1.0 and actively developed on `main`.
Only the tip of `main` is supported for security fixes at this
time. Tagged pre-release previews (`0.1.0-preview`, etc.) are **not**
long-term-supported — users should rebase onto current `main` to
pick up fixes.

| Version        | Supported        |
|----------------|------------------|
| `main` (HEAD)  | yes              |
| `0.1.x-preview`| no (rebase)      |
| anything else  | no               |

## Threat Model

`libapplegfx-vulkan` is a **host-side** library. At runtime it is
loaded into a **QEMU process** alongside the guest paravirtualized
GPU device (`mos-qemu`'s `apple-gfx-pci-linux`) and consumes
command streams that originate from an **untrusted macOS guest**.

The primary security-relevant attack surface is therefore:

1. **The PVG protocol decoder** (`src/protocol/`). The decoder
   parses guest-authored FIFO command headers, opcode-specific
   payloads, and DMA descriptors. **Input-validation bugs here —
   undersized bounds checks, integer overflows on length/stride
   fields, out-of-range opcode table indexing, untrusted guest
   pointers dereferenced without `lagfx_task_*` translation — are
   the highest-priority class of vulnerability** in this repo.
   Please prioritize these in any audit.
2. **The AIR → SPIR-V translator** (`src/air2spirv/`). Parses
   Apple AIR metallib blobs from the guest; a malformed metallib
   must not be able to escape the parser into out-of-bounds reads,
   writes, or to emit SPIR-V that will escape the lavapipe driver.
3. **Task memory aliasing** (`src/memory/`, the `memfd_create` +
   `mmap(MAP_FIXED)` path). Guest physical address → host virtual
   address translation bugs could, in principle, let a guest read
   or write host memory outside the shared task region. Treat any
   VA/PA translation shortcut with suspicion.
4. **Vulkan command translation** (`src/translate/`). A crafted
   Metal command stream translating to malformed Vulkan command
   buffers could crash or confuse lavapipe/Mesa. Out-of-scope for
   *our* CVE assignment (that's a Mesa issue) but in-scope for
   coordinated disclosure — we'll route upstream.

The library is **not** intended to be exposed to network inputs
directly. If you find a way to reach the decoder from outside the
guest kernel (e.g. through a host-side IPC interface that shouldn't
exist), that is itself a finding.

## What *isn't* in scope

- Vulnerabilities in **Mesa / lavapipe / the Vulkan loader** —
  report those upstream.
- Vulnerabilities in **QEMU's host side** outside our
  `apple-gfx-pci-linux` device — report to the QEMU security list.
- Denial-of-service by a guest that owns itself (the guest can
  already crash its own GPU driver; that's not a privilege
  boundary).

## Hardening guidance for downstream integrators

- Always run QEMU with non-root user, seccomp, and (where
  available) a sandboxed filesystem view.
- Keep `lavapipe` / Mesa up to date; our translation layer
  relies on Mesa validating SPIR-V and command buffers.
- Consider building with `-fsanitize=address,undefined` in CI (we
  do); those builds catch a large share of protocol-decoder bugs
  in pre-production testing.
