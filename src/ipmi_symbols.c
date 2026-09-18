// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipmi_symbols.h"

#include "ipmi_log.h"

#include <dlfcn.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct SymEntry {
    const char* mangled;
    const char* shortName;
    size_t      field;          // byte offset into IpmiSyms
} SymEntry;

#define SYM(field, mangled, shortName) \
    { mangled, shortName, offsetof(IpmiSyms, field) }

// Keep the mangled strings byte-exact: a typo shows up as a null lookup and a
// refusal to dispatch, not as a wrong answer.
static const SymEntry kSyms[] = {
    SYM(evhVtable,
        "_ZTVN4IPMI6Server12EventHandlerE",              "EventHandler::vtable"),
    SYM(evhD1,
        "_ZN4IPMI6Server12EventHandlerD1Ev",             "EventHandler::~D1"),
    SYM(evhD0,
        "_ZN4IPMI6Server12EventHandlerD0Ev",             "EventHandler::~D0"),
    SYM(evhD2,
        "_ZN4IPMI6Server12EventHandlerD2Ev",             "EventHandler::~D2"),
    SYM(evhSyncDataInfo,
        "_ZN4IPMI6Server12EventHandler20onSyncMethodDispatchEPNS_7SessionEjPKNS_8DataInfoEjPNS_10BufferInfoEj",
        "EventHandler::onSyncMethodDispatch(DataInfo)"),
    SYM(evhSyncRaw,
        "_ZN4IPMI6Server12EventHandler20onSyncMethodDispatchEPNS_7SessionEjPvmmS4_m",
        "EventHandler::onSyncMethodDispatch(raw)"),
    SYM(evhAsyncDataInfo,
        "_ZN4IPMI6Server12EventHandler21onAsyncMethodDispatchEPNS_7SessionEjjPKNS_8DataInfoEj",
        "EventHandler::onAsyncMethodDispatch(DataInfo)"),
    SYM(evhAsyncRaw,
        "_ZN4IPMI6Server12EventHandler21onAsyncMethodDispatchEPNS_7SessionEjjPvmS4_m",
        "EventHandler::onAsyncMethodDispatch(raw)"),
    SYM(evhSessionKilled,
        "_ZN4IPMI6Server12EventHandler15onSessionKilledEPNS_7SessionE",
        "EventHandler::onSessionKilled"),

    SYM(srvRunDispatcher,
        "_ZN4IPMI4impl10ServerImpl13runDispatcherEPvm",  "ServerImpl::runDispatcher"),
    SYM(srvShutdownDispatcher,
        "_ZN4IPMI4impl10ServerImpl18shutdownDispatcherEv", "ServerImpl::shutdownDispatcher"),
    SYM(srvTryDispatch,
        "_ZN4IPMI4impl10ServerImpl11tryDispatchEPvm",    "ServerImpl::tryDispatch"),
    SYM(srvCreateSession,
        "_ZN4IPMI4impl10ServerImpl13createSessionEPPNS_7SessionEPvS5_",
        "ServerImpl::createSession"),
    SYM(srvGetUserData,
        "_ZN4IPMI4impl10ServerImpl11getUserDataEv",      "ServerImpl::getUserData"),
    SYM(srvDestroy,
        "_ZN4IPMI4impl10ServerImpl7destroyEv",           "ServerImpl::destroy"),
    SYM(srvD0,
        "_ZN4IPMI4impl10ServerImplD0Ev",                 "ServerImpl::~D0"),
    SYM(srvD1,
        "_ZN4IPMI4impl10ServerImplD1Ev",                 "ServerImpl::~D1"),

    SYM(sessRespondSyncBuf,
        "_ZN4IPMI4impl11SessionImpl26respondToSyncMethodRequestEiPKNS_10BufferInfoEj",
        "SessionImpl::respondToSyncMethodRequest(BufferInfo)"),
    SYM(sessRespondSyncRaw,
        "_ZN4IPMI4impl11SessionImpl26respondToSyncMethodRequestEiPKvm",
        "SessionImpl::respondToSyncMethodRequest(raw)"),
    SYM(sessRespondAsyncData,
        "_ZN4IPMI4impl11SessionImpl27respondToAsyncMethodRequestEjjiPKNS_8DataInfoEj",
        "SessionImpl::respondToAsyncMethodRequest(DataInfo)"),
    SYM(sessRespondAsyncRaw,
        "_ZN4IPMI4impl11SessionImpl27respondToAsyncMethodRequestEjiPKvm",
        "SessionImpl::respondToAsyncMethodRequest(raw)"),
    SYM(sessGetClientPid,
        "_ZN4IPMI4impl11SessionImpl12getClientPidEv",    "SessionImpl::getClientPid"),
    SYM(sessGetServer,
        "_ZN4IPMI4impl11SessionImpl9getServerEv",        "SessionImpl::getServer"),
    SYM(sessDestroy,
        "_ZN4IPMI4impl11SessionImpl7destroyEv",          "SessionImpl::destroy"),
    SYM(sessIsPeerPrivileged,
        "_ZNK4IPMI4impl11SessionImpl16isPeerPrivilegedEv", "SessionImpl::isPeerPrivileged"),

    SYM(clientCreate,
        "_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_", "Client::create"),
    SYM(clientConfigCtor,
        "_ZN4IPMI6Client6ConfigC1Ev",                     "Client::Config::Config"),
    SYM(clientConfigEstimate,
        "_ZN4IPMI6Client6Config24estimateClientMemorySizeEv",
        "Client::Config::estimateClientMemorySize"),
    SYM(cliConnect,
        "_ZN4IPMI4impl10ClientImpl7connectEPKvmPi",       "ClientImpl::connect"),
    SYM(cliDisconnect,
        "_ZN4IPMI4impl10ClientImpl10disconnectEv",        "ClientImpl::disconnect"),
    SYM(cliTerminateConnection,
        "_ZN4IPMI4impl10ClientImpl19terminateConnectionEv",
        "ClientImpl::terminateConnection"),
    SYM(cliDestroy,
        "_ZN4IPMI4impl10ClientImpl7destroyEv",            "ClientImpl::destroy"),
    SYM(cliInvokeSyncDataInfo,
        "_ZN4IPMI4impl10ClientImpl16invokeSyncMethodEjPKNS_8DataInfoEjPiPNS_10BufferInfoEj",
        "ClientImpl::invokeSyncMethod(DataInfo)"),
    SYM(cliInvokeSyncRaw,
        "_ZN4IPMI4impl10ClientImpl16invokeSyncMethodEjPKvmPiPvPmm",
        "ClientImpl::invokeSyncMethod(raw)"),
    SYM(cliInvokeAsyncDataInfo,
        "_ZN4IPMI4impl10ClientImpl17invokeAsyncMethodEjPKNS_8DataInfoEjPjPKNS_6Client12EventNotifeeE",
        "ClientImpl::invokeAsyncMethod(DataInfo)"),

    SYM(serverCreate,
        "_ZN4IPMI6Server6createEPPS0_PKNS0_6ConfigEPvS6_", "Server::create"),
    SYM(serverConfigCtor,
        "_ZN4IPMI6Server6ConfigC1Ev",                     "Server::Config::Config"),
};

#undef SYM

static const size_t kSymCount = sizeof(kSyms) / sizeof(kSyms[0]);

static void** field_of(IpmiSyms* s, size_t off) {
    return (void**)((char*)(s) + off);
}

static void* const* field_of_const(const IpmiSyms* s, size_t off) {
    return (void* const*)((const char*)(s) + off);
}

static void* open_libipmi(void) {
    // Measured: RTLD_NOLOAD always answers null here even though this payload
    // links against the library, and the plain load always succeeds.
    void* h = dlopen("libSceIpmi.sprx", RTLD_LAZY);
    logf_("dlopen(\"libSceIpmi.sprx\", RTLD_LAZY) -> %p", h);
    if (!h) {
        const char* err = dlerror();
        if (err) logf_("  dlerror: %s", err);
    }
    return h;
}


bool ipmi_syms_resolve(IpmiSyms* s) {
    memset(s, 0, sizeof(*s));

    void* h = open_libipmi();
    if (!h) {
        logf_("libSceIpmi could not be opened; every slot identification below "
              "would be a guess, so the service will not register");
        return false;
    }

    int found = 0;
    for (size_t i = 0; i < kSymCount; i++) {
        void* addr = dlsym(h, kSyms[i].mangled);
        *field_of(s, kSyms[i].field) = addr;
        if (addr) found++;
        logf_("  %-52s = %p%s", kSyms[i].shortName, addr,
              addr ? "" : "   <-- MISSING");
    }
    logf_("resolved %d/%zu libSceIpmi symbols", found, kSymCount);
    return true;
}

const char* ipmi_syms_name(const IpmiSyms* s, const void* addr) {
    if (!addr) return NULL;
    for (size_t i = 0; i < kSymCount; i++) {
        if (*field_of_const(s, kSyms[i].field) == addr) return kSyms[i].shortName;
    }
    return NULL;
}

void ipmi_dump_vtable(const IpmiSyms* s, const void* obj, const char* label,
                      int slots) {
    if (!obj) return;
    void* const* vt = *(void* const* const*)(obj);
    logf_("%s vtable = %p", label, (void*)vt);
    // Offsets are printed relative to Server::create rather than to a module
    // base, so no per-firmware constant is involved and a wrong one cannot make
    // the dump quietly misleading.
    for (int i = 0; i < slots; i++) {
        const char* name = ipmi_syms_name(s, vt[i]);
        if (s->serverCreate && vt[i]) {
            logf_("  %s vtbl[%#04x] = %p  create%+ld  %s", label, i * 8, vt[i],
                  (long)((intptr_t)(vt[i]) - (intptr_t)(s->serverCreate)),
                  name ? name : "");
        } else {
            logf_("  %s vtbl[%#04x] = %p  %s", label, i * 8, vt[i],
                  name ? name : "");
        }
    }
}

int ipmi_vtable_slot_of(const void* obj, const void* fn, int slots) {
    if (!obj || !fn) return -1;
    void* const* vt = *(void* const* const*)(obj);
    for (int i = 0; i < slots; i++) {
        if (vt[i] == fn) return i;
    }
    return -1;
}
