/*
 * libapplegfx-vulkan — crash diagnostic handler
 * src/common/crash_handler.c
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: AGPL-3.0-or-later
 *
 * On a fatal signal, log: signal number, faulting data address, the
 * faulting instruction pointer (RIP), and — via dladdr — the module and
 * NEAREST exported symbol (dli_sname) plus the offset into it. On the
 * Alpine/musl deploy target there is no execinfo backtrace() and no
 * in-container addr2line, but dli_sname names the crashing function
 * directly (or the nearest preceding exported symbol, which narrows a
 * static-function crash to a single translation unit). After logging we
 * restore the previous handler and re-raise, so the process still dies
 * exactly as before (no swallowed signal, no changed exit behaviour).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "crash_handler.h"
#include "log.h"

#include <signal.h>
#include <string.h>
#include <stdint.h>

#if defined(__linux__) && defined(__x86_64__)
#include <ucontext.h>
#include <dlfcn.h>
#ifndef REG_RIP
#define REG_RIP 16
#endif
#endif

static struct sigaction s_prev_segv;
static struct sigaction s_prev_bus;
static struct sigaction s_prev_abrt;

static void lagfx_crash_handler(int sig, siginfo_t *si, void *ctx) {
    void *fault = si ? si->si_addr : (void *)0;
#if defined(__linux__) && defined(__x86_64__)
    ucontext_t *uc = (ucontext_t *)ctx;
    uintptr_t rip = (uintptr_t)uc->uc_mcontext.gregs[REG_RIP];
    Dl_info info;
    memset(&info, 0, sizeof(info));
    const char *mod = "?";
    const char *sym = "?";
    uintptr_t sym_off = 0;
    uintptr_t mod_off = 0;
    if (dladdr((void *)rip, &info)) {
        if (info.dli_fname) {
            mod = info.dli_fname;
        }
        if (info.dli_sname) {
            sym = info.dli_sname;
        }
        if (info.dli_saddr) {
            sym_off = rip - (uintptr_t)info.dli_saddr;
        }
        if (info.dli_fbase) {
            mod_off = rip - (uintptr_t)info.dli_fbase;
        }
    }
    LAGFX_ERR("FATAL signal %d fault_addr=%p rip=0x%lx  %s : %s+0x%lx  (module_off=0x%lx)",
              sig, fault, (unsigned long)rip, mod, sym,
              (unsigned long)sym_off, (unsigned long)mod_off);
#else
    LAGFX_ERR("FATAL signal %d fault_addr=%p", sig, fault);
    (void)ctx;
#endif

    /* Restore the previous disposition and re-raise so the original crash
     * behaviour (core / exit 139) is preserved unchanged. */
    struct sigaction *prev =
        (sig == SIGSEGV) ? &s_prev_segv :
        (sig == SIGBUS)  ? &s_prev_bus  : &s_prev_abrt;
    sigaction(sig, prev, (struct sigaction *)0);
    raise(sig);
}

void lagfx_install_crash_handler(void) {
    static int installed = 0;
    if (installed) {
        return;
    }
    installed = 1;

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = lagfx_crash_handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);

    sigaction(SIGSEGV, &sa, &s_prev_segv);
    sigaction(SIGBUS, &sa, &s_prev_bus);
    sigaction(SIGABRT, &sa, &s_prev_abrt);

    LAGFX_LOG("crash diagnostic handler installed (SIGSEGV/SIGBUS/SIGABRT)");
}
