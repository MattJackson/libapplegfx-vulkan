# src/air2spirv/meson.build — runtime AIR→SPIR-V exploratory scaffold
#                              (Phase 3.C.2)
#
# Called from src/meson.build via subdir('air2spirv'). Appends the
# two translation-helper TUs to lagfx_sources. No external dependencies
# at this phase: the container parser + bitcode triple rewriter are
# both hand-rolled against the documented MTLB and LLVM Bitcode layouts
# (see src/air2spirv/README.md §"Format references"). LLVM proper is
# out of scope — the caller invokes `llc -mtriple=spirv64-...` against
# the emitted bitcode blob.
#
# See paravirt-re/shader-translation-research-2026-04-20.md for the
# background (R3 downgrade + pipeline shape) and
# paravirt-re/shader-llvm-spirv-poc-runbook.md for the end-to-end
# proof-of-concept this scaffold is staged to eventually drive.

lagfx_sources += files(
  'metallib_extract.c',
  'bitcode_retarget.c',
  'spv_entrypoint_rewrite.c',
  'spv_signature_transform.c',
  'shader_translate.c',
)
