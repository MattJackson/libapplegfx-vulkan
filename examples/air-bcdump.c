/*
 * libapplegfx-vulkan — air-bcdump
 * examples/air-bcdump.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Standalone dump tool: loads any .air.bc file via lagfx_air_module_open
 * and prints a bcanalyzer-like structural summary, plus the decoded
 * FUNCTION_BLOCK instruction stream for every non-prototype function.
 *
 * Intended uses:
 *   - debugging the clean-room Phase 2 decoder against real shaders
 *   - feeding paravirt-re/library/diag/air_reader_diff.py
 *   - producing human-readable AIR for the project journey log
 *
 * Build:  meson compile -C builddir
 * Run:    builddir/examples/air-bcdump /tmp/scoping/triangle_vertex.air.bc
 */

#include "air/bitcode_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t *slurp(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (r != (size_t)sz) { free(buf); return NULL; }
    *out_len = (size_t)sz;
    return buf;
}

/* Map the LAGFX_AIR_INST_* enum to its mnemonic. Keep aligned with the
 * record codes commented after each label so it doubles as a lookup
 * table for cross-referencing bcanalyzer's --dump output. */
static const char *inst_mnemonic(lagfx_air_inst_code_t c, uint32_t raw_code) {
    switch (c) {
        case LAGFX_AIR_INST_DECLAREBLOCKS:    return "DECLAREBLOCKS";  /* 1  */
        case LAGFX_AIR_INST_BINOP:            return "BINOP";          /* 2  */
        case LAGFX_AIR_INST_CAST:             return "CAST";           /* 3  */
        case LAGFX_AIR_INST_GEP_OLD:          return "GEP_OLD";        /* 4  */
        case LAGFX_AIR_INST_SELECT:           return "SELECT";         /* 5  */
        case LAGFX_AIR_INST_EXTRACTELT:       return "EXTRACTELT";     /* 6  */
        case LAGFX_AIR_INST_INSERTELT:        return "INSERTELT";      /* 7  */
        case LAGFX_AIR_INST_SHUFFLEVEC:       return "SHUFFLEVEC";     /* 8  */
        case LAGFX_AIR_INST_CMP:              return "CMP";            /* 9  */
        case LAGFX_AIR_INST_RET:              return "RET";            /* 10 */
        case LAGFX_AIR_INST_BR:               return "BR";             /* 11 */
        case LAGFX_AIR_INST_SWITCH:           return "SWITCH";         /* 12 */
        case LAGFX_AIR_INST_UNREACHABLE:      return "UNREACHABLE";    /* 15 */
        case LAGFX_AIR_INST_PHI:              return "PHI";            /* 16 */
        case LAGFX_AIR_INST_ALLOCA:           return "ALLOCA";         /* 19 */
        case LAGFX_AIR_INST_LOAD:             return "LOAD";           /* 20 */
        case LAGFX_AIR_INST_STORE_OLD:        return "STORE_OLD";      /* 24 */
        case LAGFX_AIR_INST_EXTRACTVAL:       return "EXTRACTVAL";     /* 26 */
        case LAGFX_AIR_INST_INSERTVAL:        return "INSERTVAL";      /* 27 */
        case LAGFX_AIR_INST_CMP2:             return "CMP2";           /* 28 */
        case LAGFX_AIR_INST_VSELECT:          return "VSELECT";        /* 29 */
        case LAGFX_AIR_INST_INDIRECTBR:       return "INDIRECTBR";     /* 31 */
        case LAGFX_AIR_INST_DEBUG_LOC_AGAIN:  return "DEBUG_LOC_AGAIN";/* 33 */
        case LAGFX_AIR_INST_CALL:             return "CALL";           /* 34 */
        case LAGFX_AIR_INST_DEBUG_LOC:        return "DEBUG_LOC";      /* 35 */
        case LAGFX_AIR_INST_FENCE:            return "FENCE";          /* 36 */
        case LAGFX_AIR_INST_GEP:              return "GEP";            /* 43 */
        case LAGFX_AIR_INST_STORE:            return "STORE";          /* 44 */
        case LAGFX_AIR_INST_CMPXCHG:          return "CMPXCHG";        /* 46 */
        case LAGFX_AIR_INST_UNOP:             return "UNOP";           /* 56 */
        case LAGFX_AIR_INST_UNKNOWN: default:
            /* Use the raw record code so the consumer can cross-check
             * against llvm/Bitcode/LLVMBitCodes.h FUNC_CODE_*. */
            (void)raw_code;
            return "UNKNOWN";
    }
}

static const char *type_kind_name(lagfx_air_type_kind_t k) {
    switch (k) {
        case LAGFX_AIR_TYPE_VOID:         return "void";
        case LAGFX_AIR_TYPE_INTEGER:      return "i";
        case LAGFX_AIR_TYPE_FLOAT:        return "float";
        case LAGFX_AIR_TYPE_DOUBLE:       return "double";
        case LAGFX_AIR_TYPE_HALF:         return "half";
        case LAGFX_AIR_TYPE_VECTOR:       return "vec";
        case LAGFX_AIR_TYPE_POINTER:      return "ptr";
        case LAGFX_AIR_TYPE_STRUCT_ANON:  return "struct";
        case LAGFX_AIR_TYPE_STRUCT_NAMED: return "struct named";
        case LAGFX_AIR_TYPE_ARRAY:        return "array";
        case LAGFX_AIR_TYPE_FUNCTION:     return "fn";
        case LAGFX_AIR_TYPE_METADATA:     return "metadata";
        case LAGFX_AIR_TYPE_LABEL:        return "label";
        default:                          return "?";
    }
}

static void dump_module(const lagfx_air_module_t *m) {
    const char *triple = lagfx_air_module_triple(m);
    const char *dl     = lagfx_air_module_datalayout(m);
    const char *src    = lagfx_air_module_source_filename(m);
    printf("Module:\n");
    printf("  triple          = '%s'\n", triple ? triple : "(absent)");
    printf("  datalayout      = '%s'\n", dl ? dl : "(absent)");
    printf("  source_filename = '%s'\n", src ? src : "(absent)");

    uint32_t nt = 0;
    const lagfx_air_type_t *types = lagfx_air_module_types(m, &nt);
    printf("\nTypes (%u):\n", nt);
    for (uint32_t i = 0; i < nt; i++) {
        printf("  [%2u] %-12s num_op=%u", i, type_kind_name(types[i].kind), types[i].num_op);
        for (uint32_t j = 0; j < types[i].num_op && j < 8u; j++) {
            printf(" %u", types[i].op[j]);
        }
        if (types[i].num_op > 8u) printf(" ...");
        printf("\n");
    }

    uint32_t nc = 0;
    (void)lagfx_air_module_constants(m, &nc);
    uint32_t npag = 0;
    (void)lagfx_air_module_param_attr_groups(m, &npag);
    printf("\nConstants:           %u\n", nc);
    printf("Param attr groups:   %u\n", npag);

    uint32_t nf = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    printf("\nFunctions (%u):\n", nf);
    for (uint32_t i = 0; i < nf; i++) {
        printf("  [%u] type_index=%u is_proto=%d linkage=%u name_offset=%u "
               "body_offset=%zu body_length=%zu\n",
               i, fns[i].type_index, (int)fns[i].is_proto, fns[i].linkage,
               fns[i].name_offset, fns[i].body_offset, fns[i].body_length);
    }

    uint32_t nms = 0;
    const char * const *mstrings = lagfx_air_module_metadata_strings(m, &nms);
    uint32_t nmd = 0;
    const lagfx_air_metadata_t *mds = lagfx_air_module_metadata(m, &nmd);
    printf("\nModule-level METADATA:\n");
    printf("  strings (%u):\n", nms);
    for (uint32_t i = 0; i < nms && i < 64u; i++) {
        printf("    [md#%u] '%s'\n", i, mstrings[i] ? mstrings[i] : "(null)");
    }
    if (nms > 64u) printf("    ... %u more\n", nms - 64u);

    uint32_t n_value = 0, n_node = 0, n_named = 0, n_unknown = 0;
    for (uint32_t i = 0; i < nmd; i++) {
        switch (mds[i].kind) {
            case LAGFX_AIR_MD_VALUE:      n_value++; break;
            case LAGFX_AIR_MD_NODE:       n_node++; break;
            case LAGFX_AIR_MD_NAMED_NODE: n_named++; break;
            default:                      n_unknown++; break;
        }
    }
    printf("  records (%u): %u VALUE, %u NODE, %u NAMED_NODE, %u UNKNOWN\n",
           nmd, n_value, n_node, n_named, n_unknown);
    for (uint32_t i = 0; i < nmd; i++) {
        const lagfx_air_metadata_t *md = &mds[i];
        const char *kind = (md->kind == LAGFX_AIR_MD_VALUE) ? "VALUE"
                         : (md->kind == LAGFX_AIR_MD_NODE)  ? "NODE"
                         : (md->kind == LAGFX_AIR_MD_NAMED_NODE) ? "NAMED_NODE"
                         : "UNKNOWN";
        uint32_t global_id = nms + i;
        printf("    [md#%u] %-11s", global_id, kind);
        if (md->kind == LAGFX_AIR_MD_NAMED_NODE && md->name_offset != 0u) {
            printf(" name='%s'", lagfx_air_module_string(m, md->name_offset));
        }
        printf(" ops=");
        for (uint32_t j = 0; j < md->num_operands && j < 8u; j++) {
            printf("%s%u", j == 0u ? "[" : ",", md->operands[j]);
        }
        printf("%s\n", md->num_operands > 8u ? ",...]" : (md->num_operands ? "]" : "[]"));
    }
}

static int dump_body(const lagfx_air_module_t *m, uint32_t fn_idx) {
    lagfx_air_function_body_t *body = NULL;
    lagfx_status_t st = lagfx_air_function_body_open(m, fn_idx, &body);
    if (st != LAGFX_OK || !body) {
        printf("  function_body_open failed: st=%d\n", (int)st);
        return 1;
    }
    uint32_t nb = lagfx_air_function_body_num_blocks(body);
    uint32_t ni = 0;
    const lagfx_air_inst_t *insts = lagfx_air_function_body_instructions(body, &ni);
    printf("  num_basic_blocks=%u num_instructions=%u\n", nb, ni);
    for (uint32_t i = 0; i < ni; i++) {
        const lagfx_air_inst_t *inst = &insts[i];
        printf("    %2u: %-14s (raw_code=%u num_ops=%u)",
               i, inst_mnemonic(inst->code, inst->raw_code),
               inst->raw_code, inst->num_ops);
        for (uint32_t j = 0; j < inst->num_ops && j < 8u; j++) {
            printf(" op%u=%llu", j, (unsigned long long)inst->ops[j]);
        }
        if (inst->num_ops > 8u) printf(" ...");
        printf("\n");
    }
    lagfx_air_function_body_free(body);
    return 0;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.air.bc>\n", argv[0]);
        return 2;
    }

    size_t len = 0;
    uint8_t *blob = slurp(argv[1], &len);
    if (!blob) {
        fprintf(stderr, "error: failed to read '%s'\n", argv[1]);
        return 1;
    }

    lagfx_air_module_t *m = NULL;
    lagfx_status_t st = lagfx_air_module_open(blob, len, &m);
    if (st != LAGFX_OK || !m) {
        fprintf(stderr, "error: lagfx_air_module_open st=%d\n", (int)st);
        free(blob);
        return 1;
    }

    printf("=== %s ===\n", argv[1]);
    dump_module(m);

    /* Dump every non-prototype function body. */
    uint32_t nf = 0;
    const lagfx_air_function_t *fns = lagfx_air_module_functions(m, &nf);
    printf("\nFunction bodies:\n");
    int rc = 0;
    for (uint32_t i = 0; i < nf; i++) {
        if (fns[i].is_proto) continue;
        printf("\n  Function [%u]:\n", i);
        rc |= dump_body(m, i);
    }

    lagfx_air_module_free(m);
    free(blob);
    return rc;
}
