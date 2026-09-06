// SPDX-License-Identifier: GPL-3.0-or-later
//
// The IPMI server interface. There is no SDK header for it -- sys/ipmi.h is
// FreeBSD's BMC driver header and unrelated -- so the types are declared here,
// and every offset and size in them was confirmed against a live, working
// registration on hardware.
//
// EventHandler is deliberately not declared as a class. The signatures of its
// methods are known but their order is not, so a hand-written subclass would be
// a guess at the vtable layout. ipmi_symbols.h resolves the base vtable and each
// method by name, and ipmi_handler.c identifies the slots by address.

#pragma once

#include <stddef.h>
#include <stdint.h>

// A {pointer, length} pair. The client builds an array of these and hands it to
// invokeSyncMethod; the server receives the same array. Confirmed from the
// client marshal in libSceAppContent (0x1300) and from a working client.
typedef struct IpmiDataInfo {
    const void *data;
    size_t      size;
} IpmiDataInfo;

typedef struct IpmiBufferInfo {
    void  *data;
    size_t size;
} IpmiBufferInfo;

// The server-side out-argument entry: 24 bytes, not 16. The in and out argument
// arrays a dispatch receives do not share a stride.
//
// The framework fills only `data` and `capacity` and leaves `written`
// uninitialised for the handler. Forgetting it is fatal: the first command with
// an out-arg died inside respondToSyncMethodRequest reading stack garbage as a
// length (IPMIMGR signo=0xa0020320 opt32=0x0232000a).
//
// The client side uses a 16-byte {ptr,size} instead. They are separate
// in-process structs and need not match.
typedef struct IpmiOutBuffer {
    void  *data;
    size_t capacity;
    size_t written;
} IpmiOutBuffer;

// Opaque on purpose. We only ever hold pointers to these and call through the
// vtable slots resolved by address; nothing reads a field.
typedef struct IpmiSession IpmiSession;
typedef struct IpmiEventHandler IpmiEventHandler;

// IPMI::Server::Config -- 0x38 bytes. Offsets are load-bearing; do not reorder.
typedef struct IpmiServerConfig {
    uint64_t unknown00;             // +0x00 ctor writes 0xf00, never overwritten
    uint64_t poolSize;              // +0x08 0x20000 is a known-good value
    // +0x10 the event handler. create() returns EINVAL with this null and
    // succeeds with an EventHandler* here, which is why create() takes no
    // handler argument: the handler travels in the Config.
    IpmiEventHandler *eventHandler;
    uint8_t  flag;                  // +0x18 must be 1
    char     name[16];              // +0x19 service name, NUL padded
    // The tail is byte arrays, not scalars: `name` ends at the unaligned offset
    // 0x29, so a uint64_t there is aligned up to 0x30 and silently grows the
    // struct to 0x40. The assertion below caught exactly that.
    uint8_t  reserved29[8];         // +0x29 written 0
    uint8_t  reserved31;            // +0x31 written 0
    // create() reads both of these. gate32 must be zero: it is copied to
    // ServerImpl+0x28, and tryDispatch refuses to run on a non-zero value.
    // gate33 is only consulted when `flag` is zero. Measured 0/0 in a working
    // registration; zeroing the whole Config keeps them that way.
    uint8_t  gate32;                // +0x32
    uint8_t  gate33;                // +0x33
    uint8_t  pad[4];                // to 0x38
} IpmiServerConfig;

_Static_assert(sizeof(IpmiServerConfig) == 0x38,
               "Config layout is fixed by the ABI; do not resize or reorder");
_Static_assert(sizeof(IpmiOutBuffer) == 24,
               "server out-arg stride is 0x18, confirmed on hardware");
_Static_assert(offsetof(IpmiServerConfig, gate32) == 0x32,
               "create() reads this byte; ServerImpl+0x28 gates tryDispatch");

// Config's constructor, which writes 0xf00 to +0x00. Reached through an
// asm-labelled declaration so it can be re-run on the same storage.
void ipmi_server_config_ctor(IpmiServerConfig *cfg)
    __asm__("_ZN4IPMI6Server6ConfigC1Ev");

// Sizes the working buffer the dispatcher wants; measured 0x20100. Called after
// create(), and the result allocated, before dispatching.
// Returns uint64_t rather than size_t deliberately: if the firmware returns a
// 32-bit value the upper half of RAX is undefined, and the caller checks for
// exactly that rather than trusting it.
uint64_t ipmi_server_config_estimate(const IpmiServerConfig *cfg)
    __asm__("_ZNK4IPMI6Server6Config29estimateTempWorkingMemorySizeEv");

// create(&out, &cfg, NULL, storage).
//
// `storage` is NOT scratch: create() placement-constructs a ServerImpl into it
// and returns that same pointer as the Server*, so it must outlive the server.
// Measured -- out == storage, and the object occupies 0x30 bytes: vtable +0x00,
// serverKid +0x08, mutex +0x10, status +0x18, temp buffer +0x20, gate +0x28.
// `out` is a Server** in the real declaration; void** here because nothing
// dereferences a Server except through its vtable.
int ipmi_server_create(void **out, const IpmiServerConfig *cfg, void *p3,
                       void *initBuf)
    __asm__("_ZN4IPMI6Server6createEPPS0_PKNS0_6ConfigEPvS6_");
