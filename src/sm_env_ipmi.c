// SPDX-License-Identifier: GPL-3.0-or-later
//
// The environment service: its identity, the one command it answers, and a
// lifecycle that never lets a registration outlive its dispatcher. The IPMI
// machinery it sits on is in ipmi_symbols.c, ipmi_client.c and ipmi_handler.c.

#include "ipmi.h"
#include "ipmi_client.h"
#include "ipmi_handler.h"
#include "ipmi_log.h"
#include "ipmi_symbols.h"
#include "sm_env_ipmi_dispatch.h"
#include "sm_kstuff.h"
#include "sm_kstuff_caps.h"
#include "sm_log.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

// ---------------------------------------------------------------------------
// The logging shim the IPMI files use. One logging system, not two.
// ---------------------------------------------------------------------------

void logf_(const char *fmt, ...) {
  char line[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  log_debug("  [ENVSVC] %s", line);
}

void log_hexdump(const char *label, const void *p, size_t n) {
  const unsigned char *b = (const unsigned char *)(p);
  char line[128];

  if (!b) {
    log_debug("  [ENVSVC] %s: (null)", label ? label : "");
    return;
  }
  for (size_t off = 0; off < n; off += 16) {
    size_t used = 0;
    for (size_t i = 0; i < 16 && off + i < n; i++) {
      const int wrote = snprintf(line + used, sizeof(line) - used, "%02x ",
                                 b[off + i]);
      if (wrote <= 0 || (size_t)wrote >= sizeof(line) - used)
        break;
      used += (size_t)wrote;
    }
    log_debug("  [ENVSVC] %s +%04zx  %s", label ? label : "", off, line);
  }
}

// tryDispatch takes the working buffer sized by estimateTempWorkingMemorySize()
// on the same Config. Omitting it kills the dispatcher thread while main carries
// on, leaving the service registered and unable to ever answer.
typedef int (*TryDispatchFn)(void *self, void *buf, uint64_t size);
typedef int (*DestroyFn)(void *self);

static IpmiSyms g_syms;
static bool g_syms_ok;
// A Server* in the real declaration; nothing here reaches it except through its
// vtable, so void* is the honest type. It points into g_srv_storage.
static void *g_srv;
static void *g_work_buf;
static uint64_t g_work_size;
static volatile bool g_disp_stop;
static volatile bool g_disp_alive;

// Poll tryDispatch, never runDispatcher. runDispatcher checks its shutdown flag
// only before each receive, so one already asleep in receivePacket survives
// SIGKILL, keeps the name, and blocks every client forever.
static const unsigned kPollMs = 10;

static void *dispatcher_thread(void *arg) {
  (void)arg;

  // Never name this thread. thr_set_name writes p_comm, the process name, so a
  // worker naming itself renames the whole payload -- it stops being findable as
  // shadowmountplus.elf, and you cannot kill what you cannot find.
  TryDispatchFn tryDispatch = (TryDispatchFn)g_syms.srvTryDispatch;

  g_disp_alive = true;
  if (!tryDispatch) {
    logf_("no tryDispatch symbol; refusing to fall back to runDispatcher -- an "
          "unkillable process is worse than an unserved one");
    g_disp_alive = false;
    return NULL;
  }

  logf_("polling tryDispatch every %ums", kPollMs);
  while (!g_disp_stop) {
    const int rc = tryDispatch(g_srv, g_work_buf, g_work_size);
    if (rc != 0) {
      logf_("tryDispatch rc=%#010x -- stopping", (unsigned)rc);
      break;
    }
    // Print whatever the connect callback captured. It cannot log from inside
    // the connection window itself.
    handler_drain_connect_log();
    usleep(kPollMs * 1000);
  }

  g_disp_alive = false;
  return NULL;
}

// The probe runs on its own thread because connect() can block forever against
// a wedged predecessor and libSceIpmi's connect has no timeout. A probe that
// does not report back counts as held: creating on a held name kills us.
typedef struct ProbeResult {
  volatile bool done;
  volatile bool connected;
} ProbeResult;
static ProbeResult g_probe = {false, false};

static void *probe_thread(void *arg) {
  (void)arg;

  IpmiClient c;
  if (ipmi_client_open(&c, &g_syms, SMP_ENV_IPMI_SERVICE, false))
    g_probe.connected = ipmi_client_connect(&c, SMP_ENV_IPMI_SERVICE);
  ipmi_client_close(&c);
  g_probe.done = true;
  return NULL;
}

static const int kProbeTimeoutSecs = 5;

// -> true when the name is ours to take.
static bool name_is_free(void) {
  g_probe.done = g_probe.connected = false;

  pthread_t pt;
  if (pthread_create(&pt, NULL, probe_thread, NULL) != 0) {
    logf_("could not start the probe thread -- assuming the name is held");
    return false;
  }
  pthread_detach(pt);

  for (int i = 0; i < kProbeTimeoutSecs && !g_probe.done; i++)
    sleep(1);

  if (!g_probe.done) {
    logf_("connect to " SMP_ENV_IPMI_SERVICE " did not return in %ds. A "
          "predecessor is wedged -- it holds the name, cannot be killed, and "
          "every client that connects to it blocks forever. Reboot the console; "
          "nothing this payload can do will clear it.",
          kProbeTimeoutSecs);
    return false;
  }

  if (g_probe.connected) {
    logf_("another instance already holds " SMP_ENV_IPMI_SERVICE
          " -- not registering. Creating a server on a held name does not fail, "
          "it kills this process from inside create(). Stop the old "
          "ShadowMountPlus and start this one again.");
    return false;
  }

  logf_("nobody holds " SMP_ENV_IPMI_SERVICE " -- the name is ours");
  return true;
}

// The invariant: a registration must never outlive its dispatcher. Every exit
// path comes through here, including the failure paths inside serve() itself.
static void server_teardown(const char *why) {
  void *const srv = g_srv;

  if (!srv)
    return;
  g_srv = NULL; // before the calls: nothing may reuse it

  // Ask, then wait for the dispatcher to actually be out. Destroying the server
  // while tryDispatch is inside it is a use-after-free, and the window is a
  // whole poll interval wide.
  g_disp_stop = true;
  for (int i = 0; i < 200 && g_disp_alive; i++)
    usleep(10 * 1000);

  if (g_disp_alive) {
    // Do not destroy. destroy() succeeds only on status == 0; a dispatch in
    // flight either refuses or wedges this process unkillably. Leaving it
    // undestroyed is harmless -- process exit releases the name.
    logf_("%s: dispatcher is still inside tryDispatch -- not destroying; "
          "process exit will release the name",
          why);
    return;
  }

  // Resolve destroy by address in this object's own vtable: the slot is what
  // the instance actually implements. shutdownDispatcher is deliberately not
  // called -- it sets status |= 4, which only one more tryDispatch would clear,
  // and destroy would then refuse and leave the name held.
  void *const *vt = *(void *const *const *)(srv);
  const int dsSlot =
      g_syms.srvDestroy ? ipmi_vtable_slot_of(srv, g_syms.srvDestroy, 24) : -1;
  if (dsSlot >= 0)
    (void)((DestroyFn)vt[dsSlot])(srv);
  else
    logf_("no destroy slot resolved -- the name may still be held. If the next "
          "start dies inside create(), reboot.");

  free(g_work_buf);
  g_work_buf = NULL;
  logf_("%s: " SMP_ENV_IPMI_SERVICE " destroyed (slot=%d)", why, dsSlot);
}


// ---------------------------------------------------------------------------
// The one command.
// ---------------------------------------------------------------------------

int sm_env_ipmi_dispatch(IpmiSession *session, uint32_t method,
                         const IpmiDataInfo *in, uint32_t inCount,
                         IpmiOutBuffer *out, uint32_t outCount) {
  (void)session;
  (void)in;
  (void)inCount;

  if (method != SMP_ENV_IPMI_CMD_QUERY) {
    logf_("unknown method %#x -- refusing", method);
    return SM_ENV_IPMI_ENOTSUP;
  }

  if (outCount < 1 || !out || !out[0].data ||
      out[0].capacity < sizeof(SmpEnvReply)) {
    logf_("QUERY with no room for the reply (outCount=%u capacity=%zu, need "
          "%zu) -- refusing",
          outCount, (outCount && out) ? out[0].capacity : 0,
          sizeof(SmpEnvReply));
    return SM_ENV_IPMI_ENOTSUP;
  }

  SmpEnvReply reply;
  memset(&reply, 0, sizeof(reply));
  reply.reply_version = SMP_ENV_REPLY_VERSION;
  // Both forms of our own version: the comparable one a gate tests against, and
  // the git tag a human reads. Sending both makes a forgotten
  // SMP_ENV_SMP_VERSION bump visible instead of silent.
  reply.smp_version_num = SMP_ENV_SMP_VERSION;
  (void)strlcpy(reply.smp_version, SHADOWMOUNT_VERSION,
                sizeof(reply.smp_version));

  // Is kstuff loaded, and is it on right now.
  if (sm_kstuff_is_supported()) {
    reply.flags |= SMP_ENV_FLAG_KSTUFF_PRESENT;
    if (sm_kstuff_is_enabled())
      reply.flags |= SMP_ENV_FLAG_KSTUFF_ENABLED;
  }

  // What it actually patched, read out of ShellCore's live text. Without
  // CAPS_VALID the capabilities are unknown, not absent.
  uint32_t caps = 0u;
  if (sm_kstuff_probe_caps(&caps)) {
    reply.kstuff_caps = caps;
    reply.flags |= SMP_ENV_FLAG_CAPS_VALID;
  }

  memcpy(out[0].data, &reply, sizeof(reply));

  // A truthful length, always. The framework reuses the out buffer and leaves
  // `written` uninitialised; respondToSyncMethodRequest reads it and IPMIMGR
  // kills the client over a mismatch.
  out[0].written = sizeof(reply);

  logf_("QUERY -> flags=%#x caps=%#x smp=%s", (unsigned)reply.flags,
        (unsigned)reply.kstuff_caps, reply.smp_version);
  return 0;
}

// ---------------------------------------------------------------------------
// Lifecycle.
// ---------------------------------------------------------------------------

bool sm_env_ipmi_serve(void) {
  _Static_assert(sizeof(SMP_ENV_IPMI_SERVICE) <= 14,
                 "Config::name is char[16] with the NUL; the client and server "
                 "halves truncate differently, so 14 is the budget");
  _Static_assert(sizeof(SmpEnvReply) == 48,
                 "SmpEnvReply crosses a process boundary as raw bytes; "
                 "every client's copy must agree byte for byte");

  if (g_srv)
    return true;

  if (!g_syms_ok && !(g_syms_ok = ipmi_syms_resolve(&g_syms))) {
    logf_("libSceIpmi is unreadable -- not registering");
    return false;
  }

  if (!name_is_free())
    return false;

  const HandlerBuild build = handler_build(&g_syms);
  if (!build.handler) {
    logf_("EventHandler layout not measurable -- refusing to guess it");
    return false;
  }
  if (!build.syncDispatchProven) {
    logf_("the sync-dispatch slot was not proven; the service would register "
          "but never serve. Refusing -- a name held by something that cannot "
          "answer is worse than no service at all.");
    return false;
  }

  // Zeroed before the constructor runs, which is also what keeps cfg.gate32 and
  // cfg.gate33 at 0 -- create() reads both. See IpmiServerConfig.
  static IpmiServerConfig cfg;
  memset(&cfg, 0, sizeof(cfg));
  ipmi_server_config_ctor(&cfg);
  cfg.poolSize = 0x20000;           // known-good pool size
  cfg.eventHandler = build.handler; // +0x10, load-bearing: null gives EINVAL
  cfg.flag = 1;
  memset(cfg.name, 0, sizeof(cfg.name));
  strncpy(cfg.name, SMP_ENV_IPMI_SERVICE, sizeof(cfg.name) - 1);

  // Not scratch: create() constructs the ServerImpl into this and returns it as
  // the Server*, so it has to outlive the registration -- hence static. The
  // object measures 0x30 bytes; this is roomy on purpose, since being short
  // here would corrupt the server itself.
  static unsigned char g_srv_storage[0x1000];
  memset(g_srv_storage, 0, sizeof(g_srv_storage));

  logf_("calling Server::create for " SMP_ENV_IPMI_SERVICE " -- if the log "
        "stops here with an IPMIMGR exception, the name was taken after all");
  const int rc = ipmi_server_create(&g_srv, &cfg, NULL, g_srv_storage);
  logf_("Server::create -> rc=%#010x srv=%p", rc, (void *)g_srv);
  if (rc < 0 || !g_srv) {
    g_srv = NULL;
    return false;
  }

  // Size the working buffer off the same Config that built the server;
  // measured 0x20100. The return type is not published, so an implausible value
  // is a decoding problem, not a genuine request for that much memory.
  const uint64_t rawEstimate = ipmi_server_config_estimate(&cfg);
  g_work_size = rawEstimate;
  if (g_work_size == 0 || g_work_size > 0x1000000u)
    g_work_size = rawEstimate & 0xffffffffu;
  if (g_work_size == 0 || g_work_size > 0x1000000u) {
    logf_("estimateTempWorkingMemorySize returned %#llx, implausible -- using "
          "0x20000",
          (unsigned long long)rawEstimate);
    g_work_size = 0x20000;
  }

  g_work_buf = malloc(g_work_size);
  if (!g_work_buf) {
    logf_("could not allocate %#llx bytes for the dispatcher",
          (unsigned long long)g_work_size);
    server_teardown("no working buffer");
    return false;
  }
  memset(g_work_buf, 0, g_work_size);

  g_disp_stop = false;
  pthread_t th;
  if (pthread_create(&th, NULL, dispatcher_thread, NULL) != 0) {
    logf_("could not start the dispatcher thread -- unregistering rather than "
          "holding a name nothing will answer");
    server_teardown("dispatcher thread would not start");
    return false;
  }
  pthread_detach(th);

  logf_(SMP_ENV_IPMI_SERVICE " registered and serving");
  return true;
}

void sm_env_ipmi_shutdown(void) {
  server_teardown("shutdown");
}
