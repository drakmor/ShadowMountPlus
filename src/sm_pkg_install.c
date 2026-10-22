#include "sm_platform.h"

#include <errno.h>
#include <pthread.h>
#include <strings.h>
#include <dirent.h>

#include "sm_appdb.h"
#include "sm_appinst.h"
#include "sm_config_mount.h"
#include "sm_game_lifecycle.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_pkg.h"
#include "sm_pkg_install.h"
#include "sm_pkg_server.h"
#include "sm_runtime.h"
#include "sm_scanner.h"
#include "sm_shellcore_service.h"
#include "sm_time.h"

#define PKG_INSTALL_TIMEOUT_US (900ull * 1000000ull)
#define PKG_INSTALL_RETRY_US (5ull * 1000000ull)
#define PKG_INSTALL_POLL_US (2ull * 1000000ull)
#define PKG_INSTALL_IDLE_GRACE_US (5ull * 1000000ull)
#define PKG_INSTALL_MAX 128
#define PKG_INSTALL_MAX_FAILED_ATTEMPTS 3
#define PKG_INSTALL_BLOCKED_RETRY_US (5ull * 1000000ull)
#define PKG_INSTALL_ATTEMPT_MAX 64
#define PKG_INSTALL_URI_MAX (MAX_PATH * 3 + PKG_INSTALL_ATTEMPT_MAX + 64)

typedef enum {
  PKG_STATE_NONE = 0,
  PKG_STATE_QUEUED,
  PKG_STATE_SUBMITTING,
  PKG_STATE_WAITING,
  PKG_STATE_DONE,
  PKG_STATE_FAILED,
} pkg_state_t;

typedef struct {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  char content_id[MAX_CONTENT_ID];
  char category[16];
  char app_ver[MAX_GAME_VERSION];
  char attempt[PKG_INSTALL_ATTEMPT_MAX];
  char uri[PKG_INSTALL_URI_MAX];
  sm_pkg_server_t *server;
  uint64_t source_size;
  uint64_t source_device;
  uint64_t source_inode;
  uint64_t source_ctime;
  uint64_t source_fingerprint;
  time_t source_mtime;
  pkg_kind_t pkg_kind;
  uint64_t requested_at_us;
  uint64_t next_attempt_us;
  uint64_t last_bytes_served;
  uint64_t last_bytes_progress_us;
  uint32_t failed_attempts;
  uint32_t last_code;
  pkg_state_t state;
} pkg_entry_t;

static pkg_entry_t g_entries[PKG_INSTALL_MAX];
static bool g_pkg_native_recovery_required;
static bool g_pkg_shutdown_requested;
static bool g_pkg_submit_thread_started;
static bool g_pkg_submit_thread_done;
static pthread_t g_pkg_submit_thread;
static pthread_mutex_t g_pkg_mutex = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_pkg_attempt_sequence;

static void require_native_recovery(const char *reason) {
  if (!g_pkg_native_recovery_required) {
    log_debug("  [PKG] native installer recovery required (%s); package "
              "servicing is paused until ShadowMountPlus restarts",
              reason ? reason : "unsafe Sony installer state");
  }
  g_pkg_native_recovery_required = true;
}


static pkg_entry_t *find_path_locked(const char *path) {
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].state != PKG_STATE_NONE &&
        strcmp(g_entries[i].path, path) == 0)
      return &g_entries[i];
  }
  return NULL;
}

static bool same_identity(const pkg_entry_t *entry,
                          const scan_candidate_t *candidate) {
  return entry->pkg_kind == candidate->pkg_kind &&
         entry->source_fingerprint == candidate->source_fingerprint &&
         strcmp(entry->content_id, candidate->content_id) == 0 &&
         strcmp(entry->app_ver, candidate->app_ver) == 0;
}

static pkg_entry_t *find_identity_locked(const scan_candidate_t *candidate) {
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    pkg_entry_t *entry = &g_entries[i];
    if (entry->state != PKG_STATE_NONE && same_identity(entry, candidate))
      return entry;
  }
  return NULL;
}

static pkg_entry_t *reserve_entry_locked(const scan_candidate_t *candidate) {
  pkg_entry_t *entry = find_identity_locked(candidate);
  if (entry)
    return entry;
  entry = find_path_locked(candidate->path);
  if (entry)
    return entry;
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].state == PKG_STATE_NONE)
      return &g_entries[i];
  }
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].state == PKG_STATE_DONE ||
        g_entries[i].state == PKG_STATE_FAILED)
      return &g_entries[i];
  }
  return NULL;
}

static bool same_source(const pkg_entry_t *entry,
                        const scan_candidate_t *candidate) {
  return entry->source_size == candidate->source_size &&
         entry->source_device == candidate->source_device &&
         entry->source_inode == candidate->source_inode &&
         entry->source_ctime == candidate->source_ctime &&
         entry->source_fingerprint == candidate->source_fingerprint &&
         entry->source_mtime == candidate->source_mtime &&
         strcmp(entry->title_id, candidate->title_id) == 0 &&
         strcmp(entry->content_id, candidate->content_id) == 0 &&
         strcmp(entry->category, candidate->category) == 0 &&
         strcmp(entry->app_ver, candidate->app_ver) == 0 &&
         entry->pkg_kind == candidate->pkg_kind;
}

static bool source_still_matches(const pkg_entry_t *entry) {
  struct stat st;
  scan_candidate_t candidate;
  if (stat(entry->path, &st) != 0 ||
      !sm_pkg_parse_file(entry->path, &st, &candidate))
    return false;
  return same_source(entry, &candidate);
}

bool sm_pkg_install_queue_candidate(const scan_candidate_t *candidate) {
  if (!candidate || candidate->kind != SCAN_CANDIDATE_PKG ||
      candidate->path[0] == '\0' || candidate->title_id[0] == '\0' ||
      !runtime_config().pkg_install_enabled)
    return false;

  pthread_mutex_lock(&g_pkg_mutex);
  if (!sm_pkg_kind_is_installable(candidate->pkg_kind) ||
      !candidate->pkg_classification_confident ||
      sm_pkg_is_installed_content_path(candidate->path)) {
    pthread_mutex_unlock(&g_pkg_mutex);
    return false;
  }
  pkg_entry_t *entry = reserve_entry_locked(candidate);
  if (!entry) {
    pthread_mutex_unlock(&g_pkg_mutex);
    log_debug("  [PKG] package queue full: %s", candidate->path);
    return false;
  }

  if (entry->state != PKG_STATE_NONE && same_identity(entry, candidate)) {
    if (entry->state == PKG_STATE_FAILED &&
        entry->failed_attempts < PKG_INSTALL_MAX_FAILED_ATTEMPTS) {
      entry->state = PKG_STATE_QUEUED;
      entry->next_attempt_us = 1;
    }
    pthread_mutex_unlock(&g_pkg_mutex);
    return true;
  }

  // Do not replace a package while the native installer may still be reading it.
  if (entry->state == PKG_STATE_SUBMITTING ||
      entry->state == PKG_STATE_WAITING) {
    log_debug("  [PKG] source changed while install is active: %s",
              candidate->path);
    pthread_mutex_unlock(&g_pkg_mutex);
    return true;
  }

  memset(entry, 0, sizeof(*entry));
  (void)strlcpy(entry->path, candidate->path, sizeof(entry->path));
  (void)strlcpy(entry->title_id, candidate->title_id, sizeof(entry->title_id));
  (void)strlcpy(entry->title_name, candidate->title_name,
                sizeof(entry->title_name));
  (void)strlcpy(entry->content_id, candidate->content_id,
                sizeof(entry->content_id));
  (void)strlcpy(entry->category, candidate->category, sizeof(entry->category));
  (void)strlcpy(entry->app_ver, candidate->app_ver, sizeof(entry->app_ver));
  entry->source_size = candidate->source_size;
  entry->source_device = candidate->source_device;
  entry->source_inode = candidate->source_inode;
  entry->source_ctime = candidate->source_ctime;
  entry->source_fingerprint = candidate->source_fingerprint;
  entry->source_mtime = candidate->source_mtime;
  entry->pkg_kind = candidate->pkg_kind;
  entry->state = PKG_STATE_QUEUED;
  entry->next_attempt_us = 1;
  pthread_mutex_unlock(&g_pkg_mutex);
  return true;
}

static bool is_base_package(const pkg_entry_t *entry) {
  return entry->pkg_kind == PKG_KIND_BASE;
}

static bool is_patch_package(const pkg_entry_t *entry) {
  return entry->pkg_kind == PKG_KIND_PATCH;
}

static bool is_dlc_package(const pkg_entry_t *entry) {
  return entry->pkg_kind == PKG_KIND_DLC;
}

#define compare_version_strings sm_pkg_compare_versions

static bool package_matches_app_db(const pkg_entry_t *entry) {
  sm_app_db_game_info_t *games = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&games, &count))
    return false;

  const sm_app_db_game_info_t *game =
      app_db_find_game_info(games, count, entry->title_id);
  bool matches = false;
  if (game) {
    if (is_patch_package(entry) && entry->app_ver[0] != '\0' &&
        game->version[0] != '\0') {
      matches = compare_version_strings(game->version, entry->app_ver) >= 0;
    } else if (is_base_package(entry) && entry->content_id[0] != '\0' &&
               game->content_id[0] != '\0') {
      matches = strcmp(game->content_id, entry->content_id) == 0;
    }
  }
  free(games);
  return matches;
}

static bool installed_base_artifact_present(const char *title_id) {
  static const char *const roots[] = {
      "/user/app", "/mnt/ext0/user/app", "/mnt/ext0/ps5/user/app",
      "/mnt/ext1/user/app", "/mnt/ext1/ps5/user/app"};
  char path[MAX_PATH];
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    int written = snprintf(path, sizeof(path), "%s/%s/app.pkg", roots[i],
                           title_id);
    struct stat st;
    if (written > 0 && (size_t)written < sizeof(path) &&
        stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0)
      return true;
  }
  return false;
}

static bool installed_base_content_matches(const pkg_entry_t *entry) {
  static const char *const roots[] = {
      "/user/app", "/mnt/ext0/user/app", "/mnt/ext0/ps5/user/app",
      "/mnt/ext1/user/app", "/mnt/ext1/ps5/user/app"};
  char path[MAX_PATH];
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    int written = snprintf(path, sizeof(path), "%s/%s/app.pkg", roots[i],
                           entry->title_id);
    if (written <= 0 || (size_t)written >= sizeof(path))
      continue;
    struct stat st;
    scan_candidate_t installed;
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
        sm_pkg_parse_file(path, &st, &installed) &&
        installed.pkg_kind == PKG_KIND_BASE &&
        (entry->content_id[0] == '\0' ||
         strcmp(installed.content_id, entry->content_id) == 0))
      return true;
  }
  return false;
}

static bool installed_patch_version_matches(const pkg_entry_t *entry) {
  if (!is_patch_package(entry))
    return false;
  char patch_path[MAX_PATH];
  static const char *const roots[] = {
      "/user/patch", "/mnt/ext0/user/patch", "/mnt/ext0/ps5/user/patch",
      "/mnt/ext1/user/patch", "/mnt/ext1/ps5/user/patch"};
  bool patch_present = false;
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    int written = snprintf(patch_path, sizeof(patch_path), "%s/%s/patch.pkg",
                           roots[i], entry->title_id);
    if (written <= 0 || (size_t)written >= sizeof(patch_path))
      continue;
    struct stat st;
    scan_candidate_t installed;
    if (stat(patch_path, &st) == 0 && S_ISREG(st.st_mode) &&
        sm_pkg_parse_file(patch_path, &st, &installed) &&
        installed.pkg_kind == PKG_KIND_PATCH &&
        (entry->content_id[0] == '\0' ||
         strcmp(installed.content_id, entry->content_id) == 0)) {
      patch_present = true;
      break;
    }
  }
  if (!patch_present)
    return false;

  sm_app_db_game_info_t *games = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&games, &count))
    return false;
  const sm_app_db_game_info_t *game =
      app_db_find_game_info(games, count, entry->title_id);
  bool matches = game && entry->app_ver[0] != '\0' && game->version[0] != '\0' &&
                 compare_version_strings(game->version, entry->app_ver) >= 0;
  free(games);
  return matches;
}

static bool package_is_already_installed(const pkg_entry_t *entry) {
  invalidate_app_db_title_cache();
  if (is_base_package(entry))
    return package_matches_app_db(entry) &&
           installed_base_content_matches(entry);
  if (is_patch_package(entry))
    return installed_patch_version_matches(entry);
  return false;
}

static bool installed_different_base_variant(const pkg_entry_t *entry) {
  if (!is_base_package(entry))
    return false;
  sm_app_db_game_info_t *games = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&games, &count))
    return false;
  const sm_app_db_game_info_t *game =
      app_db_find_game_info(games, count, entry->title_id);
  bool different = game && game->content_id[0] != '\0' &&
                   entry->content_id[0] != '\0' &&
                   strcmp(game->content_id, entry->content_id) != 0 &&
                   installed_base_artifact_present(entry->title_id);
  free(games);
  return different;
}

static bool installed_dlc_tree_matches(const pkg_entry_t *entry,
                                       const char *path, unsigned depth) {
  if (depth > 4)
    return false;
  DIR *dir = opendir(path);
  if (!dir)
    return false;
  struct dirent *item;
  while ((item = readdir(dir)) != NULL) {
    if (item->d_name[0] == '.' || strcmp(item->d_name, "..") == 0)
      continue;
    char child[MAX_PATH];
    int written = snprintf(child, sizeof(child), "%s/%s", path, item->d_name);
    if (written <= 0 || (size_t)written >= sizeof(child))
      continue;
    struct stat st;
    if (stat(child, &st) != 0)
      continue;
    if (S_ISDIR(st.st_mode)) {
      if (installed_dlc_tree_matches(entry, child, depth + 1u)) {
        closedir(dir);
        return true;
      }
      continue;
    }
    size_t length = strlen(item->d_name);
    if (!S_ISREG(st.st_mode) || length < 4 ||
        strcasecmp(item->d_name + length - 4, ".pkg") != 0)
      continue;
    scan_candidate_t candidate;
    if (sm_pkg_parse_file(child, &st, &candidate) &&
        candidate.pkg_kind == PKG_KIND_DLC &&
        strcmp(candidate.content_id, entry->content_id) == 0) {
      closedir(dir);
      return true;
    }
  }
  closedir(dir);
  return false;
}

static bool installed_dlc_content_matches(const pkg_entry_t *entry) {
  static const char *const roots[] = {
      "/user/addcont", "/mnt/ext0/user/addcont", "/mnt/ext0/ps5/user/addcont",
      "/mnt/ext1/user/addcont", "/mnt/ext1/ps5/user/addcont"};
  char root[MAX_PATH];
  for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
    int written = snprintf(root, sizeof(root), "%s/%s", roots[i], entry->title_id);
    if (written > 0 && (size_t)written < sizeof(root) &&
        installed_dlc_tree_matches(entry, root, 0))
      return true;
  }
  return false;
}

static bool package_is_complete(const pkg_entry_t *entry) {
  invalidate_app_db_title_cache();
  if (is_patch_package(entry))
    return installed_patch_version_matches(entry);
  if (is_base_package(entry))
    return package_matches_app_db(entry) &&
           installed_base_content_matches(entry);
  return is_dlc_package(entry) && installed_dlc_content_matches(entry);
}

static bool installer_path_is_safe(const char *path) {
  if (!path || path[0] != '/')
    return false;
  size_t length = strlen(path);
  if (length < 5 || path[length - 1] == '/' ||
      strcasecmp(path + length - 4, ".pkg") != 0)
    return false;
  if (strstr(path, "/../") || strstr(path, "/./") ||
      (length >= 3 && strcmp(path + length - 3, "/..") == 0) ||
      (length >= 2 && strcmp(path + length - 2, "/.") == 0))
    return false;
  if (strncmp(path, "/data/", 6) == 0 || strncmp(path, "/user/", 6) == 0)
    return true;
  const char *tail = NULL;
  if (strncmp(path, "/mnt/usb", 8) == 0 ||
      strncmp(path, "/mnt/ext", 8) == 0)
    tail = path + 8;
  if (!tail || *tail < '0' || *tail > '9')
    return false;
  while (*tail >= '0' && *tail <= '9')
    tail++;
  return *tail == '/';
}

static void schedule_retry(pkg_entry_t *entry, uint64_t now_us,
                           uint32_t code, bool count_failure) {
  entry->last_code = code;
  if (count_failure && entry->failed_attempts < UINT32_MAX)
    entry->failed_attempts++;
  if (count_failure &&
      entry->failed_attempts >= PKG_INSTALL_MAX_FAILED_ATTEMPTS) {
    entry->state = PKG_STATE_FAILED;
    entry->next_attempt_us = 0;
    log_debug("  [PKG] giving up after %u attempts: %s code=0x%08X",
              (unsigned)entry->failed_attempts, entry->path, code);
    return;
  }
  entry->state = PKG_STATE_QUEUED;
  entry->next_attempt_us = now_us + PKG_INSTALL_RETRY_US;
  log_debug("  [PKG] retrying %s code=0x%08X", entry->path, code);
}

static void mark_permanent_failure(pkg_entry_t *entry, uint32_t code) {
  entry->state = PKG_STATE_FAILED;
  entry->failed_attempts = PKG_INSTALL_MAX_FAILED_ATTEMPTS;
  entry->last_code = code;
  entry->next_attempt_us = 0;
}

static int package_kind_priority(pkg_kind_t kind) {
  if (kind == PKG_KIND_BASE)
    return 0;
  if (kind == PKG_KIND_PATCH)
    return 1;
  if (kind == PKG_KIND_DLC)
    return 2;
  return 3;
}

static int compare_package_selection(const pkg_entry_t *left,
                                     const pkg_entry_t *right) {
  int left_priority = package_kind_priority(left->pkg_kind);
  int right_priority = package_kind_priority(right->pkg_kind);
  if (left_priority != right_priority)
    return left_priority < right_priority ? -1 : 1;
  int comparison = strcmp(left->title_id, right->title_id);
  if (comparison != 0)
    return comparison;
  comparison = strcmp(left->content_id, right->content_id);
  if (comparison != 0)
    return comparison;
  if (left->pkg_kind == PKG_KIND_PATCH && right->pkg_kind == PKG_KIND_PATCH) {
    comparison = sm_pkg_compare_versions(left->app_ver, right->app_ver);
    if (comparison != 0)
      return comparison;
  }
  comparison = strcmp(left->app_ver, right->app_ver);
  if (comparison != 0)
    return comparison;
  if (left->source_fingerprint < right->source_fingerprint)
    return -1;
  if (left->source_fingerprint > right->source_fingerprint)
    return 1;
  return strcmp(left->path, right->path);
}

static void complete_entry(pkg_entry_t *entry) {
  log_debug("  [PKG] installed: %s (%s)", entry->title_name,
            entry->title_id);
  entry->state = PKG_STATE_DONE;
  if (entry->server) {
    sm_pkg_server_stop(entry->server);
    entry->server = NULL;
  }
  entry->attempt[0] = '\0';
  entry->uri[0] = '\0';
  entry->next_attempt_us = 0;
}

static bool base_preflight_ready(const pkg_entry_t *entry) {
  if (is_base_package(entry))
    return true;
  invalidate_app_db_title_cache();
  sm_app_db_game_info_t *games = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&games, &count))
    return true;
  const sm_app_db_game_info_t *game =
      app_db_find_game_info(games, count, entry->title_id);
  bool ready = game != NULL &&
               installed_base_artifact_present(entry->title_id);
  free(games);
  return ready;
}

static bool native_url_encode(const char *input, char *output,
                              size_t output_size) {
  static const char hex[] = "0123456789ABCDEF";
  size_t used = 0;
  for (const unsigned char *cursor = (const unsigned char *)input; *cursor;
       cursor++) {
    bool unreserved = (*cursor >= 'A' && *cursor <= 'Z') ||
                      (*cursor >= 'a' && *cursor <= 'z') ||
                      (*cursor >= '0' && *cursor <= '9') || *cursor == '-' ||
                      *cursor == '_' || *cursor == '.' || *cursor == '~';
    size_t required = unreserved ? 1u : 3u;
    if (used + required >= output_size)
      return false;
    if (unreserved) {
      output[used++] = (char)*cursor;
    } else {
      output[used++] = '%';
      output[used++] = hex[*cursor >> 4];
      output[used++] = hex[*cursor & 0x0f];
    }
  }
  output[used] = '\0';
  return true;
}

static bool native_url_for_entry(pkg_entry_t *entry) {
  uint16_t port = 0;
  char encoded_basename[MAX_PATH * 3];
  if (sm_pkg_server_start(&entry->server, entry->path, entry->attempt,
                          &port) != 0)
    return false;
  const char *slash = strrchr(entry->path, '/');
  const char *basename = slash ? slash + 1 : entry->path;
  if (!native_url_encode(basename, encoded_basename,
                         sizeof(encoded_basename))) {
    sm_pkg_server_stop(entry->server);
    entry->server = NULL;
    return false;
  }
  int written = snprintf(entry->uri, sizeof(entry->uri),
                         "http://127.0.0.1:%u/%s/%s", (unsigned)port,
                         entry->attempt, encoded_basename);
  if (written <= 0 || (size_t)written >= sizeof(entry->uri)) {
    sm_pkg_server_stop(entry->server);
    entry->server = NULL;
    entry->uri[0] = '\0';
    return false;
  }
  return true;
}

static void reap_finished_submit_thread(void) {
  pthread_t thread;
  bool join = false;
  pthread_mutex_lock(&g_pkg_mutex);
  if (g_pkg_submit_thread_started && g_pkg_submit_thread_done) {
    thread = g_pkg_submit_thread;
    g_pkg_submit_thread_started = false;
    join = true;
  }
  pthread_mutex_unlock(&g_pkg_mutex);
  if (join)
    (void)pthread_join(thread, NULL);
}

static void *native_submit_worker(void *opaque) {
  pkg_entry_t *entry = opaque;
  char uri[PKG_INSTALL_URI_MAX];
  char title_name[MAX_TITLE_NAME];
  sm_pkg_server_t *server_to_stop = NULL;
  uint64_t now_us = monotonic_time_us();
  bool mutation_claimed = false;
  int result = EBUSY;

  pthread_mutex_lock(&g_pkg_mutex);
  if (entry->state == PKG_STATE_SUBMITTING && !g_pkg_shutdown_requested) {
    (void)strlcpy(uri, entry->uri, sizeof(uri));
    (void)strlcpy(title_name, entry->title_name, sizeof(title_name));
  } else {
    g_pkg_submit_thread_done = true;
    pthread_mutex_unlock(&g_pkg_mutex);
    return NULL;
  }
  pthread_mutex_unlock(&g_pkg_mutex);

  if (!runtime_sleep_mode_active() && !sm_game_lifecycle_has_active_game() &&
      !sm_shellcore_service_has_prepared_mount() &&
      sm_shellcore_try_begin_external_mutation()) {
    mutation_claimed = true;
    pthread_mutex_lock(&g_pkg_mutex);
    bool cancelled = g_pkg_shutdown_requested ||
                     entry->state != PKG_STATE_SUBMITTING;
    pthread_mutex_unlock(&g_pkg_mutex);
    if (!cancelled && source_still_matches(entry))
      result = sm_appinst_install_by_package(uri, title_name);
  }
  if (mutation_claimed)
    sm_shellcore_end_external_mutation();

  pthread_mutex_lock(&g_pkg_mutex);
  if (entry->state == PKG_STATE_SUBMITTING) {
    entry->last_code = (uint32_t)result;
    if (result == 0) {
      entry->state = PKG_STATE_WAITING;
      entry->failed_attempts = 0;
      entry->next_attempt_us = now_us + PKG_INSTALL_POLL_US;
      log_debug("  [PKG] native InstallByPackage accepted: %s", entry->path);
    } else if (g_pkg_shutdown_requested) {
      server_to_stop = entry->server;
      entry->server = NULL;
      entry->state = PKG_STATE_FAILED;
      entry->next_attempt_us = 0;
    } else if (!source_still_matches(entry)) {
      server_to_stop = entry->server;
      entry->server = NULL;
      mark_permanent_failure(entry, ESTALE);
      log_debug("  [PKG] source changed before native submission: %s",
                entry->path);
    } else {
      server_to_stop = entry->server;
      entry->server = NULL;
      entry->uri[0] = '\0';
      entry->attempt[0] = '\0';
      schedule_retry(entry, monotonic_time_us(), (uint32_t)result, true);
      log_debug("  [PKG] native InstallByPackage refused: %s code=0x%08X",
                entry->path, (uint32_t)result);
    }
  }
  g_pkg_submit_thread_done = true;
  bool shutting_down = g_pkg_shutdown_requested;
  pthread_mutex_unlock(&g_pkg_mutex);

  if (server_to_stop)
    sm_pkg_server_stop(server_to_stop);
  if (!shutting_down)
    sm_scanner_wake();
  return NULL;
}

static void service_entry(pkg_entry_t *entry, uint64_t now_us) {
  if (entry->state == PKG_STATE_QUEUED) {
    if (entry->next_attempt_us > now_us)
      return;
    if (!installer_path_is_safe(entry->path) ||
        sm_pkg_is_installed_content_path(entry->path)) {
      mark_permanent_failure(entry, EINVAL);
      log_debug("  [PKG] native installer rejected path policy: %s", entry->path);
      return;
    }
    if (!source_still_matches(entry)) {
      mark_permanent_failure(entry, ESTALE);
      log_debug("  [PKG] source changed before install: %s", entry->path);
      return;
    }
    if (!base_preflight_ready(entry)) {
      entry->next_attempt_us = now_us + PKG_INSTALL_RETRY_US;
      log_debug("  [PKG] waiting for base title in app.db or /user/app: %s",
                entry->path);
      return;
    }
    if (package_is_already_installed(entry)) {
      log_debug("  [PKG] already installed: %s", entry->path);
      complete_entry(entry);
      return;
    }
    if (installed_different_base_variant(entry)) {
      mark_permanent_failure(entry, EEXIST);
      log_debug("  [PKG] refusing different base variant over installed title: %s",
                entry->path);
      return;
    }
    if (entry->server || entry->uri[0] != '\0') {
      require_native_recovery("stale native package state before submission");
      mark_permanent_failure(entry, EBUSY);
      return;
    }

    int written = snprintf(entry->attempt, sizeof(entry->attempt),
                           "sm-%u", ++g_pkg_attempt_sequence);
    if (written <= 0 || (size_t)written >= sizeof(entry->attempt) ||
        !native_url_for_entry(entry)) {
      schedule_retry(entry, now_us, EIO, true);
      return;
    }
    entry->state = PKG_STATE_SUBMITTING;
    entry->requested_at_us = now_us;
    entry->last_bytes_served = 0;
    entry->last_bytes_progress_us = now_us;
    entry->next_attempt_us = now_us + PKG_INSTALL_POLL_US;
    g_pkg_submit_thread_done = false;
    if (pthread_create(&g_pkg_submit_thread, NULL, native_submit_worker,
                       entry) != 0) {
      sm_pkg_server_t *server = entry->server;
      entry->server = NULL;
      entry->uri[0] = '\0';
      entry->attempt[0] = '\0';
      entry->state = PKG_STATE_QUEUED;
      entry->last_code = EAGAIN;
      entry->next_attempt_us = now_us + PKG_INSTALL_RETRY_US;
      entry->failed_attempts++;
      if (entry->failed_attempts >= PKG_INSTALL_MAX_FAILED_ATTEMPTS)
        entry->state = PKG_STATE_FAILED;
      if (server)
        sm_pkg_server_stop(server);
    } else {
      g_pkg_submit_thread_started = true;
    }
    return;
  }

  if (entry->state != PKG_STATE_WAITING || entry->next_attempt_us > now_us)
    return;
  if (!source_still_matches(entry)) {
    require_native_recovery("package source changed during native install");
    mark_permanent_failure(entry, ESTALE);
    return;
  }

  uint64_t bytes_served = entry->server
                               ? sm_pkg_server_bytes_served(entry->server)
                               : 0;
  uint64_t total = entry->server ? sm_pkg_server_total(entry->server) : 0;
  if (bytes_served > entry->last_bytes_served) {
    entry->last_bytes_served = bytes_served;
    entry->last_bytes_progress_us = now_us;
  }
  if (entry->requested_at_us != 0 && now_us > entry->requested_at_us &&
      now_us - entry->requested_at_us > PKG_INSTALL_TIMEOUT_US) {
    require_native_recovery("native package install timed out");
    mark_permanent_failure(entry, ETIMEDOUT);
    return;
  }
  if (total > 0 && bytes_served < total && entry->server &&
      now_us > entry->last_bytes_progress_us &&
      now_us - entry->last_bytes_progress_us > PKG_INSTALL_TIMEOUT_US) {
    require_native_recovery("native package delivery stalled");
    mark_permanent_failure(entry, ETIMEDOUT);
    return;
  }
  if (entry->server && sm_pkg_server_complete(entry->server)) {
    uint64_t last_request = sm_pkg_server_last_request_us(entry->server);
    if (last_request != 0 && now_us > last_request &&
        now_us - last_request >= PKG_INSTALL_IDLE_GRACE_US) {
      sm_pkg_server_stop(entry->server);
      entry->server = NULL;
    }
  }
  // Full delivery is not installation completion. Require the server to be
  // retired before accepting category-specific artifact verification.
  if (!entry->server && package_is_complete(entry)) {
    complete_entry(entry);
    return;
  }
  entry->next_attempt_us = now_us + PKG_INSTALL_POLL_US;
}

static void prune_missing_entries_locked(void) {
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].state == PKG_STATE_NONE ||
        g_entries[i].state == PKG_STATE_SUBMITTING ||
        g_entries[i].state == PKG_STATE_WAITING)
      continue;
    struct stat st;
    if (stat(g_entries[i].path, &st) != 0 && errno == ENOENT)
      memset(&g_entries[i], 0, sizeof(g_entries[i]));
  }
}

void sm_pkg_install_service(void) {
  if (runtime_sleep_mode_active() || !runtime_config().pkg_install_enabled)
    return;

  reap_finished_submit_thread();
  uint64_t now_us = monotonic_time_us();
  pthread_mutex_lock(&g_pkg_mutex);
  prune_missing_entries_locked();

  pkg_entry_t *selected = NULL;
  pkg_entry_t *waiting = NULL;
  bool blocked = sm_game_lifecycle_has_active_game() ||
                 sm_shellcore_service_has_prepared_mount();
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    pkg_entry_t *entry = &g_entries[i];
    if (entry->state == PKG_STATE_WAITING) {
      waiting = entry;
      break;
    }
  }
  if (waiting) {
    if (waiting->next_attempt_us <= now_us)
      selected = waiting;
  } else if (!g_pkg_submit_thread_started) {
    for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
      pkg_entry_t *entry = &g_entries[i];
      if (entry->state != PKG_STATE_QUEUED ||
          entry->next_attempt_us > now_us)
        continue;
      if (!selected || compare_package_selection(entry, selected) < 0)
        selected = entry;
    }
  }

  if (g_pkg_native_recovery_required) {
    pthread_mutex_unlock(&g_pkg_mutex);
    return;
  }

  if (selected && blocked) {
    selected->next_attempt_us = now_us + PKG_INSTALL_BLOCKED_RETRY_US;
  } else if (selected) {
    service_entry(selected, now_us);
  }
  pthread_mutex_unlock(&g_pkg_mutex);
}

uint64_t sm_pkg_install_next_wake_us(uint64_t now_us) {
  if (!runtime_config().pkg_install_enabled)
    return 0;

  uint64_t next = 0;
  pthread_mutex_lock(&g_pkg_mutex);
  if (g_pkg_native_recovery_required) {
    pthread_mutex_unlock(&g_pkg_mutex);
    return 0;
  }
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    const pkg_entry_t *entry = &g_entries[i];
    if (entry->state != PKG_STATE_QUEUED &&
        entry->state != PKG_STATE_WAITING)
      continue;
    uint64_t deadline = entry->next_attempt_us;
    if (deadline == 0)
      deadline = now_us == 0 ? 1 : now_us;
    if (next == 0 || deadline < next)
      next = deadline;
  }
  pthread_mutex_unlock(&g_pkg_mutex);
  return next;
}

bool sm_pkg_install_has_pending_work(void) {
  bool pending = false;
  pthread_mutex_lock(&g_pkg_mutex);
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].state == PKG_STATE_QUEUED ||
        g_entries[i].state == PKG_STATE_SUBMITTING ||
        g_entries[i].state == PKG_STATE_WAITING) {
      pending = true;
      break;
    }
  }
  pthread_mutex_unlock(&g_pkg_mutex);
  return pending;
}

void sm_pkg_install_forget_path(const char *path) {
  if (!path)
    return;
  pthread_mutex_lock(&g_pkg_mutex);
  pkg_entry_t *entry = find_path_locked(path);
  if (entry && entry->state != PKG_STATE_SUBMITTING &&
      entry->state != PKG_STATE_WAITING) {
    memset(entry, 0, sizeof(*entry));
  }
  pthread_mutex_unlock(&g_pkg_mutex);
}

void sm_pkg_install_shutdown(void) {
  pthread_t submit_thread;
  bool join_submit = false;
  sm_pkg_server_t *servers[PKG_INSTALL_MAX];
  size_t server_count = 0;

  pthread_mutex_lock(&g_pkg_mutex);
  g_pkg_shutdown_requested = true;
  if (g_pkg_submit_thread_started) {
    submit_thread = g_pkg_submit_thread;
    join_submit = true;
  }
  pthread_mutex_unlock(&g_pkg_mutex);

  // Do not close the source server while InstallByPackage may still be
  // reading it. The synchronous Sony call owns that decision until it returns.
  if (join_submit)
    (void)pthread_join(submit_thread, NULL);

  pthread_mutex_lock(&g_pkg_mutex);
  g_pkg_submit_thread_started = false;
  g_pkg_submit_thread_done = false;
  for (size_t i = 0; i < PKG_INSTALL_MAX; i++) {
    if (g_entries[i].server && server_count < PKG_INSTALL_MAX) {
      servers[server_count++] = g_entries[i].server;
      g_entries[i].server = NULL;
    }
    g_entries[i].uri[0] = '\0';
    g_entries[i].attempt[0] = '\0';
  }
  pthread_mutex_unlock(&g_pkg_mutex);

  for (size_t i = 0; i < server_count; i++)
    sm_pkg_server_stop(servers[i]);
}
