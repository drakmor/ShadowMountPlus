/* Guarded DATA transaction adapted from NeoEnabler.
 * The existing pre-spawn IPC holds ShellCore before the budget constructor.
 * No kernel TEXT calls, extra thread, or persistent changes to game profiles. */
#include "sm_platform.h"
#include <pthread.h>
#include "sm_config_mount.h"
#include "sm_game_lifecycle.h"
#include "sm_gameinfo.h"
#include "sm_log.h"
#include "sm_ps4_memory.h"
#include "sm_shellcore_remote.h"
#include "sm_time.h"

#define MEMORY_DELTA UINT64_C(0xc0000000)
#define MEMORY_CONSTRUCTOR_US UINT64_C(30000000)

typedef enum {
  MEMORY_OK,
  MEMORY_REBOOT_REQUIRED,
  MEMORY_WAIT_FOR_CLOSE,
} memory_fault_t;

typedef struct {
  uint16_t firmware;
  uint32_t extension, container, backing, container_id, profile, maximum;
  uint16_t budget_off, reserved_off, available_off, category_off, flags_off, extra_off;
  uint16_t bonus_proc_off;
} memory_profile_t;

static const memory_profile_t profiles[] = {
#include "sm_ps4_memory_profiles.inc"
};

typedef struct {
  uint64_t container, extension, maximum, budget, reserved, available, backing, extra;
  uint32_t app_id, category, container_id, profile;
  uint16_t flags;
  uint8_t mode;
} memory_snapshot_t;

extern const uint8_t sm_shellcore_bridge_blob_start[];
extern const uint8_t sm_shellcore_bridge_memory_guard[];

static pthread_mutex_t memory_mutex = PTHREAD_MUTEX_INITIALIZER;
static struct {
  const memory_profile_t *profile;
  memory_snapshot_t owner;
  char title[MAX_TITLE_ID];
  pid_t pid, bridge_pid;
  uintptr_t guard;
  uint32_t guard_value;
  uint64_t applied_us, next_check_us;
  bool guard_known, touched, verified, teardown, stopping;
  memory_fault_t blocked;
} memory;

static const memory_profile_t *find_profile(void) {
  uint32_t fw = kernel_get_fw_version() >> 16;
  for (size_t i = 0; i < sizeof(profiles) / sizeof(profiles[0]); ++i)
    if (profiles[i].firmware == fw)
      return &profiles[i];
  return NULL;
}

bool sm_ps4_memory_supported(void) {
  pthread_mutex_lock(&memory_mutex);
  // Firmware cannot change within a payload's lifetime. Reuse the hook
  // owner's profile instead of issuing sysctl for every library entry.
  if (!memory.profile)
    memory.profile = find_profile();
  bool supported = memory.profile != NULL;
  pthread_mutex_unlock(&memory_mutex);
  return supported;
}

static bool heap_pointer(uint64_t p) {
  return p >= UINT64_C(0xffff800000000000) &&
         p < UINT64_C(0xffffffff00000000) && !(p & 7u);
}

// Classify unrelated Spawn2 calls without reading the owner's whole budget.
// Recheck the pointer even when it is zero; this never authorizes a DATA write.
static bool read_container_app_id(const memory_profile_t *p, uint32_t *app_id) {
  uintptr_t address = KERNEL_ADDRESS_DATA_BASE + p->container;
  uint64_t container, after;
  *app_id = 0;
  if (kernel_copyout(address, &container, sizeof(container)))
    return false;
  if (container && (!heap_pointer(container) ||
      kernel_copyout(container + p->category_off + 4, app_id, sizeof(*app_id))))
    return false;
  return !kernel_copyout(address, &after, sizeof(after)) && after == container;
}

static bool read_snapshot(const memory_profile_t *p, memory_snapshot_t *s) {
  uintptr_t base = KERNEL_ADDRESS_DATA_BASE;
  memset(s, 0, sizeof(*s));
  if (kernel_copyout(base + p->extension, &s->extension, 8) ||
      kernel_copyout(base + p->maximum, &s->maximum, 8) ||
      kernel_copyout(base + p->container_id, &s->container_id, 4) ||
      kernel_copyout(base + p->profile, &s->profile, 4) ||
      kernel_copyout(base + p->container, &s->container, 8))
    return false;
  if (!s->container)
    return true;
  uint8_t bytes[0x148];
  uint16_t flags_before;
  uint64_t descriptor, after;
  if (!heap_pointer(s->container) || p->flags_off + 2u > sizeof(bytes) ||
      kernel_copyout(s->container + p->flags_off, &flags_before, 2) ||
      kernel_copyout(s->container, bytes, p->flags_off + 2u))
    return false;
  memcpy(&descriptor, bytes + 8, 8);
  memcpy(&s->budget, bytes + p->budget_off, 8);
  memcpy(&s->reserved, bytes + p->reserved_off, 8);
  memcpy(&s->available, bytes + p->available_off, 8);
  memcpy(&s->category, bytes + p->category_off, 4);
  memcpy(&s->app_id, bytes + p->category_off + 4, 4);
  memcpy(&s->flags, bytes + p->flags_off, 2);
  memcpy(&s->extra, bytes + p->extra_off, 8);
  // The constructor publishes budget before bit 0x40. A bulk copy can span
  // that publication and pair the old budget with the new flag; retry it.
  return !((flags_before ^ s->flags) & 0x40) && heap_pointer(descriptor) &&
         !kernel_copyout(base + p->backing, &s->backing, 8) &&
         !kernel_copyout(descriptor + 0x58, &s->mode, 1) &&
         !kernel_copyout(base + p->container, &after, 8) && after == s->container;
}

static uint64_t expected_budget(const memory_snapshot_t *s) {
  return UINT64_C(0x150000000) + s->extra + MEMORY_DELTA;
}

static bool eligible(const memory_profile_t *p, const memory_snapshot_t *s) {
  return heap_pointer(s->container) && s->app_id && s->app_id != UINT32_MAX &&
         s->category == 0xf000 &&
         s->mode == 2 && !(s->flags & 0xc0) && !s->budget && !s->reserved &&
         !s->extension && !s->maximum && s->container_id == 1 && s->profile == 1 &&
         s->backing == UINT64_C(0x100000000) && s->available >= s->backing &&
         s->extra <= UINT64_C(0x40000000) && !(s->extra & 0x3fff) &&
         s->available - s->backing >= expected_budget(s) +
             (p->bonus_proc_off && (s->flags & 0x2000) ? UINT64_C(0x20000000) : 0);
}

static bool publish_guard(bool armed) {
  uint32_t value = !armed ? 0 : memory.touched && !memory.blocked
      ? memory.owner.app_id : UINT32_MAX;
  uint32_t actual;
  if (!memory.guard || (memory.guard & 3u) || memory.bridge_pid <= 0)
    return !armed;
  if (memory.guard_known && memory.guard_value == value)
    return true;
  // Aligned instruction immediate, fetched even on execute-only text.
  (void)sm_remote_process_write(memory.bridge_pid, memory.guard, &value, sizeof(value));
  memory.guard_known = sm_remote_process_read(memory.bridge_pid, memory.guard,
                                              &actual, sizeof(actual));
  if (memory.guard_known)
    memory.guard_value = actual;
  return memory.guard_known && actual == value;
}

static void block_memory(const char *reason, memory_fault_t fault) {
  if (!memory.blocked)
    log_debug("  [PS4-MEM] %s title=%s app=0x%08x; launches blocked %s",
              reason, memory.title, memory.owner.app_id,
              fault == MEMORY_WAIT_FOR_CLOSE ? "until container teardown" : "until reboot");
  if (!memory.blocked || fault == MEMORY_REBOOT_REQUIRED)
    memory.blocked = fault;
  (void)publish_guard(true);
}

bool sm_ps4_memory_bind_bridge(pid_t pid, uintptr_t address) {
  pthread_mutex_lock(&memory_mutex);
  memory.profile = find_profile();
  memory.bridge_pid = pid;
  memory.guard = address + (uintptr_t)(sm_shellcore_bridge_memory_guard -
                                      sm_shellcore_bridge_blob_start);
  uint32_t previous;
  bool ok = !(memory.guard & 3u) &&
      sm_remote_process_read(pid, memory.guard, &previous, sizeof(previous));
  memory.guard_known = ok;
  if (ok)
    memory.guard_value = previous;
  // A restarted payload must never disarm another instance's budget guard.
  if (ok && previous && !memory.touched && !memory.blocked) {
    memory_snapshot_t first, second;
    // ShellCore is stopped here: recover an inactive guard only after two
    // clean snapshots prove there is no container or outstanding increment.
    if (memory.profile && read_snapshot(memory.profile, &first) &&
        !first.container && !first.container_id && !first.extension && !first.maximum &&
        read_snapshot(memory.profile, &second) && !memcmp(&first, &second, sizeof(first))) {
      log_debug("  [PS4-MEM] inactive guard recovered; DATA and container are clear");
    } else {
      block_memory("retained budget guard requires reboot", MEMORY_REBOOT_REQUIRED);
    }
  }
  if (ok)
    ok = publish_guard(memory.touched || memory.blocked);
  pthread_mutex_unlock(&memory_mutex);
  return ok;
}

// Only a vanished physical container permits restoration. PID exit or a
// deadline alone does not establish that the kernel finished teardown.
// False means this observation cannot authorize a protected launch. Background
// cleanup keeps its deadline; a transient read failure does not surrender DATA.
static bool poll_locked(void) {
  if (!memory.touched)
    return true;
  uint64_t now = monotonic_time_us();
  memory.next_check_us = now + (memory.blocked || (memory.verified && !memory.pid)
      ? UINT64_C(1000000) : GAME_LIFECYCLE_POLL_INTERVAL_US);
  memory_snapshot_t s;
  if (!read_snapshot(memory.profile, &s)) {
    if (!memory.verified && now && memory.applied_us &&
        now - memory.applied_us >= MEMORY_CONSTRUCTOR_US)
      block_memory("constructor snapshot unavailable", MEMORY_WAIT_FOR_CLOSE);
    return false;
  }
  if (!s.container && !s.container_id) {
    if (s.extension == MEMORY_DELTA) {
      uint8_t zero = 0;
      (void)kernel_copyin(&zero, KERNEL_ADDRESS_DATA_BASE + memory.profile->extension + 3, 1);
      if (kernel_copyout(KERNEL_ADDRESS_DATA_BASE + memory.profile->extension,
                          &s.extension, 8) || s.extension) {
        block_memory("DATA restoration unconfirmed", MEMORY_REBOOT_REQUIRED);
        return false;
      }
    }
    if (s.extension) {
      block_memory("foreign DATA value during teardown", MEMORY_REBOOT_REQUIRED);
      return false;
    }
    log_debug("  [PS4-MEM] original budget restored: %s", memory.title);
    memory.touched = memory.verified = memory.teardown = false;
    memory.title[0] = '\0';
    memory.pid = 0;
    if (memory.blocked == MEMORY_WAIT_FOR_CLOSE)
      memory.blocked = MEMORY_OK;
    if (!publish_guard(memory.blocked))
      block_memory("bridge disarm unconfirmed", MEMORY_REBOOT_REQUIRED);
    return !memory.blocked;
  }
  if (!s.container || !s.container_id)
    return false; // Physical-container teardown may publish these separately.
  if (s.container != memory.owner.container || s.app_id != memory.owner.app_id ||
      s.container_id != 1 || s.profile != memory.owner.profile ||
      s.mode != memory.owner.mode || s.category != memory.owner.category ||
      s.extra != memory.owner.extra || s.backing != memory.owner.backing ||
      s.maximum || s.extension != MEMORY_DELTA) {
    block_memory("container owner or DATA guards changed", MEMORY_REBOOT_REQUIRED);
    return false;
  }
  if (s.flags & 0x40) {
    // A physical container keeps its validated capacity across LoadExec and
    // resume. A replacement PID must not reclassify that capacity through its
    // own flags, but its current budget must still be checked.
    uint64_t capacity = memory.owner.budget + memory.owner.reserved;
    uint64_t expected = capacity ? capacity : expected_budget(&memory.owner);
    bool budget_known = true;
    bool bonus512 = expected > expected_budget(&memory.owner);
    uint8_t process_flags = 0;
    if (!capacity && memory.profile->bonus_proc_off && (s.flags & 0x2000)) {
      intptr_t proc = memory.pid > 0 ? kernel_get_proc(memory.pid) : 0;
      budget_known = heap_pointer((uint64_t)proc) &&
          !kernel_copyout(proc + memory.profile->bonus_proc_off, &process_flags, 1);
      bonus512 = budget_known && !(process_flags & 2);
      if (bonus512)
        expected += UINT64_C(0x20000000);
    }
    bool invalid_reserved = s.reserved > expected;
    if (!invalid_reserved)
      expected -= s.reserved;
    if (budget_known && (invalid_reserved || s.budget != expected)) {
      if (!memory.blocked)
        log_debug("  [PS4-MEM] budget mismatch: actual=0x%llx expected=0x%llx "
                  "extra=0x%llx reserved=0x%llx extension=0x%llx mode=%u flags=0x%x proc_flags=0x%x",
                  (unsigned long long)s.budget,
                  (unsigned long long)expected,
                  (unsigned long long)s.extra, (unsigned long long)s.reserved,
                  (unsigned long long)s.extension, s.mode, s.flags, process_flags);
      block_memory("constructed budget mismatch", MEMORY_WAIT_FOR_CLOSE);
      return false;
    }
    if (budget_known) {
      bool newly_verified = !memory.verified;
      memory.owner.budget = s.budget;
      memory.owner.reserved = s.reserved;
      memory.verified = true;
      if (newly_verified)
        log_debug("  [PS4-MEM] budget verified: %s app=0x%08x bytes=0x%llx "
                  "NEO_mode=%u bonus512=%d",
                  memory.title, s.app_id, (unsigned long long)s.budget, s.mode,
                  bonus512 ? 1 : 0);
    }
  } else if (memory.verified) {
    memory.verified = false;
    memory.applied_us = now;
  }
  if (!memory.verified && now && memory.applied_us &&
      now - memory.applied_us >= MEMORY_CONSTRUCTOR_US)
    block_memory("constructor confirmation deadline", MEMORY_WAIT_FOR_CLOSE);
  return !memory.blocked;
}

int sm_ps4_memory_on_launch(const char *title_id, uint8_t policy) {
  if (!is_supported_game_title_id(title_id))
    return 0;
  pthread_mutex_lock(&memory_mutex);
  bool checked = poll_locked();
  int status = 0;
  if (memory.stopping || memory.blocked ||
      (memory.touched && strcmp(memory.title, title_id)))
    status = EBUSY;
  else if (!checked)
    status = EAGAIN;
  else if ((policy & 8u) && (!(policy & 4u) || !memory.profile))
    status = ENOTSUP;
  else if (memory.profile && (policy & 8u) && !memory.touched) {
    uint64_t extension;
    if (kernel_copyout(KERNEL_ADDRESS_DATA_BASE + memory.profile->extension,
                        &extension, 8)) {
      status = EAGAIN;
    } else if (extension) {
      block_memory("unowned DATA extension", MEMORY_REBOOT_REQUIRED);
      status = EBUSY;
    }
  }
  if (!status && !publish_guard(memory.touched || (policy & 8u)))
    status = EIO;
  if (status)
    log_debug("  [PS4-MEM] launch denied: %s rc=%d fault=%u container_owned=%d",
              title_id, status, (unsigned)memory.blocked, memory.touched ? 1 : 0);
  pthread_mutex_unlock(&memory_mutex);
  return status;
}

int sm_ps4_memory_prepare(const char *title_id, uint32_t app_id, bool wanted) {
  pthread_mutex_lock(&memory_mutex);
  bool checked = true;
  if (memory.touched && app_id == memory.owner.app_id)
    checked = poll_locked();
  int status = 0;
  bool wake = false;
  if (!checked) {
    status = memory.blocked ? EBUSY : EAGAIN;
  } else if (memory.touched) {
    if (app_id == memory.owner.app_id) {
      if (memory.blocked || memory.stopping ||
          (title_id && strcmp(memory.title, title_id)))
        status = EBUSY;
    } else {
      uint32_t current_app_id;
      if (!read_container_app_id(memory.profile, &current_app_id) || current_app_id == app_id)
        status = EBUSY;
      else
        status = ENOENT;
    }
  } else if (wanted) {
    memory_snapshot_t s, check;
    if (memory.stopping || memory.blocked || !memory.profile || !app_id ||
        !sm_config_is_ps4_title(title_id))
      status = ENOTSUP;
    else if (!read_snapshot(memory.profile, &s))
      status = EBUSY;
    else if (s.container && s.category == 0xf000 && s.app_id && s.app_id != app_id)
      status = ENOENT;
    else if (!eligible(memory.profile, &s) || s.app_id != app_id || !read_snapshot(memory.profile, &check) ||
             memcmp(&s, &check, sizeof(s)))
      status = EBUSY;
    else {
      memory.owner = s;
      memory.touched = true; // Own recovery BEFORE the write, even on error.
      wake = true;
      memory.verified = memory.teardown = false;
      memory.pid = 0;
      memory.applied_us = monotonic_time_us();
      memory.next_check_us = memory.applied_us + GAME_LIFECYCLE_POLL_INTERVAL_US;
      (void)strlcpy(memory.title, title_id, sizeof(memory.title));
      uint8_t increment = 0xc0;
      int rc = publish_guard(true) ? kernel_copyin(&increment,
          KERNEL_ADDRESS_DATA_BASE + memory.profile->extension + 3, 1) : -1;
      uint64_t actual;
      if (rc || kernel_copyout(KERNEL_ADDRESS_DATA_BASE + memory.profile->extension,
                               &actual, 8) || actual != MEMORY_DELTA) {
        block_memory("DATA write unconfirmed", MEMORY_REBOOT_REQUIRED);
        memory.teardown = true;
        status = EIO;
      } else {
        log_debug("  [PS4-MEM] extension applied: %s app=0x%08x "
                  "NEO_mode=%u base_budget=0x%llx extra=0x%llx flags=0x%x",
                  title_id, app_id, s.mode,
                  (unsigned long long)expected_budget(&s), (unsigned long long)s.extra, s.flags);
      }
    }
  } else if (memory.blocked) {
    uint32_t current_app_id;
    if (sm_config_is_ps4_title(title_id) || !memory.profile ||
        !read_container_app_id(memory.profile, &current_app_id) || current_app_id == app_id)
      status = EBUSY;
    else
      status = ENOENT;
  }
  pthread_mutex_unlock(&memory_mutex);
  if (wake)
    wake_game_lifecycle_watcher();
  return status;
}

void sm_ps4_memory_launch_failed(const char *title_id, bool pending_launch) {
  pthread_mutex_lock(&memory_mutex);
  bool owned = memory.touched && title_id && !strcmp(memory.title, title_id);
  bool wake = owned && !memory.pid && !memory.teardown;
  if (wake)
    memory.teardown = true;
  if (pending_launch && !memory.touched && !memory.blocked)
    (void)publish_guard(false);
  if (owned)
    poll_locked();
  pthread_mutex_unlock(&memory_mutex);
  if (wake)
    wake_game_lifecycle_watcher();
}

void sm_ps4_memory_on_exec(pid_t pid, const char *title_id, uint32_t app_id) {
  pthread_mutex_lock(&memory_mutex);
  if (memory.touched && title_id && !strcmp(memory.title, title_id) &&
      app_id == memory.owner.app_id) {
    memory.pid = pid;
    memory.teardown = false;
    memory.verified = false;
    memory.applied_us = monotonic_time_us();
    poll_locked();
  }
  pthread_mutex_unlock(&memory_mutex);
}

void sm_ps4_memory_on_exit(pid_t pid) {
  pthread_mutex_lock(&memory_mutex);
  if (memory.touched && memory.pid == pid) {
    memory.teardown = true;
    poll_locked();
  }
  pthread_mutex_unlock(&memory_mutex);
}

static bool needs_poll(void) {
  return memory.touched && (memory.teardown ||
      !memory.pid || (!memory.verified && !memory.blocked));
}

void sm_ps4_memory_poll(void) {
  pthread_mutex_lock(&memory_mutex);
  if (needs_poll()) {
    uint64_t now = monotonic_time_us();
    if (!now || now >= memory.next_check_us)
      poll_locked();
  }
  pthread_mutex_unlock(&memory_mutex);
}

uint64_t sm_ps4_memory_next_wake_us(void) {
  pthread_mutex_lock(&memory_mutex);
  uint64_t next = needs_poll() ? memory.next_check_us : 0;
  pthread_mutex_unlock(&memory_mutex);
  return next;
}

const char *sm_ps4_memory_status(const char *title_id) {
  pthread_mutex_lock(&memory_mutex);
  const char *status = memory.blocked ? "blocked" : "off";
  if (!memory.blocked && memory.touched && title_id && !strcmp(memory.title, title_id))
    status = memory.verified ? "active" : "pending";
  pthread_mutex_unlock(&memory_mutex);
  return status;
}

bool sm_ps4_memory_shutdown(void) {
  pthread_mutex_lock(&memory_mutex);
  memory.stopping = true;
  bool waiting = memory.touched;
  pthread_mutex_unlock(&memory_mutex);
  if (waiting)
    log_debug("  [PS4-MEM] shutdown waits for the game container to close");
  for (;;) {
    pthread_mutex_lock(&memory_mutex);
    poll_locked();
    bool done = !memory.touched;
    bool ok = !memory.blocked;
    pthread_mutex_unlock(&memory_mutex);
    if (done)
      return ok;
    (void)sceKernelUsleep(GAME_LIFECYCLE_POLL_INTERVAL_US);
  }
}
