// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipmi_client.h"

#include "ipmi_log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Reached through asm-labelled declarations. Only part of Client::Config's
// layout is known, so it is a byte buffer with named offsets rather than an
// invented struct.
void ipmi_cfg_ctor(void* cfg)
    __asm__("_ZN4IPMI6Client6ConfigC1Ev");
uint64_t ipmi_cfg_estimate(void* cfg)
    __asm__("_ZN4IPMI6Client6Config24estimateClientMemorySizeEv");
int ipmi_cli_create(void** out, const void* cfg, void* p3, void* storage)
    __asm__("_ZN4IPMI6Client6createEPPS0_PKNS0_6ConfigEPvS6_");

typedef int (*ConnectFn)(void* client, void* arg, uint64_t argLen, int* serviceResult);
typedef int (*DestroyFn)(void* client);

// Negotiated limits. estimateClientMemorySize() reports 0x1000 for this Config,
// so the storage size is generous rather than tight.
static const uint64_t kRequestBufferSize = 0x200u;
static const uint64_t kClientStorageSize = 0xf800u;


bool ipmi_client_open(IpmiClient* c, const IpmiSyms* syms, const char* name,
                      bool dumpVtable) {
    memset(c, 0, sizeof(*c));
    // -1, NOT the 0 memset leaves behind: 0 is a valid slot index, so a client
    // that failed before resolution would call vt[0] -- a destructor -- on close.
    c->connectSlot = -1;
    c->invokeSlot = -1;
    c->destroySlot = -1;

    if (!syms->clientCreate || !syms->clientConfigCtor) {
        logf_("  client: Client::create/Config::Config did not resolve");
        return false;
    }

    // Measured: the constructor writes up to +0x4a of this buffer. 0x200 leaves
    // room for a firmware whose Config is larger, since a too-small one would be
    // a silent overrun of the ctor's own writes.
    unsigned char cfg[0x200] __attribute__((aligned(16)));
    memset(cfg, 0, sizeof(cfg));
    ipmi_cfg_ctor(cfg);

    /* NUL-terminated, and the same truncation rule the server uses. A flat
     * memcpy of 16 copies a name with no terminator when it is exactly 16 long,
     * and truncates differently from the server's strncpy(size - 1), so the two
     * halves disagree about what the service is called and every connect answers
     * ESRCH against a service that is registered and serving. */
    char nameBuf[16];
    memset(nameBuf, 0, sizeof(nameBuf));
    strncpy(nameBuf, name, sizeof(nameBuf) - 1);
    memcpy(cfg, nameBuf, sizeof(nameBuf));                     // +0x00 name[16]
    *(uint64_t*)(cfg + 0x10) = 0;
    *(uint64_t*)(cfg + 0x28) = kRequestBufferSize;
    *(uint64_t*)(cfg + 0x30) = kClientStorageSize;

    uint64_t storageSize = kClientStorageSize;
    if (syms->clientConfigEstimate) {
        const uint64_t est = ipmi_cfg_estimate(cfg);
        logf_("  estimateClientMemorySize = %#lx (we reserve %#lx)",
              (unsigned long)est, (unsigned long)kClientStorageSize);
        if (est > 0 && est < 0x100000u && est > storageSize) storageSize = est;
    }

    c->storage = malloc(storageSize);
    if (!c->storage) { logf_("  client: storage alloc failed"); return false; }
    memset(c->storage, 0, storageSize);

    const int rc = ipmi_cli_create(&c->handle, cfg, NULL, c->storage);
    logf_("  Client::create(\"%s\") -> rc=%#010x client=%p", name, (unsigned)rc,
          c->handle);
    if (rc < 0 || !c->handle) {
        free(c->storage);
        c->storage = NULL;
        return false;
    }

    if (dumpVtable) ipmi_dump_vtable(syms, c->handle, "client", 20);

    c->connectSlot = ipmi_vtable_slot_of(c->handle, syms->cliConnect, 20);
    c->invokeSlot  = ipmi_vtable_slot_of(c->handle, syms->cliInvokeSyncDataInfo, 20);
    // Resolved here rather than in close(): close() has no syms, and a slot
    // number is cheaper to carry than the whole table.
    c->destroySlot = ipmi_vtable_slot_of(c->handle, syms->cliDestroy, 20);
    logf_("  connect slot = %d (%#x), invokeSyncMethod(DataInfo) slot = %d (%#x)",
          c->connectSlot, c->connectSlot < 0 ? 0 : c->connectSlot * 8,
          c->invokeSlot, c->invokeSlot < 0 ? 0 : c->invokeSlot * 8);
    return c->connectSlot >= 0 && c->invokeSlot >= 0;
}

bool ipmi_client_connect(IpmiClient* c, const char* name) {
    void* const* vt = *(void* const* const*)(c->handle);
    int serviceResult = 0;

    // Logged before the call on purpose: if connect() ever blocks, this line is
    // the last thing in the log and that is the answer.
    logf_("  calling connect() on \"%s\" -- if the log stops here, connect BLOCKS",
          name);
    const int rc = ((ConnectFn)vt[c->connectSlot])(
        c->handle, NULL, 0, &serviceResult);
    // rc decodes as 0x80020000 | errno: 0x16 EINVAL, 0x03 ESRCH (no such
    // service), 0x0d EACCES (refused).
    logf_("  connect -> rc=%#010x serviceResult=%#010x%s", (unsigned)rc,
          (unsigned)serviceResult,
          (rc == (int)(0x8002000d)) ? "   [EACCES: permission]"
          : (rc == (int)(0x80020003)) ? "   [ESRCH: no such service]"
          : "");
    return rc >= 0 && serviceResult >= 0;
}

void ipmi_client_close(IpmiClient* c) {
    if (!c) return;

    if (c->handle) {
        void* const* vt = *(void* const* const*)(c->handle);

        /* Destroy, never disconnect. destroy() calls sceIpmiMgrDestroyClient,
         * which releases the client kid and everything under it. disconnect()
         * makes a blocking request to the peer instead, and the peer we are
         * closing against is typically a predecessor midway through exiting, so
         * it never returns -- measured as stranded successors that then ignore
         * their quit file forever. Dropping the disconnect took that from 5 of
         * 10 to 0 of 15. The session the far side keeps is not worth it; that
         * process is about to exit, which drops the session anyway. */
        if (c->destroySlot >= 0)
            (void)((DestroyFn)vt[c->destroySlot])(c->handle);
        else
            logf_("  client: no destroy slot -- handle %p leaked", c->handle);

        c->handle = NULL;
    }

    /* Ours to free either way: storage is our malloc, not the library's. */
    free(c->storage);
    c->storage = NULL;
    c->connectSlot = c->invokeSlot = c->destroySlot = -1;
}
