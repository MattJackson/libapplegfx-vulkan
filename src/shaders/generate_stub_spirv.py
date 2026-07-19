#!/usr/bin/env python3
# libapplegfx-vulkan — stub SPIR-V generator (Phase 3.C scaffold)
# src/shaders/generate_stub_spirv.py
#
# Copyright © 2026 Matthew Jackson
# SPDX-License-Identifier: MIT
#
# Emits minimal-but-valid SPIR-V "passthrough" modules for each
# (shader, stage) pair in the Phase 3.C catalog. The generated
# .spv files satisfy two constraints:
#
#   1. Begin with the SPIR-V magic word 0x07230203 (little-endian)
#      so tests/shader-catalog.c's CHECK passes.
#   2. Are structurally valid enough that vkCreateShaderModule
#      does not reject them (one OpCapability, OpMemoryModel,
#      OpEntryPoint with the correct ExecutionModel, OpExecutionMode
#      where required, and an empty main function).
#
# These are PLACEHOLDERS. Phase 3.C.2 replaces them with real
# glslangValidator output against the GLSL twins. The script runs
# at build time from src/shaders/meson.build when glslangValidator
# is NOT found on PATH; when glslangValidator IS found, meson
# prefers it.
#
# See paravirt-re/shader-catalog-plan.md §8 for the dual-author
# MSL + GLSL pipeline this script belongs to.

import os
import struct
import sys

# SPIR-V magic + version (1.0) + generator (0) + bound (varies)
# + schema (0). Version layout: 0x00MMmmNN where MM=major, mm=minor.
SPIRV_MAGIC      = 0x07230203
SPIRV_VERSION_10 = 0x00010000
SPIRV_GENERATOR  = 0x00000000
SPIRV_SCHEMA     = 0x00000000

# Opcode encoding: top 16 bits = word count (including opcode word),
# bottom 16 bits = opcode.
def ins(op, *operands):
    words = [op] + list(operands)
    header = (len(words) << 16) | (op & 0xFFFF)
    return [header] + list(operands)

# SPIR-V opcodes we need.
OP_NOP              = 0
OP_SOURCE           = 3
OP_NAME             = 5
OP_MEMBER_NAME      = 6
OP_EXT_INST_IMPORT  = 11
OP_MEMORY_MODEL     = 14
OP_ENTRY_POINT      = 15
OP_EXECUTION_MODE   = 16
OP_CAPABILITY       = 17
OP_TYPE_VOID        = 19
OP_TYPE_FUNCTION    = 33
OP_FUNCTION         = 54
OP_FUNCTION_END     = 56
OP_LABEL            = 248
OP_RETURN           = 253

# Capability constants.
CAP_SHADER = 1

# Addressing + memory model.
ADDR_LOGICAL = 0
MEM_GLSL450  = 1

# Execution models.
EXEC_VERTEX    = 0
EXEC_FRAGMENT  = 4

# Execution-mode ids used here.
EXEC_MODE_ORIGIN_UPPER_LEFT = 7

# String → words (null-terminated, padded to 4 bytes).
def str_to_words(s):
    b = s.encode('utf-8') + b'\x00'
    pad = (-len(b)) & 3
    b += b'\x00' * pad
    return list(struct.unpack('<' + 'I' * (len(b) // 4), b))

def build_module(exec_model, entry_name='main'):
    """Build a minimal passthrough SPIR-V module for the given
    execution model. Returns a bytes object. IDs are assigned in
    order:
        %1 = OpTypeVoid
        %2 = OpTypeFunction %1
        %3 = OpFunction %1 None %2 (main)
        %4 = OpLabel
    Bound = 5 (max ID + 1)."""
    words = []
    # OpCapability Shader.
    words += ins(OP_CAPABILITY, CAP_SHADER)
    # OpMemoryModel Logical GLSL450.
    words += ins(OP_MEMORY_MODEL, ADDR_LOGICAL, MEM_GLSL450)
    # OpEntryPoint <model> %3 "main" (no interface IDs).
    entry_name_words = str_to_words(entry_name)
    words += ins(OP_ENTRY_POINT, exec_model, 3, *entry_name_words)
    # OpExecutionMode required for fragment (OriginUpperLeft).
    if exec_model == EXEC_FRAGMENT:
        words += ins(OP_EXECUTION_MODE, 3, EXEC_MODE_ORIGIN_UPPER_LEFT)
    # Type declarations: void, function-of-void-returning-void.
    words += ins(OP_TYPE_VOID, 1)
    words += ins(OP_TYPE_FUNCTION, 2, 1)
    # Function %3 = main.
    words += ins(OP_FUNCTION, 1, 3, 0, 2)  # result type, id, control, type
    words += ins(OP_LABEL, 4)
    words += ins(OP_RETURN)
    words += ins(OP_FUNCTION_END)

    bound = 5
    header = [
        SPIRV_MAGIC,
        SPIRV_VERSION_10,
        SPIRV_GENERATOR,
        bound,
        SPIRV_SCHEMA,
    ]
    all_words = header + words
    return struct.pack('<' + 'I' * len(all_words), *all_words)

def main(argv):
    if len(argv) < 3:
        print('usage: generate_stub_spirv.py <outdir> <name>...',
              file=sys.stderr)
        return 1
    outdir = argv[1]
    names  = argv[2:]
    os.makedirs(outdir, exist_ok=True)
    for name in names:
        vert = build_module(EXEC_VERTEX)
        frag = build_module(EXEC_FRAGMENT)
        with open(os.path.join(outdir, name + '.vert.spv'), 'wb') as f:
            f.write(vert)
        with open(os.path.join(outdir, name + '.frag.spv'), 'wb') as f:
            f.write(frag)
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
