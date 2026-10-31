/*  Clock wrapper/retail ABIs adapted from NeoEnabler. */
#include "sm_platform.h"

#include <pthread.h>
#include <sys/mman.h>

#include "sm_config_mount.h"
#include "sm_kstuff.h"
#include "sm_log.h"
#include "sm_ps4_profiles.h"
#include "sm_shellcore_remote.h"

typedef struct {
  uint16_t firmware;
  uint32_t clock_site, clock_original, clock_slot, pro_slot;
  uint32_t pro_site, pro_original;
  uint16_t attribute;
  uint8_t three_args, mode_offset, size, four_args;
  uint8_t before[16], after[16], entry[32], call[5];
  uint8_t pro_before[16], pro_after[3], pro_call[5], pro_entry[16];
} ps4_profile_t;

static const ps4_profile_t g_profiles[] = {
#include "sm_ps4_profiles.inc"
};

extern const uint8_t sm_ps4_clock_start[];
extern const uint8_t sm_ps4_clock_end[];
extern const uint8_t sm_ps4_clock_original[];
extern const uint8_t sm_ps4_clock_policy[];
extern const uint8_t sm_ps4_clock_three_args[];
extern const uint8_t sm_ps4_clock_copy_size[];
extern const uint8_t sm_ps4_clock_mode_offset[];
extern const uint8_t sm_ps4_pro_start[];
extern const uint8_t sm_ps4_pro_end[];
extern const uint8_t sm_ps4_pro_original[];
extern const uint8_t sm_ps4_pro_policy[];
extern const uint8_t sm_ps4_pro_attribute[];
extern const uint8_t sm_ps4_pro_four_args[];
extern const uint8_t sm_ps4_pro_title[];
extern const uint8_t sm_ps4_pro_clock_policy[];

#define CLOCK_SLOT_SIZE 256u
#define PRO_SLOT_SIZE 160u

typedef struct {
  bool attached;
  bool blocked;
  uint64_t policy;
  uint8_t original[5];
} ps4_hook_t;

/* The hook owner installs/removes code. The existing launch service only
 * publishes aligned, one-byte policy values; it never attaches ShellCore.
 * Its mutex is independent of g_install_mutex, which can be held by an RPC. */
static pthread_mutex_t g_ps4_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
  pid_t pid;
  uintptr_t base;
  uint64_t config_revision;
  bool stopped_on_error;
  unsigned rollback_hook; /* 0: detach only, 1: clock CALL, 2: Pro CALL. */
  const ps4_profile_t *profile;
  ps4_hook_t hooks[2];
} g_ps4;

static size_t symbol_offset(const uint8_t *symbol, const uint8_t *start) {
  return (uintptr_t)symbol - (uintptr_t)start;
}

static bool read_bytes(uintptr_t address, void *buffer, size_t size) {
  return sm_remote_process_read(g_ps4.pid, address, buffer, size);
}

static bool write_bytes(uintptr_t address, const void *buffer, size_t size) {
  uint8_t actual[CLOCK_SLOT_SIZE];
  return size <= sizeof(actual) &&
         sm_remote_process_write(g_ps4.pid, address, buffer, size) &&
         read_bytes(address, actual, size) && !memcmp(actual, buffer, size);
}

static bool make_call(uint8_t call[5], uintptr_t site, uintptr_t target) {
  int64_t distance = (int64_t)target - (int64_t)site - 5;
  if (distance < INT32_MIN || distance > INT32_MAX)
    return false;
  int32_t relative = (int32_t)distance;
  call[0] = 0xe8;
  memcpy(call + 1, &relative, sizeof(relative));
  return true;
}

static uintptr_t call_target(const uint8_t call[5], uintptr_t site) {
  int32_t relative;
  if (call[0] != 0xe8)
    return 0;
  memcpy(&relative, call + 1, sizeof(relative));
  return site + 5 + (intptr_t)relative;
}

static bool original_target_valid(uintptr_t target, bool pro) {
  const ps4_profile_t *p = g_ps4.profile;
  uint8_t entry[32];
  uintptr_t stock = g_ps4.base + (pro ? p->pro_original : p->clock_original);
  size_t size = pro ? sizeof(p->pro_entry) : sizeof(p->entry);
  if (target == stock)
    return read_bytes(target, entry, size) &&
           !memcmp(entry, pro ? p->pro_entry : p->entry, size);
  /* Preserve a prior SFO wrapper (e.g. ps-patch-system) instead of bypassing
   * it. The call context must still match the audited SFO ABI; never chain
   * back into either of our own reservations. Clock calls require stock. */
  if (!pro || target < 0x10000 || target >= UINT64_C(0x800000000000))
    return false;
  int protection = kernel_get_vmem_protection(g_ps4.pid,
      target & ~(uintptr_t)0x3fff, 0x4000);
  return protection >= 0 && (protection & PROT_EXEC) != 0 &&
         !(target >= g_ps4.base + p->clock_slot &&
           target < g_ps4.base + p->clock_slot + CLOCK_SLOT_SIZE) &&
         !(target >= g_ps4.base + p->pro_slot &&
           target < g_ps4.base + p->pro_slot + PRO_SLOT_SIZE) &&
         read_bytes(target, entry, size);
}

static bool context_valid(bool pro) {
  const ps4_profile_t *p = g_ps4.profile;
  uintptr_t site = g_ps4.base + (pro ? p->pro_site : p->clock_site);
  uint8_t before[16], after[16];
  size_t after_size = pro ? sizeof(p->pro_after) : sizeof(p->after);
  return read_bytes(site - 16, before, sizeof(before)) &&
         !memcmp(before, pro ? p->pro_before : p->before, sizeof(before)) &&
         read_bytes(site + 5, after, after_size) &&
         !memcmp(after, pro ? p->pro_after : p->after, after_size);
}

static void populate_code(bool pro, uintptr_t original,
                          uint8_t code[CLOCK_SLOT_SIZE]) {
  const ps4_profile_t *p = g_ps4.profile;
  const uint8_t *start = pro ? sm_ps4_pro_start : sm_ps4_clock_start;
  size_t size = (uintptr_t)(pro ? sm_ps4_pro_end : sm_ps4_clock_end) -
                (uintptr_t)start;
  memset(code, 0, CLOCK_SLOT_SIZE);
  memcpy(code, start, size);
  memcpy(code + symbol_offset(pro ? sm_ps4_pro_original : sm_ps4_clock_original,
                               start), &original, sizeof(original));
  if (pro) {
    uint32_t attribute = p->attribute;
    memcpy(code + symbol_offset(sm_ps4_pro_attribute, start), &attribute, 4);
    code[symbol_offset(sm_ps4_pro_four_args, start)] = p->four_args;
    uintptr_t clock_policy = g_ps4.base + p->clock_slot +
        symbol_offset(sm_ps4_clock_policy, sm_ps4_clock_start);
    memcpy(code + symbol_offset(sm_ps4_pro_clock_policy, start), &clock_policy, 8);
  } else {
    uint32_t size = p->size;
    memcpy(code + symbol_offset(sm_ps4_clock_copy_size, start), &size, 4);
    code[symbol_offset(sm_ps4_clock_mode_offset, start)] = p->mode_offset;
    code[symbol_offset(sm_ps4_clock_three_args, start)] = p->three_args;
  }
}

static bool code_matches(bool pro, const uint8_t actual[CLOCK_SLOT_SIZE],
                         uintptr_t original) {
  uint8_t expected[CLOCK_SLOT_SIZE];
  populate_code(pro, original, expected);
  const uint8_t *start = pro ? sm_ps4_pro_start : sm_ps4_clock_start;
  size_t policy = symbol_offset(pro ? sm_ps4_pro_policy : sm_ps4_clock_policy,
                                start);
  memcpy(expected + policy, actual + policy, 1);
  if (pro) {
    size_t title = symbol_offset(sm_ps4_pro_title, start);
    memcpy(expected + title, actual + title, 9);
  }
  return !memcmp(actual, expected, pro ? PRO_SLOT_SIZE : CLOCK_SLOT_SIZE);
}

static bool sync_hook(bool pro, uint64_t policy, bool enabled, bool *attached) {
  ps4_hook_t *hook = &g_ps4.hooks[pro ? 1 : 0];
  if (g_ps4.stopped_on_error || hook->blocked)
    return false;
  const ps4_profile_t *p = g_ps4.profile;
  uintptr_t site = g_ps4.base + (pro ? p->pro_site : p->clock_site);
  uintptr_t slot = g_ps4.base + (pro ? p->pro_slot : p->clock_slot);
  size_t size = pro ? PRO_SLOT_SIZE : CLOCK_SLOT_SIZE;
  const uint8_t *start = pro ? sm_ps4_pro_start : sm_ps4_clock_start;
  size_t policy_offset = symbol_offset(pro ? sm_ps4_pro_policy : sm_ps4_clock_policy,
                                       start);
  uint8_t flags = (uint8_t)policy;
  uint8_t redirect[5], call[5];
  if (!make_call(redirect, site, slot) || !read_bytes(site, call, sizeof(call)))
    goto failed;
  bool ours = !memcmp(call, redirect, sizeof(call));
  if (!ours && !hook->attached && !enabled)
    return true;
  if (ours && hook->attached && hook->policy == policy && enabled)
    return true;
  if (!*attached) {
    if (!sm_remote_process_attach(g_ps4.pid))
      goto failed;
    *attached = true;
  }

  bool ok = false;
  bool call_attempted = false;
  uint8_t actual[CLOCK_SLOT_SIZE] = {0};
  uint8_t code[CLOCK_SLOT_SIZE];
  uint8_t original_call[5];
  uintptr_t original = 0;
  if (!read_bytes(site, call, sizeof(call)) || !context_valid(pro) ||
      !read_bytes(slot, actual, size))
    goto done;
  ours = !memcmp(call, redirect, sizeof(call));
  if (ours) {
    size_t offset = symbol_offset(pro ? sm_ps4_pro_original : sm_ps4_clock_original,
                                  start);
    memcpy(&original, actual + offset, sizeof(original));
    if (!code_matches(pro, actual, original) ||
        !make_call(original_call, site, original) ||
        (hook->attached && memcmp(original_call, hook->original, 5)) ||
        ((!hook->attached || enabled) &&
         !original_target_valid(original, pro)))
      goto done;
  } else {
    if (hook->attached && memcmp(call, hook->original, sizeof(call))) {
      /* Another owner may have wrapped our CALL. Disable our retained
       * policy, but never restore bytes over that owner's instructions. */
      uintptr_t previous = call_target(hook->original, site);
      const uint8_t zero = 0;
      if (code_matches(pro, actual, previous) &&
          write_bytes(slot + policy_offset, &zero, sizeof(zero)))
        hook->policy = 0;
      goto done;
    }
    original = call_target(call, site);
    if (enabled && !original_target_valid(original, pro))
      goto done;
    memcpy(original_call, call, sizeof(call));
    bool empty = true;
    for (size_t i = 0; i < size; ++i)
      empty = empty && actual[i] == 0;
    if (!empty && !code_matches(pro, actual, original))
      goto done;
    if (!enabled) {
      const uint8_t zero = 0;
      if (!empty && !write_bytes(slot + policy_offset, &zero, sizeof(zero)))
        goto done;
      ok = true;
      hook->attached = false;
      hook->policy = 0;
      goto done;
    }
    /* The entire reservation lies in audited mapped segment padding. Keep
     * code and patched pages for ShellCore's lifetime, including after stop. */
    if (!sm_kstuff_remote_mprotect(g_ps4.pid, slot & ~(uintptr_t)0x3fff,
                                   0x4000, PROT_READ | PROT_WRITE | PROT_EXEC) ||
        ((kernel_get_fw_version() >> 16) > 0x0820u &&
         (!sm_kstuff_remote_mlock(g_ps4.pid, slot, size) ||
          !sm_kstuff_remote_mlock(g_ps4.pid, site, sizeof(call)))))
      goto done;
    if (empty) {
      populate_code(pro, original, code);
      code[policy_offset] = flags;
      if (!write_bytes(slot, code, size)) {
        /* No CALL points here yet: undo our first write, rather than leave
         * a partial image that all later attempts must treat as foreign. */
        uint8_t zero[CLOCK_SLOT_SIZE] = {0};
        if (!write_bytes(slot, zero, size))
          log_debug("  [PS4] %s unused reservation rollback failed",
                    pro ? "Pro" : "clock");
        goto done;
      }
      memcpy(actual, code, size);
    }
  }
  memcpy(hook->original, original_call, sizeof(original_call));
  /* Publish rollback ownership before any possibly partial site write. */
  hook->attached = ours;
  if (actual[policy_offset] != flags &&
      !write_bytes(slot + policy_offset, &flags, sizeof(flags)))
    goto rollback;
  if (!ours && enabled) {
    call_attempted = true;
    if (!write_bytes(site, redirect, sizeof(redirect)))
      goto rollback;
    hook->attached = true;
  }
  if (!enabled && ours) {
    call_attempted = true;
    if (!write_bytes(site, original_call, sizeof(original_call)))
      goto rollback;
    hook->attached = false;
  }
  hook->policy = policy;
  ok = true;
  goto done;

rollback: {
    const uint8_t zero = 0;
    bool disarmed = write_bytes(slot + policy_offset, &zero, sizeof(zero));
    bool restored = !call_attempted ||
                    write_bytes(site, original_call, sizeof(original_call));
    hook->attached = !restored || (ours && !call_attempted);
    hook->policy = 0;
    if (!restored) {
      /* The CALL may contain a torn displacement. Do not let ShellCore
       * execute unconfirmed instructions. Shutdown can retry the owned
       * rollback once; otherwise the console needs a reboot. */
      g_ps4.stopped_on_error = true;
      g_ps4.rollback_hook = pro ? 2u : 1u;
      log_debug("  [PS4] CRITICAL: %s CALL rollback failed; ShellCore kept "
                "stopped, reboot required", pro ? "Pro" : "clock");
    } else if (!disarmed) {
      log_debug("  [PS4] %s policy disarm unconfirmed; restart ShellCore "
                "before retry", pro ? "Pro" : "clock");
    }
  }
done:
  if (ok) {
    log_debug("  [PS4] %s policy=%#lx", pro ? "Pro" : "clock",
              (unsigned long)policy);
    return true;
  }
failed:
  hook->blocked = true;
  log_debug("  [PS4] %s update failed; policy change or ShellCore restart "
            "required", pro ? "Pro" : "clock");
  return false;
}

static bool disarm_owned_policy(bool pro);
static bool owned_code_matches(bool pro);

static void sync_policies(bool enabled,
                          uint64_t clocks, uint64_t guard_policy) {
  bool attached = false;
  bool clock_ok = sync_hook(false, clocks, enabled, &attached);
  // The SFO guard also protects CPU-only policies from a stale/missed launch.
  // Keep the guard for a verified owned clock wrapper if its data write
  // fails. A foreign clock reservation must not retain our SFO writer.
  if (!clock_ok && !disarm_owned_policy(false) && owned_code_matches(false)) {
    (void)disarm_owned_policy(true);
  } else if (!sync_hook(true, guard_policy,
                        enabled && g_ps4.hooks[0].attached && !g_ps4.hooks[0].blocked,
                        &attached)) {
    (void)disarm_owned_policy(false);
  }
  if (attached && !g_ps4.stopped_on_error &&
      !sm_remote_process_detach(g_ps4.pid) &&
      !sm_remote_process_detach(g_ps4.pid)) {
    /* Keep ownership even if both policies were applied successfully. */
    g_ps4.stopped_on_error = true;
    g_ps4.rollback_hook = 0;
    log_debug("  [PS4] CRITICAL: ShellCore detach failed; stopped process "
              "retained for shutdown recovery");
  }
}

static bool publish_policy_byte(uintptr_t address, uint8_t value) {
  uint8_t actual;
  if (read_bytes(address, &actual, 1) && actual == value)
    return true;
  (void)sm_remote_process_write(g_ps4.pid, address, &value, 1);
  return read_bytes(address, &actual, 1) && actual == value;
}

static bool owned_code_matches(bool pro) {
  const ps4_profile_t *p = g_ps4.profile;
  uintptr_t slot = g_ps4.base + (pro ? p->pro_slot : p->clock_slot);
  uintptr_t site = g_ps4.base + (pro ? p->pro_site : p->clock_site);
  uint8_t code[CLOCK_SLOT_SIZE] = {0};
  return read_bytes(slot, code, pro ? PRO_SLOT_SIZE : CLOCK_SLOT_SIZE) &&
         code_matches(pro, code, call_target(g_ps4.hooks[pro ? 1 : 0].original, site));
}

static bool disarm_owned_policy(bool pro) {
  ps4_hook_t *hook = &g_ps4.hooks[pro ? 1 : 0];
  if (!hook->attached)
    return true;
  const ps4_profile_t *p = g_ps4.profile;
  uintptr_t slot = g_ps4.base + (pro ? p->pro_slot : p->clock_slot);
  size_t offset = pro ? symbol_offset(sm_ps4_pro_policy, sm_ps4_pro_start)
                      : symbol_offset(sm_ps4_clock_policy, sm_ps4_clock_start);
  if (!owned_code_matches(pro) || !publish_policy_byte(slot + offset, 0))
    return false;
  hook->policy = 0;
  return true;
}

static void sync_profiles_locked(pid_t pid) {
  if (pid <= 0)
    return;
  uintptr_t base = (uintptr_t)kernel_dynlib_mapbase_addr(pid, 0);
  if (base < 0x10000 || base >= UINT64_C(0x800000000000) - UINT32_MAX - CLOCK_SLOT_SIZE)
    return;
  if (pid != g_ps4.pid || base != g_ps4.base) {
    memset(&g_ps4, 0, sizeof(g_ps4));
    uint16_t firmware = (uint16_t)(kernel_get_fw_version() >> 16);
    for (size_t i = 0; i < sizeof(g_profiles) / sizeof(g_profiles[0]); ++i) {
      if (g_profiles[i].firmware == firmware)
        g_ps4.profile = &g_profiles[i];
    }
    g_ps4.pid = pid;
    g_ps4.base = base;
  }
  if (!g_ps4.profile)
    return;
  if ((uintptr_t)sm_ps4_clock_end - (uintptr_t)sm_ps4_clock_start > CLOCK_SLOT_SIZE ||
      (uintptr_t)sm_ps4_pro_end - (uintptr_t)sm_ps4_pro_start > PRO_SLOT_SIZE)
    return;
  uint64_t revision;
  bool enabled = sm_config_ps4_hooks_needed(&revision);
  if (g_ps4.config_revision != revision) {
    // Explicit configuration changes can retry a failed hook. Validate every
    // retained reservation first; a different payload's bytes stay untouched.
    for (size_t i = 0; i < 2; ++i) {
      g_ps4.hooks[i].blocked = g_ps4.hooks[i].attached && !owned_code_matches(i != 0);
      if (g_ps4.hooks[i].blocked)
        log_debug("  [PS4] retained %s wrapper changed; update skipped", i ? "Pro" : "clock");
    }
    if (g_ps4.hooks[1].blocked)
      (void)disarm_owned_policy(false);
    if (g_ps4.hooks[1].attached && !g_ps4.hooks[1].blocked) {
      uintptr_t title = g_ps4.base + g_ps4.profile->pro_slot +
          symbol_offset(sm_ps4_pro_title, sm_ps4_pro_start);
      if (!publish_policy_byte(title, 0)) {
        g_ps4.hooks[1].blocked = true;
        (void)disarm_owned_policy(false);
        log_debug("  [PS4] could not revoke the previous title selection");
      }
    }
    g_ps4.config_revision = revision;
  }
  // Config reload prepares interception for future launches. Keep the selected
  // game's policy until a launch callback chooses another title (also resume).
  // The in-process SFO guard can clear the clock byte after a missed IPC.
  // Read it before repair so a resume never republishes a stale cached value.
  for (size_t i = 0; i < 2; ++i) {
    if (!g_ps4.hooks[i].attached)
      continue;
    uintptr_t slot = g_ps4.base + (i ? g_ps4.profile->pro_slot : g_ps4.profile->clock_slot);
    size_t offset = i ? symbol_offset(sm_ps4_pro_policy, sm_ps4_pro_start)
                      : symbol_offset(sm_ps4_clock_policy, sm_ps4_clock_start);
    uint8_t value;
    if (!read_bytes(slot + offset, &value, 1))
      return;
    g_ps4.hooks[i].policy = value;
  }
  sync_policies(enabled, enabled ? g_ps4.hooks[0].policy : 0,
                enabled ? g_ps4.hooks[1].policy : 0);
}

static void stop_profiles_locked(void) {
  if (!g_ps4.profile || !g_ps4.base || g_ps4.pid <= 0)
    return;
  if (kill(g_ps4.pid, 0) != 0 && errno == ESRCH) {
    memset(&g_ps4, 0, sizeof(g_ps4));
    return;
  }
  uintptr_t base = (uintptr_t)kernel_dynlib_mapbase_addr(g_ps4.pid, 0);
  if (!base)
    return;
  if (base != g_ps4.base) {
    memset(&g_ps4, 0, sizeof(g_ps4));
    return;
  }
  if (g_ps4.stopped_on_error) {
    if (g_ps4.rollback_hook != 0) {
      unsigned index = g_ps4.rollback_hook - 1u;
      ps4_hook_t *hook = &g_ps4.hooks[index];
      uintptr_t site = g_ps4.base + (index ? g_ps4.profile->pro_site
                                          : g_ps4.profile->clock_site);
      if (!write_bytes(site, hook->original, sizeof(hook->original)))
        return;
      hook->attached = false;
    }
    if (!sm_remote_process_detach(g_ps4.pid))
      return;
    g_ps4.stopped_on_error = false;
  }
  g_ps4.hooks[0].blocked = false;
  g_ps4.hooks[1].blocked = false;
  sync_policies(false, 0, 0);
}

void sm_ps4_profiles_sync(pid_t pid) {
  pthread_mutex_lock(&g_ps4_mutex);
  sync_profiles_locked(pid);
  pthread_mutex_unlock(&g_ps4_mutex);
}

void sm_ps4_profiles_stop(void) {
  pthread_mutex_lock(&g_ps4_mutex);
  stop_profiles_locked();
  pthread_mutex_unlock(&g_ps4_mutex);
}


bool sm_ps4_profiles_apply_title(const char *title_id, bool launching,
                                 uint8_t requested, uint64_t revision) {
  if (launching && title_id && strncmp(title_id, "NPXS", 4) == 0)
    return true;
  pthread_mutex_lock(&g_ps4_mutex);
  requested = sm_config_is_ps4_title(title_id) ? requested & 63u : 0;
  uint8_t policy = requested & 23u;
  bool ok = !g_ps4.stopped_on_error;
  // Begin with both policies disarmed. The title token may be copied in parts;
  // no policy becomes visible until the complete token has been verified.
  for (size_t i = 0; ok && i < 2; ++i) {
    ps4_hook_t *hook = &g_ps4.hooks[i];
    if (!hook->attached) {
      ok = policy == 0;
      continue;
    }
    const ps4_profile_t *p = g_ps4.profile;
    uintptr_t site = g_ps4.base + (i ? p->pro_site : p->clock_site);
    uintptr_t slot = g_ps4.base + (i ? p->pro_slot : p->clock_slot);
    const uint8_t *start = i ? sm_ps4_pro_start : sm_ps4_clock_start;
    size_t offset = symbol_offset(i ? sm_ps4_pro_policy : sm_ps4_clock_policy, start);
    uint8_t call[5], expected[5];
    ok = !hook->blocked && make_call(expected, site, slot) &&
         read_bytes(site, call, sizeof(call)) && memcmp(call, expected, sizeof(call)) == 0 &&
         publish_policy_byte(slot + offset, 0);
    if (ok) {
      hook->policy = 0;
    } else {
      if (!hook->blocked)
        log_debug("  [PS4] launch policy unavailable for %s", title_id ? title_id : "");
      hook->blocked = true;
    }
  }
  if (ok && g_ps4.hooks[1].attached) {
    uint8_t title[9] = {0}, actual[9];
    if (sm_config_is_ps4_title(title_id))
      memcpy(title, title_id, sizeof(title));
    uintptr_t address = g_ps4.base + g_ps4.profile->pro_slot +
        symbol_offset(sm_ps4_pro_title, sm_ps4_pro_start);
    if (!read_bytes(address, actual, sizeof(actual)) || memcmp(actual, title, sizeof(title))) {
      (void)sm_remote_process_write(g_ps4.pid, address, title, sizeof(title));
      ok = read_bytes(address, actual, sizeof(actual)) && !memcmp(actual, title, sizeof(title));
    }
    if (!ok) {
      if (!g_ps4.hooks[1].blocked)
        log_debug("  [PS4] launch title token publication failed for %s", title_id ? title_id : "");
      g_ps4.hooks[1].blocked = true;
    }
  }
  for (size_t i = 0; ok && i < 2; ++i) {
    if (!g_ps4.hooks[i].attached)
      continue;
    // A fresh launch arms clocks in SFO only after the title is validated.
    // Failed-launch recovery restores the already-running game's clocks now.
    uint8_t value = i ? policy : (launching ? 0 : policy & 19u);
    uintptr_t slot = g_ps4.base + (i ? g_ps4.profile->pro_slot : g_ps4.profile->clock_slot);
    size_t offset = i ? symbol_offset(sm_ps4_pro_policy, sm_ps4_pro_start)
                      : symbol_offset(sm_ps4_clock_policy, sm_ps4_clock_start);
    ok = value == 0 || publish_policy_byte(slot + offset, value);
    if (ok) {
      g_ps4.hooks[i].policy = value;
    } else {
      if (!g_ps4.hooks[i].blocked)
        log_debug("  [PS4] launch flags publication failed for %s", title_id ? title_id : "");
      g_ps4.hooks[i].blocked = true;
    }
  }
  if (!ok && !g_ps4.stopped_on_error) {
    // Leave any owned wrapper inactive even if another payload changed a CALL.
    (void)disarm_owned_policy(false);
    (void)disarm_owned_policy(true);
  }
  if (ok) {
    g_ps4.config_revision = revision;
    if (launching && sm_config_is_ps4_title(title_id))
      log_debug("  [PS4] launch policy selected: %s Pro=%s CPU=%s GPU=%s memory=%s; "
                "publication verified (clock request follows SFO validation)",
                title_id, policy & 4u ? "on" : "stock",
                policy & 1u ? "Pstate0" : "stock", policy & 16u ? "2350MHz target" : policy & 2u ? "2200MHz target" : "stock",
                requested & 8u ? "8GB" : "stock");
  }
  pthread_mutex_unlock(&g_ps4_mutex);
  return ok;
}

void sm_ps4_profiles_on_exec(const char *title_id) {
  if (!sm_config_is_ps4_title(title_id) || !sm_config_debug_enabled())
    return;
  pthread_mutex_lock(&g_ps4_mutex);
  if (g_ps4.profile && g_ps4.hooks[1].attached && !g_ps4.hooks[1].blocked) {
    uint8_t title[9], flags, clocks;
    uintptr_t pro = g_ps4.base + g_ps4.profile->pro_slot;
    uintptr_t clock = g_ps4.base + g_ps4.profile->clock_slot;
    if (read_bytes(pro + symbol_offset(sm_ps4_pro_title, sm_ps4_pro_start), title, sizeof(title)) &&
        read_bytes(pro + symbol_offset(sm_ps4_pro_policy, sm_ps4_pro_start), &flags, 1) &&
        read_bytes(clock + symbol_offset(sm_ps4_clock_policy, sm_ps4_clock_start), &clocks, 1)) {
      bool accepted = !memcmp(title, title_id, sizeof(title)) && flags && clocks == flags;
      log_debug("  [PS4] SFO readback: %s accepted=%s Pro=%s CPU_request=%s GPU_request=%s "
                "flags=0x%x clocks=0x%x (hardware clocks not sampled)",
                title_id, accepted ? "yes" : "not observed",
                accepted && (flags & 4u) ? "enabled" : "not confirmed",
                accepted && (flags & 1u) ? "Pstate0 armed" : "stock/unconfirmed",
                accepted && (flags & 16u) ? "2350MHz armed" :
                accepted && (flags & 2u) ? "2200MHz armed" : "stock/unconfirmed", flags, clocks);
    } else {
      log_debug("  [PS4] SFO readback unavailable: %s", title_id);
    }
  }
  pthread_mutex_unlock(&g_ps4_mutex);
}
