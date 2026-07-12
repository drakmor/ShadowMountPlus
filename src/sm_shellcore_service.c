#include "sm_platform.h"

#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sm_filesystem.h"
#include "sm_image.h"
#include "sm_log.h"
#include "sm_path_utils.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_shellcore_protocol.h"
#include "sm_shellcore_service.h"

_Static_assert(sizeof(sm_shellcore_request_t) == 40,
               "unexpected ShellCore request size");
_Static_assert(sizeof(sm_shellcore_response_t) == 4,
               "unexpected ShellCore response size");

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  bool started;
  bool stop_requested;
  int listen_fd;
  // Owner of the prepared runtime mount, not the lifecycle active-game state.
  char prepared_title_id[MAX_TITLE_ID];
} shellcore_service_state_t;

static shellcore_service_state_t g_service = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
};

static void publish_prepared_title(const char *title_id) {
  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  if (strcmp(g_service.prepared_title_id, title_id) != 0) {
    (void)strlcpy(g_service.prepared_title_id, title_id,
                  sizeof(g_service.prepared_title_id));
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
}

static void clear_prepared_title(const char *title_id) {
  bool changed = false;
  pthread_mutex_lock(&g_service.mutex);
  if (strcmp(g_service.prepared_title_id, title_id) == 0) {
    g_service.prepared_title_id[0] = '\0';
    changed = true;
  }
  pthread_mutex_unlock(&g_service.mutex);
  if (changed)
    sm_scanner_wake();
}

static bool read_full(int fd, void *buffer, size_t size) {
  uint8_t *bytes = (uint8_t *)buffer;
  while (size != 0) {
    ssize_t count = recv(fd, bytes, size, 0);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
}

static bool write_full(int fd, const void *buffer, size_t size) {
  const uint8_t *bytes = (const uint8_t *)buffer;
  while (size != 0) {
    ssize_t count = send(fd, bytes, size, 0);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
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
  bool ready = !runtime_sleep_mode_active() &&
               mount_title_nullfs(title_id, source_path);
  if (ready)
    ready = mount_backport_overlay_for_title(source_path, title_id, NULL, NULL);
  runtime_mount_state_unlock();
  return ready;
}

bool sm_shellcore_ensure_title_runtime(const char *title_id) {
  if (!title_id || title_id[0] == '\0' || runtime_sleep_mode_active())
    return false;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return true;

  return prepare_title_runtime(title_id, source_path);
}

static int handle_launch_request(const char *title_id) {
  if (!title_id || title_id[0] == '\0' || runtime_sleep_mode_active())
    return EINVAL;

  char source_path[MAX_PATH];
  if (!read_mount_link(title_id, source_path, sizeof(source_path)))
    return ENOENT;

  publish_prepared_title(title_id);

  bool ready = prepare_title_runtime(title_id, source_path);
  if (!ready) {
    clear_prepared_title(title_id);
    return EIO;
  }
  log_debug("  [SHELLCORE] launch mount ready: %s", title_id);
  return 0;
}

static void release_backing_image(const char *title_id) {
  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (!read_mount_image_chain(title_id, image_chain, &image_count))
    return;

  for (size_t layer = image_count; layer > 0; --layer) {
    (void)release_runtime_image_mount(image_chain[layer - 1u]);
  }
}

bool sm_shellcore_release_title_runtime(const char *title_id) {
  if (!title_id || title_id[0] == '\0')
    return false;
  runtime_mount_state_lock();
  bool released = unmount_title_runtime_layers(title_id);
  if (released)
    release_backing_image(title_id);
  runtime_mount_state_unlock();
  if (released)
    clear_prepared_title(title_id);
  return released;
}

bool sm_shellcore_service_has_prepared_mount(void) {
  pthread_mutex_lock(&g_service.mutex);
  bool prepared = g_service.prepared_title_id[0] != '\0';
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

static void handle_client(int fd) {
  sm_shellcore_request_t request;
  sm_shellcore_response_t response;
  if (!read_full(fd, &request, sizeof(request)))
    return;
  if (request.magic != SM_SHELLCORE_PROTOCOL_MAGIC ||
      request.version != SM_SHELLCORE_PROTOCOL_VERSION ||
      strnlen(request.title_id, sizeof(request.title_id)) ==
          sizeof(request.title_id)) {
    response.status = EPROTO;
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH) {
    response.status = handle_launch_request(request.title_id);
  } else if (request.operation == SM_SHELLCORE_REQUEST_LAUNCH_FAILED) {
    response.status = handle_launch_failed_request(request.title_id);
  } else {
    response.status = ENOTSUP;
  }
  (void)write_full(fd, &response, sizeof(response));
}

static void *service_thread_main(void *arg) {
  (void)arg;
  while (true) {
    int client = accept(g_service.listen_fd, NULL, NULL);
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
    handle_client(client);
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
  g_service.stop_requested = false;
  g_service.prepared_title_id[0] = '\0';
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
  g_service.listen_fd = -1;
  pthread_mutex_unlock(&g_service.mutex);
  if (fd >= 0)
    (void)shutdown(fd, SHUT_RDWR);
  if (fd >= 0)
    close(fd);
  pthread_join(g_service.thread, NULL);
  g_service.started = false;
  g_service.stop_requested = false;
  g_service.prepared_title_id[0] = '\0';
  (void)unlink(SM_SHELLCORE_SOCKET_PATH);
}
