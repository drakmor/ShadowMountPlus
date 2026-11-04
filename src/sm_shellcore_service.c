#include "sm_platform.h"

#include <stddef.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sm_filesystem.h"
#include "sm_fakelib.h"
#include "sm_game_lifecycle.h"
#include "sm_gameinfo.h"
#include "sm_image.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_pkg_backport.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_shellcore_protocol.h"
#include "sm_shellcore_service.h"
#include "sm_socket_io.h"
#include "sm_time.h"

_Static_assert(MAX_TITLE_ID == SM_SHELLCORE_REQUEST_TITLE_ID_SIZE,
               "ShellCore title id size mismatch");
_Static_assert(sizeof(sm_shellcore_request_t) == SM_SHELLCORE_REQUEST_SIZE,
               "unexpected ShellCore request size");
_Static_assert(offsetof(sm_shellcore_request_t, title_id) == 8,
               "unexpected ShellCore request title id offset");
_Static_assert(sizeof(sm_shellcore_response_t) == SM_SHELLCORE_RESPONSE_SIZE,
               "unexpected ShellCore response size");
_Static_assert(
    sizeof(SM_SHELLCORE_SOCKET_PATH) <=
        sizeof(((struct sockaddr_un *)0)->sun_path),
    "ShellCore socket path is too long");

typedef struct {
  char title_id[MAX_TITLE_ID];
  uint32_t app_id;
  pid_t pid;
  bool game_exited;
  bool sandbox_ready_handled;
  uint64_t exit_time_us;
} shellcore_mount_owner_t;

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool stop_requested;
  int listen_fd;
  int client_fd;
  // The incoming title is mounted before launchApp closes the outgoing game.
  shellcore_mount_owner_t prepared;
  shellcore_mount_owner_t outgoing;
  bool prepare_in_progress;
  bool launch_pending;
  char pending_launch_title_id[MAX_TITLE_ID];
  bool pending_sandbox_ready_handled;
  bool external_mutation_in_progress;
  char releasing_title_id[MAX_TITLE_ID];
} shellcore_service_state_t;

static shellcore_service_state_t g_service = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .listen_fd = -1,
    .client_fd = -1,
};

#define SHELLCORE_RELEASE_MAX_ATTEMPTS 5u
#define SHELLCORE_RELEASE_RETRY_DELAY_US 150000u

static bool launch_preparation_pending_locked(void) {
  return g_service.prepare_in_progress || g_service.launch_pending ||
         g_service.pending_launch_title_id[0] != '\0';
}

static bool mount_owner_matches(const shellcore_mount_owner_t *owner,
                                const char *title_id) {
  return owner->title_id[0] != '\0' && strcmp(owner->title_id, title_id) == 0;
}

static shellcore_mount_owner_t *find_mount_owner_locked(const char *title_id) {
  if (mount_owner_matches(&g_service.prepared, title_id))
    return &g_service.prepared;
  if (mount_owner_matches(&g_service.outgoing, title_id))
    return &g_service.outgoing;
  return NULL;
}

static void clear_mount_owner(shellcore_mount_owner_t *owner) {
  memset(owner, 0, sizeof(*owner));
}

static void set_mount_owner(shellcore_mount_owner_t *owner,
                            const char *title_id) {
  clear_mount_owner(owner);
  (void)strlcpy(owner->title_id, title_id, sizeof(owner->title_id));
}

static int claim_prepared_title(const char *title_id, bool launch_request,
                                bool *claimed_out) {
  bool changed = false;
  bool switched = false;
  char outgoing_title_id[MAX_TITLE_ID] = {0};
  if (claimed_out)
    *claimed_out = false;
  pthread_mutex_lock(&g_service.mutex);
  bool was_busy = launch_preparation_pending_locked();
  if (g_service.prepare_in_progress ||
      g_service.external_mutation_in_progress ||
      strcmp(g_service.releasing_title_id, title_id) == 0) {
    pthread_mutex_unlock(&g_service.mutex);
    return EBUSY;
  }
  if (g_service.prepared.title_id[0] == '\0') {
    if (mount_owner_matches(&g_service.outgoing, title_id)) {
      if (g_service.outgoing.game_exited) {
        pthread_mutex_unlock(&g_service.mutex);
        return EBUSY;
      }
      // Resuming a still-running app need not emit another NOTE_EXEC.
      g_service.prepared = g_service.outgoing;
      clear_mount_owner(&g_service.outgoing);
      if (g_service.prepared.pid > 0 &&
          strcmp(g_service.pending_launch_title_id, title_id) == 0) {
        g_service.pending_launch_title_id[0] = '\0';
        g_service.pending_sandbox_ready_handled = false;
      }
    } else {
      set_mount_owner(&g_service.prepared, title_id);
      if (claimed_out)
        *claimed_out = true;
    }
    g_service.prepare_in_progress = true;
    g_service.launch_pending = launch_request && g_service.prepared.pid <= 0;
    changed = true;
  } else if (!mount_owner_matches(&g_service.prepared, title_id)) {
    if (!launch_request || g_service.prepared.app_id == 0 ||
        g_service.outgoing.title_id[0] != '\0') {
      pthread_mutex_unlock(&g_service.mutex);
      return EBUSY;
    }
    g_service.outgoing = g_service.prepared;
    (void)strlcpy(outgoing_title_id, g_service.outgoing.title_id,
                  sizeof(outgoing_title_id));
    set_mount_owner(&g_service.prepared, title_id);
    g_service.prepare_in_progress = true;
    g_service.launch_pending = true;
    changed = true;
    switched = true;
    if (claimed_out)
      *claimed_out = true;
  } else if (g_service.prepared.game_exited) {
    pthread_mutex_unlock(&g_service.mutex);
    return EBUSY;
  } else {
    g_service.prepare_in_progress = true;
    if (launch_request)
      g_service.launch_pending = true;
  }
  changed |= was_busy != launch_preparation_pending_locked();
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
  if (switched) {
    log_debug("  [SHELLCORE] game switch prepared: outgoing=%s incoming=%s",
              outgoing_title_id, title_id);
  }
  return 0;
}

static void clear_owned_title(const char *title_id) {
  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  shellcore_mount_owner_t *owner = find_mount_owner_locked(title_id);
  if (owner) {
    if (owner == &g_service.prepared) {
      g_service.prepare_in_progress = false;
      g_service.launch_pending = false;
    }
    clear_mount_owner(owner);
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
}

static void rollback_prepared_title(const char *title_id) {
  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  if (mount_owner_matches(&g_service.prepared, title_id)) {
    clear_mount_owner(&g_service.prepared);
    if (g_service.outgoing.title_id[0] != '\0') {
      g_service.prepared = g_service.outgoing;
      clear_mount_owner(&g_service.outgoing);
    }
    g_service.prepare_in_progress = false;
    g_service.launch_pending = false;
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
}

static void finish_prepared_title(const char *title_id,
                                  bool cancel_launch) {
  pthread_mutex_lock(&g_service.mutex);
  bool was_busy = launch_preparation_pending_locked();
  if (mount_owner_matches(&g_service.prepared, title_id)) {
    g_service.prepare_in_progress = false;
    if (cancel_launch)
      g_service.launch_pending = false;
  }
  bool changed = was_busy != launch_preparation_pending_locked();
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
}

static int begin_runtime_release(const char *title_id, bool public_request,
                                 const shellcore_mount_owner_t *expected) {
  pthread_mutex_lock(&g_service.mutex);
  shellcore_mount_owner_t *owner = find_mount_owner_locked(title_id);
  bool prepared = owner == &g_service.prepared;
  bool has_owner = g_service.prepared.title_id[0] != '\0' ||
                   g_service.outgoing.title_id[0] != '\0';
  // A worker must be able to clean its unowned temporary runtime even if the
  // live game exited meanwhile and its old owner is still awaiting cleanup.
  bool external_cleanup = !expected && g_service.external_mutation_in_progress;
  bool different_title = has_owner && !owner && !external_cleanup &&
                         !(public_request && sm_game_lifecycle_has_active_game());
  bool superseded = expected &&
      (!owner || !owner->game_exited || owner->pid != expected->pid ||
       owner->app_id != expected->app_id ||
       owner->exit_time_us != expected->exit_time_us ||
       (prepared && g_service.launch_pending) ||
       strcmp(g_service.pending_launch_title_id, title_id) == 0);
  // A failed/repeated launch is not an exit of the process already bound to
  // this runtime. Only the lifecycle's exit snapshot may release that owner.
  bool live_owner = !public_request && !expected && owner &&
                    owner->pid > 0 && !owner->game_exited;
  bool blocked = live_owner || superseded || different_title ||
                 g_service.releasing_title_id[0] != '\0' ||
                 (prepared && g_service.prepare_in_progress) ||
                 (public_request && g_service.external_mutation_in_progress) ||
                 (public_request && launch_preparation_pending_locked()) ||
                 (public_request && owner && owner->pid > 0 &&
                  !owner->game_exited) ||
                 (public_request && prepared &&
                  (g_service.launch_pending || g_service.prepared.app_id != 0));
  if (!blocked) {
    (void)strlcpy(g_service.releasing_title_id, title_id,
                  sizeof(g_service.releasing_title_id));
  }
  pthread_mutex_unlock(&g_service.mutex);
  return blocked ? EBUSY : 0;
}

static void end_runtime_release(const char *title_id) {
  pthread_mutex_lock(&g_service.mutex);
  if (strcmp(g_service.releasing_title_id, title_id) == 0)
    g_service.releasing_title_id[0] = '\0';
  pthread_mutex_unlock(&g_service.mutex);
}

static bool shellcore_service_stopping(void) {
  pthread_mutex_lock(&g_service.mutex);
  bool stopping = g_service.stop_requested;
  pthread_mutex_unlock(&g_service.mutex);
  return stopping;
}

static bool find_required_image_layer(const char *root,
                                      const char *runtime_source,
                                      unsigned int depth,
                                      char image_path[MAX_PATH]) {
  DIR *dir = opendir(root);
  if (!dir)
    return false;

  bool found = false;
  struct dirent *entry;
  while (!found && (entry = readdir(dir)) != NULL) {
    if (entry->d_name[0] == '.')
      continue;

    char path[MAX_PATH];
    int written = snprintf(path, sizeof(path), "%s/%s", root, entry->d_name);
    if (written < 0 || (size_t)written >= sizeof(path))
      continue;

    struct stat st;
    if (stat(path, &st) != 0)
      continue;
    if (S_ISREG(st.st_mode) &&
        is_supported_image_file_path(path, entry->d_name)) {
      char mount_point[MAX_PATH];
      get_image_mount_point_for_source(path, mount_point);
      if (path_matches_root_or_child(runtime_source, mount_point)) {
        (void)strlcpy(image_path, path, MAX_PATH);
        found = true;
      }
      continue;
    }
    if (depth > 0 && S_ISDIR(st.st_mode))
      found = find_required_image_layer(path, runtime_source, depth - 1u,
                                        image_path);
  }

  closedir(dir);
  return found;
}

static bool repair_image_chain_for_runtime_source(
    const char *title_id, const char *runtime_source,
    char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH], size_t *image_count,
    const bool *mount_read_only_override) {
  while (*image_count < MAX_IMAGE_CHAIN_DEPTH) {
    char mounted_root[MAX_PATH];
    get_image_mount_point_for_source(image_chain[*image_count - 1u],
                                     mounted_root);
    char image_path[MAX_PATH];
    if (!find_required_image_layer(mounted_root, runtime_source,
                                   MAX_SCAN_DEPTH, image_path)) {
      return false;
    }

    const char *name = strrchr(image_path, '/');
    name = name ? name + 1 : image_path;
    bool unstable = false;
    if (!maybe_mount_image_file_with_mode(image_path, name, &unstable,
                                          mount_read_only_override))
      return false;

    (void)strlcpy(image_chain[*image_count], image_path, MAX_PATH);
    (*image_count)++;
    if (!write_mount_image_chain(title_id, image_chain, *image_count)) {
      log_debug("  [SHELLCORE] failed to persist repaired image chain: %s",
                title_id);
    } else {
      log_debug("  [SHELLCORE] image chain repaired: title=%s layers=%zu",
                title_id, *image_count);
    }

    if (directory_has_game_executable(runtime_source)) {
      return true;
    }
  }
  return false;
}

static bool prepare_image_source(const char *title_id,
                                 const char *runtime_source,
                                 const bool *mount_read_only_override) {
  if (directory_has_game_executable(runtime_source) && !mount_read_only_override)
    return true;
  if (!is_under_image_mount_base(runtime_source))
    return false;

  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (!read_mount_image_chain(title_id, image_chain, &image_count))
    return false;
  // The USB filesystem can become available shortly after WORKING. Avoid
  // repeatedly entering the image attach path until its outer layer exists.
  if (!path_exists(image_chain[0]))
    return false;
  for (size_t i = 0; i < image_count; ++i) {
    const char *name = strrchr(image_chain[i], '/');
    name = name ? name + 1 : image_chain[i];
    bool unstable = false;
    if (!maybe_mount_image_file_with_mode(image_chain[i], name, &unstable,
                                          mount_read_only_override)) {
      log_debug("  [SHELLCORE] image chain mount failed: title=%s layer=%zu "
                "path=%s",
                title_id, i, image_chain[i]);
      return false;
    }
  }
  if (directory_has_game_executable(runtime_source))
    return true;
  if (repair_image_chain_for_runtime_source(title_id, runtime_source,
                                            image_chain,
                                            &image_count,
                                            mount_read_only_override))
    return true;
  log_debug("  [SHELLCORE] image chain ready but eboot.bin/eboot2.bin missing: %s",
            runtime_source);
  return false;
}

static bool resumed_runtime_source_ready(const char *title_id,
                                         char runtime_source[MAX_PATH]) {
  char refreshed_source[MAX_PATH];
  if (read_mount_link(title_id, refreshed_source, sizeof(refreshed_source)) &&
      strcmp(refreshed_source, runtime_source) != 0) {
    (void)strlcpy(runtime_source, refreshed_source, MAX_PATH);
    log_debug("  [SHELLCORE] resumed source path updated: %s -> %s", title_id,
              runtime_source);
  }

  if (is_usb_storage_path(runtime_source))
    return directory_has_game_executable(runtime_source);
  if (!is_under_image_mount_base(runtime_source))
    return true;

  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (!read_mount_image_chain(title_id, image_chain, &image_count) ||
      image_count == 0) {
    return false;
  }
  return !is_usb_storage_path(image_chain[0]) || path_exists(image_chain[0]);
}

static bool wait_for_resumed_runtime_source(const char *title_id,
                                            char runtime_source[MAX_PATH]) {
  if (!runtime_resume_grace_active())
    return true;

  if (resumed_runtime_source_ready(title_id, runtime_source))
    return true;

  log_debug("  [SHELLCORE] waiting for resumed source: %s", title_id);
  while (runtime_resume_grace_active() && !runtime_sleep_mode_active() &&
         !should_stop_requested() && !shellcore_service_stopping()) {
    if (resumed_runtime_source_ready(title_id, runtime_source)) {
      log_debug("  [SHELLCORE] resumed source ready: %s", title_id);
      return true;
    }
    (void)sceKernelUsleep(RUNTIME_RESUME_POLL_US);
  }
  if (runtime_sleep_mode_active() || should_stop_requested() ||
      shellcore_service_stopping()) {
    return false;
  }
  return resumed_runtime_source_ready(title_id, runtime_source);
}

static bool prepare_title_runtime(const char *title_id,
                                  const char *source_path,
                                  const bool *mount_read_only_override) {
  if (!prepare_image_source(title_id, source_path,
                            mount_read_only_override))
    return false;
  sm_fakelib_prepare_title_cache(title_id, source_path);
  runtime_mount_state_lock();
  bool title_mounted = !runtime_sleep_mode_active() &&
                       mount_title_nullfs(title_id, source_path);
  bool ready = title_mounted;
  if (ready)
    ready = mount_backport_overlay_for_title(source_path, title_id, NULL, NULL);
  if (title_mounted && !ready)
    (void)unmount_title_runtime_layers(title_id);
  runtime_mount_state_unlock();
  return ready;
}

static bool release_title_runtime(const char *title_id,
                                  bool public_request);

static void cleanup_abandoned_launch(const char *next_title_id) {
  char abandoned[MAX_TITLE_ID] = {0};
  pthread_mutex_lock(&g_service.mutex);
  const shellcore_mount_owner_t *owner = &g_service.prepared;
  if (owner->title_id[0] != '\0' &&
      !mount_owner_matches(owner, next_title_id) &&
      owner->pid <= 0 && owner->app_id == 0 && g_service.launch_pending &&
      !g_service.prepare_in_progress && !g_service.external_mutation_in_progress &&
      g_service.outgoing.title_id[0] == '\0' &&
      g_service.releasing_title_id[0] == '\0' &&
      strcmp(g_service.pending_launch_title_id, owner->title_id) != 0) {
    (void)strlcpy(abandoned, owner->title_id, sizeof(abandoned));
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (abandoned[0] == '\0')
    return;

  // A different explicit launch ends ShellCore's retry of this unbound title.
  // The release gate rechecks ownership if NOTE_EXEC bound it meanwhile.
  if (release_title_runtime(abandoned, false)) {
    log_debug("  [SHELLCORE] abandoned launch released: %s (launching %s)",
              abandoned, next_title_id);
  } else {
    log_debug("  [SHELLCORE] abandoned launch cleanup incomplete: %s (%s)",
              abandoned, strerror(errno));
  }
}

static int mount_managed_title_runtime(const char *title_id,
                                       bool allow_unmanaged,
                                       bool launch_request,
                                       const bool *mount_read_only_override) {
  if (!is_supported_game_title_id(title_id))
    return EINVAL;
  if (runtime_sleep_mode_active())
    return EBUSY;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return allow_unmanaged ? 0 : ENOENT;
  if (mount_read_only_override && !is_under_image_mount_base(source_path))
    return ENOTSUP;
  if (!wait_for_resumed_runtime_source(title_id, source_path))
    return EIO;

  bool claimed = false;
  int claim_status =
      claim_prepared_title(title_id, launch_request, &claimed);
  if (claim_status != 0)
    return claim_status;
  if (!claimed && is_data_mounted(title_id) &&
      !mount_read_only_override) {
    if (launch_request)
      sm_fakelib_prepare_title_cache(title_id, source_path);
    finish_prepared_title(title_id, false);
    return 0;
  }

  errno = 0;
  if (prepare_title_runtime(title_id, source_path,
                            mount_read_only_override)) {
    finish_prepared_title(title_id, false);
    return 0;
  }
  int prepare_errno = errno;
  if (claimed)
    rollback_prepared_title(title_id);
  else
    finish_prepared_title(title_id, true);
  if (prepare_errno == EBUSY ||
      (mount_read_only_override && prepare_errno == ENOTSUP)) {
    return prepare_errno;
  }
  return EIO;
}

bool sm_shellcore_ensure_title_runtime(const char *title_id) {
  return mount_managed_title_runtime(title_id, true, true, NULL) == 0;
}

static int handle_launch_request(const char *title_id) {
  pthread_mutex_lock(&g_service.mutex);
  bool was_busy = launch_preparation_pending_locked();
  g_service.pending_launch_title_id[0] = '\0';
  g_service.pending_sandbox_ready_handled = false;
  if (is_supported_game_title_id(title_id)) {
    (void)strlcpy(g_service.pending_launch_title_id, title_id,
                  sizeof(g_service.pending_launch_title_id));
    if (mount_owner_matches(&g_service.prepared, title_id))
      g_service.prepared.sandbox_ready_handled = false;
  }
  bool changed = was_busy != launch_preparation_pending_locked();
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();

  if (is_supported_game_title_id(title_id) && !runtime_sleep_mode_active())
    cleanup_abandoned_launch(title_id);

  int status = mount_managed_title_runtime(title_id, false, true, NULL);
  if (status != 0) {
    // A rejected managed request must not veto the old owner's exit cleanup.
    // Stock titles still need their pending identity for sandbox-ready.
    if (status != ENOENT && status != EINVAL) {
      pthread_mutex_lock(&g_service.mutex);
      was_busy = launch_preparation_pending_locked();
      if (!g_service.prepare_in_progress && !g_service.launch_pending &&
          strcmp(g_service.pending_launch_title_id, title_id) == 0) {
        g_service.pending_launch_title_id[0] = '\0';
        g_service.pending_sandbox_ready_handled = false;
      }
      changed = was_busy != launch_preparation_pending_locked();
      pthread_mutex_unlock(&g_service.mutex);
      if (changed)
        sm_scanner_wake();
    }
    // The launch hook also observes stock games and ShellCore system apps.
    // ENOENT/EINVAL mean "not managed here", not a runtime mount failure.
    if (status != ENOENT && status != EINVAL) {
      log_debug("  [SHELLCORE] launch mount unavailable: %s status=%d (%s)",
                title_id, status, strerror(status));
    }
    return status;
  }
  log_debug("  [SHELLCORE] launch mount ready: %s", title_id);
  return 0;
}

static bool title_runtime_is_protected(const char *title_id,
                                       const char *source_path) {
  if (sm_shellcore_title_is_protected(title_id) ||
      sm_shellcore_source_is_protected(source_path))
    return true;
  char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t count = 0;
  if (!read_mount_image_chain(title_id, chain, &count))
    return is_under_image_mount_base(source_path);
  for (size_t i = 0; i < count; ++i)
    if (sm_shellcore_source_is_protected(chain[i]))
      return true;
  return false;
}

int sm_shellcore_mount_title_runtime(
    const char *title_id, const bool *mount_read_only_override) {
  if (!is_supported_game_title_id(title_id))
    return EINVAL;
  if (!sm_game_lifecycle_has_active_game())
    return mount_managed_title_runtime(title_id, false, false,
                                       mount_read_only_override);
  if (!sm_shellcore_try_begin_external_mutation())
    return EBUSY;

  // Discovery-style mounts do not take the live game's prepared owner slot.
  // The persisted mount link/chain remains the owner of this inactive title.
  int status = 0;
  char source_path[MAX_PATH];
  if (runtime_sleep_mode_active() || sm_shellcore_background_work_blocked())
    status = EBUSY;
  else if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    status = ENOENT;
  else if (title_runtime_is_protected(title_id, source_path))
    status = EBUSY;
  else if (mount_read_only_override && !is_under_image_mount_base(source_path))
    status = ENOTSUP;
  else {
    errno = 0;
    if (!prepare_title_runtime(title_id, source_path,
                                 mount_read_only_override))
      status = errno != 0 ? errno : EIO;
  }
  sm_shellcore_end_external_mutation();
  return status;
}

int sm_shellcore_unmount_title_runtime(const char *title_id) {
  if (!is_supported_game_title_id(title_id))
    return EINVAL;
  if (runtime_sleep_mode_active())
    return EBUSY;
  if (sm_game_lifecycle_has_active_game() &&
      sm_shellcore_background_work_blocked())
    return EBUSY;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return ENOENT;
  if (title_runtime_is_protected(title_id, source_path))
    return EBUSY;
  bool released = release_title_runtime(title_id, true);
  int status = released ? 0 : (errno == EBUSY ? EBUSY : EIO);
  return status;
}

static size_t read_other_owned_image_chain(
    const char *releasing_title_id,
    char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH]) {
  char other_title_id[MAX_TITLE_ID] = {0};
  pthread_mutex_lock(&g_service.mutex);
  if (g_service.prepared.title_id[0] != '\0' &&
      strcmp(g_service.prepared.title_id, releasing_title_id) != 0)
    (void)strlcpy(other_title_id, g_service.prepared.title_id, MAX_TITLE_ID);
  else if (g_service.outgoing.title_id[0] != '\0' &&
           strcmp(g_service.outgoing.title_id, releasing_title_id) != 0)
    (void)strlcpy(other_title_id, g_service.outgoing.title_id, MAX_TITLE_ID);
  pthread_mutex_unlock(&g_service.mutex);

  if (other_title_id[0] == '\0')
    return 0;

  size_t image_count = 0;
  if (!read_mount_image_chain(other_title_id, image_chain, &image_count))
    return 0;
  return image_count;
}

static bool image_chain_contains(
    const char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH],
    size_t image_count, const char *image_path) {
  for (size_t i = 0; i < image_count; ++i) {
    if (strcmp(image_chain[i], image_path) == 0)
      return true;
  }
  return false;
}

static bool release_backing_image(const char *title_id) {
  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (!read_mount_image_chain(title_id, image_chain, &image_count))
    return true;

  char retained_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t retained_count =
      read_other_owned_image_chain(title_id, retained_chain);
  for (size_t layer = image_count; layer > 0; --layer) {
    if (image_chain_contains(retained_chain, retained_count,
                             image_chain[layer - 1u]))
      continue;
    if (!release_runtime_image_mount(image_chain[layer - 1u]))
      return false;
  }
  return true;
}

static bool release_title_runtime_expected(
    const char *title_id, bool public_request,
    const shellcore_mount_owner_t *expected) {
  if (!title_id || title_id[0] == '\0')
    return false;
  int begin_status = begin_runtime_release(title_id, public_request, expected);
  if (begin_status != 0) {
    errno = begin_status;
    return false;
  }
  log_debug("  [SHELLCORE] runtime release start: %s", title_id);
  bool released = false;
  int release_errno = 0;
  unsigned int attempts = public_request ? 1u : SHELLCORE_RELEASE_MAX_ATTEMPTS;
  unsigned int attempt = 0;
  for (; attempt < attempts; ++attempt) {
    bool diagnose_busy = attempt + 1u == attempts;
    runtime_mount_state_lock();
    released = diagnose_busy
                   ? unmount_title_runtime_layers(title_id)
                   : unmount_title_runtime_layers_quiet(title_id);
    if (released)
      released = release_backing_image(title_id);
    release_errno = released ? 0 : errno;
    runtime_mount_state_unlock();

    if (released || release_errno != EBUSY || attempt + 1u == attempts ||
        shellcore_service_stopping()) {
      break;
    }
    if (attempt == 0) {
      log_debug("  [SHELLCORE] runtime release busy; fast retry armed: %s "
                "attempts=%u delay_ms=%u",
                title_id, attempts - 1u,
                SHELLCORE_RELEASE_RETRY_DELAY_US / 1000u);
    }
    (void)sceKernelUsleep(SHELLCORE_RELEASE_RETRY_DELAY_US);
  }
  if (released && attempt > 0) {
    log_debug("  [SHELLCORE] runtime release completed after retry: %s "
              "attempt=%u",
              title_id, attempt + 1u);
  }
  // Do not leave a failed release marked as prepared forever. The intact stack
  // remains discoverable through mount.lnk and normal scanner cleanup retries
  // it after any remaining transient reference disappears.
  clear_owned_title(title_id);
  end_runtime_release(title_id);
  if (!released && release_errno != 0)
    errno = release_errno;
  return released;
}

static bool release_title_runtime(const char *title_id, bool public_request) {
  return release_title_runtime_expected(title_id, public_request, NULL);
}

bool sm_shellcore_release_title_runtime(const char *title_id) {
  if (sm_shellcore_title_is_protected(title_id)) {
    errno = EBUSY;
    return false;
  }
  return release_title_runtime(title_id, false);
}

bool sm_shellcore_service_bind_prepared_app(const char *title_id,
                                            uint32_t app_id, pid_t pid) {
  if (!title_id || title_id[0] == '\0' || pid <= 0)
    return false;

  char source_path[MAX_PATH];
  bool managed = read_mount_link(title_id, source_path, sizeof(source_path));
  pthread_mutex_lock(&g_service.mutex);
  shellcore_mount_owner_t *owner = find_mount_owner_locked(title_id);
  if (strcmp(g_service.releasing_title_id, title_id) == 0 ||
      (!owner && managed)) {
    pthread_mutex_unlock(&g_service.mutex);
    return false;
  }
  bool was_busy = launch_preparation_pending_locked();
  if (owner) {
    owner->app_id = app_id;
    owner->pid = pid;
    owner->game_exited = false;
    owner->exit_time_us = 0;
    if (owner == &g_service.prepared) {
      g_service.prepare_in_progress = false;
      g_service.launch_pending = false;
    }
  }
  if (strcmp(g_service.pending_launch_title_id, title_id) == 0) {
    g_service.pending_launch_title_id[0] = '\0';
    g_service.pending_sandbox_ready_handled = false;
  }
  bool changed = was_busy != launch_preparation_pending_locked();
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
  return true;
}

bool sm_shellcore_service_note_game_exit(pid_t pid, uint64_t exit_time_us,
                                         char title_id_out[MAX_TITLE_ID]) {
  if (title_id_out)
    title_id_out[0] = '\0';
  if (pid <= 0)
    return false;

  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  shellcore_mount_owner_t *owner = NULL;
  if (g_service.outgoing.pid == pid)
    owner = &g_service.outgoing;
  else if (g_service.prepared.pid == pid)
    owner = &g_service.prepared;
  if (owner && !owner->game_exited) {
    owner->game_exited = true;
    owner->exit_time_us = exit_time_us != 0 ? exit_time_us : monotonic_time_us();
    if (title_id_out)
      (void)strlcpy(title_id_out, owner->title_id, MAX_TITLE_ID);
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  return changed;
}

bool sm_shellcore_service_title_sandbox_exists(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  DIR *dir = opendir("/mnt/sandbox");
  if (!dir) {
    if (errno != ENOENT) {
      log_debug("  [SHELLCORE] sandbox lookup failed for %s: %s", title_id,
                strerror(errno));
      return true;
    }
    return false;
  }

  size_t title_len = strlen(title_id);
  bool found = false;
  struct dirent *entry;
  errno = 0;
  while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, title_id, title_len) != 0 ||
        entry->d_name[title_len] != '_' ||
        entry->d_name[title_len + 1u] == '\0') {
      continue;
    }
    found = true;
    break;
  }
  int read_errno = errno;
  closedir(dir);
  if (!found && read_errno != 0) {
    log_debug("  [SHELLCORE] sandbox scan failed for %s: %s", title_id,
              strerror(read_errno));
    return true;
  }
  return found;
}

uint64_t sm_shellcore_service_next_exit_cleanup_us(uint64_t now_us) {
  uint64_t next_us = 0;
  pthread_mutex_lock(&g_service.mutex);
  const shellcore_mount_owner_t *owners[] = {&g_service.prepared,
                                            &g_service.outgoing};
  for (size_t i = 0; i < 2; ++i) {
    const shellcore_mount_owner_t *owner = owners[i];
    if (!owner->title_id[0] || !owner->game_exited)
      continue;
    uint64_t due_us = now_us + GAME_PROCESS_HANDOFF_FALLBACK_POLL_US;
    uint64_t deadline_us = owner->exit_time_us + GAME_EXIT_SANDBOX_LINGER_US;
    if (owner->exit_time_us != 0 && now_us < deadline_us && deadline_us < due_us)
      due_us = deadline_us;
    if (next_us == 0 || due_us < next_us)
      next_us = due_us;
  }
  pthread_mutex_unlock(&g_service.mutex);
  return next_us;
}

int sm_shellcore_service_release_exited_titles(void) {
  shellcore_mount_owner_t exited[2] = {0};
  size_t exited_count = 0;

  pthread_mutex_lock(&g_service.mutex);
  if (g_service.outgoing.title_id[0] != '\0' &&
      g_service.outgoing.game_exited) {
    exited[exited_count++] = g_service.outgoing;
  }
  if (g_service.prepared.title_id[0] != '\0' &&
      g_service.prepared.game_exited &&
      (exited_count == 0 ||
        strcmp(exited[0].title_id, g_service.prepared.title_id) != 0)) {
    exited[exited_count++] = g_service.prepared;
  }
  pthread_mutex_unlock(&g_service.mutex);

  bool waiting = false;
  int release_error = 0;
  for (size_t i = 0; i < exited_count; ++i) {
    const char *title_id = exited[i].title_id;
    if (sm_shellcore_service_title_sandbox_exists(title_id)) {
      uint64_t now_us = monotonic_time_us();
      if (now_us == 0 || exited[i].exit_time_us == 0 ||
          now_us - exited[i].exit_time_us < GAME_EXIT_SANDBOX_LINGER_US) {
        waiting = true;
        continue;
      }
    }

    if (!release_title_runtime_expected(title_id, false, &exited[i])) {
      if (errno == EBUSY) {
        waiting = true;
        continue;
      }
      release_error = errno != 0 ? errno : EIO;
      log_debug("  [SHELLCORE] sandbox-triggered release incomplete; scanner "
                "cleanup will retry: %s (%s)",
                title_id, strerror(release_error));
    }
  }
  if (release_error != 0)
    return release_error;
  if (waiting)
    return EAGAIN;

  pthread_mutex_lock(&g_service.mutex);
  bool exit_pending = g_service.prepared.game_exited ||
                      g_service.outgoing.game_exited;
  pthread_mutex_unlock(&g_service.mutex);
  if (exit_pending)
    return EAGAIN;
  return 0;
}

bool sm_shellcore_service_has_prepared_mount(void) {
  pthread_mutex_lock(&g_service.mutex);
  // Keep scanner cleanup blocked until the lifecycle watcher releases the
  // owned stack, including its bounded wait for lingering sandbox entries.
  bool prepared = g_service.prepared.title_id[0] != '\0' ||
                  g_service.outgoing.title_id[0] != '\0';
  pthread_mutex_unlock(&g_service.mutex);
  return prepared;
}

bool sm_shellcore_background_work_blocked(void) {
  char title_id[MAX_TITLE_ID];
  if (!sm_game_lifecycle_background_title(title_id))
    return true;
  pthread_mutex_lock(&g_service.mutex);
  bool blocked = g_service.stop_requested ||
      launch_preparation_pending_locked() ||
      g_service.releasing_title_id[0] != '\0' ||
      g_service.outgoing.title_id[0] != '\0' ||
      (g_service.prepared.title_id[0] != '\0' &&
       (strcmp(g_service.prepared.title_id, title_id) != 0 ||
        g_service.prepared.pid <= 0 || g_service.prepared.game_exited));
  bool managed = g_service.prepared.title_id[0] != '\0';
  pthread_mutex_unlock(&g_service.mutex);
  if (blocked || title_id[0] == '\0')
    return blocked;

  char runtime_path[MAX_PATH];
  char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t count = 0;
  // Never permit cleanup with an unknown backing chain for an active image.
  if (!read_mount_link(title_id, runtime_path, sizeof(runtime_path)))
    return managed;
  return is_under_image_mount_base(runtime_path) &&
         (!read_mount_image_chain(title_id, chain, &count) || count == 0);
}

static size_t snapshot_protected_titles(char titles[3][MAX_TITLE_ID]) {
  if (runtime_sleep_mode_active() || should_stop_requested())
    return 0;
  size_t count = 0;
  char background_title[MAX_TITLE_ID];
  (void)sm_game_lifecycle_background_title(background_title);
  if (background_title[0] != '\0') {
    (void)strlcpy(titles[count++], background_title, MAX_TITLE_ID);
  }
  pthread_mutex_lock(&g_service.mutex);
  if (g_service.stop_requested) {
    pthread_mutex_unlock(&g_service.mutex);
    return 0;
  }
  const shellcore_mount_owner_t *owners[] = {&g_service.prepared,
                                            &g_service.outgoing};
  for (size_t i = 0; i < 2u; ++i) {
    const shellcore_mount_owner_t *owner = owners[i];
    if (!owner->title_id[0] || owner->game_exited || owner->pid <= 0)
      continue;
    bool duplicate = false;
    for (size_t j = 0; j < count; ++j)
      duplicate |= strcmp(titles[j], owner->title_id) == 0;
    if (!duplicate)
      (void)strlcpy(titles[count++], owner->title_id, MAX_TITLE_ID);
  }
  pthread_mutex_unlock(&g_service.mutex);
  return count;
}

bool sm_shellcore_title_is_protected(const char *title_id) {
  if (!title_id || !title_id[0])
    return false;
  char titles[3][MAX_TITLE_ID];
  size_t count = snapshot_protected_titles(titles);
  for (size_t i = 0; i < count; ++i)
    if (strcmp(title_id, titles[i]) == 0)
      return true;
  return false;
}

bool sm_shellcore_source_is_protected(const char *path) {
  if (!path || !path[0])
    return false;
  if (sm_fakelib_source_is_in_use(path) || sm_pkg_backport_source_is_in_use(path))
    return true;
  char titles[3][MAX_TITLE_ID];
  size_t count = snapshot_protected_titles(titles);
  for (size_t i = 0; i < count; ++i) {
    char runtime_path[MAX_PATH];
    if (!read_mount_link(titles[i], runtime_path, sizeof(runtime_path))) {
      if (sm_shellcore_service_title_is_prepared(titles[i]))
        return true;
      continue; // Stock PKGs have no managed source; title protection applies.
    }
    if (paths_overlap(path, runtime_path))
      return true;
    char chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
    size_t layers = 0;
    if (!read_mount_image_chain(titles[i], chain, &layers) || layers == 0) {
      if (is_under_image_mount_base(runtime_path))
        return true;
      continue;
    }
    for (size_t layer = 0; layer < layers; ++layer)
      if (paths_overlap(path, chain[layer]))
        return true;
  }
  return false;
}

bool sm_shellcore_try_begin_external_mutation(void) {
  pthread_mutex_lock(&g_service.mutex);
  bool available = !g_service.external_mutation_in_progress &&
                   !launch_preparation_pending_locked() &&
                   g_service.releasing_title_id[0] == '\0';
  if (available)
    g_service.external_mutation_in_progress = true;
  pthread_mutex_unlock(&g_service.mutex);
  return available;
}

void sm_shellcore_end_external_mutation(void) {
  pthread_mutex_lock(&g_service.mutex);
  g_service.external_mutation_in_progress = false;
  pthread_mutex_unlock(&g_service.mutex);
}

bool sm_shellcore_service_title_is_prepared(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  pthread_mutex_lock(&g_service.mutex);
  bool prepared = find_mount_owner_locked(title_id) != NULL;
  pthread_mutex_unlock(&g_service.mutex);
  return prepared;
}

static bool reset_switch_title_mount(const char *title_id) {
  int status = begin_runtime_release(title_id, false, NULL);
  if (status != 0) {
    errno = status;
    return false;
  }
  runtime_mount_state_lock();
  bool reset = unmount_title_runtime_layers(title_id);
  int reset_errno = reset ? 0 : errno;
  runtime_mount_state_unlock();
  end_runtime_release(title_id);
  if (!reset)
    errno = reset_errno != 0 ? reset_errno : EIO;
  return reset;
}

static int handle_launch_failed_request(const char *title_id) {
  if (runtime_sleep_mode_active())
    return EBUSY;
  if (!title_id || title_id[0] == '\0')
    return 0;

  sm_fakelib_game_on_launch_failed(title_id);
  sm_pkg_backport_on_launch_failed(title_id);

  pthread_mutex_lock(&g_service.mutex);
  bool was_busy = launch_preparation_pending_locked();
  if (strcmp(g_service.pending_launch_title_id, title_id) == 0) {
    g_service.pending_launch_title_id[0] = '\0';
    g_service.pending_sandbox_ready_handled = false;
  }
  bool prepared = mount_owner_matches(&g_service.prepared, title_id);
  pid_t bound_pid = prepared ? g_service.prepared.pid : 0;
  if (bound_pid > 0)
    g_service.launch_pending = false;
  bool switch_pending = prepared && g_service.outgoing.title_id[0] != '\0';
  bool changed = was_busy != launch_preparation_pending_locked();
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
  if (!prepared)
    return 0;
  if (bound_pid > 0) {
    log_debug("  [SHELLCORE] launch failure ignored for bound process: %s pid=%ld",
              title_id, (long)bound_pid);
    return 0;
  }
  if (switch_pending) {
    // ShellCore retries launchApp while closing the outgoing game. Keep the
    // expensive backing images attached, but recreate title nullfs/overlay on
    // every retry so the final mount is newer than ShellCore's exit cleanup.
    bool reset = reset_switch_title_mount(title_id);
    if (reset) {
      log_debug("  [SHELLCORE] launch retry prepared during game switch: %s",
                title_id);
    } else {
      log_debug("  [SHELLCORE] failed to reset switch mount: %s (%s)",
                title_id, strerror(errno));
    }
    return reset ? 0 : EBUSY;
  }
  return sm_shellcore_release_title_runtime(title_id) ? 0 : EBUSY;
}

static bool shellcore_client_active(int fd) {
  pthread_mutex_lock(&g_service.mutex);
  bool active = !g_service.stop_requested && !runtime_sleep_mode_active() &&
                g_service.client_fd == fd;
  pthread_mutex_unlock(&g_service.mutex);
  return active;
}

static int handle_sandbox_ready_request(const char *title_id) {
  if (runtime_sleep_mode_active())
    return EBUSY;

  char effective_title_id[MAX_TITLE_ID] = {0};
  bool already_handled = false;
  bool pending_launch = false;
  pthread_mutex_lock(&g_service.mutex);
  if (title_id && is_supported_game_title_id(title_id)) {
    (void)strlcpy(effective_title_id, title_id,
                  sizeof(effective_title_id));
  } else if (is_supported_game_title_id(
                 g_service.pending_launch_title_id)) {
    (void)strlcpy(effective_title_id,
                  g_service.pending_launch_title_id,
                  sizeof(effective_title_id));
  } else if (is_supported_game_title_id(g_service.prepared.title_id)) {
    (void)strlcpy(effective_title_id, g_service.prepared.title_id,
                  sizeof(effective_title_id));
  }
  pending_launch =
      effective_title_id[0] != '\0' &&
      strcmp(g_service.pending_launch_title_id, effective_title_id) == 0;
  shellcore_mount_owner_t *owner =
      find_mount_owner_locked(effective_title_id);
  if (pending_launch)
    already_handled = g_service.pending_sandbox_ready_handled;
  else if (owner)
    already_handled = owner->sandbox_ready_handled;
  pthread_mutex_unlock(&g_service.mutex);

  if (effective_title_id[0] == '\0' || already_handled)
    return 0;

  errno = 0;
  bool package_ready =
      sm_pkg_backport_on_sandbox_ready(effective_title_id);
  int package_status = package_ready ? 0 : (errno != 0 ? errno : EIO);

  if (sm_fakelib_game_on_sandbox_ready(effective_title_id)) {
    pthread_mutex_lock(&g_service.mutex);
    if (strcmp(g_service.pending_launch_title_id,
               effective_title_id) == 0) {
      g_service.pending_sandbox_ready_handled = true;
    }
    owner = find_mount_owner_locked(effective_title_id);
    if (owner)
      owner->sandbox_ready_handled = true;
    pthread_mutex_unlock(&g_service.mutex);
    log_debug("  [SHELLCORE] sandbox ready handled: %s",
              effective_title_id);
    if (package_ready)
      return 0;
    log_debug("  [SHELLCORE] package fallback unavailable: %s status=%d "
              "(%s)",
              effective_title_id, package_status, strerror(package_status));
    return package_status;
  }

  int status = errno != 0 ? errno : EIO;
  log_debug("  [SHELLCORE] sandbox fakelib unavailable: %s status=%d (%s)",
            effective_title_id, status, strerror(status));
  return status;
}

static void handle_client(int fd) {
  if (!shellcore_client_active(fd))
    return;

  sm_shellcore_request_t request;
  sm_shellcore_response_t response;
  if (!sm_socket_read_full(fd, &request, sizeof(request)))
    return;
  if (!shellcore_client_active(fd))
    return;
  if (request.magic != SM_SHELLCORE_PROTOCOL_MAGIC ||
      request.version != SM_SHELLCORE_PROTOCOL_VERSION ||
      ((request.operation == SM_SHELLCORE_REQUEST_LAUNCH ||
        request.operation == SM_SHELLCORE_REQUEST_LAUNCH_FAILED ||
        request.operation == SM_SHELLCORE_REQUEST_SANDBOX_READY) &&
       strnlen(request.title_id, sizeof(request.title_id)) ==
           sizeof(request.title_id))) {
    response.status = EPROTO;
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH) {
    response.status = handle_launch_request(request.title_id);
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH_FAILED) {
    response.status = handle_launch_failed_request(request.title_id);
  } else if (request.operation == SM_SHELLCORE_REQUEST_SANDBOX_READY) {
    response.status = handle_sandbox_ready_request(request.title_id);
  } else {
    response.status = ENOTSUP;
  }
  if (shellcore_client_active(fd))
    (void)sm_socket_write_full(fd, &response, sizeof(response));
}

static int open_shellcore_listener(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;

  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  (void)strlcpy(address.sun_path, SM_SHELLCORE_SOCKET_PATH,
                sizeof(address.sun_path));
  (void)unlink(SM_SHELLCORE_SOCKET_PATH);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 4) != 0) {
    int saved_errno = errno;
    close(fd);
    (void)unlink(SM_SHELLCORE_SOCKET_PATH);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

static bool wait_until_shellcore_runtime_awake(void) {
  pthread_mutex_lock(&g_service.mutex);
  while (!g_service.stop_requested && runtime_sleep_mode_active()) {
    int rc = pthread_cond_wait(&g_service.cond, &g_service.mutex);
    if (rc != 0) {
      log_debug("  [SHELLCORE] sleep wait failed: %s", strerror(rc));
      pthread_mutex_unlock(&g_service.mutex);
      return false;
    }
  }
  bool keep_running = !g_service.stop_requested;
  pthread_mutex_unlock(&g_service.mutex);
  return keep_running;
}

static void *service_thread_main(void *arg) {
  (void)arg;
  int listen_fd = g_service.listen_fd;
  while (true) {
    if (!wait_until_shellcore_runtime_awake())
      break;

    if (listen_fd >= 0) {
      pthread_mutex_lock(&g_service.mutex);
      bool owns_listener = g_service.listen_fd == listen_fd;
      pthread_mutex_unlock(&g_service.mutex);
      if (!owns_listener)
        listen_fd = -1;
    }

    if (listen_fd < 0) {
      listen_fd = open_shellcore_listener();
      if (listen_fd < 0) {
        log_debug("  [SHELLCORE] listener restore failed: %s",
                  strerror(errno));
        break;
      }

      pthread_mutex_lock(&g_service.mutex);
      if (g_service.stop_requested || runtime_sleep_mode_active()) {
        bool stopping = g_service.stop_requested;
        pthread_mutex_unlock(&g_service.mutex);
        close(listen_fd);
        (void)unlink(SM_SHELLCORE_SOCKET_PATH);
        listen_fd = -1;
        if (stopping)
          break;
        continue;
      }
      g_service.listen_fd = listen_fd;
      pthread_mutex_unlock(&g_service.mutex);
      log_debug("  [SHELLCORE] Unix socket service resumed");
    }

    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      int accept_error = errno;
      pthread_mutex_lock(&g_service.mutex);
      bool stopping = g_service.stop_requested;
      bool owns_listener = g_service.listen_fd == listen_fd;
      pthread_mutex_unlock(&g_service.mutex);
      if (stopping)
        break;
      if (!owns_listener) {
        listen_fd = -1;
        continue;
      }
      if (accept_error == EINTR)
        continue;
      log_debug("  [SHELLCORE] accept failed: %s", strerror(accept_error));
      break;
    }

    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    pthread_mutex_lock(&g_service.mutex);
    bool owns_listener = g_service.listen_fd == listen_fd;
    if (g_service.stop_requested || runtime_sleep_mode_active() ||
        !owns_listener) {
      bool stopping = g_service.stop_requested;
      pthread_mutex_unlock(&g_service.mutex);
      close(client);
      if (!owns_listener)
        listen_fd = -1;
      if (stopping)
        break;
      continue;
    }
    g_service.client_fd = client;
    pthread_mutex_unlock(&g_service.mutex);

    handle_client(client);

    pthread_mutex_lock(&g_service.mutex);
    if (g_service.client_fd == client)
      g_service.client_fd = -1;
    pthread_mutex_unlock(&g_service.mutex);
    close(client);
  }
  return NULL;
}

bool sm_shellcore_service_start(void) {
  if (g_service.started)
    return true;

  int fd = open_shellcore_listener();
  if (fd < 0)
    return false;

  g_service.listen_fd = fd;
  g_service.client_fd = -1;
  g_service.stop_requested = false;
  clear_mount_owner(&g_service.prepared);
  clear_mount_owner(&g_service.outgoing);
  g_service.prepare_in_progress = false;
  g_service.launch_pending = false;
  g_service.pending_launch_title_id[0] = '\0';
  g_service.pending_sandbox_ready_handled = false;
  g_service.external_mutation_in_progress = false;
  g_service.releasing_title_id[0] = '\0';
  int rc = pthread_create(&g_service.thread, NULL, service_thread_main, NULL);
  if (rc != 0) {
    close(fd);
    g_service.listen_fd = -1;
    (void)unlink(SM_SHELLCORE_SOCKET_PATH);
    errno = rc;
    return false;
  }
  g_service.started = true;
  return true;
}

void sm_shellcore_service_stop(void) {
  if (!g_service.started) {
    sm_pkg_backport_shutdown();
    return;
  }
  pthread_mutex_lock(&g_service.mutex);
  g_service.stop_requested = true;
  int fd = g_service.listen_fd;
  int client_fd = g_service.client_fd;
  if (client_fd >= 0)
    (void)shutdown(client_fd, SHUT_RDWR);
  if (fd >= 0) {
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  g_service.listen_fd = -1;
  pthread_cond_broadcast(&g_service.cond);
  pthread_mutex_unlock(&g_service.mutex);
  pthread_join(g_service.thread, NULL);
  sm_pkg_backport_shutdown();
  pthread_mutex_lock(&g_service.mutex);
  g_service.started = false;
  g_service.stop_requested = false;
  g_service.client_fd = -1;
  clear_mount_owner(&g_service.prepared);
  clear_mount_owner(&g_service.outgoing);
  g_service.prepare_in_progress = false;
  g_service.launch_pending = false;
  g_service.pending_launch_title_id[0] = '\0';
  g_service.pending_sandbox_ready_handled = false;
  g_service.external_mutation_in_progress = false;
  g_service.releasing_title_id[0] = '\0';
  pthread_mutex_unlock(&g_service.mutex);
  (void)unlink(SM_SHELLCORE_SOCKET_PATH);
}

void sm_shellcore_service_on_sleep_change(bool active) {
  pthread_mutex_lock(&g_service.mutex);
  if (!g_service.started) {
    pthread_mutex_unlock(&g_service.mutex);
    return;
  }

  if (active) {
    if (g_service.client_fd >= 0)
      (void)shutdown(g_service.client_fd, SHUT_RDWR);
    if (g_service.listen_fd >= 0) {
      (void)shutdown(g_service.listen_fd, SHUT_RDWR);
      close(g_service.listen_fd);
    }
    g_service.listen_fd = -1;
    g_service.client_fd = -1;
    (void)unlink(SM_SHELLCORE_SOCKET_PATH);
  }
  pthread_cond_broadcast(&g_service.cond);
  pthread_mutex_unlock(&g_service.mutex);
}
