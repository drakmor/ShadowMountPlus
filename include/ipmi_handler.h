// SPDX-License-Identifier: GPL-3.0-or-later
//
// Assembles an IPMI::Server::EventHandler without ever declaring one.
//
// A hand-written subclass would be a guess at the vtable layout, and create()
// accepting one is not validation. So we copy the firmware's own EventHandler
// vtable and replace the slots we can name, naming each by comparing its value
// against the address dlsym gave for that exported method. Slots that cannot be
// named keep a logging thunk, so a surprise is visible rather than silent.

#pragma once

#include "ipmi.h"
#include "ipmi_symbols.h"

#include <stdbool.h>

typedef struct HandlerBuild {
    IpmiEventHandler* handler;  // null if the layout was not provable
    int  slotCount;             // virtuals found in the base vtable
    bool syncDispatchProven;    // the slot we actually need to serve
} HandlerBuild;

// Reads the base vtable, identifies every slot it can, logs the result as a
// table, and builds our object. Never returns a handler built on a layout it
// could not read.
HandlerBuild handler_build(const IpmiSyms* syms);

// Logs anything the connect callback recorded. Called from the dispatcher loop,
// because the callback itself must not log: logf_ does klog plus file I/O to
// /data, and it runs inside the window the kernel gives the server to answer a
// connection request. With logging in the callback every connect was refused.
void handler_drain_connect_log(void);
