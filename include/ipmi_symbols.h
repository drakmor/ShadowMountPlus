// SPDX-License-Identifier: GPL-3.0-or-later
//
// Runtime symbol resolution against the firmware's libSceIpmi.
//
// libSceIpmi exports the functions that occupy the vtable slots we care about,
// so a slot can be identified by comparing its value against the address the
// loader bound that export to -- no offset arithmetic and no waiting for a
// dispatch to arrive at the wrong method.
//
// Taking the address of a member directly would not work: that yields a PLT
// stub inside this payload, not the firmware function. dlsym returns what the
// loader actually resolved. It takes the plain symbol string and NID-hashes it
// internally, and IPMI import NIDs hash the mangled name, so the mangled names
// go in verbatim.

#pragma once

#include <stdbool.h>

typedef struct IpmiSyms {
    // The handler interface we subclass by hand. evhVtable is the vtable object
    // itself; the rest are its virtual methods. Both dispatch methods are
    // overloaded: one form takes the {ptr,len} descriptor arrays, the other raw
    // pointer+length pairs. We implement the descriptor form, which is the
    // one a real client has been served on; the raw forms are hooked anyway so a
    // dispatch arriving there is visible instead of silent.
    void* evhVtable;
    void* evhD1;
    void* evhD0;
    void* evhD2;
    void* evhSyncDataInfo;
    void* evhSyncRaw;
    void* evhAsyncDataInfo;
    void* evhAsyncRaw;
    void* evhSessionKilled;

    // The concrete Server type create() returns. tryDispatch is the one we
    // call; runDispatcher and shutdownDispatcher are resolved only so a vtable
    // dump reads as names, because calling runDispatcher makes the process
    // unkillable and shutdownDispatcher makes destroy() refuse. See the
    // dispatcher loop in sm_env_ipmi.c.
    void* srvRunDispatcher;
    void* srvShutdownDispatcher;
    void* srvTryDispatch;
    void* srvCreateSession;
    void* srvGetUserData;
    void* srvDestroy;
    void* srvD0;
    void* srvD1;

    // The concrete Session type a dispatch hands us. respondToSyncMethodRequest
    // is the reply path and is called on every sync dispatch, located by address
    // in that session's own vtable; the rest are resolved so the vtable dump
    // reads as names rather than hex.
    void* sessRespondSyncBuf;
    void* sessRespondSyncRaw;
    void* sessGetClientPid;
    void* sessGetServer;
    void* sessDestroy;
    void* sessIsPeerPrivileged;

    // The client half. Resolved so it finds connect and destroy by address
    // rather than by a hardcoded vtable index.
    void* clientCreate;
    void* clientConfigCtor;
    void* clientConfigEstimate;
    void* cliConnect;
    void* cliDisconnect;
    void* cliTerminateConnection;
    void* cliDestroy;
    void* cliInvokeSyncDataInfo;
    void* cliInvokeSyncRaw;
    void* cliInvokeAsyncDataInfo;

    // Resolved so a vtable dump can print offsets relative to a known export.
    void* serverCreate;
    void* serverConfigCtor;
} IpmiSyms;

// Resolves every symbol above and logs each lookup individually. Returns false
// only when the library itself could not be opened -- individual misses are
// reported as null and left for the caller to judge, because which ones matter
// depends on what the caller is about to do.
bool ipmi_syms_resolve(IpmiSyms* s);

// Reverse lookup: the short name of whatever `addr` is, or NULL. Used to
// annotate vtable dumps so they read as names instead of hex.
const char* ipmi_syms_name(const IpmiSyms* s, const void* addr);

// Dumps an object's vtable, each slot annotated by ipmi_syms_name and offset
// from Server::create so the dump is comparable across firmwares.
void ipmi_dump_vtable(const IpmiSyms* s, const void* obj, const char* label, int slots);

// Index of the slot in obj's vtable holding `fn`, or -1. This is how every slot
// this payload calls is chosen, rather than by a hardcoded index.
int ipmi_vtable_slot_of(const void* obj, const void* fn, int slots);
