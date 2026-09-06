// SPDX-License-Identifier: GPL-3.0-or-later
//
// The IPMI client half. Only used to probe whether our own service name is
// already held, but kept general so a second caller inherits the fixes.

#pragma once

#include "ipmi.h"
#include "ipmi_symbols.h"

#include <stdbool.h>

typedef struct IpmiClient {
    void* handle;
    void* storage;
    int   connectSlot;      // all four resolved by address, never by a
    int   invokeSlot;       // hardcoded index
    int   destroySlot;
} IpmiClient;

// Creates a client for `name`. Logs every step, including the vtable slot
// numbers, and returns false having said why on any failure.
bool ipmi_client_open(IpmiClient* c, const IpmiSyms* syms, const char* name,
                      bool dumpVtable);

// Connects. Logs before calling, so that if connect ever blocks the last line
// in the log is the answer.
bool ipmi_client_connect(IpmiClient* c, const char* name);

// Destroys and frees. Idempotent, and safe on a client that never connected --
// the callers that need it most are error paths.
void ipmi_client_close(IpmiClient* c);
