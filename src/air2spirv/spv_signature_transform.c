/*
 * libapplegfx-vulkan — SPV signature transform (Phase 3.C.2 M5)
 * src/air2spirv/spv_signature_transform.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * Implementation of spv_signature_transform.h. See that header's
 * extended doc-comment for the algorithm and scope.
 *
 * ------------------------------------------------------------------
 * Structural notes — Apple LLVM SPIR-V backend output shape
 * ------------------------------------------------------------------
 *
 * The vertex triangle produced by llc -mtriple=spirv-unknown-vulkan1.3
 * looks like (excerpt from examples/triangle/out/triangle_vertex.raw.spv.dis):
 *
 *     OpCapability Shader
 *     OpCapability Int64           ; kept
 *     OpCapability Int8            ; kept
 *     OpCapability Linkage         ; DROP
 *     OpMemoryModel Logical GLSL450
 *     OpSource Unknown 0
 *     OpName %triangle_vertex "triangle_vertex"
 *     OpDecorate %_struct_3 CPacked                          ; DROP
 *     OpDecorate %triangle_vertex LinkageAttributes ... Export ; DROP
 *     %float    = OpTypeFloat 32
 *     %v4float  = OpTypeVector %float 4
 *     %_struct_3 = OpTypeStruct %v4float
 *     %uint     = OpTypeInt 32 0
 *     %15       = OpTypeFunction %_struct_3 %uint     ; old fn type
 *     ...constants...
 *     %triangle_vertex = OpFunction %_struct_3 Pure %15
 *     %30 = OpFunctionParameter %uint
 *     %43 = OpLabel
 *         ...body...
 *         %42 = OpCompositeInsert %_struct_3 %40 %28 0
 *               OpReturnValue %42
 *               OpFunctionEnd
 *
 * The fragment case is simpler: return type is %v4float directly, no
 * function params, body is `%10 = OpLabel; OpReturnValue %const`.
 *
 * Our rewrite re-creates the module in-order while splicing fresh
 * instructions at well-defined landmarks (post-OpMemoryModel,
 * pre-OpFunction, post-OpLabel, at OpReturnValue). All synthesized
 * result-ids come from a fresh-id allocator seeded by the incoming
 * module's Bound field.
 */

#include "spv_signature_transform.h"
#include "common/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* ---- SPIR-V opcode / enum constants ----------------------------------- */

enum {
    SPV_OP_SOURCE            = 3,
    SPV_OP_SOURCE_EXTENSION  = 4,
    SPV_OP_NAME              = 5,
    SPV_OP_MEMBER_NAME       = 6,
    SPV_OP_STRING            = 7,
    SPV_OP_LINE              = 8,
    SPV_OP_EXTENSION         = 10,
    SPV_OP_EXT_INST_IMPORT   = 11,
    SPV_OP_MEMORY_MODEL      = 14,
    SPV_OP_ENTRY_POINT       = 15,
    SPV_OP_EXECUTION_MODE    = 16,
    SPV_OP_CAPABILITY        = 17,
    SPV_OP_TYPE_VOID         = 19,
    SPV_OP_TYPE_BOOL         = 20,
    SPV_OP_TYPE_INT          = 21,
    SPV_OP_TYPE_FLOAT        = 22,
    SPV_OP_TYPE_VECTOR       = 23,
    SPV_OP_TYPE_ARRAY        = 28,
    SPV_OP_TYPE_STRUCT       = 30,
    SPV_OP_TYPE_POINTER      = 32,
    SPV_OP_TYPE_FUNCTION     = 33,
    SPV_OP_CONSTANT          = 43,
    SPV_OP_CONSTANT_COMPOSITE = 44,
    SPV_OP_UNDEF             = 1,
    SPV_OP_SPEC_CONSTANT_OP  = 52,
    SPV_OP_FUNCTION          = 54,
    SPV_OP_FUNCTION_PARAMETER = 55,
    SPV_OP_FUNCTION_END      = 56,
    SPV_OP_VARIABLE          = 59,
    SPV_OP_LOAD              = 61,
    SPV_OP_STORE             = 62,
    SPV_OP_COMPOSITE_EXTRACT = 81,
    SPV_OP_DECORATE          = 71,
    SPV_OP_MEMBER_DECORATE   = 72,
    SPV_OP_BITCAST           = 124,
    SPV_OP_ACCESS_CHAIN      = 65,
    SPV_OP_LABEL             = 248,
    SPV_OP_RETURN            = 253,
    SPV_OP_RETURN_VALUE      = 254,
    SPV_OP_LIFETIME_START    = 256,
    SPV_OP_LIFETIME_STOP     = 257,
};

enum {
    SPV_DEC_BUILTIN             = 11,
    SPV_DEC_LOCATION            = 30,
    SPV_DEC_CPACKED             = 10,
    SPV_DEC_LINKAGE_ATTRIBUTES  = 41,
};

enum {
    SPV_BUILTIN_POSITION    = 0,
    SPV_BUILTIN_VERTEX_INDEX = 42,
};

enum {
    SPV_STORAGE_INPUT  = 1,
    SPV_STORAGE_OUTPUT = 3,
    SPV_STORAGE_FUNCTION = 7,
};

enum {
    SPV_CAP_SHADER  = 1,
    SPV_CAP_LINKAGE = 5,
    SPV_CAP_KERNEL  = 6,
};

enum {
    SPV_EXEC_MODE_ORIGIN_UPPER_LEFT = 7,
};

/* ---- Tiny helpers ----------------------------------------------------- */

static uint32_t spv_pack_insn(uint32_t word_count, uint32_t opcode) {
    return (word_count << 16) | (opcode & 0xFFFFu);
}

static void spv_unpack_insn(uint32_t w, uint32_t *word_count, uint32_t *opcode) {
    *word_count = (w >> 16) & 0xFFFFu;
    *opcode     = w & 0xFFFFu;
}

static bool spv_has_magic(const uint8_t *buf, size_t buf_len) {
    if (!buf || buf_len < 4u) return false;
    return buf[0] == 0x03u && buf[1] == 0x02u
        && buf[2] == 0x23u && buf[3] == 0x07u;
}

/* Compare a SPIR-V NUL-terminated string against a C string. */
static bool spv_str_eq(const uint32_t *words, size_t max_words,
                       const char *s) {
    const uint8_t *bytes = (const uint8_t *)words;
    size_t max_bytes = max_words * 4u;
    size_t i = 0;
    while (i < max_bytes) {
        uint8_t b = bytes[i];
        char    c = s[i];
        if (b == 0u) return c == '\0';
        if (c == '\0') return false;
        if (b != (uint8_t)c) return false;
        i++;
    }
    return false;
}

/* Emit raw words. */
static void emit_words(uint32_t *dst, size_t *idx,
                       const uint32_t *words, size_t count) {
    for (size_t i = 0; i < count; ++i) dst[*idx + i] = words[i];
    *idx += count;
}

static size_t spv_pack_string(const char *s, uint32_t *dst) {
    size_t len = strlen(s);
    size_t bytes = len + 1u;
    size_t padded = (bytes + 3u) & ~(size_t)3u;
    size_t words = padded / 4u;
    if (words > 0) dst[words - 1u] = 0u;
    uint8_t *out = (uint8_t *)dst;
    for (size_t i = 0; i < len; ++i) out[i] = (uint8_t)s[i];
    out[len] = 0u;
    return words;
}

/* ---- Scan-pass scratch state ----------------------------------------- */

#define MAX_PARAMS 8
#define MAX_STRUCT_MEMBERS 8
#define MAX_POINTER_TYPES 64
#define MAX_LIFETIME_OPERANDS 16

typedef struct {
    /* Entry function. */
    uint32_t fn_id;           /* OpFunction's result id */
    bool     found_fn;

    /* Function type — one OpTypeFunction instruction. */
    uint32_t fn_type_id;
    uint32_t fn_ret_type_id;

    /* Return type structural info. */
    bool     ret_is_struct;
    uint32_t ret_struct_type_id; /* == fn_ret_type_id if struct */
    size_t   ret_member_count;
    uint32_t ret_member_types[MAX_STRUCT_MEMBERS];

    /* Parameters — from OpTypeFunction. */
    size_t   param_count;
    uint32_t param_types[MAX_PARAMS];

    /* Pre-existing type ids we need references to. These are scanned
     * from the module; 0 means "not found, must synthesize". */
    uint32_t void_id;
    uint32_t uint_type_id;     /* OpTypeInt 32 0 */
    uint32_t uint_zero_id;     /* OpConstant %uint 0, if present */

    /* All OpTypePointer result-ids. Lets us detect OpBitcast whose
     * target type is a pointer (which is illegal in Logical
     * addressing). Bounded set; overflow is logged but non-fatal — in
     * practice Apple's blobs have <10 distinct pointer types. */
    size_t   pointer_type_count;
    uint32_t pointer_type_ids[MAX_POINTER_TYPES];

    /* Operand ids of OpLifetimeStart / OpLifetimeStop we plan to
     * strip. Any OpBitcast whose result-id appears here is only alive
     * to feed the (now-dropped) lifetime op, so we drop it too. */
    size_t   lifetime_operand_count;
    uint32_t lifetime_operand_ids[MAX_LIFETIME_OPERANDS];

    /* Bound from header — anything >= this is a fresh id. */
    uint32_t bound;

    /* OpMemoryModel position — splice landmark. */
    bool     has_shader_cap;

    /* How many OpReturnValue instructions appear inside the entry fn
     * (we assume one, but log if more). */
    size_t   return_value_count;
} scan_state_t;

/* ---- Scan pass -------------------------------------------------------- */

/* Walk the module once to locate the entry function, its signature,
 * and well-known type ids. Also validates basic instruction framing. */
static lagfx_status_t scan_module(const uint32_t *in_words,
                                  size_t in_word_count,
                                  const char *entry_point_name,
                                  scan_state_t *s) {
    memset(s, 0, sizeof(*s));
    s->bound = in_words[3];

    /* Pass 1a: find the function id corresponding to entry_point_name
     * by matching OpName's literal string. */
    size_t i = 5u;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (wc == 0u || i + wc > in_word_count) {
            LAGFX_ERR("sig_xform: malformed at word %zu (wc=%u op=%u)",
                      i, wc, op);
            return LAGFX_ERR_PROTOCOL;
        }
        if (op == SPV_OP_CAPABILITY
            && wc >= 2u && in_words[i + 1u] == SPV_CAP_SHADER) {
            s->has_shader_cap = true;
        }
        if (op == SPV_OP_NAME && wc >= 3u) {
            uint32_t target = in_words[i + 1u];
            size_t str_words = wc - 2u;
            if (spv_str_eq(&in_words[i + 2u], str_words,
                           entry_point_name)) {
                s->fn_id = target;
                s->found_fn = true;
            }
        }
        if (op == SPV_OP_TYPE_VOID && wc >= 2u) {
            s->void_id = in_words[i + 1u];
        }
        if (op == SPV_OP_TYPE_INT && wc >= 4u
            && in_words[i + 2u] == 32u && in_words[i + 3u] == 0u) {
            /* OpTypeInt 32 0 (unsigned) */
            s->uint_type_id = in_words[i + 1u];
        }
        if (op == SPV_OP_TYPE_POINTER && wc >= 2u) {
            if (s->pointer_type_count < MAX_POINTER_TYPES) {
                s->pointer_type_ids[s->pointer_type_count++] =
                    in_words[i + 1u];
            }
        }
        if (op == SPV_OP_CONSTANT && wc >= 4u
            && s->uint_type_id != 0u
            && in_words[i + 1u] == s->uint_type_id
            && in_words[i + 3u] == 0u
            && s->uint_zero_id == 0u) {
            /* OpConstant %uint 0 */
            s->uint_zero_id = in_words[i + 2u];
        }
        if ((op == SPV_OP_LIFETIME_START || op == SPV_OP_LIFETIME_STOP)
            && wc >= 2u
            && s->lifetime_operand_count < MAX_LIFETIME_OPERANDS) {
            s->lifetime_operand_ids[s->lifetime_operand_count++] =
                in_words[i + 1u];
        }
        i += wc;
    }

    if (!s->found_fn) {
        LAGFX_ERR("sig_xform: no OpName match for '%s'", entry_point_name);
        return LAGFX_ERR_INVALID_ARG;
    }

    /* Pass 1b: find the OpFunction for that id; extract return type,
     * function-type id. Then find the OpTypeFunction for that fn type
     * to get the parameter type list. */
    i = 5u;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (op == SPV_OP_FUNCTION && wc >= 5u
            && in_words[i + 2u] == s->fn_id) {
            s->fn_ret_type_id = in_words[i + 1u];
            s->fn_type_id     = in_words[i + 4u];
            break;
        }
        i += wc;
    }
    if (s->fn_ret_type_id == 0u || s->fn_type_id == 0u) {
        LAGFX_ERR("sig_xform: OpFunction for id %u not found",
                  s->fn_id);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Pass 1c: resolve OpTypeFunction operand list. */
    i = 5u;
    bool found_fn_type = false;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (op == SPV_OP_TYPE_FUNCTION && wc >= 3u
            && in_words[i + 1u] == s->fn_type_id) {
            /* word[2] = return type, word[3..] = param types */
            /* (we already have return type from OpFunction, but the
             *  two should agree; verify.) */
            if (in_words[i + 2u] != s->fn_ret_type_id) {
                LAGFX_ERR("sig_xform: OpTypeFunction ret mismatch "
                          "(%u vs %u)",
                          in_words[i + 2u], s->fn_ret_type_id);
                return LAGFX_ERR_PROTOCOL;
            }
            s->param_count = wc - 3u;
            if (s->param_count > MAX_PARAMS) {
                LAGFX_ERR("sig_xform: too many params (%zu > %d)",
                          s->param_count, MAX_PARAMS);
                return LAGFX_ERR_PROTOCOL;
            }
            for (size_t k = 0; k < s->param_count; ++k) {
                s->param_types[k] = in_words[i + 3u + k];
            }
            found_fn_type = true;
            break;
        }
        i += wc;
    }
    if (!found_fn_type) {
        LAGFX_ERR("sig_xform: OpTypeFunction %u not found",
                  s->fn_type_id);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Pass 1d: resolve return type — is it a struct? If so, extract
     * the member types. */
    i = 5u;
    bool found_ret_type = false;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (wc >= 2u && in_words[i + 1u] == s->fn_ret_type_id) {
            if (op == SPV_OP_TYPE_STRUCT) {
                s->ret_is_struct = true;
                s->ret_struct_type_id = s->fn_ret_type_id;
                s->ret_member_count = wc - 2u;
                if (s->ret_member_count > MAX_STRUCT_MEMBERS) {
                    LAGFX_ERR("sig_xform: struct has too many members "
                              "(%zu > %d)", s->ret_member_count,
                              MAX_STRUCT_MEMBERS);
                    return LAGFX_ERR_PROTOCOL;
                }
                for (size_t k = 0; k < s->ret_member_count; ++k) {
                    s->ret_member_types[k] = in_words[i + 2u + k];
                }
                found_ret_type = true;
                break;
            } else if (op == SPV_OP_TYPE_VECTOR
                    || op == SPV_OP_TYPE_FLOAT
                    || op == SPV_OP_TYPE_INT) {
                s->ret_is_struct = false;
                s->ret_member_count = 1u;
                s->ret_member_types[0] = s->fn_ret_type_id;
                found_ret_type = true;
                break;
            }
        }
        i += wc;
    }
    if (!found_ret_type) {
        LAGFX_ERR("sig_xform: return type id %u is not struct/vec/scalar",
                  s->fn_ret_type_id);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Count OpReturnValue inside the entry function (diagnostic). */
    i = 5u;
    bool in_target_fn = false;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (op == SPV_OP_FUNCTION && wc >= 3u
            && in_words[i + 2u] == s->fn_id) {
            in_target_fn = true;
        } else if (op == SPV_OP_FUNCTION_END) {
            in_target_fn = false;
        } else if (in_target_fn && op == SPV_OP_RETURN_VALUE) {
            s->return_value_count++;
        }
        i += wc;
    }

    return LAGFX_OK;
}

static bool is_pointer_type_id(const scan_state_t *s, uint32_t id) {
    for (size_t k = 0; k < s->pointer_type_count; ++k) {
        if (s->pointer_type_ids[k] == id) return true;
    }
    return false;
}

static bool is_lifetime_operand_id(const scan_state_t *s, uint32_t id) {
    for (size_t k = 0; k < s->lifetime_operand_count; ++k) {
        if (s->lifetime_operand_ids[k] == id) return true;
    }
    return false;
}

/* ---- Fresh-id allocator ---------------------------------------------- */

typedef struct {
    uint32_t next;
} fresh_ids_t;

static uint32_t fresh(fresh_ids_t *f) {
    return f->next++;
}

/* ---- Emit helpers ---------------------------------------------------- */

static void emit_capability(uint32_t *out, size_t *idx, uint32_t cap) {
    uint32_t w[2];
    w[0] = spv_pack_insn(2, SPV_OP_CAPABILITY);
    w[1] = cap;
    emit_words(out, idx, w, 2);
}

static void emit_decorate_builtin(uint32_t *out, size_t *idx,
                                  uint32_t target, uint32_t builtin) {
    uint32_t w[4];
    w[0] = spv_pack_insn(4, SPV_OP_DECORATE);
    w[1] = target;
    w[2] = SPV_DEC_BUILTIN;
    w[3] = builtin;
    emit_words(out, idx, w, 4);
}

static void emit_decorate_location(uint32_t *out, size_t *idx,
                                   uint32_t target, uint32_t loc) {
    uint32_t w[4];
    w[0] = spv_pack_insn(4, SPV_OP_DECORATE);
    w[1] = target;
    w[2] = SPV_DEC_LOCATION;
    w[3] = loc;
    emit_words(out, idx, w, 4);
}

static void emit_type_void(uint32_t *out, size_t *idx, uint32_t id) {
    uint32_t w[2];
    w[0] = spv_pack_insn(2, SPV_OP_TYPE_VOID);
    w[1] = id;
    emit_words(out, idx, w, 2);
}

static void emit_type_function_void(uint32_t *out, size_t *idx,
                                    uint32_t id, uint32_t void_id) {
    uint32_t w[3];
    w[0] = spv_pack_insn(3, SPV_OP_TYPE_FUNCTION);
    w[1] = id;
    w[2] = void_id;
    emit_words(out, idx, w, 3);
}

static void emit_type_pointer(uint32_t *out, size_t *idx,
                              uint32_t id, uint32_t storage,
                              uint32_t pointee) {
    uint32_t w[4];
    w[0] = spv_pack_insn(4, SPV_OP_TYPE_POINTER);
    w[1] = id;
    w[2] = storage;
    w[3] = pointee;
    emit_words(out, idx, w, 4);
}

static void emit_variable(uint32_t *out, size_t *idx,
                          uint32_t ptr_type_id, uint32_t result_id,
                          uint32_t storage) {
    uint32_t w[4];
    w[0] = spv_pack_insn(4, SPV_OP_VARIABLE);
    w[1] = ptr_type_id;
    w[2] = result_id;
    w[3] = storage;
    emit_words(out, idx, w, 4);
}

static void emit_load(uint32_t *out, size_t *idx,
                      uint32_t result_type, uint32_t result_id,
                      uint32_t ptr) {
    uint32_t w[4];
    w[0] = spv_pack_insn(4, SPV_OP_LOAD);
    w[1] = result_type;
    w[2] = result_id;
    w[3] = ptr;
    emit_words(out, idx, w, 4);
}

static void emit_store(uint32_t *out, size_t *idx,
                       uint32_t ptr, uint32_t value) {
    uint32_t w[3];
    w[0] = spv_pack_insn(3, SPV_OP_STORE);
    w[1] = ptr;
    w[2] = value;
    emit_words(out, idx, w, 3);
}

static void emit_composite_extract(uint32_t *out, size_t *idx,
                                   uint32_t result_type,
                                   uint32_t result_id,
                                   uint32_t composite,
                                   uint32_t member_index) {
    uint32_t w[5];
    w[0] = spv_pack_insn(5, SPV_OP_COMPOSITE_EXTRACT);
    w[1] = result_type;
    w[2] = result_id;
    w[3] = composite;
    w[4] = member_index;
    emit_words(out, idx, w, 5);
}

static void emit_return(uint32_t *out, size_t *idx) {
    uint32_t w[1];
    w[0] = spv_pack_insn(1, SPV_OP_RETURN);
    emit_words(out, idx, w, 1);
}

/* ---- Synthesized id layout ------------------------------------------- */

/* A collection of fresh ids we allocate up front and reference from
 * multiple emit sites. Each is >= scan_state.bound. */
typedef struct {
    uint32_t void_id;            /* OpTypeVoid (may reuse pre-existing) */
    uint32_t void_fn_type_id;    /* OpTypeFunction %void */

    /* Per-output-member: pointer type + variable. Index matches
     * scan_state.ret_member_types[]. */
    uint32_t out_ptr_type[MAX_STRUCT_MEMBERS];
    uint32_t out_var_id [MAX_STRUCT_MEMBERS];

    /* Per-input-param: pointer type + variable. */
    uint32_t in_ptr_type[MAX_PARAMS];
    uint32_t in_var_id  [MAX_PARAMS];

    /* uint type + constant zero used by the OpBitcast-to-AccessChain
     * rewrite below. Reuses pre-existing ids when available. */
    uint32_t uint_type_id;
    uint32_t uint_zero_id;
} synth_ids_t;

static void plan_synth_ids(const scan_state_t *s, fresh_ids_t *f,
                           synth_ids_t *ids) {
    memset(ids, 0, sizeof(*ids));
    if (s->void_id != 0u) {
        ids->void_id = s->void_id;
    } else {
        ids->void_id = fresh(f);
    }
    ids->void_fn_type_id = fresh(f);
    for (size_t k = 0; k < s->ret_member_count; ++k) {
        ids->out_ptr_type[k] = fresh(f);
        ids->out_var_id [k] = fresh(f);
    }
    for (size_t k = 0; k < s->param_count; ++k) {
        ids->in_ptr_type[k] = fresh(f);
        ids->in_var_id  [k] = fresh(f);
    }
    /* uint / uint-0 are reused if found during scan. If not, we'll
     * allocate fresh ids here; they're only emitted when we actually
     * need them (i.e. when we synthesize the types-section splice). */
    if (s->uint_type_id != 0u) {
        ids->uint_type_id = s->uint_type_id;
    } else {
        ids->uint_type_id = fresh(f);
    }
    if (s->uint_zero_id != 0u) {
        ids->uint_zero_id = s->uint_zero_id;
    } else {
        ids->uint_zero_id = fresh(f);
    }
}

/* ---- OpEntryPoint emission with interface list ----------------------- */

static void emit_entry_point(uint32_t *out, size_t *idx,
                             lagfx_spv_stage_t stage,
                             uint32_t fn_id,
                             const char *name,
                             const uint32_t *iface_ids,
                             size_t iface_count) {
    size_t name_words = (strlen(name) + 1u + 3u) / 4u;
    size_t total_words = 3u + name_words + iface_count;
    uint32_t *w = &out[*idx];
    w[0] = spv_pack_insn((uint32_t)total_words, SPV_OP_ENTRY_POINT);
    w[1] = (uint32_t)stage;
    w[2] = fn_id;
    (void)spv_pack_string(name, &w[3]);
    for (size_t k = 0; k < iface_count; ++k) {
        w[3 + name_words + k] = iface_ids[k];
    }
    *idx += total_words;
}

static void emit_execution_mode_origin_upper_left(uint32_t *out,
                                                  size_t *idx,
                                                  uint32_t fn_id) {
    uint32_t w[3];
    w[0] = spv_pack_insn(3, SPV_OP_EXECUTION_MODE);
    w[1] = fn_id;
    w[2] = SPV_EXEC_MODE_ORIGIN_UPPER_LEFT;
    emit_words(out, idx, w, 3);
}

/* ---- Decoration planning --------------------------------------------- */

/* Decide what BuiltIn / Location to assign to each synthesized output
 * and input variable based on stage. For Apple's `[[position]]` and
 * `[[color(N)]]` attributes — which the LLVM backend does NOT carry
 * through to the SPIR-V as decorations — we make stage-appropriate
 * assumptions:
 *
 *   Vertex stage, return is struct-of-N: assume member 0 is Position
 *   and remaining members are Location 0..N-2 varyings.
 *   Vertex stage, return is bare v4float: BuiltIn Position (rare).
 *
 *   Fragment stage, return is struct-of-N: each member is Location N.
 *   Fragment stage, return is bare v4float: Location 0 (the common
 *   `fragment float4 f()` case in our triangle.metal).
 *
 * Input params, vertex stage: first uint param is VertexIndex. Others
 * are left undecorated (FIXME(phase-3c3-attr-plumbing)).
 *
 * Input params, fragment stage: no params in triangle.metal, left
 * undecorated if any appear. */
static void emit_output_decorations(uint32_t *out, size_t *idx,
                                    lagfx_spv_stage_t stage,
                                    const scan_state_t *s,
                                    const synth_ids_t *ids) {
    for (size_t k = 0; k < s->ret_member_count; ++k) {
        if (stage == LAGFX_SPV_STAGE_VERTEX && k == 0) {
            emit_decorate_builtin(out, idx, ids->out_var_id[k],
                                  SPV_BUILTIN_POSITION);
        } else if (stage == LAGFX_SPV_STAGE_VERTEX) {
            /* Additional vertex outputs -> varyings at Location k-1. */
            emit_decorate_location(out, idx, ids->out_var_id[k],
                                   (uint32_t)(k - 1u));
        } else {
            /* Fragment: Location k (0-based). */
            emit_decorate_location(out, idx, ids->out_var_id[k],
                                   (uint32_t)k);
        }
    }
}

static void emit_input_decorations(uint32_t *out, size_t *idx,
                                   lagfx_spv_stage_t stage,
                                   const scan_state_t *s,
                                   const synth_ids_t *ids) {
    for (size_t k = 0; k < s->param_count; ++k) {
        if (stage == LAGFX_SPV_STAGE_VERTEX && k == 0) {
            emit_decorate_builtin(out, idx, ids->in_var_id[k],
                                  SPV_BUILTIN_VERTEX_INDEX);
        } else {
            /* Fall back to Location k. Will need refinement when
             * real vertex attributes land (FIXME phase-3c3). */
            emit_decorate_location(out, idx, ids->in_var_id[k],
                                   (uint32_t)k);
        }
    }
}

/* ---- Main transform -------------------------------------------------- */

lagfx_status_t lagfx_spv_signature_transform(
    const uint8_t *in_buf,
    size_t in_len,
    const char *entry_point_name,
    lagfx_spv_stage_t stage,
    uint8_t **out_buf,
    size_t *out_len)
{
    if (!in_buf || !entry_point_name || !*entry_point_name
        || !out_buf || !out_len) {
        return LAGFX_ERR_INVALID_ARG;
    }
    *out_buf = NULL;
    *out_len = 0;

    if (!spv_has_magic(in_buf, in_len)) {
        LAGFX_ERR("sig_xform: missing SPIR-V magic (in_len=%zu)", in_len);
        return LAGFX_ERR_INVALID_ARG;
    }
    if (in_len < 5u * 4u || (in_len % 4u) != 0u) {
        LAGFX_ERR("sig_xform: truncated (in_len=%zu)", in_len);
        return LAGFX_ERR_PROTOCOL;
    }

    const uint32_t *in_words = (const uint32_t *)in_buf;
    size_t in_word_count = in_len / 4u;

    /* ---- Scan -------------------------------------------------------- */
    scan_state_t s;
    lagfx_status_t rc = scan_module(in_words, in_word_count,
                                    entry_point_name, &s);
    if (rc != LAGFX_OK) return rc;

    /* ---- Plan fresh ids --------------------------------------------- */
    fresh_ids_t fids = { .next = s.bound };
    synth_ids_t ids;
    plan_synth_ids(&s, &fids, &ids);
    uint32_t new_bound = fids.next;

    /* Allocate a generous output buffer. Upper bound: input size +
     * injected instructions. The biggest category is per-param and
     * per-member injections; each param contributes:
     *   - OpTypePointer Input        4 words
     *   - OpVariable Input           4 words
     *   - OpDecorate                 4 words
     *   - OpLoad inside function     4 words
     *   - interface list operand     1 word
     * so ~17 words per param. Similarly ~15 words per output member
     * (includes OpCompositeExtract + OpStore in the body). Add fixed
     * overhead (OpTypeVoid, OpTypeFunction %void, OpCapability,
     * OpEntryPoint name, OpExecutionMode, OpReturn) ≈ 64 words. Pad
     * generously. */
    size_t slack = 256u
        + 32u * (size_t)(MAX_PARAMS + MAX_STRUCT_MEMBERS)
        + ((strlen(entry_point_name) + 4u) / 4u);
    size_t max_out_words = in_word_count + slack;
    uint32_t *out = (uint32_t *)malloc(max_out_words * 4u);
    if (!out) return LAGFX_ERR_OUT_OF_MEMORY;

    size_t out_idx = 0;

    /* ---- Header ------------------------------------------------------ */
    out[0] = in_words[0];                 /* magic */
    out[1] = in_words[1];                 /* version */
    out[2] = in_words[2];                 /* generator */
    out[3] = new_bound;                   /* updated bound */
    out[4] = in_words[4];                 /* schema (0) */
    out_idx = 5u;

    /* ---- Build interface list for OpEntryPoint ----------------------- */
    uint32_t iface[MAX_STRUCT_MEMBERS + MAX_PARAMS];
    size_t iface_count = 0;
    for (size_t k = 0; k < s.ret_member_count; ++k) {
        iface[iface_count++] = ids.out_var_id[k];
    }
    for (size_t k = 0; k < s.param_count; ++k) {
        iface[iface_count++] = ids.in_var_id[k];
    }

    /* ---- Pass 2: emit ------------------------------------------------ */
    /*
     * We split the emit into three "zones" based on module layout:
     *   zone A: capabilities, extensions, imports, memory model  (≤ op 17)
     *   zone B: entry-point / execution-mode / source / names / decorations
     *           (this is the logical "pre-types" region — SPIR-V §2.4)
     *   zone C: everything from first OpType* onwards, ending with
     *           OpFunctionEnd of the last function.
     *
     * Hooks:
     *   - Just after OpMemoryModel: inject OpCapability Shader (if
     *     absent), OpEntryPoint, OpExecutionMode. These must appear
     *     in the right layout section per spec §2.4.
     *   - After zone B (first type encountered), inject our new
     *     decorations (BuiltIn Position / VertexIndex / Location N).
     *     In practice we emit them right before the first type op.
     *   - Right before the first OpFunction (entry function in our
     *     blobs): inject OpTypeVoid (if needed), OpTypeFunction %void,
     *     OpTypePointer Input/Output types, OpVariable declarations.
     *   - Inside the entry OpFunction:
     *       - rewrite the OpFunction header (ret type, fn type,
     *         FunctionControl=0)
     *       - drop OpFunctionParameter
     *       - after the first OpLabel, emit OpLoad for each param
     *         (hijacking original param result-ids)
     *       - replace OpReturnValue with OpCompositeExtract(s) +
     *         OpStore(s) + OpReturn
     */

    bool injected_zone_a = false;        /* after OpMemoryModel */
    bool injected_pre_types = false;     /* before first OpType* */
    bool injected_pre_function = false;  /* before first OpFunction */
    bool in_target_fn = false;
    bool emitted_post_label_loads = false;

    size_t i = 5u;
    while (i < in_word_count) {
        uint32_t wc = 0u, op = 0u;
        spv_unpack_insn(in_words[i], &wc, &op);
        if (wc == 0u || i + wc > in_word_count) {
            LAGFX_ERR("sig_xform: malformed insn at word %zu", i);
            free(out);
            return LAGFX_ERR_PROTOCOL;
        }

        /* ---- Drop-list filtering ------------------------------------- */
        bool drop = false;
        if (op == SPV_OP_CAPABILITY && wc >= 2u) {
            uint32_t cap = in_words[i + 1u];
            if (cap == SPV_CAP_LINKAGE || cap == SPV_CAP_KERNEL) drop = true;
        } else if (op == SPV_OP_DECORATE && wc >= 3u) {
            uint32_t dec = in_words[i + 2u];
            if (dec == SPV_DEC_CPACKED || dec == SPV_DEC_LINKAGE_ATTRIBUTES)
                drop = true;
        } else if (op == SPV_OP_MEMBER_DECORATE && wc >= 4u) {
            uint32_t dec = in_words[i + 3u];
            if (dec == SPV_DEC_CPACKED || dec == SPV_DEC_LINKAGE_ATTRIBUTES)
                drop = true;
        } else if (op == SPV_OP_FUNCTION_PARAMETER
                   && in_target_fn) {
            /* Dropped — replaced by OpLoad after OpLabel. */
            drop = true;
        } else if (op == SPV_OP_LIFETIME_START
                || op == SPV_OP_LIFETIME_STOP) {
            /* Kernel-capability-only opcodes. LLVM's SPIR-V backend
             * emits these to annotate alloca lifetimes; they are
             * illegal under the Shader capability so we strip them. */
            drop = true;
        }

        /* Replace OpSpecConstantOp with opcode Bitcast (Kernel-only
         * under Shader capability) with an equivalent OpUndef. In the
         * Apple vertex blob the sole use is
         *   %26 = OpSpecConstantOp %float Bitcast %24   (%24 is OpUndef)
         * whose result is dead-end-consumed by a vector shuffle that
         * never reads those lanes. OpUndef preserves semantics without
         * pulling in the Kernel capability. */
        if (op == SPV_OP_SPEC_CONSTANT_OP && wc >= 4u) {
            uint32_t spec_op = in_words[i + 3u];
            /* Bitcast = 124 per SPIR-V spec §3.32.11 "Conversion". */
            if (spec_op == 124u) {
                uint32_t w[3];
                w[0] = spv_pack_insn(3, SPV_OP_UNDEF);
                w[1] = in_words[i + 1u];  /* result type */
                w[2] = in_words[i + 2u];  /* result id */
                emit_words(out, &out_idx, w, 3);
                i += wc;
                continue;
            }
        }

        /* Pointer-to-pointer OpBitcast is illegal under Logical
         * addressing (spirv-val: "OpStore Pointer … is not a logical
         * pointer"). LLVM's SPIR-V backend emits it when lowering MSL
         * / C `T[]` initializers: it allocates `_ptr_Function_array_N`
         * and then bitcasts to `_ptr_Function_element` for the first
         * store. Rewrite any such bitcast as
         *     %id = OpAccessChain <tgt_ptr> <src_ptr> %uint_0
         * which, for a pointer-to-array source, yields a pointer to
         * element 0 of identical runtime address. Bitcasts whose
         * results are consumed only by OpLifetimeStart/Stop (which
         * we also strip above) end up dead but valid.
         *
         * We detect "target type is a pointer" by looking up the
         * bitcast's result-type-id in the pointer-type set gathered
         * during the scan pass. */
        if (op == SPV_OP_BITCAST && wc >= 4u
            && is_pointer_type_id(&s, in_words[i + 1u])) {
            uint32_t result_id = in_words[i + 2u];
            if (is_lifetime_operand_id(&s, result_id)) {
                /* Dead bitcast — its sole consumer was an
                 * OpLifetimeStart/Stop that we've already dropped.
                 * Dropping the bitcast avoids its type-problematic
                 * pointer conversion (e.g. array-of-v2f* -> uchar*
                 * which doesn't cleanly lower to OpAccessChain). */
                i += wc;
                continue;
            }
            uint32_t w[5];
            w[0] = spv_pack_insn(5, SPV_OP_ACCESS_CHAIN);
            w[1] = in_words[i + 1u]; /* target pointer type */
            w[2] = result_id;        /* result id (unchanged) */
            w[3] = in_words[i + 3u]; /* source pointer */
            w[4] = ids.uint_zero_id; /* zero index -> element 0 */
            emit_words(out, &out_idx, w, 5);
            i += wc;
            continue;
        }

        /* ---- Zone A → zone B splice (after OpMemoryModel) ------------ */
        /* We emit the current instruction if it's the memory model,
         * then inject our metadata right after. */
        if (!injected_zone_a && op == SPV_OP_MEMORY_MODEL) {
            emit_words(out, &out_idx, &in_words[i], wc);
            if (!s.has_shader_cap) {
                /* Non-canonical (spec wants cap before MemoryModel)
                 * but Mesa-tolerated; Apple blobs always carry it. */
                emit_capability(out, &out_idx, SPV_CAP_SHADER);
                LAGFX_WARN("sig_xform: injected OpCapability Shader "
                           "after OpMemoryModel");
            }
            emit_entry_point(out, &out_idx, stage, s.fn_id,
                             entry_point_name, iface, iface_count);
            if (stage == LAGFX_SPV_STAGE_FRAGMENT) {
                emit_execution_mode_origin_upper_left(out, &out_idx,
                                                     s.fn_id);
            }
            injected_zone_a = true;
            i += wc;
            continue;
        }

        /* ---- Zone B → zone C splice (before first OpType*) ----------- */
        /* Spec §2.4: decorations come before types. Inject our
         * decorations just before the first type opcode. */
        bool is_type_op = (op >= SPV_OP_TYPE_VOID && op <= SPV_OP_TYPE_FUNCTION)
                          || op == SPV_OP_TYPE_POINTER;
        if (!injected_pre_types && is_type_op) {
            emit_output_decorations(out, &out_idx, stage, &s, &ids);
            emit_input_decorations (out, &out_idx, stage, &s, &ids);
            injected_pre_types = true;
        }

        /* ---- Pre-function splice: inject synthesized types + vars --- */
        if (!injected_pre_function && op == SPV_OP_FUNCTION) {
            /* OpTypeVoid — only if the module didn't already have one. */
            if (s.void_id == 0u) {
                emit_type_void(out, &out_idx, ids.void_id);
            }
            /* Synthesize uint / uint-0 only if the module didn't
             * already have them — the OpBitcast→OpAccessChain rewrite
             * below needs a zero index. */
            if (s.uint_type_id == 0u) {
                uint32_t w[4];
                w[0] = spv_pack_insn(4, SPV_OP_TYPE_INT);
                w[1] = ids.uint_type_id;
                w[2] = 32u;   /* width */
                w[3] = 0u;    /* unsigned */
                emit_words(out, &out_idx, w, 4);
            }
            if (s.uint_zero_id == 0u) {
                uint32_t w[4];
                w[0] = spv_pack_insn(4, SPV_OP_CONSTANT);
                w[1] = ids.uint_type_id;
                w[2] = ids.uint_zero_id;
                w[3] = 0u;
                emit_words(out, &out_idx, w, 4);
            }
            emit_type_function_void(out, &out_idx, ids.void_fn_type_id,
                                    ids.void_id);
            /* Output pointers + variables. */
            for (size_t k = 0; k < s.ret_member_count; ++k) {
                emit_type_pointer(out, &out_idx,
                                  ids.out_ptr_type[k],
                                  SPV_STORAGE_OUTPUT,
                                  s.ret_member_types[k]);
                emit_variable(out, &out_idx,
                              ids.out_ptr_type[k],
                              ids.out_var_id[k],
                              SPV_STORAGE_OUTPUT);
            }
            /* Input pointers + variables. */
            for (size_t k = 0; k < s.param_count; ++k) {
                emit_type_pointer(out, &out_idx,
                                  ids.in_ptr_type[k],
                                  SPV_STORAGE_INPUT,
                                  s.param_types[k]);
                emit_variable(out, &out_idx,
                              ids.in_ptr_type[k],
                              ids.in_var_id[k],
                              SPV_STORAGE_INPUT);
            }
            injected_pre_function = true;
        }

        /* ---- Intercept OpFunction for the entry function ------------- */
        if (op == SPV_OP_FUNCTION && wc >= 5u
            && in_words[i + 2u] == s.fn_id) {
            /* Emit rewritten OpFunction: ret=void, control=0,
             * type=void_fn_type. */
            uint32_t w[5];
            w[0] = spv_pack_insn(5, SPV_OP_FUNCTION);
            w[1] = ids.void_id;
            w[2] = s.fn_id;
            w[3] = 0u;                   /* FunctionControl = None */
            w[4] = ids.void_fn_type_id;
            emit_words(out, &out_idx, w, 5);
            in_target_fn = true;
            emitted_post_label_loads = false;
            i += wc;
            continue;
        }

        if (op == SPV_OP_FUNCTION_END) {
            in_target_fn = false;
        }

        /* ---- Inside the target function: several special cases ------ */
        if (in_target_fn) {
            /* Drop OpFunctionParameter (already flagged above). */
            if (drop) {
                i += wc;
                continue;
            }
            if (op == SPV_OP_LABEL) {
                /* Emit OpLabel as-is. The OpLoad-for-params splice is
                 * deferred to AFTER the function-scope OpVariable
                 * instructions — SPIR-V §2.4 requires all Function-
                 * storage OpVariable ops to be the first instructions
                 * of the entry block, before any other compute ops. */
                emit_words(out, &out_idx, &in_words[i], wc);
                i += wc;
                continue;
            }
            if (!emitted_post_label_loads
                && op != SPV_OP_VARIABLE
                && op != SPV_OP_LABEL) {
                /* We've reached the first non-Variable op in the
                 * entry block — splice in OpLoads now, before emitting
                 * the current instruction. Parameter result-ids come
                 * from walking the original module between OpFunction
                 * and the first OpLabel. */
                size_t fp_scan = 5u;
                while (fp_scan < in_word_count) {
                    uint32_t swc = 0u, sop = 0u;
                    spv_unpack_insn(in_words[fp_scan], &swc, &sop);
                    if (sop == SPV_OP_FUNCTION && swc >= 3u
                        && in_words[fp_scan + 2u] == s.fn_id) {
                        fp_scan += swc;
                        break;
                    }
                    fp_scan += swc;
                }
                size_t p = 0;
                while (fp_scan < in_word_count && p < s.param_count) {
                    uint32_t swc = 0u, sop = 0u;
                    spv_unpack_insn(in_words[fp_scan], &swc, &sop);
                    if (sop == SPV_OP_FUNCTION_PARAMETER && swc >= 3u) {
                        uint32_t orig_param_id = in_words[fp_scan + 2u];
                        emit_load(out, &out_idx,
                                  s.param_types[p],
                                  orig_param_id,
                                  ids.in_var_id[p]);
                        p++;
                        fp_scan += swc;
                    } else {
                        break;
                    }
                }
                emitted_post_label_loads = true;
                /* fall through to normal emission for the current op */
            }
            if (op == SPV_OP_RETURN_VALUE && wc >= 2u) {
                uint32_t retval = in_words[i + 1u];
                if (s.ret_is_struct) {
                    /* OpCompositeExtract per member, then OpStore. */
                    for (size_t k = 0; k < s.ret_member_count; ++k) {
                        uint32_t extracted = fresh(&fids);
                        /* We grew fids beyond new_bound. We'll fix up
                         * the Bound field at the end. */
                        emit_composite_extract(out, &out_idx,
                                               s.ret_member_types[k],
                                               extracted,
                                               retval,
                                               (uint32_t)k);
                        emit_store(out, &out_idx,
                                   ids.out_var_id[k],
                                   extracted);
                    }
                } else {
                    /* Direct store for bare v4float. */
                    emit_store(out, &out_idx,
                               ids.out_var_id[0], retval);
                }
                emit_return(out, &out_idx);
                i += wc;
                continue;
            }
        }

        /* ---- Default emit -------------------------------------------- */
        if (!drop) {
            emit_words(out, &out_idx, &in_words[i], wc);
        }
        i += wc;
    }

    if (!injected_zone_a) {
        LAGFX_ERR("sig_xform: OpMemoryModel not seen — unsupported input");
        free(out);
        return LAGFX_ERR_PROTOCOL;
    }

    /* Fix up Bound — the OpReturnValue rewrite may have allocated
     * extra OpCompositeExtract result-ids beyond the original plan. */
    out[3] = fids.next;

    LAGFX_LOG("sig_xform: entry='%s' stage=%s fn_id=%u "
              "ret_is_struct=%d members=%zu params=%zu "
              "bound: %u -> %u returns=%zu in_words=%zu out_words=%zu",
              entry_point_name,
              stage == LAGFX_SPV_STAGE_VERTEX ? "vertex" : "fragment",
              s.fn_id,
              (int)s.ret_is_struct,
              s.ret_member_count, s.param_count,
              s.bound, fids.next,
              s.return_value_count,
              in_word_count, out_idx);

    *out_buf = (uint8_t *)out;
    *out_len = out_idx * 4u;
    return LAGFX_OK;
}
