/*
 * libapplegfx-vulkan — crash diagnostic handler
 * src/common/crash_handler.h
 *
 * Copyright © 2026 Matthew Jackson
 * SPDX-License-Identifier: MIT
 *
 * Installs a SIGSEGV/SIGBUS/SIGABRT handler that logs the faulting
 * instruction pointer + nearest symbol (via dladdr) to the lagfx log,
 * then chains to the previous handler so QEMU's crash behaviour is
 * unchanged. Diagnostic only — turns an opaque exit=139 into a named
 * crashing function. Idempotent; install once at device creation.
 */
#ifndef LAGFX_COMMON_CRASH_HANDLER_H
#define LAGFX_COMMON_CRASH_HANDLER_H

void lagfx_install_crash_handler(void);

#endif /* LAGFX_COMMON_CRASH_HANDLER_H */
