// SPDX-License-Identifier: GPL-3.0-or-later

#include "ipmi_handler.h"

#include "sm_env_ipmi_dispatch.h"
#include "ipmi_log.h"

#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef uint64_t u64;

typedef enum SlotKind {
    KIND_UNKNOWN = 0,
    KIND_DTOR,
    KIND_SYNC_DATAINFO,
    KIND_SYNC_RAW,
    KIND_ASYNC_DATAINFO,
    KIND_ASYNC_RAW,
    KIND_SESSION_KILLED,
} SlotKind;

static const char* kind_name(SlotKind k) {
    switch (k) {
        case KIND_DTOR:            return "~EventHandler";
        case KIND_SYNC_DATAINFO:   return "onSyncMethodDispatch(DataInfo)";
        case KIND_SYNC_RAW:        return "onSyncMethodDispatch(raw)";
        case KIND_ASYNC_DATAINFO:  return "onAsyncMethodDispatch(DataInfo)";
        case KIND_ASYNC_RAW:       return "onAsyncMethodDispatch(raw)";
        case KIND_SESSION_KILLED:  return "onSessionKilled";
        default:                   return "UNIDENTIFIED";
    }
}

// Widest window we are willing to treat as EventHandler's vtable.
//
// Measured: the class has nine virtuals, not the seven its exported methods
// imply, and they are not in export order:
//
//   [0x00] ~D1  [0x08] ~D0  [0x10] onSyncMethodDispatch(DataInfo)
//   [0x18] onAsyncMethodDispatch(DataInfo)
//   [0x20] [0x28] two slots sharing one address outside libSceIpmi --
//                 unexported, so unnameable by this method
//   [0x30] onSessionKilled  [0x38] onSyncMethodDispatch(raw)
//   [0x40] onAsyncMethodDispatch(raw), by elimination
//
// A cap of 8 stopped one slot short of that last one and reported it missing.
// Twelve covers it with slack; the scan stops at the last slot it can name, so
// a larger cap cannot run past the end of the vtable.
enum { kMaxSlots = 12 };

// Our vtable, laid out as the Itanium ABI wants it: offset-to-top and typeinfo
// first, then the function pointers. The object's vptr points at g_vtable[2].
//
// Only the slots the scan named are copied. Reading further is not free
// padding: measured, the two words after the last virtual are the next vtable's
// offset-to-top and typeinfo, and the words after those are another class's
// methods. Copying them would install unrelated functions as our own.
static void*    g_vtable[2 + kMaxSlots];
static SlotKind g_kind[kMaxSlots];

// The connect slot. It and the disconnect callback beside it are both unnamed
// and share one address, but take different arguments, and capture_connect
// writes through its cfg pointer -- so only connect may run it. Identified by
// position between named neighbours; see the claim loop in handler_build.
static int g_connectSlot = -1;

// The handler object itself. EventHandler is an interface and should carry no
// data, but the padding costs nothing and a wrong guess about that would
// otherwise be a memory corruption rather than a log line.
typedef struct HandlerObject {
    void**        vptr;
    unsigned char reserved[0x40];
} HandlerObject;
static HandlerObject g_handler;

// One-shot: the first Session* we are handed gets its vtable dumped, which is
// how SessionImpl's slots get identified for the reply path later.
static const IpmiSyms* g_syms;
static bool            g_sessionDumped;

// What the connect callback saw, captured without touching a file or klog. Read
// and printed later by handler_drain_connect_log() from the resident loop.
typedef struct ConnectRecord {
    volatile bool pending;
    unsigned      calls;
    int           slot;
    u64           srv, cfg, extra;
    // 0x48, not 0x40: numMsgQueue sits at +0x40 and the drain reads it. At
    // 0x40 that read left the array and picked up memorySize below instead,
    // so every drained connect logged the low half of memorySize as
    // numMsgQueue. The firmware Config runs to at least 0x150 (memorySize
    // lives at +0x148), so copying 0x48 stays inside it.
    unsigned char cfgHead[0x48];
    uint64_t      memorySize;
    uint64_t      memorySizeSet;
    int           createSessionSlot;
    int           createSessionRc;
    u64           session;
    bool          noSessionSlot;
} ConnectRecord;
static ConnectRecord g_connect;

// Session memory for createSession. Static and pre-carved, not malloc'd: this
// runs inside the connection window, where the less that happens the better.
//
// One buffer per live session, because sessions overlap as a matter of course.
// Measured on 4.03 over four title launches: every title connects TWICE, about
// two seconds apart, and the second connect lands while the first session is
// still alive -- eight connects, high-water mark of two live at once, never
// fewer. Sharing one buffer meant createSession placement-constructed each new
// session over the live previous one, and all eight sessions duly reported the
// same Session* (0x200949560): two handles, one object.
//
// Four slots for headroom over the observed two. No lock: the same measurement
// showed connect, dispatch and onSessionKilled all arrive on the dispatcher
// thread. Released in onSessionKilled, which the same run showed fires for
// every session (8 created, 8 killed, live back to 0).
enum { kSessionSlots = 4 };
typedef struct SessionSlot {
    unsigned char mem[0x20000] __attribute__((aligned(16)));
    void*         session;  // non-NULL while this buffer backs a live session
} SessionSlot;
static SessionSlot g_sessionSlots[kSessionSlots];

static SessionSlot* session_slot_claim(void) {
    for (int i = 0; i < kSessionSlots; i++)
        if (!g_sessionSlots[i].session) return &g_sessionSlots[i];
    return NULL;
}

// Matching by pointer is sound only because each live session now has its own
// buffer, and so its own address. It would have been ambiguous before this
// change -- which was the symptom, not a reason to key on something else.
static void session_slot_release(void* session) {
    for (int i = 0; i < kSessionSlots; i++)
        if (g_sessionSlots[i].session == session)
            g_sessionSlots[i].session = NULL;
}

// Deliberately branch-light and I/O-free: this runs inside the connection
// window. Two memcpys and some stores.
static void capture_connect(int slot, void* self, u64 srv, u64 cfg, u64 extra) {
    (void)self;
    g_connect.calls++;
    g_connect.slot  = slot;
    g_connect.srv   = srv;
    g_connect.cfg   = cfg;
    g_connect.extra = extra;
    if (cfg) {
        memcpy(g_connect.cfgHead, (const void*)(cfg),
               sizeof(g_connect.cfgHead));
        memcpy(&g_connect.memorySize,
               (const unsigned char*)(cfg) + 0x148, 8);
    }

    // Create the session the connection needs. Our return value reaches the
    // client as serviceResult=0, yet without this the client's transport status
    // is 1, which libSceIpmi maps to 0x8002000d -- "accepted, but no session
    // exists". The framework hands this callback exactly what createSession
    // wants: the Server*, the SessionImpl::Config*, and a seeded memorySize.
    g_connect.createSessionSlot = -1;
    g_connect.createSessionRc = 0;
    g_connect.session = 0;
    // cfg is written through below, so it is required here as well. Line 106
    // above already treats it as possibly null; a null reaching this branch
    // would write eight bytes to 0x148 inside the connection window, where a
    // fault kills the process.
    SessionSlot* const slot_mem = session_slot_claim();
    g_connect.noSessionSlot = (slot_mem == NULL);
    if (srv && cfg && slot_mem && g_syms && g_syms->srvCreateSession) {
        // Say how big the buffer is. The framework seeds memorySize with a
        // floor of 0x10 and expects the handler to supply both the memory and
        // its size. Left at 0x10, createSession succeeded and one command
        // dispatched, then IPMIMGR killed the process (signo=0xa0020320
        // opt32=0x02010006) -- a session running off the end of 16 bytes.
        const uint64_t have = sizeof(slot_mem->mem);
        memcpy((unsigned char*)(cfg) + 0x148, &have, 8);
        g_connect.memorySizeSet = have;

        const int slotIdx = ipmi_vtable_slot_of((void*)(srv),
                                                g_syms->srvCreateSession, 24);
        g_connect.createSessionSlot = slotIdx;
        if (slotIdx >= 0) {
            void* const* vt = *(void* const* const*)(srv);
            typedef int (*CreateSessionFn)(void* self, void** out, void* cfg,
                                           void* mem);
            void* session = NULL;
            g_connect.createSessionRc =
                ((CreateSessionFn)vt[slotIdx])(
                    (void*)(srv), &session,
                    (void*)(cfg), slot_mem->mem);
            g_connect.session = (u64)(session);
            // Held only once it actually backs a session; a failed create must
            // not strand the buffer.
            if (g_connect.createSessionRc >= 0 && session)
                slot_mem->session = session;
        }
    }
    g_connect.pending = true;
}

// Refuse an async request on the wire, with the DESCRIPTOR form.
//
// The raw form cannot do this job: it has no methodId field and hardcodes that
// field to zero, so a client waiting on its own method id never matches the
// reply and blocks in tryGetResult forever. Measured -- a probe hung there
// while the server logged rc=0.
//
// The pair is (methodId, ticket), the reverse of the dispatch order. Also
// measured: tryGetResult answers EINVAL when handed (ticket, methodId) and
// EAGAIN when handed (methodId, ticket), so only the latter is a shape the
// kernel accepts. One zero-length descriptor, because a refusal carries no data
// and count=1 is the shape already known to be accepted.
static void respond_async_refusal(IpmiSession* session, uint32_t ticket,
                                  uint32_t methodId, const char* what) {
    if (!session || !g_syms) return;
    // Says so rather than returning quietly, which is the only difference a
    // missing symbol can make here: with no reply path the caller blocks, and
    // that is already the firmware's own behaviour -- EventHandler's async
    // slots are a bare ret on every firmware from 1.00 to 12.70. Measured:
    // libSceIpmi exports this symbol on all of them, so the branch is drift
    // insurance, not a state any console reaches.
    if (!g_syms->sessRespondAsyncData) {
        logf_("  -> %s NOT refused: respondToAsyncMethodRequest(DataInfo) did "
              "not resolve, so there is no reply path and the caller blocks",
              what);
        return;
    }
    const int respondSlot =
        ipmi_vtable_slot_of(session, g_syms->sessRespondAsyncData, 24);
    if (respondSlot < 0) {
        logf_("  -> %s NOT refused: respondToAsyncMethodRequest(DataInfo) not "
              "in the session vtable", what);
        return;
    }
    void* const* svt = *(void* const* const*)(session);
    typedef int (*RespondAsyncFn)(void* self, uint32_t methodId, uint32_t ticket,
                                  int result, const IpmiDataInfo* out,
                                  uint32_t outCount);
    const IpmiDataInfo none = { NULL, 0 };
    const int rc = ((RespondAsyncFn)svt[respondSlot])(
        session, methodId, ticket, SM_ENV_IPMI_ENOTSUP, &none, 1);
    logf_("  -> %s refused, respondToAsyncMethodRequest(DataInfo) slot=%d "
          "ticket=%#x method=%#x rc=%#010x",
          what, respondSlot, ticket, methodId, (unsigned)rc);
}

static int64_t slot_dispatch(int slot, void* self, u64 a1, u64 a2, u64 a3, u64 a4,
                      u64 a5, u64 a6) {
    const SlotKind kind = (slot >= 0 && slot < kMaxSlots) ? g_kind[slot]
                                                          : KIND_UNKNOWN;

    switch (kind) {
        case KIND_DTOR:
            // Deliberately does not free anything: the object is static. The
            // deleting destructor (~D0) landing here would otherwise call
            // operator delete on a global.
            logf_("EVH slot[%#04x] ~EventHandler self=%p (no-op: object is static)",
                  slot * 8, self);
            return 0;

        case KIND_SYNC_DATAINFO: {
            IpmiSession* session      = (IpmiSession*)(a1);
            uint32_t       method       = (uint32_t)(a2);
            const IpmiDataInfo* in    = (const IpmiDataInfo*)(a3);
            uint32_t       inCount      = (uint32_t)(a4);
            IpmiOutBuffer* out        = (IpmiOutBuffer*)(a5);
            uint32_t       outCount     = (uint32_t)(a6);

            logf_("SYNC  slot[%#04x] session=%p method=%#x inCount=%u outCount=%u",
                  slot * 8, (void*)session, method, inCount, outCount);

            if (session && !g_sessionDumped) {
                g_sessionDumped = true;
                ipmi_dump_vtable(g_syms, session, "session", 24);
            }

            for (uint32_t i = 0; i < inCount && i < 8; i++) {
                logf_("  in[%u]  ptr=%p size=%zu", i, in ? in[i].data : NULL,
                      in ? in[i].size : 0);
                if (in && in[i].data) log_hexdump("       data", in[i].data, in[i].size);
            }
            // Zero `written` for every out entry, first. The framework leaves
            // it uninitialised in a buffer it reuses between commands, so a
            // command that returns without writing an out-param inherits the
            // previous one's length. Measured: an unhandled command with a
            // 4-byte out buffer responded with written=8 left over from the
            // previous one and the client was killed for the mismatch
            // (_ipmimgrRaiseException signo=0xa002031f opt64=0x18). Every path
            // out -- answered, refused or unhandled -- must leave a truthful
            // length.
            for (uint32_t i = 0; i < outCount && out; i++) out[i].written = 0;
            for (uint32_t i = 0; i < outCount && i < 8; i++) {
                logf_("  out[%u] ptr=%p capacity=%zu", i,
                      out ? out[i].data : NULL, out ? out[i].capacity : 0);
            }

            const int rc = sm_env_ipmi_dispatch(session, method, in, inCount,
                                              out, outCount);

            // Answer the request. Returning does not imply it: the framework
            // calls this vtable slot and then returns without replying itself,
            // and leaving a request unanswered gets the server killed
            // (_ipmimgrRaiseException signo=0xa0020320 opt32=0x02010006).
            // Writing into the out buffer is not sufficient on its own.
            // Responding comes before logging so the reply does not wait on
            // file I/O.
            int respondRc = 0;
            int respondSlot = -1;
            if (session && g_syms && g_syms->sessRespondSyncBuf) {
                respondSlot = ipmi_vtable_slot_of(session,
                                                  g_syms->sessRespondSyncBuf, 24);
                if (respondSlot >= 0) {
                    void* const* svt =
                        *(void* const* const*)(session);
                    typedef int (*RespondFn)(void* self, int result,
                                             const IpmiOutBuffer* out,
                                             uint32_t outCount);
                    respondRc = ((RespondFn)svt[respondSlot])(
                        session, rc, out, outCount);
                }
            }

            logf_("  -> rc=%#010x, out[0].written=%zu, "
                  "respondToSyncMethodRequest slot=%d rc=%#010x%s",
                  (unsigned)rc, (outCount && out) ? out[0].written : 0,
                  respondSlot,
                  (unsigned)respondRc,
                  respondSlot < 0 ? "   <-- NOT FOUND: the request is unanswered "
                                    "and the kernel will kill us" : "");
            return rc;
        }

        case KIND_SYNC_RAW: {
            // No command is served in this form: the descriptor form is what our
            // client sends and what a real sandboxed title has been served on,
            // and nothing has ever arrived here. A dispatch that did would mean
            // the wire shape is not what we measured, so it stays loud in the
            // log -- but it is still answered, because refusing by return value
            // is not refusing at all.
            IpmiSession* session = (IpmiSession*)(a1);
            logf_("SYNC-RAW slot[%#04x] session=%p method=%#x a3=%#lx a4=%#lx "
                  "a5=%#lx a6=%#lx -- not implemented, refusing",
                  slot * 8, (void*)a1, (unsigned)a2, (unsigned long)a3,
                  (unsigned long)a4, (unsigned long)a5, (unsigned long)a6);

            // Answer it. Returning does not, exactly as on the DataInfo slot:
            // in 4.03 libSceIpmi the only callers of the reply primitive are
            // the two respondToSyncMethodRequest overloads, so nothing replies
            // on our behalf and an unanswered request gets the server killed
            // (_ipmimgrRaiseException signo=0xa0020320 opt32=0x02010006). The
            // raw overload is a thin wrapper that packs (buf, len) into a
            // one-entry BufferInfo and calls that same primitive, so a null
            // buffer of length 0 is a complete reply -- and with no out entry
            // there is no `written` length to get wrong.
            int respondRc = 0;
            int respondSlot = -1;
            if (session && g_syms && g_syms->sessRespondSyncRaw) {
                respondSlot = ipmi_vtable_slot_of(session,
                                                  g_syms->sessRespondSyncRaw, 24);
                if (respondSlot >= 0) {
                    void* const* svt = *(void* const* const*)(session);
                    typedef int (*RespondRawFn)(void* self, int result,
                                                const void* buf, size_t len);
                    respondRc = ((RespondRawFn)svt[respondSlot])(
                        session, SM_ENV_IPMI_ENOTSUP, NULL, 0);
                }
            }
            logf_("  -> refused, respondToSyncMethodRequest(raw) slot=%d "
                  "rc=%#010x%s",
                  respondSlot, (unsigned)respondRc,
                  respondSlot < 0 ? "   <-- NOT FOUND: the request is unanswered "
                                    "and the kernel will kill us" : "");
            return SM_ENV_IPMI_ENOTSUP;
        }

        // Both async slots arrive as (Session*, ticket, methodId, ...) -- the
        // ticket FIRST, which is the reverse of the order the reply wants, and
        // the easy mistake to make. The return value is discarded: these slots
        // are declared void, so refusing has to be said on the wire.
        case KIND_ASYNC_DATAINFO: {
            const IpmiDataInfo* in = (const IpmiDataInfo*)(a4);
            uint32_t inCount         = (uint32_t)(a5);
            logf_("ASYNC slot[%#04x] session=%p ticket=%#x method=%#x inCount=%u",
                  slot * 8, (void*)a1, (unsigned)a2, (unsigned)a3, inCount);
            for (uint32_t i = 0; i < inCount && i < 8; i++) {
                logf_("  in[%u]  ptr=%p size=%zu", i, in ? in[i].data : NULL,
                      in ? in[i].size : 0);
            }
            respond_async_refusal((IpmiSession*)(a1), (uint32_t)a2, (uint32_t)a3,
                                  "ASYNC");
            return SM_ENV_IPMI_ENOTSUP;
        }

        case KIND_ASYNC_RAW:
            logf_("ASYNC-RAW slot[%#04x] session=%p ticket=%#x method=%#x "
                  "a4=%#lx a5=%#lx a6=%#lx -- not implemented, refusing",
                  slot * 8, (void*)a1, (unsigned)a2, (unsigned)a3,
                  (unsigned long)a4, (unsigned long)a5, (unsigned long)a6);
            respond_async_refusal((IpmiSession*)(a1), (uint32_t)a2, (uint32_t)a3,
                                  "ASYNC-RAW");
            return SM_ENV_IPMI_ENOTSUP;

        case KIND_SESSION_KILLED:
            logf_("SESSION KILLED slot[%#04x] session=%p", slot * 8, (void*)a1);
            session_slot_release((void*)a1);
            g_sessionDumped = false;      // next session dumps again
            return 0;

        default:
            // An unnamed slot, accepted rather than refused. Slot 0x20 is on
            // the connect path: it arrives as (this, Server*, Session::Config*,
            // void* extra) with memorySize seeded to a floor of 0x10, and
            // whatever it returns goes straight back to the connecting client.
            // While this refused, the client saw connect rc=0x8002000d
            // serviceResult=0x80d90009. A callback that gates the connection
            // has to succeed or nothing else ever runs; an unknown command is
            // the opposite, and is refused.
            //
            // Not hooking these is not the safer option either: 0x20 and 0x28
            // share one address we cannot name or vet. But only the connect
            // slot may be CAPTURED: the one beside it takes two arguments, so
            // capture_connect would write through a stale register. Returning 0
            // is all the rest need; teardown belongs to onSessionKilled.
            if (slot == g_connectSlot) capture_connect(slot, self, a1, a2, a3);
            return 0;
    }
}

// One thunk per slot so that the slot index is known without reading any
// per-call state -- the firmware tells us nothing about which slot it entered.
#define SLOT_THUNK(n)                                                        \
    static int64_t evh_slot##n(void* self, u64 a1, u64 a2, u64 a3,       \
                                   u64 a4, u64 a5, u64 a6) {                 \
        return slot_dispatch(n, self, a1, a2, a3, a4, a5, a6);               \
    }
SLOT_THUNK(0)  SLOT_THUNK(1)  SLOT_THUNK(2)  SLOT_THUNK(3)
SLOT_THUNK(4)  SLOT_THUNK(5)  SLOT_THUNK(6)  SLOT_THUNK(7)
SLOT_THUNK(8)  SLOT_THUNK(9)  SLOT_THUNK(10) SLOT_THUNK(11)
#undef SLOT_THUNK

static void* const kThunks[kMaxSlots] = {
    (void*)evh_slot0,  (void*)evh_slot1,  (void*)evh_slot2,  (void*)evh_slot3,
    (void*)evh_slot4,  (void*)evh_slot5,  (void*)evh_slot6,  (void*)evh_slot7,
    (void*)evh_slot8,  (void*)evh_slot9,  (void*)evh_slot10, (void*)evh_slot11,
};

// Bit per known EventHandler method, so that a slot whose address matches more
// than one of them (identical bodies folded to one address by the linker) can
// be reported as ambiguous rather than silently resolved to whichever we
// checked first.
enum {
    M_D1 = 1 << 0, M_D0 = 1 << 1, M_D2 = 1 << 2,
    M_SYNC_DI = 1 << 3, M_SYNC_RAW = 1 << 4,
    M_ASYNC_DI = 1 << 5, M_ASYNC_RAW = 1 << 6, M_KILLED = 1 << 7,
    M_DTORS = M_D1 | M_D0 | M_D2,
};

static unsigned match_mask(const IpmiSyms* s, const void* v) {
    unsigned m = 0;
    if (!v) return 0;
    if (v == s->evhD1)             m |= M_D1;
    if (v == s->evhD0)             m |= M_D0;
    if (v == s->evhD2)             m |= M_D2;
    if (v == s->evhSyncDataInfo)   m |= M_SYNC_DI;
    if (v == s->evhSyncRaw)        m |= M_SYNC_RAW;
    if (v == s->evhAsyncDataInfo)  m |= M_ASYNC_DI;
    if (v == s->evhAsyncRaw)       m |= M_ASYNC_RAW;
    if (v == s->evhSessionKilled)  m |= M_KILLED;
    return m;
}

static SlotKind kind_from_mask(unsigned m) {
    if (!m) return KIND_UNKNOWN;
    // Destructors routinely share one address (D1 and D2 are the same code),
    // so a mask that is entirely destructors is still an unambiguous answer.
    if ((m & ~(unsigned)M_DTORS) == 0) return KIND_DTOR;
    switch (m) {
        case M_SYNC_DI:    return KIND_SYNC_DATAINFO;
        case M_SYNC_RAW:   return KIND_SYNC_RAW;
        case M_ASYNC_DI:   return KIND_ASYNC_DATAINFO;
        case M_ASYNC_RAW:  return KIND_ASYNC_RAW;
        case M_KILLED:     return KIND_SESSION_KILLED;
        default:           return KIND_UNKNOWN;   // ambiguous: two names, one address
    }
}


void handler_drain_connect_log(void) {
    if (!g_connect.pending) return;
    g_connect.pending = false;

    // SessionImpl::Config: clientPid +0x00, maxOutstanding +0x08, sync in/out
    // size limits +0x10/+0x18, async maxOutstanding +0x20 and in/out
    // +0x28/+0x30, numEventFlag +0x38, numMsgQueue +0x40, memorySize +0x148.
    // The four size limits are the wall a command hits; nothing logged them.
    uint32_t clientPid = 0, maxOut = 0, maxOutAsync = 0;
    uint32_t numEventFlag = 0, numMsgQueue = 0;
    uint64_t inLimit = 0, outLimit = 0, inLimitAsync = 0, outLimitAsync = 0;
    memcpy(&clientPid,     g_connect.cfgHead + 0x00, 4);
    memcpy(&maxOut,        g_connect.cfgHead + 0x08, 4);
    memcpy(&inLimit,       g_connect.cfgHead + 0x10, 8);
    memcpy(&outLimit,      g_connect.cfgHead + 0x18, 8);
    memcpy(&maxOutAsync,   g_connect.cfgHead + 0x20, 4);
    memcpy(&inLimitAsync,  g_connect.cfgHead + 0x28, 8);
    memcpy(&outLimitAsync, g_connect.cfgHead + 0x30, 8);
    memcpy(&numEventFlag,  g_connect.cfgHead + 0x38, 4);
    memcpy(&numMsgQueue,   g_connect.cfgHead + 0x40, 4);

    logf_("CONNECT callback (drained) slot[%#04x] call#%u srv=%#lx cfg=%#lx "
          "extra=%#lx -- returned 0 with NO logging inside the callback",
          g_connect.slot * 8, g_connect.calls, (unsigned long)g_connect.srv,
          (unsigned long)g_connect.cfg, (unsigned long)g_connect.extra);
    logf_("  SessionImpl::Config clientPid=%u maxOutstanding=%u "
          "numEventFlag=%u numMsgQueue=%u memorySize=%#lx",
          clientPid, maxOut, numEventFlag, numMsgQueue,
          (unsigned long)g_connect.memorySize);
    logf_("  hard limits sync in=%#lx out=%#lx | async maxOutstanding=%u "
          "in=%#lx out=%#lx",
          (unsigned long)inLimit, (unsigned long)outLimit, maxOutAsync,
          (unsigned long)inLimitAsync, (unsigned long)outLimitAsync);
    logf_("  memorySize seeded %#lx -> set to %#lx before createSession",
          (unsigned long)g_connect.memorySize,
          (unsigned long)g_connect.memorySizeSet);
    logf_("  createSession slot=%d rc=%#010x session=%#lx%s",
          g_connect.createSessionSlot, (unsigned)g_connect.createSessionRc,
          (unsigned long)g_connect.session,
          g_connect.createSessionSlot < 0
              ? "   <-- NOT FOUND in the Server vtable"
              : (g_connect.session ? "   <-- a session exists now"
                                   : "   <-- no session was produced"));
    if (g_connect.noSessionSlot)
        logf_("  no free session buffer -- all %d in use, so this connect was "
              "refused rather than given a buffer another session is on",
              kSessionSlots);
}

HandlerBuild handler_build(const IpmiSyms* syms) {
    HandlerBuild out = {NULL, 0, false};
    g_syms = syms;

    if (!syms->evhVtable) {
        logf_("the EventHandler vtable symbol did not resolve; the layout cannot "
              "be measured and this does not guess it");
        return out;
    }

    void** ztv = (void**)(syms->evhVtable);

    // Locate the address point. A _ZTV symbol normally points at the start of
    // the vtable object -- offset-to-top, then typeinfo, then the methods -- so
    // the address point is +2 slots, but some toolchains export the address
    // point itself. Rather than assume either, find the first slot that is one
    // of the methods we resolved by name.
    int ap = -1;
    for (int i = 0; i < 6 && ap < 0; i++) {
        if (match_mask(syms, ztv[i])) ap = i;
    }
    if (ap < 0) {
        logf_("none of the exported EventHandler methods appears in the first six "
              "words at %p -- either the vtable symbol is not what we think, or "
              "every method folded to an address we did not resolve. Refusing to "
              "build a handler on that.", syms->evhVtable);
        for (int i = 0; i < 6; i++) logf_("  ztv[%#04x] = %p", i * 8, ztv[i]);
        return out;
    }
    logf_("EventHandler vtable %p, address point +%#x", syms->evhVtable, ap * 8);

    void** base = ztv + ap;

    // Length: run to the last slot that matches something we resolved. Stopping
    // at the first miss would truncate on an unexported virtual; running to a
    // fixed count would install a thunk over whatever follows the vtable.
    int slots = 0;
    unsigned seen = 0;
    for (int i = 0; i < kMaxSlots; i++) {
        const unsigned m = match_mask(syms, base[i]);
        if (m) { slots = i + 1; seen |= m; }
    }
    out.slotCount = slots;

    logf_("---- EventHandler vtable order, measured (%d slots)", slots);
    for (int i = 0; i < slots; i++) {
        const unsigned m = match_mask(syms, base[i]);
        g_kind[i] = kind_from_mask(m);
        const char* sym = ipmi_syms_name(syms, base[i]);
        logf_("  [%#04x] %p  %-32s%s", i * 8, base[i], kind_name(g_kind[i]),
              (g_kind[i] == KIND_UNKNOWN && sym) ? " (ambiguous address)" : "");
    }
    for (int i = slots; i < kMaxSlots; i++) g_kind[i] = KIND_UNKNOWN;

    // Claim the connect slot, and only that one. Anchored on BOTH sides, not
    // taken as the first unnamed slot: a symbol that fails to resolve leaves
    // its OWN slot unnamed, and first-unnamed would then hand connect to
    // whatever that was -- a destructor, say -- whose arguments capture_connect
    // would write through. Nothing is claimed unless the run of two unnamed
    // slots sits exactly between the async dispatch and onSessionKilled.
    for (int i = 0; i + 3 < slots; i++) {
        if (g_kind[i]     == KIND_ASYNC_DATAINFO &&
            g_kind[i + 1] == KIND_UNKNOWN &&
            g_kind[i + 2] == KIND_UNKNOWN &&
            g_kind[i + 3] == KIND_SESSION_KILLED) {
            g_connectSlot = i + 1;
            break;
        }
    }
    if (g_connectSlot < 0) {
        logf_("  the connect slot could not be identified: no unnamed pair sits "
              "between the async dispatch and onSessionKilled. Connections will "
              "be ACCEPTED but get no session, so every client sees 0x8002000d. "
              "Nothing is captured, which is the safe half of the failure.");
    } else {
        logf_("  connect slot taken as [%#04x], anchored between the async "
              "dispatch and onSessionKilled; every other unnamed slot returns 0 "
              "without being read", g_connectSlot * 8);
    }

    const unsigned wanted = M_SYNC_DI | M_SYNC_RAW | M_ASYNC_DI | M_ASYNC_RAW |
                            M_KILLED;
    if ((seen & wanted) != wanted) {
        logf_("  note: not every exported method was located in the vtable "
              "(seen=%#x wanted=%#x); unidentified slots log rather than "
              "answering", seen & wanted, wanted);
    }

    // Carry the offset-to-top and typeinfo across when the symbol pointed at
    // the start of the vtable object rather than at its address point. Nothing
    // here reads them, but the real pair is more honest than whatever happened
    // to precede the methods.
    g_vtable[0] = (ap >= 2) ? ztv[ap - 2] : NULL;
    g_vtable[1] = (ap >= 1) ? ztv[ap - 1] : NULL;
    for (int i = 0; i < slots && i < kMaxSlots; i++) g_vtable[2 + i] = kThunks[i];

    memset(&g_handler, 0, sizeof(g_handler));
    g_handler.vptr = &g_vtable[2];

    logf_("handler object=%p vptr=%p (offset-to-top=%p typeinfo=%p)",
          (void*)&g_handler, (void*)g_handler.vptr, g_vtable[0], g_vtable[1]);
    for (int i = 0; i < slots; i++) {
        logf_("  ours [%#04x] = %p  %s", i * 8, g_vtable[2 + i],
              kind_name(g_kind[i]));
    }

    for (int i = 0; i < slots; i++) {
        if (g_kind[i] == KIND_SYNC_DATAINFO) out.syncDispatchProven = true;
    }
    if (!out.syncDispatchProven) {
        logf_("the descriptor-form sync dispatch slot was not identified; the "
              "service would come up and log whatever arrives, but refuse every "
              "command");
    }

    out.handler = (IpmiEventHandler*)(&g_handler);
    return out;
}
