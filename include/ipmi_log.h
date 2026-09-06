// SPDX-License-Identifier: GPL-3.0-or-later
//
// The logging seam the IPMI files use: a shim onto the existing log_debug(),
// not a second logging system. Both are defined in src/sm_env_ipmi.c.

#pragma once

#include <stddef.h>

void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void log_hexdump(const char *label, const void *p, size_t n);
