#include "sm_platform.h"

#include <dirent.h>
#include <pthread.h>
#include <time.h>
#include <strings.h>

#include "sm_config_file.h"
#include "sm_config_mount.h"
#include "sm_dynlib.h"
#include "sm_game_patches.h"
#include "sm_gameinfo.h"
#include "sm_game_lifecycle.h"
#include "sm_patch_sources.h"
#include "sm_patch_index.h"
#include "sm_patch_internal.h"
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

int sm_game_patches_set_enabled(const char *title_id, sm_game_patch_list_t *list,
                                 const char *id, bool enabled) {
  if (!is_supported_game_title_id(title_id) || !valid_patch_id(id) ||
      !list || list->count > SM_PATCH_MAX_ENTRIES) return EINVAL;
  size_t index = 0;
  while (index < list->count && strcmp(list->entries[index].id, id)) ++index;
  if (index == list->count) return ENOENT;
  pthread_mutex_lock(&g_patch_settings_mutex);
  sm_ini_t doc = {0};
  int error = read_selection(&doc);
  char section[48];
  snprintf(section, sizeof(section), "game:%s", title_id);
  bool next[SM_PATCH_MAX_ENTRIES] = {0};
  for (size_t i = 0; !error && i < list->count; ++i) {
    const char *value = sm_ini_get(&doc, section, list->entries[i].id);
    next[i] = value && (!strcmp(value, "on") || !strcmp(value, "1"));
    if (value && !next[i] && strcmp(value, "off") && strcmp(value, "0")) error = EINVAL;
  }
  size_t chain[SM_PATCH_MAX_ENTRIES], count = 0;
  if (!error && enabled) {
    error = sm_patch_dependency_chain(list, index, chain, &count);
    for (size_t i = 0; !error && i < count; ++i) next[chain[i]] = true;
  } else if (!error) {
    for (size_t i = 0; !error && i < list->count; ++i) {
      if (i == index || !next[i]) continue;
      // Check the fresh INI under the writer lock, including indirect users.
      if (sm_patch_dependency_chain(list, i, chain, &count)) continue;
      for (size_t j = 1; j < count; ++j)
        if (chain[j] == index) error = EBUSY;
    }
    next[index] = false;
  }
  for (size_t i = 0; !error && i < list->count; ++i) {
    const char *value = sm_ini_get(&doc, section, list->entries[i].id);
    bool previous = value && (!strcmp(value, "on") || !strcmp(value, "1"));
    if ((i == index || next[i] != previous) &&
        !sm_ini_set(&doc, section, list->entries[i].id, next[i] ? "on" : "off")) error = errno ? errno : EIO;
  }
  if (!error && !sm_ini_write(SM_PATCH_SETTINGS, &doc)) error = errno ? errno : EIO;
  if (!error)
    for (size_t i = 0; i < list->count; ++i) list->entries[i].enabled = next[i];
  sm_ini_free(&doc);
  pthread_mutex_unlock(&g_patch_settings_mutex);
  return error;
}

static int compare_paths(const void *a, const void *b) {
  return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int list_patches(const char *title_id, const char *version,
                         sm_game_patch_list_t *list) {
  memset(list, 0, sizeof(*list));
  if (!is_supported_game_title_id(title_id) || !version) return EINVAL;
  // Local files retain priority; title matching comes from their content index.
  char *paths[SM_PATCH_MAX_FILES] = {0};
  size_t count = 0;
  int error = sm_patch_index_collect_local(title_id, paths, &count);
  sm_patch_source_config_t *sources = malloc(sizeof(*sources));
  if (!sources) error = ENOMEM;
  else sm_config_patch_sources_snapshot(sources);
  for (size_t i = 0; !error && i < sources->count; ++i) {
    char directory[192];
    if (sm_patch_source_current(&sources->sources[i], directory))
      error = sm_patch_index_collect(directory, title_id, sources->sources[i].cheats, true, paths, &count);
  }
  free(sources);
  qsort(paths, count, sizeof(paths[0]), compare_paths);
  for (size_t i = 0; i < count; ++i) {
    if (!error) {
      size_t first = list->count;
      const char *extension = strrchr(paths[i], '.');
      error = extension && !strcasecmp(extension, ".xml") ? sm_patch_xml_load(paths[i], title_id, version, list) :
              sm_cheat_load(paths[i], title_id, version, list);
      for (size_t j = first; !error && j < list->count; ++j) {
        list->entries[j].source_path = strdup(paths[i]);
        list->entries[j].file_deletable = strncmp(paths[i], SM_PATCH_SOURCE_ROOT "/", sizeof(SM_PATCH_SOURCE_ROOT)) != 0;
        if (!list->entries[j].source_path) error = ENOMEM;
      }
      if (error) log_debug("  [PATCH] file rejected: %s status=%d", paths[i], error);
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
    // Legacy/manual selections may name only the dependent cheat. Report the
    // effective startup state without rewriting configuration during a read.
    for (size_t i = 0; !error && i < list->count; ++i) {
      if (!list->entries[i].enabled || !*list->entries[i].master_id) continue;
      size_t chain[SM_PATCH_MAX_ENTRIES], count;
      if (!sm_patch_dependency_chain(list, i, chain, &count))
        for (size_t j = 1; j < count; ++j) list->entries[chain[j]].enabled = true;
    }
    sm_ini_free(&doc);
    pthread_mutex_unlock(&g_patch_settings_mutex);
  }
  if (error) sm_game_patches_free(list);
  return error;
}

int sm_game_patches_list(const char *title_id, const char *version, sm_game_patch_list_t *list) {
  sm_patch_sources_read_lock();
  int error = list_patches(title_id, version, list);
  sm_patch_sources_read_unlock();
  return error;
}

int sm_game_patches_upload(const char *title_id, const char *name, const void *data, size_t size) {
  if (!is_supported_game_title_id(title_id) || !name || !*name || *name == '.' || strlen(name) > 200 || !data || !size)
    return EINVAL;
  if (size > SM_PATCH_MAX_FILE_SIZE) return EFBIG;
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (*p < 32 || *p == 127 || *p == '/' || *p == '\\' || *p == ':') return EINVAL;
  const char *ext = strrchr(name, '.');
  bool xml = ext && !strcasecmp(ext, ".xml");
  if (!ext || (!xml && strcasecmp(ext, ".json") && strcasecmp(ext, ".shn") && strcasecmp(ext, ".mc4"))) return EINVAL;
  const char *directory = xml ? SM_PATCH_DIRECTORY : SM_CHEAT_DIRECTORY;
  char parent[MAX_PATH];
  strlcpy(parent, directory, sizeof(parent));
  *strrchr(parent, '/') = 0;
  if (mkdir(parent, 0777) && errno != EEXIST) return errno;
  if (mkdir(directory, 0777) && errno != EEXIST) return errno;
  char filename[224], stage[128], temporary[MAX_PATH], target[MAX_PATH];
  snprintf(filename, sizeof(filename), "%s%s%s", strstr(name, title_id) ? "" : title_id, strstr(name, title_id) ? "" : "_", name);
  snprintf(stage, sizeof(stage), "%s/.upload-XXXXXX", directory);
  if (!mkdtemp(stage)) return errno;
  snprintf(temporary, sizeof(temporary), "%s/%s", stage, filename);
  snprintf(target, sizeof(target), "%s/%s", directory, filename);
  int fd = open(temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0644);
  int error = fd < 0 ? errno : 0;
  for (size_t offset = 0; !error && offset < size;) {
    ssize_t n = write(fd, (const char *)data + offset, size - offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) error = n < 0 ? errno : EIO;
    else offset += (size_t)n;
  }
  if (!error && fsync(fd)) error = errno;
  if (fd >= 0 && close(fd) && !error) error = errno;
  if (!error) {
    sm_patch_sources_write_lock();
    struct stat st;
    if (!lstat(target, &st)) error = EEXIST;
    else if (errno != ENOENT) error = errno;
    else if (rename(temporary, target)) error = errno;
    else {
      // Validate the effective catalog too: one valid file must not exhaust
      // the shared file, entry or write budgets and break every other entry.
      sm_game_patch_list_t list;
      error = list_patches(title_id, "", &list);
      bool present = false;
      for (size_t i = 0; !error && i < list.count; ++i)
        if (!strcmp(list.entries[i].source_path, target)) present = true;
      if (!error && !present) error = EINVAL;
      sm_game_patches_free(&list);
      if (error && unlink(target)) log_debug("  [PATCH] failed to discard upload: %s status=%d", target, errno);
    }
    sm_patch_sources_write_unlock();
  }
  unlink(temporary);
  rmdir(stage);
  return error;
}

int sm_game_patches_delete_file(const char *title_id, const char *id) {
  if (!is_supported_game_title_id(title_id) || !valid_patch_id(id)) return EINVAL;
  // Keep the catalog available while the lifecycle owner can still apply or
  // undo live cheats, including a game running before SM+ was started.
  char active[MAX_TITLE_ID];
  if (sm_game_lifecycle_active_title(active) || sm_game_lifecycle_has_active_game()) return EBUSY;
  sm_patch_sources_write_lock();
  sm_game_patch_list_t list;
  int error = list_patches(title_id, "", &list);
  const char *path = NULL;
  for (size_t i = 0; !error && i < list.count; ++i) {
    if (strcmp(list.entries[i].id, id)) continue;
    if (!list.entries[i].file_deletable) error = EPERM;
    else path = list.entries[i].source_path;
  }
  if (!error && !path) error = ENOENT;
  if (!error && (sm_game_lifecycle_active_title(active) || sm_game_lifecycle_has_active_game())) error = EBUSY;
  if (!error && unlink(path)) error = errno;
  // Local directories refresh from file metadata on the next lookup.
  sm_game_patches_free(&list);
  sm_patch_sources_write_unlock();
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

// One bounded request slot; process ownership stays on the lifecycle thread.
static struct {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  enum { CHEAT_IDLE, CHEAT_PENDING, CHEAT_RUNNING, CHEAT_DONE } state;
  char title[MAX_TITLE_ID], id[17];
  bool enabled, abandoned;
  int result;
  char active_title[MAX_TITLE_ID];
  char applied[SM_PATCH_MAX_ENTRIES][17];
  size_t applied_count;
  unsigned applied_types;
} g_cheat_request = {.mutex = PTHREAD_MUTEX_INITIALIZER, .condition = PTHREAD_COND_INITIALIZER};

int sm_game_cheat_request(const char *title_id, const char *id, bool enabled) {
  if (!is_supported_game_title_id(title_id) || !valid_patch_id(id)) return EINVAL;
  if (should_stop_requested() || runtime_sleep_mode_active()) return EBUSY;
  struct timespec until;
  if (clock_gettime(CLOCK_REALTIME, &until)) return errno;
  until.tv_sec += 5;
  pthread_mutex_lock(&g_cheat_request.mutex);
  if (g_cheat_request.state != CHEAT_IDLE) { pthread_mutex_unlock(&g_cheat_request.mutex); return EBUSY; }
  strlcpy(g_cheat_request.title, title_id, sizeof(g_cheat_request.title));
  strlcpy(g_cheat_request.id, id, sizeof(g_cheat_request.id));
  g_cheat_request.enabled = enabled;
  g_cheat_request.abandoned = false;
  g_cheat_request.state = CHEAT_PENDING;
  pthread_mutex_unlock(&g_cheat_request.mutex);
  wake_game_lifecycle_watcher();
  pthread_mutex_lock(&g_cheat_request.mutex);
  int error = 0;
  while (g_cheat_request.state != CHEAT_DONE) {
    error = pthread_cond_timedwait(&g_cheat_request.condition, &g_cheat_request.mutex, &until);
    if (error && g_cheat_request.state != CHEAT_DONE) {
      g_cheat_request.abandoned = true;
      if (g_cheat_request.state == CHEAT_PENDING) g_cheat_request.state = CHEAT_IDLE;
      break;
    }
  }
  if (g_cheat_request.state == CHEAT_DONE) { error = g_cheat_request.result; g_cheat_request.state = CHEAT_IDLE; }
  pthread_mutex_unlock(&g_cheat_request.mutex);
  return error;
}

void sm_game_patches_runtime_state(const char *title_id, sm_game_patch_list_t *list) {
  pthread_mutex_lock(&g_cheat_request.mutex);
  for (size_t i = 0; i < list->count; ++i) list->entries[i].applied = false;
  if (!strcmp(title_id, g_cheat_request.active_title))
    for (size_t i = 0; i < list->count; ++i)
      for (size_t j = 0; j < g_cheat_request.applied_count; ++j)
        if (!strcmp(list->entries[i].id, g_cheat_request.applied[j])) list->entries[i].applied = true;
  pthread_mutex_unlock(&g_cheat_request.mutex);
}

int sm_game_patches_badges(sm_patch_badges_t *items, size_t count, const sm_patch_source_config_t *sources) {
  if ((!items && count) || !sources) return EINVAL;
  sm_ini_t selection = {0};
  pthread_mutex_lock(&g_patch_settings_mutex);
  int selection_error = read_selection(&selection);
  pthread_mutex_unlock(&g_patch_settings_mutex);
  int error = sm_patch_index_badges(items, count, sources, selection_error ? NULL : &selection);
  sm_ini_free(&selection);
  pthread_mutex_lock(&g_cheat_request.mutex);
  for (size_t i = 0; i < count; ++i)
    items[i].applied = items[i].title_id && !strcmp(items[i].title_id, g_cheat_request.active_title) ? g_cheat_request.applied_types : 0;
  pthread_mutex_unlock(&g_cheat_request.mutex);
  return error ? error : selection_error;
}

unsigned sm_game_patches_applied_types(const char *title_id) {
  pthread_mutex_lock(&g_cheat_request.mutex);
  unsigned types = title_id && !strcmp(title_id, g_cheat_request.active_title) ? g_cheat_request.applied_types : 0;
  pthread_mutex_unlock(&g_cheat_request.mutex);
  return types;
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
  uint64_t handle;
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
      module->handle = object.handle;
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
    if (!strcmp(module->name, name) || (!strncmp(name, "@handle:", 8) &&
        module->handle == strtoull(name + 8, NULL, 10))) {
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
  pthread_mutex_lock(&g_cheat_request.mutex);
  bool cancelled = g_cheat_request.state == CHEAT_RUNNING && g_cheat_request.abandoned;
  pthread_mutex_unlock(&g_cheat_request.mutex);
  return cancelled || should_stop_requested() || runtime_sleep_mode_active() || monotonic_time_us() >= g_patches.deadline;
}

static void publish_patch_state(void) {
  pthread_mutex_lock(&g_cheat_request.mutex);
  strlcpy(g_cheat_request.active_title, g_patches.title_id, sizeof(g_cheat_request.active_title));
  g_cheat_request.applied_count = 0;
  g_cheat_request.applied_types = 0;
  for (size_t i = 0; i < g_patches.list.count; ++i)
    if (g_patches.list.entries[i].applied) {
      strcpy(g_cheat_request.applied[g_cheat_request.applied_count++], g_patches.list.entries[i].id);
      g_cheat_request.applied_types |= !strcmp(g_patches.list.entries[i].format, "XML") ? SM_PATCH_BADGE_PATCHES : SM_PATCH_BADGE_CHEATS;
    }
  pthread_mutex_unlock(&g_cheat_request.mutex);
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
  publish_patch_state();
}

void sm_game_patches_on_exec(pid_t pid, const char *title_id) {
  if (pid <= 0 || !is_supported_game_title_id(title_id)) return;
  uintptr_t process = (uintptr_t)kernel_get_proc(pid);
  if (process && g_patches.process == process && g_patches.pid == pid &&
      !strcmp(g_patches.title_id, title_id)) return;
  sm_game_patches_reset();
  if (g_patches.attached || g_patches.recovery) return;
  g_patches.pid = pid;
  g_patches.process = process;
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
  publish_patch_state();
}

static void poll_patch_startup(void) {
  uint64_t now = monotonic_time_us();
  if (!g_patches.next_poll || now < g_patches.next_poll || g_patches.recovery) return;
  if ((uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process) {
    sm_game_patches_reset();
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
    for (size_t i = 0; i < g_patches.list.count; ++i) {
      sm_game_patch_t *p = &g_patches.list.entries[i];
      if (!p->enabled || !*p->master_id) continue;
      size_t chain[SM_PATCH_MAX_ENTRIES], count;
      int error = sm_patch_dependency_chain(&g_patches.list, i, chain, &count);
      for (size_t j = 0; !error && j < count; ++j)
        if (!requirements_met(g_patches.title_id, g_patches.list.entries[chain[j]].requires)) error = EINVAL;
      if (!error)
        for (size_t j = 1; j < count; ++j) g_patches.list.entries[chain[j]].enabled = true;
      else {
        log_debug("  [PATCH] skipped: %s %s (dependency status=%d)", g_patches.title_id, p->name, error);
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
    // Keep XML's all-or-nothing transaction. Cheats use the same engine with
    // their own master dependency and live conflict checks.
    bool enabled[SM_PATCH_MAX_ENTRIES] = {0};
    for (size_t i = 0; i < g_patches.list.count; ++i) {
      sm_game_patch_t *p = &g_patches.list.entries[i];
      enabled[i] = p->enabled;
      if (strcmp(p->format, "XML") || p->applied) p->enabled = false;
    }
    error = sm_patch_apply_selected(&g_patches.list, &memory, &g_patches.recovery);
    for (size_t i = 0; i < g_patches.list.count; ++i) {
      sm_game_patch_t *p = &g_patches.list.entries[i];
      if (!error && p->enabled) p->applied = true;
      p->enabled = enabled[i];
    }
    for (size_t i = 0; !error && i < g_patches.list.count; ++i) {
      sm_game_patch_t *p = &g_patches.list.entries[i];
      if (!p->enabled || p->applied) continue;
      error = sm_cheat_apply(&g_patches.list, i, true, &memory, &g_patches.recovery);
    }
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

static int reload_live_catalog(const char *title) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "/mnt/sandbox/%s_000/app0", title);
  sm_pkg_info_t metadata;
  if (!sm_pkg_read_directory_metadata(path, &metadata) || !*metadata.app_ver) return EAGAIN;
  sm_game_patch_list_t next = {0};
  int error = sm_game_patches_list(title, metadata.app_ver, &next);
  size_t missing = 0;
  for (size_t i = 0; !error && i < g_patches.list.count; ++i) {
    const sm_game_patch_t *old = &g_patches.list.entries[i];
    if (!old->applied) continue;
    size_t j = 0;
    while (j < next.count && strcmp(next.entries[j].id, old->id)) ++j;
    if (j == next.count) ++missing;
  }
  if (!error && missing > SM_PATCH_MAX_ENTRIES - next.count) error = EFBIG;
  if (!error && missing) {
    void *entries = realloc(next.entries, (next.count + missing) * sizeof(*next.entries));
    if (!entries) error = ENOMEM;
    else next.entries = entries;
  }
  if (error) { sm_game_patches_free(&next); return error; }
  // Preserve the exact writes and OFF bytes already used by this process.
  // No allocation can fail after ownership starts moving into the new list.
  for (size_t i = 0; i < g_patches.list.count; ++i) {
    sm_game_patch_t *old = &g_patches.list.entries[i];
    if (!old->applied) continue;
    size_t j = 0;
    while (j < next.count && strcmp(next.entries[j].id, old->id)) ++j;
    if (j < next.count) sm_patch_free_entry(&next.entries[j]);
    else ++next.count;
    next.entries[j] = *old;
    memset(old, 0, sizeof(*old));
  }
  sm_game_patches_free(&g_patches.list);
  g_patches.list = next;
  g_patches.loaded = true;
  return 0;
}

static int apply_live_cheat(const char *title, const char *id, bool enabled) {
  if (should_stop_requested() || runtime_sleep_mode_active()) return EBUSY;
  if (g_patches.next_poll || g_patches.attached || g_patches.recovery) return EBUSY;
  if (!g_patches.pid || (uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process) {
    char active_title[MAX_TITLE_ID];
    pid_t pid = sm_game_lifecycle_active_process(active_title);
    if (pid <= 0 || strcmp(active_title, title)) return ESRCH;
    sm_game_patches_reset();
    g_patches.pid = pid;
    g_patches.process = (uintptr_t)kernel_get_proc(pid);
    strlcpy(g_patches.title_id, title, sizeof(g_patches.title_id));
  }
  if (!g_patches.pid || strcmp(title, g_patches.title_id) ||
      (uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process) return ESRCH;
  app_info_t info = {0};
  if (sceKernelGetAppInfo(g_patches.pid, &info) || strcmp(info.title_id, title)) return ESRCH;
  size_t index = 0;
  while (index < g_patches.list.count && strcmp(g_patches.list.entries[index].id, id)) ++index;
  if (!g_patches.loaded || index == g_patches.list.count) {
    int error = reload_live_catalog(title);
    if (error) return error;
    index = 0;
    while (index < g_patches.list.count && strcmp(g_patches.list.entries[index].id, id)) ++index;
  }
  if (index == g_patches.list.count) return ENOENT;
  sm_game_patch_t *p = &g_patches.list.entries[index];
  if (p->applied == enabled && !p->button) return 0;
  patch_process_t *process = calloc(1, sizeof(*process));
  if (!process) return ENOMEM;
  process->pid = g_patches.pid;
  g_patches.deadline = monotonic_time_us() + UINT64_C(4000000);
  sm_patch_memory_t memory = {process, patch_module, patch_read, patch_write, patch_cancelled};
  int error = EAGAIN;
  if (!snapshot_modules(process)) goto done;
  if (!sm_remote_process_attach(g_patches.pid)) { error = EBUSY; goto done; }
  g_patches.attached = true;
  if ((uintptr_t)kernel_get_proc(g_patches.pid) != g_patches.process ||
      sceKernelGetAppInfo(g_patches.pid, &info) || strcmp(info.title_id, title)) error = ESRCH;
  else if (snapshot_modules(process)) error = sm_cheat_apply(&g_patches.list, index, enabled, &memory, &g_patches.recovery);
done:
  free(process);
  if (g_patches.recovery) {
    log_debug("  [CHEAT] CRITICAL: rollback unconfirmed; pid=%ld kept stopped", (long)g_patches.pid);
  } else if (!detach_patch_process()) {
    log_debug("  [CHEAT] CRITICAL: detach failed; pid=%ld remains attached", (long)g_patches.pid);
    error = EIO;
  }
  publish_patch_state();
  log_debug("  [CHEAT] %s: %s %s pid=%ld status=%d", enabled ? "enable" : "disable", title, p->name, (long)g_patches.pid, error);
  return error;
}

void sm_game_patches_poll(void) {
  poll_patch_startup();
  pthread_mutex_lock(&g_cheat_request.mutex);
  if (g_cheat_request.state != CHEAT_PENDING) { pthread_mutex_unlock(&g_cheat_request.mutex); return; }
  g_cheat_request.state = CHEAT_RUNNING;
  char title[MAX_TITLE_ID], id[17];
  strcpy(title, g_cheat_request.title); strcpy(id, g_cheat_request.id);
  bool enabled = g_cheat_request.enabled;
  pthread_mutex_unlock(&g_cheat_request.mutex);
  int error = apply_live_cheat(title, id, enabled);
  pthread_mutex_lock(&g_cheat_request.mutex);
  g_cheat_request.result = error;
  g_cheat_request.state = g_cheat_request.abandoned ? CHEAT_IDLE : CHEAT_DONE;
  pthread_cond_broadcast(&g_cheat_request.condition);
  pthread_mutex_unlock(&g_cheat_request.mutex);
}

uint64_t sm_game_patches_next_wake_us(void) {
  return g_patches.next_poll;
}
