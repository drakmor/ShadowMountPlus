#include "sm_platform.h"

#include <stddef.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "sm_api_protocol.h"
#include "sm_api_service.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_image.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_log.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_shellcore_service.h"
#include "sm_socket_io.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

_Static_assert(sizeof(sm_api_request_t) == 64,
               "unexpected public API request size");
_Static_assert(SM_API_PATH_SIZE == MAX_PATH &&
                   SM_API_TITLE_ID_SIZE == MAX_TITLE_ID &&
                   SM_API_TITLE_NAME_SIZE == MAX_TITLE_NAME,
               "public API string sizes differ from internal limits");
_Static_assert(sizeof(sm_api_response_t) == 32,
               "unexpected public API response size");
_Static_assert(sizeof(sm_api_version_info_t) == 72,
               "unexpected public API version item size");
_Static_assert(sizeof(sm_api_image_info_t) == 2080,
               "unexpected public API image item size");
_Static_assert(sizeof(sm_api_game_info_t) == 2344,
               "unexpected public API game item size");

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  bool started;
  bool stop_requested;
  int listen_fd;
  int client_fd;
} sm_api_service_state_t;

static sm_api_service_state_t g_api = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .listen_fd = -1,
    .client_fd = -1,
};

static uint32_t normalize_page_limit(uint32_t requested) {
  if (requested == 0 || requested > SM_API_MAX_PAGE_ITEMS)
    return SM_API_MAX_PAGE_ITEMS;
  return requested;
}

static bool send_response(int fd, uint16_t operation, int status,
                          uint32_t total_count, uint32_t item_count,
                          uint32_t item_size, const void *items) {
  size_t payload_size = (size_t)item_count * item_size;
  if (item_count != 0 && payload_size / item_count != item_size)
    return false;
  if (payload_size > UINT32_MAX - sizeof(sm_api_response_t))
    return false;

  sm_api_response_t response = {
      .magic = SM_API_MAGIC,
      .version = SM_API_VERSION,
      .operation = operation,
      .size = (uint32_t)(sizeof(response) + payload_size),
      .status = status,
      .total_count = total_count,
      .item_count = item_count,
      .item_size = item_size,
      .flags = 0,
  };
  if (!sm_socket_write_full(fd, &response, sizeof(response)))
    return false;
  return payload_size == 0 || sm_socket_write_full(fd, items, payload_size);
}

static void handle_get_version(int fd, const sm_api_request_t *request) {
  sm_api_version_info_t info;
  memset(&info, 0, sizeof(info));
  info.api_version = SM_API_VERSION;
  info.capabilities = SM_API_CAP_LIST_IMAGES | SM_API_CAP_LIST_GAMES |
                      SM_API_CAP_MOUNT_GAME | SM_API_CAP_UNMOUNT_GAME;
  (void)strlcpy(info.shadowmount_version, SHADOWMOUNT_VERSION,
                sizeof(info.shadowmount_version));
  (void)send_response(fd, request->operation, 0, 1, 1, sizeof(info), &info);
}

static void fill_image_info(sm_api_image_info_t *info,
                            const sm_image_index_snapshot_entry_t *source) {
  memset(info, 0, sizeof(*info));
  (void)strlcpy(info->path, source->path, sizeof(info->path));
  get_image_mount_point_for_source(source->path, info->mount_point);
  info->size = source->size;
  info->mtime_sec = source->mtime_sec;
  info->mtime_nsec = source->mtime_nsec;
  info->unit_id = -1;
  if (source->complete)
    info->flags |= SM_API_IMAGE_COMPLETE;
  if (path_exists(source->path))
    info->flags |= SM_API_IMAGE_SOURCE_AVAILABLE;

  image_cache_entry_t cached;
  if (!find_image_cache_entry(source->path, &cached, NULL))
    return;
  info->flags |= SM_API_IMAGE_MAPPED;
  (void)strlcpy(info->mount_point, cached.mount_point,
                sizeof(info->mount_point));
  info->unit_id = cached.unit_id;
  info->backend = (uint32_t)cached.backend;
  if (cached.unit_id >= 0 && cached.backend != ATTACH_BACKEND_NONE &&
      is_active_image_mount_point(cached.mount_point)) {
    info->flags |= SM_API_IMAGE_MOUNTED;
  }
}

static void handle_list_images(int fd, const sm_api_request_t *request) {
  uint32_t limit = normalize_page_limit(request->limit);
  sm_image_index_snapshot_entry_t *snapshot =
      calloc(limit, sizeof(*snapshot));
  sm_api_image_info_t *items = calloc(limit, sizeof(*items));
  if (!snapshot || !items) {
    free(snapshot);
    free(items);
    (void)send_response(fd, request->operation, ENOMEM, 0, 0, 0, NULL);
    return;
  }

  size_t total = 0;
  size_t count = sm_image_index_snapshot(request->offset, snapshot, limit,
                                         &total);
  for (size_t i = 0; i < count; ++i)
    fill_image_info(&items[i], &snapshot[i]);
  (void)send_response(fd, request->operation, 0, (uint32_t)total,
                      (uint32_t)count, sizeof(*items), items);
  free(items);
  free(snapshot);
}

static void fill_game_info(sm_api_game_info_t *info,
                           const sm_game_cache_snapshot_entry_t *source) {
  memset(info, 0, sizeof(*info));
  (void)strlcpy(info->path, source->path, sizeof(info->path));
  (void)strlcpy(info->title_id, source->title_id, sizeof(info->title_id));
  (void)strlcpy(info->title_name, source->title_name,
                sizeof(info->title_name));
  if (is_installed(source->title_id))
    info->flags |= SM_API_GAME_INSTALLED;
  if (is_data_mounted(source->title_id))
    info->flags |= SM_API_GAME_MOUNTED;

  char managed_path[MAX_PATH];
  if (read_mount_link(source->title_id, managed_path, sizeof(managed_path)))
    info->flags |= SM_API_GAME_MANAGED;
  if (read_mount_image_link(source->title_id, info->image_path,
                            sizeof(info->image_path))) {
    info->flags |= SM_API_GAME_IMAGE_BACKED;
  }
  if (path_exists(source->path) ||
      (info->image_path[0] != '\0' && path_exists(info->image_path))) {
    info->flags |= SM_API_GAME_SOURCE_AVAILABLE;
  }
}

static void handle_list_games(int fd, const sm_api_request_t *request) {
  uint32_t limit = normalize_page_limit(request->limit);
  sm_game_cache_snapshot_entry_t *snapshot =
      calloc(limit, sizeof(*snapshot));
  sm_api_game_info_t *items = calloc(limit, sizeof(*items));
  if (!snapshot || !items) {
    free(snapshot);
    free(items);
    (void)send_response(fd, request->operation, ENOMEM, 0, 0, 0, NULL);
    return;
  }

  size_t total = 0;
  size_t count =
      sm_game_cache_snapshot(request->offset, snapshot, limit, &total);
  for (size_t i = 0; i < count; ++i)
    fill_game_info(&items[i], &snapshot[i]);
  (void)send_response(fd, request->operation, 0, (uint32_t)total,
                      (uint32_t)count, sizeof(*items), items);
  free(items);
  free(snapshot);
}

static bool request_has_valid_title_id(const sm_api_request_t *request) {
  return request->title_id[0] != '\0' &&
         memchr(request->title_id, '\0', sizeof(request->title_id)) != NULL;
}

static void handle_mount_operation(int fd, const sm_api_request_t *request,
                                   bool mount) {
  if (!request_has_valid_title_id(request)) {
    (void)send_response(fd, request->operation, EINVAL, 0, 0, 0, NULL);
    return;
  }
  int status = mount
                   ? sm_shellcore_mount_title_runtime(request->title_id)
                   : sm_shellcore_unmount_title_runtime(request->title_id);
  log_debug("  [API] %s game: title=%s status=%d",
            mount ? "mount" : "unmount", request->title_id, status);
  (void)send_response(fd, request->operation, status, 0, 0, 0, NULL);
}

static void handle_client(int fd) {
  sm_api_request_t request;
  if (!sm_socket_read_full(fd, &request, sizeof(request)))
    return;
  if (request.magic != SM_API_MAGIC || request.version != SM_API_VERSION ||
      request.size != sizeof(request)) {
    (void)send_response(fd, request.operation, EPROTO, 0, 0, 0, NULL);
    return;
  }

  switch (request.operation) {
  case SM_API_GET_VERSION:
    handle_get_version(fd, &request);
    break;
  case SM_API_LIST_IMAGES:
    handle_list_images(fd, &request);
    break;
  case SM_API_LIST_GAMES:
    handle_list_games(fd, &request);
    break;
  case SM_API_MOUNT_GAME:
    handle_mount_operation(fd, &request, true);
    break;
  case SM_API_UNMOUNT_GAME:
    handle_mount_operation(fd, &request, false);
    break;
  default:
    (void)send_response(fd, request.operation, ENOTSUP, 0, 0, 0, NULL);
    break;
  }
}

static void *service_thread_main(void *arg) {
  (void)arg;
  int listen_fd = g_api.listen_fd;
  while (true) {
    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR)
        continue;
      pthread_mutex_lock(&g_api.mutex);
      bool stopping = g_api.stop_requested;
      pthread_mutex_unlock(&g_api.mutex);
      if (!stopping)
        log_debug("  [API] accept failed: %s", strerror(errno));
      break;
    }

    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                     sizeof(timeout));
    (void)setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &timeout,
                     sizeof(timeout));

    pthread_mutex_lock(&g_api.mutex);
    if (g_api.stop_requested) {
      pthread_mutex_unlock(&g_api.mutex);
      close(client);
      break;
    }
    g_api.client_fd = client;
    pthread_mutex_unlock(&g_api.mutex);

    handle_client(client);

    pthread_mutex_lock(&g_api.mutex);
    if (g_api.client_fd == client)
      g_api.client_fd = -1;
    pthread_mutex_unlock(&g_api.mutex);
    close(client);
  }
  return NULL;
}

bool sm_api_service_start(void) {
  pthread_mutex_lock(&g_api.mutex);
  bool started = g_api.started;
  pthread_mutex_unlock(&g_api.mutex);
  if (started)
    return true;

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;
  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  (void)strlcpy(address.sun_path, SM_API_SOCKET_PATH,
                sizeof(address.sun_path));
  (void)unlink(SM_API_SOCKET_PATH);
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 4) != 0 || chmod(SM_API_SOCKET_PATH, 0666) != 0) {
    int saved_errno = errno;
    close(fd);
    (void)unlink(SM_API_SOCKET_PATH);
    errno = saved_errno;
    return false;
  }

  pthread_mutex_lock(&g_api.mutex);
  g_api.listen_fd = fd;
  g_api.client_fd = -1;
  g_api.stop_requested = false;
  pthread_mutex_unlock(&g_api.mutex);
  int rc = pthread_create(&g_api.thread, NULL, service_thread_main, NULL);
  if (rc != 0) {
    close(fd);
    pthread_mutex_lock(&g_api.mutex);
    g_api.listen_fd = -1;
    pthread_mutex_unlock(&g_api.mutex);
    (void)unlink(SM_API_SOCKET_PATH);
    errno = rc;
    return false;
  }
  pthread_mutex_lock(&g_api.mutex);
  g_api.started = true;
  pthread_mutex_unlock(&g_api.mutex);
  log_debug("  [API] public socket ready: %s v%u", SM_API_SOCKET_PATH,
            SM_API_VERSION);
  return true;
}

void sm_api_service_stop(void) {
  pthread_mutex_lock(&g_api.mutex);
  if (!g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return;
  }
  g_api.stop_requested = true;
  int fd = g_api.listen_fd;
  int client_fd = g_api.client_fd;
  g_api.listen_fd = -1;
  pthread_mutex_unlock(&g_api.mutex);

  if (client_fd >= 0)
    (void)shutdown(client_fd, SHUT_RDWR);
  if (fd >= 0) {
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  pthread_join(g_api.thread, NULL);

  pthread_mutex_lock(&g_api.mutex);
  g_api.started = false;
  g_api.stop_requested = false;
  g_api.client_fd = -1;
  pthread_mutex_unlock(&g_api.mutex);
  (void)unlink(SM_API_SOCKET_PATH);
}
