#include "sm_platform.h"

#include <dirent.h>
#include <pthread.h>

#include "sm_config_file.h"
#include "sm_config_mount.h"
#include "sm_dynlib.h"
#include "sm_game_patches.h"
#include "sm_gameinfo.h"
#include "sm_log.h"
#include "sm_pkg.h"
#include "sm_runtime.h"
#include "sm_shellcore_remote.h"
#include "sm_time.h"

#define PATCH_WAIT_US UINT64_C(15000000)
#define PATCH_POLL_US UINT64_C(100000)

static pthread_mutex_t g_patch_settings_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool valid_patch_id(const char *id) {
  if (!id || strlen(id) != 16) return false;
  for (size_t i = 0; i < 16; ++i)
    if (!((id[i] >= '0' && id[i] <= '9') || (id[i] >= 'a' && id[i] <= 'f'))) return false;
  return true;
}

static int read_selection(sm_ini_t *doc) {
  return sm_ini_read(SM_PATCH_SETTINGS, doc, false) ? 0 : (errno ? errno : EIO);
}

int sm_game_patches_set_enabled(const char *title_id, const char *id, bool enabled) {
  if (!is_supported_game_title_id(title_id) || !valid_patch_id(id)) return EINVAL;
  pthread_mutex_lock(&g_patch_settings_mutex);
  sm_ini_t doc = {0};
  int error = read_selection(&doc);
  char section[48];
  snprintf(section, sizeof(section), "game:%s", title_id);
  if (!error && (!sm_ini_set(&doc, section, id, enabled ? "on" : "off") ||
                  !sm_ini_write(SM_PATCH_SETTINGS, &doc))) error = errno ? errno : EIO;
  sm_ini_free(&doc);
  pthread_mutex_unlock(&g_patch_settings_mutex);
  return error;
}

static int compare_paths(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int sm_game_patches_list(const char *title_id, const char *version,
                         sm_game_patch_list_t *list) {
  memset(list, 0, sizeof(*list));
  if (!is_supported_game_title_id(title_id) || !version) return EINVAL;
  // Own files may keep descriptive names. Conventional title-ID files from
  // ps-patch-system work without copying or modifying its selection settings.
  char *paths[64] = {0};
  size_t count = 0;
  int error = 0;
  DIR *dir = opendir(SM_PATCH_DIRECTORY);
  if (!dir && errno != ENOENT) return errno;
  if (dir) {
    struct dirent *entry;
    for (;;) {
      errno = 0;
      entry = readdir(dir);
      if (!entry) { if (errno) error = errno; break; }
      size_t length = strlen(entry->d_name);
      if (length <= 4 || strcmp(entry->d_name + length - 4, ".xml") || !strstr(entry->d_name, title_id)) continue;
      if (count == sizeof(paths) / sizeof(paths[0])) { error = EFBIG; break; }
      char path[MAX_PATH];
      int n = snprintf(path, sizeof(path), "%s/%s", SM_PATCH_DIRECTORY, entry->d_name);
      if (n < 0 || (size_t)n >= sizeof(path)) { error = ENAMETOOLONG; break; }
      paths[count] = strdup(path);
      if (!paths[count]) { error = ENOMEM; break; }
      ++count;
    }
    closedir(dir);
  }
  if (!count && !error) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/data/user/patches/%s/%s.xml",
             sm_config_is_ps4_title(title_id) ? "xml_orbis" : "xml_prospero", title_id);
    struct stat st;
    if (!stat(path, &st)) {
      paths[count] = strdup(path);
      if (paths[count]) ++count;
      else error = ENOMEM;
    } else if (errno != ENOENT) error = errno;
  }
  qsort(paths, count, sizeof(paths[0]), compare_paths);
  for (size_t i = 0; i < count; ++i) {
    if (!error) {
      error = sm_patch_xml_load(paths[i], title_id, version, list);
      if (error) log_debug("  [PATCH] XML rejected: %s status=%d", paths[i], error);
    }
    free(paths[i]);
  }
  if (!error && list->count) {
    pthread_mutex_lock(&g_patch_settings_mutex);
    sm_ini_t doc = {0};
    error = read_selection(&doc);
    char section[48];
    snprintf(section, sizeof(section), "game:%s", title_id);
    for (size_t i = 0; !error && i < list->count; ++i) {
      const char *value = sm_ini_get(&doc, section, list->entries[i].id);
      if (!value) continue;
      if (!strcmp(value, "on") || !strcmp(value, "1")) list->entries[i].enabled = true;
      else if (strcmp(value, "off") && strcmp(value, "0")) error = EINVAL;
    }
    sm_ini_free(&doc);
    pthread_mutex_unlock(&g_patch_settings_mutex);
  }
  if (error) sm_game_patches_free(list);
  return error;
}

static bool has_selection(const char *title_id) {
  pthread_mutex_lock(&g_patch_settings_mutex);
  sm_ini_t doc = {0};
  int error = read_selection(&doc);
  char section[48];
  snprintf(section, sizeof(section), "game:%s", title_id);
  bool enabled = false;
  for (size_t i = 0; !error && i < doc.count; ++i) {
    const sm_ini_line_t *line = &doc.lines[i];
    if (!line->header && line->section && !strcmp(line->section, section) &&
        valid_patch_id(line->key) && line->value &&
        (!strcmp(line->value, "on") || !strcmp(line->value, "1"))) enabled = true;
  }
  sm_ini_free(&doc);
  pthread_mutex_unlock(&g_patch_settings_mutex);
  if (error) log_debug("  [PATCH] selection unreadable: %s status=%d", title_id, error);
  return enabled;
}

// No worker/thread of its own. Only the existing lifecycle owner accesses this.
static struct {
  pid_t pid;
  char title_id[MAX_TITLE_ID];
  uint64_t deadline, next_poll;
  sm_game_patch_list_t list;
  sm_patch_recovery_t *recovery;
  uintptr_t process;
  bool loaded, attached;
} g_patches;

typedef struct {
  char name[128];
  uintptr_t base;
  size_t size;
} patch_module_t;

typedef struct {
  pid_t pid;
  size_t count;
  patch_module_t modules[256];
} patch_process_t;

static bool snapshot_modules(patch_process_t *process) {
  process->count = 0;
  dynlib_obj_t object;
  if (kernel_dynlib_obj(process->pid, 0, &object)) return false;
  for (unsigned n = 0; n < 256; ++n) {
    char path[MAX_PATH] = {0};
    if (object.path < UINT64_C(0xffff800000000000)) return false;
    bool terminated = false;
    for (size_t offset = 0; offset < sizeof(path) - 1;) {
      size_t bytes = sizeof(path) - 1 - offset;
      size_t page_left = 0x4000u - ((object.path + offset) & 0x3fffu);
      if (bytes > 64) bytes = 64;
      if (bytes > page_left) bytes = page_left;
      if (kernel_copyout(object.path + offset, path + offset, bytes)) return false;
      if (memchr(path + offset, 0, bytes)) { terminated = true; break; }
      offset += bytes;
    }
    if (!terminated) return false;
    const char *leaf = strrchr(path, '/');
    leaf = leaf ? leaf + 1 : path;
    if (strlen(leaf) < sizeof(process->modules[0].name)) {
      patch_module_t *module = &process->modules[process->count++];
      strlcpy(module->name, leaf, sizeof(module->name));
      module->base = object.mapbase ? object.mapbase : 0x400000;
      module->size = object.mapsize;
    }
    if (!object.next) return true;
    if (object.next < UINT64_C(0xffff800000000000) ||
        kernel_copyout(object.next, &object, sizeof(object))) return false;
  }
  return false;
}

static bool patch_module(void *context, const char *name, uintptr_t *base, size_t *size) {
  const patch_process_t *process = context;
  bool found = false;
  for (size_t i = 0; i < process->count; ++i) {
    const patch_module_t *module = &process->modules[i];
    if (!strcmp(module->name, name)) {
      if (found && (*base != module->base || *size != module->size)) return false;
      *base = module->base;
      *size = module->size;
      found = true;
    }
  }
  return found;
}

static bool patch_read(void *context, uintptr_t address, void *data, size_t size) {
  return sm_remote_process_read(((patch_process_t *)context)->pid, address, data, size);
}

static bool patch_write(void *context, uintptr_t address, const void *data, size_t size) {
  return sm_remote_process_write(((patch_process_t *)context)->pid, address, data, size);
}

static bool patch_cancelled(void *context) {
  (void)context;
  return should_stop_requested() || runtime_sleep_mode_active() || monotonic_time_us() >= g_patches.deadline;
}

static bool detach_patch_process(void) {
  if (!g_patches.attached) return true;
  if (!sm_remote_process_detach(g_patches.pid) && !sm_remote_process_detach(g_patches.pid)) return false;
  g_patches.attached = false;
  return true;
}

void sm_game_patches_reset(void) {
  if (g_patches.pid && (uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process) {
    sm_patch_forget_recovery(&g_patches.recovery);
    g_patches.attached = false;
  }
  if (g_patches.recovery) {
    patch_process_t *process = calloc(1, sizeof(*process));
    if (!process) return;
    process->pid = g_patches.pid;
    sm_patch_memory_t memory = {process, patch_module, patch_read, patch_write, NULL};
    bool restored = sm_patch_recover(&g_patches.recovery, &memory);
    free(process);
    if (!restored) {
      log_debug("  [PATCH] incomplete rollback: pid=%ld must be closed or console restarted", (long)g_patches.pid);
      return;
    }
  }
  if (g_patches.attached && !detach_patch_process()) {
    log_debug("  [PATCH] detach failed: pid=%ld", (long)g_patches.pid);
    return;
  }
  sm_game_patches_free(&g_patches.list);
  memset(&g_patches, 0, sizeof(g_patches));
}

void sm_game_patches_on_exec(pid_t pid, const char *title_id) {
  if (pid <= 0 || !is_supported_game_title_id(title_id)) return;
  if (g_patches.pid == pid && !strcmp(g_patches.title_id, title_id)) return;
  sm_game_patches_reset();
  if (g_patches.attached) return;
  g_patches.pid = pid;
  g_patches.process = (uintptr_t)kernel_get_proc(pid);
  strlcpy(g_patches.title_id, title_id, sizeof(g_patches.title_id));
  if (!g_patches.process || !has_selection(title_id)) return;
  uint64_t now = monotonic_time_us();
  if (!now) return;
  g_patches.deadline = now + PATCH_WAIT_US;
  g_patches.next_poll = now;
  sm_game_patches_poll();
}

static bool requirements_met(const char *title_id, const char *requirements) {
  uint64_t revision;
  uint8_t flags = sm_config_get_title_ps4_policy(title_id, &revision);
  while (*requirements) {
    const char *end = strchr(requirements, ',');
    size_t length = end ? (size_t)(end - requirements) : strlen(requirements);
    unsigned bit = length == 3 && !memcmp(requirements, "pro", 3) ? 4 :
                   length == 4 && !memcmp(requirements, "8gib", 4) ? 8 :
                   length == 5 && !memcmp(requirements, "120hz", 5) ? 32 : 0;
    if (!bit || !(flags & bit)) return false;
    if (!end) break;
    requirements = end + 1;
  }
  return true;
}

static void finish_patch_startup(void) {
  g_patches.next_poll = 0;
  if (!g_patches.recovery) sm_game_patches_free(&g_patches.list);
}

void sm_game_patches_poll(void) {
  uint64_t now = monotonic_time_us();
  if (!g_patches.next_poll || now < g_patches.next_poll || g_patches.recovery) return;
  if ((uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process) {
    g_patches.attached = false;
    finish_patch_startup();
    return;
  }
  if (g_patches.attached) {
    // Detach recovery is bounded by the same startup deadline.
    if (!detach_patch_process()) {
      finish_patch_startup();
      log_debug("  [PATCH] CRITICAL: pid=%ld remains attached", (long)g_patches.pid);
      return;
    }
  }
  if (now >= g_patches.deadline) {
    log_debug("  [PATCH] startup timeout: %s (version or target module not ready)", g_patches.title_id);
    finish_patch_startup();
    return;
  }
  g_patches.next_poll = now + PATCH_POLL_US;
  app_info_t info = {0};
  if (sceKernelGetAppInfo(g_patches.pid, &info) || strcmp(info.title_id, g_patches.title_id)) {
    finish_patch_startup();
    return;
  }
  if (!g_patches.loaded) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "/mnt/sandbox/%s_000/app0", g_patches.title_id);
    sm_pkg_info_t metadata;
    if (!sm_pkg_read_directory_metadata(path, &metadata) || !metadata.app_ver[0]) return;
    int error = sm_game_patches_list(g_patches.title_id, metadata.app_ver, &g_patches.list);
    if (error) {
      log_debug("  [PATCH] catalog rejected: %s status=%d", g_patches.title_id, error);
      finish_patch_startup();
      return;
    }
    for (size_t i = 0; i < g_patches.list.count; ++i) {
      sm_game_patch_t *p = &g_patches.list.entries[i];
      if (p->enabled && (!p->compatible || !p->supported || !requirements_met(g_patches.title_id, p->requires))) {
        log_debug("  [PATCH] skipped: %s %s (version, type or mode requirement)", g_patches.title_id, p->name);
        p->enabled = false;
      }
    }
    g_patches.loaded = true;
  }
  size_t selected = 0;
  for (size_t i = 0; i < g_patches.list.count; ++i) selected += g_patches.list.entries[i].enabled;
  if (!selected) { finish_patch_startup(); return; }
  patch_process_t *process = calloc(1, sizeof(*process));
  if (!process) { finish_patch_startup(); return; }
  process->pid = g_patches.pid;
  sm_patch_memory_t memory = {process, patch_module, patch_read, patch_write, patch_cancelled};
  int error = EAGAIN;
  if (!snapshot_modules(process) || !sm_patch_selection_ready(&g_patches.list, &memory)) goto done;
  if (!sm_remote_process_attach(g_patches.pid)) { error = EBUSY; goto done; }
  g_patches.attached = true;
  if ((uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process ||
      sceKernelGetAppInfo(g_patches.pid, &info) || strcmp(info.title_id, g_patches.title_id)) {
    error = ESRCH;
  } else if (snapshot_modules(process)) {
    error = sm_patch_apply_selected(&g_patches.list, &memory, &g_patches.recovery);
  }
done:
  free(process);
  if (g_patches.recovery) {
    finish_patch_startup();
    log_debug("  [PATCH] CRITICAL: unconfirmed rollback; pid=%ld kept stopped", (long)g_patches.pid);
    return;
  }
  bool detached = detach_patch_process();
  if (error != EAGAIN) {
    log_debug("  [PATCH] %s: %s pid=%ld entries=%zu status=%d (%s)", error ? "failed" : "applied",
              g_patches.title_id, (long)g_patches.pid, selected, error, error ? strerror(error) : "verified");
    if (!error) {
      for (size_t i = 0; i < g_patches.list.count; ++i)
        if (g_patches.list.entries[i].enabled)
          log_debug("  [PATCH] enabled: %s %s", g_patches.title_id, g_patches.list.entries[i].name);
    }
    finish_patch_startup();
  }
  if (!detached) {
    log_debug("  [PATCH] CRITICAL: detach failed; pid=%ld", (long)g_patches.pid);
    finish_patch_startup();
  }
}

uint64_t sm_game_patches_next_wake_us(void) {
  return g_patches.next_poll;
}
