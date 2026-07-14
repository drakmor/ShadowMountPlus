#include "sm_platform.h"

#include <stddef.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_gameinfo.h"
#include "sm_image.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_shellcore_protocol.h"
#include "sm_shellcore_service.h"
#include "sm_socket_io.h"

_Static_assert(sizeof(sm_shellcore_request_t) == 40,
               "unexpected ShellCore request size");
_Static_assert(offsetof(sm_shellcore_request_t, payload) == 8,
               "unexpected ShellCore request payload offset");
_Static_assert(offsetof(sm_shellcore_request_t, payload.workspace.app_id) == 8,
               "unexpected ShellCore workspace app id offset");
_Static_assert(sizeof(sm_shellcore_response_t) == 4,
               "unexpected ShellCore response size");

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  bool started;
  bool stop_requested;
  int listen_fd;
  int client_fd;
  // Owner of the prepared runtime mount, not the lifecycle active-game state.
  char prepared_title_id[MAX_TITLE_ID];
  uint32_t prepared_app_id;
  bool prepared_game_exited;
  bool prepare_in_progress;
  bool launch_pending;
  bool release_in_progress;
} shellcore_service_state_t;

static shellcore_service_state_t g_service = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
    .client_fd = -1,
};

static int claim_prepared_title(const char *title_id, bool launch_request,
                                bool *claimed_out) {
  bool changed = false;
  if (claimed_out)
    *claimed_out = false;
  pthread_mutex_lock(&g_service.mutex);
  if (g_service.release_in_progress || g_service.prepare_in_progress) {
    pthread_mutex_unlock(&g_service.mutex);
    return EBUSY;
  }
  if (g_service.prepared_title_id[0] == '\0') {
    (void)strlcpy(g_service.prepared_title_id, title_id,
                  sizeof(g_service.prepared_title_id));
    g_service.prepared_app_id = 0;
    g_service.prepared_game_exited = false;
    g_service.prepare_in_progress = true;
    g_service.launch_pending = launch_request;
    changed = true;
    if (claimed_out)
      *claimed_out = true;
  } else if (strcmp(g_service.prepared_title_id, title_id) != 0) {
    pthread_mutex_unlock(&g_service.mutex);
    return EBUSY;
  } else if (g_service.prepared_game_exited) {
    pthread_mutex_unlock(&g_service.mutex);
    return EBUSY;
  } else {
    g_service.prepare_in_progress = true;
    if (launch_request)
      g_service.launch_pending = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
  return 0;
}

static void clear_prepared_title(const char *title_id) {
  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  if (strcmp(g_service.prepared_title_id, title_id) == 0) {
    g_service.prepared_title_id[0] = '\0';
    g_service.prepared_app_id = 0;
    g_service.prepared_game_exited = false;
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
  if (strcmp(g_service.prepared_title_id, title_id) == 0) {
    g_service.prepare_in_progress = false;
    if (cancel_launch)
      g_service.launch_pending = false;
  }
  pthread_mutex_unlock(&g_service.mutex);
}

static int begin_runtime_release(const char *title_id,
                                 bool public_request) {
  pthread_mutex_lock(&g_service.mutex);
  bool different_title = g_service.prepared_title_id[0] != '\0' &&
                         strcmp(g_service.prepared_title_id, title_id) != 0;
  bool blocked = different_title || g_service.release_in_progress ||
                 g_service.prepare_in_progress ||
                 (public_request && (g_service.launch_pending ||
                                     g_service.prepared_app_id != 0));
  if (!blocked)
    g_service.release_in_progress = true;
  pthread_mutex_unlock(&g_service.mutex);
  return blocked ? EBUSY : 0;
}

static void end_runtime_release(void) {
  pthread_mutex_lock(&g_service.mutex);
  g_service.release_in_progress = false;
  pthread_mutex_unlock(&g_service.mutex);
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
    const char *title_id, const char *runtime_source, const char *eboot_path,
    char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH], size_t *image_count) {
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
    if (!maybe_mount_image_file(image_path, name, &unstable))
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

    if (path_exists(eboot_path)) {
      return true;
    }
  }
  return false;
}

static bool prepare_image_source(const char *title_id,
                                 const char *runtime_source) {
  char eboot_path[MAX_PATH];
  int written = snprintf(eboot_path, sizeof(eboot_path), "%s/eboot.bin",
                         runtime_source);
  if (written < 0 || (size_t)written >= sizeof(eboot_path))
    return false;
  if (path_exists(eboot_path))
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
    if (!maybe_mount_image_file(image_chain[i], name, &unstable)) {
      log_debug("  [SHELLCORE] image chain mount failed: title=%s layer=%zu "
                "path=%s",
                title_id, i, image_chain[i]);
      return false;
    }
  }
  if (path_exists(eboot_path))
    return true;
  if (repair_image_chain_for_runtime_source(title_id, runtime_source,
                                            eboot_path, image_chain,
                                            &image_count))
    return true;
  log_debug("  [SHELLCORE] image chain ready but eboot missing: %s",
            eboot_path);
  return false;
}

static bool prepare_title_runtime(const char *title_id,
                                  const char *source_path) {
  if (!prepare_image_source(title_id, source_path))
    return false;
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

static int mount_managed_title_runtime(const char *title_id,
                                       bool allow_unmanaged,
                                       bool launch_request) {
  if (!is_supported_game_title_id(title_id))
    return EINVAL;
  if (runtime_sleep_mode_active())
    return EBUSY;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return allow_unmanaged ? 0 : ENOENT;

  bool claimed = false;
  int claim_status =
      claim_prepared_title(title_id, launch_request, &claimed);
  if (claim_status != 0)
    return claim_status;
  if (!claimed && is_data_mounted(title_id)) {
    finish_prepared_title(title_id, false);
    return 0;
  }

  errno = 0;
  if (prepare_title_runtime(title_id, source_path)) {
    finish_prepared_title(title_id, false);
    return 0;
  }
  int prepare_errno = errno;
  if (claimed)
    clear_prepared_title(title_id);
  else
    finish_prepared_title(title_id, true);
  return prepare_errno == EBUSY ? EBUSY : EIO;
}

bool sm_shellcore_ensure_title_runtime(const char *title_id) {
  return mount_managed_title_runtime(title_id, true, true) == 0;
}

static int handle_launch_request(const char *title_id) {
  int status = mount_managed_title_runtime(title_id, false, true);
  if (status != 0)
    return status;
  log_debug("  [SHELLCORE] launch mount ready: %s", title_id);
  return 0;
}

int sm_shellcore_mount_title_runtime(const char *title_id) {
  if (sm_game_lifecycle_has_active_game())
    return EBUSY;
  return mount_managed_title_runtime(title_id, false, false);
}

int sm_shellcore_unmount_title_runtime(const char *title_id) {
  if (!is_supported_game_title_id(title_id))
    return EINVAL;
  if (sm_game_lifecycle_has_active_game())
    return EBUSY;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return ENOENT;
  bool released = release_title_runtime(title_id, true);
  int status = released ? 0 : (errno == EBUSY ? EBUSY : EIO);
  return status;
}

static bool release_backing_image(const char *title_id) {
  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (!read_mount_image_chain(title_id, image_chain, &image_count))
    return true;

  for (size_t layer = image_count; layer > 0; --layer) {
    if (!release_runtime_image_mount(image_chain[layer - 1u]))
      return false;
  }
  return true;
}

static bool release_title_runtime(const char *title_id,
                                  bool public_request) {
  if (!title_id || title_id[0] == '\0')
    return false;
  int begin_status = begin_runtime_release(title_id, public_request);
  if (begin_status != 0) {
    errno = begin_status;
    return false;
  }
  runtime_mount_state_lock();
  log_debug("  [SHELLCORE] runtime release start: %s", title_id);
  bool released = unmount_title_runtime_layers(title_id);
  if (released)
    released = release_backing_image(title_id);
  runtime_mount_state_unlock();
  // The lifecycle owner is gone even when a short-lived ShellCore sandbox
  // nullfs still pins the title stack. Leaving the title marked as prepared
  // would block scanner cleanup forever. The intact stack remains discoverable
  // through mount.lnk and is retried by the normal full-scan cleanup cycle.
  clear_prepared_title(title_id);
  end_runtime_release();
  return released;
}

bool sm_shellcore_release_title_runtime(const char *title_id) {
  return release_title_runtime(title_id, false);
}

void sm_shellcore_service_bind_prepared_app(const char *title_id,
                                            uint32_t app_id) {
  if (!title_id || title_id[0] == '\0' || app_id == 0)
    return;

  pthread_mutex_lock(&g_service.mutex);
  if (strcmp(g_service.prepared_title_id, title_id) == 0) {
    g_service.prepared_app_id = app_id;
    g_service.prepared_game_exited = false;
    g_service.prepare_in_progress = false;
    g_service.launch_pending = false;
  }
  pthread_mutex_unlock(&g_service.mutex);
}

bool sm_shellcore_service_note_game_exit(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  bool prepared = strcmp(g_service.prepared_title_id, title_id) == 0;
  if (prepared && !g_service.prepared_game_exited) {
    g_service.prepared_game_exited = true;
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  return changed;
}

bool sm_shellcore_service_has_prepared_mount(void) {
  pthread_mutex_lock(&g_service.mutex);
  // Keep scanner cleanup blocked after process exit as well. Only the
  // post-unmountWorkspace event (or the explicit no-hook fallback) owns the
  // transition that clears this state and releases the runtime stack.
  bool prepared = g_service.prepared_title_id[0] != '\0';
  pthread_mutex_unlock(&g_service.mutex);
  return prepared;
}

bool sm_shellcore_service_title_is_prepared(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;

  pthread_mutex_lock(&g_service.mutex);
  bool prepared = strcmp(g_service.prepared_title_id, title_id) == 0;
  pthread_mutex_unlock(&g_service.mutex);
  return prepared;
}

static int handle_launch_failed_request(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return 0;

  pthread_mutex_lock(&g_service.mutex);
  bool prepared = strcmp(g_service.prepared_title_id, title_id) == 0;
  pthread_mutex_unlock(&g_service.mutex);
  if (!prepared)
    return 0;
  return sm_shellcore_release_title_runtime(title_id) ? 0 : EBUSY;
}

static int handle_workspace_unmounted(uint32_t app_id) {
  if (app_id == 0)
    return 0;

  char title_id[MAX_TITLE_ID] = {0};
  pthread_mutex_lock(&g_service.mutex);
  if (g_service.prepared_app_id == app_id &&
      g_service.prepared_title_id[0] != '\0') {
    (void)strlcpy(title_id, g_service.prepared_title_id, sizeof(title_id));
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (title_id[0] == '\0')
    return 0;

  log_debug("  [SHELLCORE] unmountWorkspace complete: %s app_id=0x%08X",
            title_id, app_id);
  return sm_shellcore_release_title_runtime(title_id) ? 0 : EBUSY;
}

static void handle_client(int fd) {
  sm_shellcore_request_t request;
  sm_shellcore_response_t response;
  if (!sm_socket_read_full(fd, &request, sizeof(request)))
    return;
  if (request.magic != SM_SHELLCORE_PROTOCOL_MAGIC ||
      request.version != SM_SHELLCORE_PROTOCOL_VERSION ||
      ((request.operation == SM_SHELLCORE_REQUEST_LAUNCH ||
        request.operation == SM_SHELLCORE_REQUEST_LAUNCH_FAILED) &&
       strnlen(request.payload.title_id, sizeof(request.payload.title_id)) ==
           sizeof(request.payload.title_id))) {
    response.status = EPROTO;
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH) {
    response.status = handle_launch_request(request.payload.title_id);
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH_FAILED) {
    response.status = handle_launch_failed_request(request.payload.title_id);
  } else if (request.operation == SM_SHELLCORE_REQUEST_WORKSPACE_UNMOUNTED) {
    response.status =
        handle_workspace_unmounted(request.payload.workspace.app_id);
  } else {
    response.status = ENOTSUP;
  }
  (void)sm_socket_write_full(fd, &response, sizeof(response));
}

static void *service_thread_main(void *arg) {
  (void)arg;
  int listen_fd = g_service.listen_fd;
  while (true) {
    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      pthread_mutex_lock(&g_service.mutex);
      bool stopping = g_service.stop_requested;
      pthread_mutex_unlock(&g_service.mutex);
      if (!stopping)
        log_debug("  [SHELLCORE] accept failed: %s", strerror(errno));
      break;
    }

    struct timeval timeout = {.tv_sec = 2, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    pthread_mutex_lock(&g_service.mutex);
    if (g_service.stop_requested) {
      pthread_mutex_unlock(&g_service.mutex);
      close(client);
      break;
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

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
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
    return false;
  }

  g_service.listen_fd = fd;
  g_service.client_fd = -1;
  g_service.stop_requested = false;
  g_service.prepared_title_id[0] = '\0';
  g_service.prepared_app_id = 0;
  g_service.prepared_game_exited = false;
  g_service.prepare_in_progress = false;
  g_service.launch_pending = false;
  g_service.release_in_progress = false;
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
  if (!g_service.started)
    return;
  pthread_mutex_lock(&g_service.mutex);
  g_service.stop_requested = true;
  int fd = g_service.listen_fd;
  int client_fd = g_service.client_fd;
  g_service.listen_fd = -1;
  pthread_mutex_unlock(&g_service.mutex);
  if (client_fd >= 0)
    (void)shutdown(client_fd, SHUT_RDWR);
  if (fd >= 0)
    (void)shutdown(fd, SHUT_RDWR);
  if (fd >= 0)
    close(fd);
  pthread_join(g_service.thread, NULL);
  pthread_mutex_lock(&g_service.mutex);
  g_service.started = false;
  g_service.stop_requested = false;
  g_service.client_fd = -1;
  g_service.prepared_title_id[0] = '\0';
  g_service.prepared_app_id = 0;
  g_service.prepared_game_exited = false;
  g_service.prepare_in_progress = false;
  g_service.launch_pending = false;
  g_service.release_in_progress = false;
  pthread_mutex_unlock(&g_service.mutex);
  (void)unlink(SM_SHELLCORE_SOCKET_PATH);
}
