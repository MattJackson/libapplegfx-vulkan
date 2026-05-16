/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include "metallib/metallib_reader.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <unistd.h>

/* Test-local log stubs — production impls in src/device.c pull Vulkan deps. */
void lagfx_log_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_warn_impl(const char *fmt, ...)  { (void)fmt; }
void lagfx_err_impl(const char *fmt, ...)   { (void)fmt; }
void lagfx_trace_impl(const char *fmt, ...) { (void)fmt; }

static uint8_t *read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1u, (size_t)sz, f);
    fclose(f);
    if (n != (size_t)sz) {
        free(buf);
        return NULL;
    }
    *out_len = n;
    return buf;
}

static int cmd_list(lagfx_metallib_t *ml) {
    lagfx_metallib_function_t funcs[256];
    size_t n = lagfx_metallib_list_functions(ml, funcs, 256);
    for (size_t i = 0; i < n; i++) {
        printf("%s\t%u\n", funcs[i].name, (unsigned)funcs[i].type_code);
    }
    return 0;
}

static int cmd_extract(lagfx_metallib_t *ml, const char *name) {
    const uint8_t *bytes = NULL;
    size_t n = lagfx_metallib_get_bitcode(ml, name, &bytes);
    if (n == 0 || !bytes) {
        fprintf(stderr, "metallib_dump: function '%s' not found\n", name);
        return 4;
    }
    if (fwrite(bytes, 1, n, stdout) != n) {
        return 5;
    }
    return 0;
}

static int cmd_extract_all(lagfx_metallib_t *ml, const char *dir) {
    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        fprintf(stderr, "metallib_dump: cannot create directory '%s': %s\n",
                dir, strerror(errno));
        return 3;
    }

    lagfx_metallib_function_t funcs[256];
    size_t n = lagfx_metallib_list_functions(ml, funcs, 256);
    int failed = 0;

    for (size_t i = 0; i < n; i++) {
        const uint8_t *bytes = NULL;
        size_t len = lagfx_metallib_get_bitcode(ml, funcs[i].name, &bytes);
        if (len == 0 || !bytes) {
            fprintf(stderr, "metallib_dump: cannot get bitcode for '%s'\n",
                    funcs[i].name);
            failed = 1;
            continue;
        }

        char path[4096];
        int rc = snprintf(path, sizeof(path), "%s/%s.bc", dir, funcs[i].name);
        if (rc < 0 || (size_t)rc >= sizeof(path)) {
            fprintf(stderr, "metallib_dump: path too long for '%s'\n",
                    funcs[i].name);
            failed = 1;
            continue;
        }

        FILE *f = fopen(path, "wb");
        if (!f) {
            fprintf(stderr, "metallib_dump: cannot write to '%s': %s\n",
                    path, strerror(errno));
            failed = 1;
            continue;
        }

        size_t written = fwrite(bytes, 1, len, f);
        fclose(f);

        if (written != len) {
            fprintf(stderr, "metallib_dump: short write to '%s'\n", path);
            failed = 1;
        }
    }

    return failed ? 5 : 0;
}

static void print_usage(const char *prog) {
    fprintf(stderr, "Usage: %s <input.metallib> --list\n", prog);
    fprintf(stderr, "       %s <input.metallib> --extract <name>\n", prog);
    fprintf(stderr, "       %s <input.metallib> --extract-all <dir>\n", prog);
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 2;
    }

    const char *metallib_path = argv[1];
    int op = -1;
    const char *arg1 = NULL;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            op = 0;
        } else if (strcmp(argv[i], "--extract") == 0) {
            op = 1;
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 2;
            }
            arg1 = argv[++i];
        } else if (strcmp(argv[i], "--extract-all") == 0) {
            op = 2;
            if (i + 1 >= argc) {
                print_usage(argv[0]);
                return 2;
            }
            arg1 = argv[++i];
        } else {
            fprintf(stderr, "metallib_dump: unknown flag '%s'\n", argv[i]);
            print_usage(argv[0]);
            return 2;
        }
    }

    if (op < 0) {
        print_usage(argv[0]);
        return 2;
    }

    size_t len = 0;
    uint8_t *data = read_file(metallib_path, &len);
    if (!data) {
        fprintf(stderr, "metallib_dump: cannot read '%s'\n", metallib_path);
        return 3;
    }

    lagfx_metallib_t *ml = lagfx_metallib_open(data, len);
    free(data);

    if (!ml) {
        fprintf(stderr, "metallib_dump: cannot open '%s' as metallib\n",
                metallib_path);
        return 3;
    }

    int rc = 0;
    switch (op) {
        case 0:
            rc = cmd_list(ml);
            break;
        case 1:
            rc = cmd_extract(ml, arg1);
            break;
        case 2:
            rc = cmd_extract_all(ml, arg1);
            break;
    }

    lagfx_metallib_close(ml);
    return rc;
}
