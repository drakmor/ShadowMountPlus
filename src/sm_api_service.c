#include "sm_platform.h"

#include <arpa/inet.h>
#include <json-c/json.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "sm_api_protocol.h"
#include "sm_api_service.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_game_lifecycle.h"
#include "sm_gameinfo.h"
#include "sm_image.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_install_queue.h"
#include "sm_log.h"
#include "sm_manual.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"
#include "sm_scanner.h"
#include "sm_shellcore_service.h"
#include "sm_storage.h"
#include "sm_title_state.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

#define SM_API_RETRY_INITIAL_MS 250u
#define SM_API_RETRY_MAX_MS 5000u
#define SM_API_ROUTE_SIZE 128u
#define SM_API_WORKER_COUNT 4u
#define SM_API_CONNECTION_LIMIT 16u
#define SM_API_CONNECTION_TIMEOUT_SECONDS 5u
#define SM_API_LISTEN_BACKLOG 32u
#define SM_API_CONNECTION_MEMORY_LIMIT 16384u

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool stop_requested;
  struct MHD_Daemon *daemon;
  char bind_address[MAX_API_BIND_ADDRESS];
  uint16_t port;
} sm_api_service_state_t;

typedef struct {
  char body[SM_API_MAX_JSON_BODY_SIZE + 1u];
  size_t body_size;
  size_t expected_body_size;
  bool header_size_checked;
  bool headers_validated;
  bool response_queued;
} sm_http_request_t;

typedef struct {
  int fd;
} sm_file_response_t;

typedef enum {
  GAME_STORAGE_MOVE,
  GAME_STORAGE_COPY,
  GAME_STORAGE_DELETE,
} game_storage_operation_t;

static sm_api_service_state_t g_api = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static int operation_http_status(int status);
static void handle_game_storage_operation(
    struct MHD_Connection *connection, struct json_object *request,
    game_storage_operation_t operation);

static uint32_t next_retry_delay_ms(uint32_t current) {
  if (current >= SM_API_RETRY_MAX_MS)
    return SM_API_RETRY_MAX_MS;
  if (current > SM_API_RETRY_MAX_MS / 2u)
    return SM_API_RETRY_MAX_MS;
  return current * 2u;
}

static bool wait_until_runtime_awake(void) {
  pthread_mutex_lock(&g_api.mutex);
  while (!g_api.stop_requested && runtime_sleep_mode_active()) {
    int rc = pthread_cond_wait(&g_api.cond, &g_api.mutex);
    if (rc != 0) {
      log_debug("  [API] sleep wait failed: %s", strerror(rc));
      pthread_mutex_unlock(&g_api.mutex);
      return false;
    }
  }
  bool keep_running = !g_api.stop_requested;
  pthread_mutex_unlock(&g_api.mutex);
  return keep_running;
}

static bool wait_before_listener_retry(uint32_t delay_ms) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    sceKernelUsleep(SM_API_RETRY_INITIAL_MS * 1000u);
    pthread_mutex_lock(&g_api.mutex);
    bool keep_running = !g_api.stop_requested;
    pthread_mutex_unlock(&g_api.mutex);
    return keep_running;
  }

  deadline.tv_sec += (time_t)(delay_ms / 1000u);
  deadline.tv_nsec += (long)(delay_ms % 1000u) * 1000000l;
  if (deadline.tv_nsec >= 1000000000l) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000l;
  }

  pthread_mutex_lock(&g_api.mutex);
  int wait_status = 0;
  while (!g_api.stop_requested && !runtime_sleep_mode_active()) {
    wait_status =
        pthread_cond_timedwait(&g_api.cond, &g_api.mutex, &deadline);
    if (wait_status != 0)
      break;
  }
  bool keep_running = !g_api.stop_requested;
  pthread_mutex_unlock(&g_api.mutex);

  if (keep_running && wait_status != 0 && wait_status != ETIMEDOUT) {
    sceKernelUsleep(SM_API_RETRY_INITIAL_MS * 1000u);
    pthread_mutex_lock(&g_api.mutex);
    keep_running = !g_api.stop_requested;
    pthread_mutex_unlock(&g_api.mutex);
  }
  return keep_running;
}

static bool add_common_response_headers(struct MHD_Response *response) {
  return MHD_add_response_header(response,
                                 MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN,
                                 "*") == MHD_YES &&
         MHD_add_response_header(response, MHD_HTTP_HEADER_CONNECTION,
                                 "close") == MHD_YES;
}

static bool send_json_text(struct MHD_Connection *connection, int http_status,
                           const char *body, size_t body_size) {
  struct MHD_Response *response = MHD_create_response_from_buffer(
      body_size, (void *)body, MHD_RESPMEM_MUST_COPY);
  if (!response)
    return false;
  bool ready = add_common_response_headers(response) &&
               MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                                       "application/json") == MHD_YES &&
               MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL,
                                       "no-store") == MHD_YES;
  enum MHD_Result result =
      ready ? MHD_queue_response(connection, (unsigned int)http_status,
                                 response)
            : MHD_NO;
  MHD_destroy_response(response);
  return result == MHD_YES;
}

static bool send_preflight_response(struct MHD_Connection *connection) {
  struct MHD_Response *response =
      MHD_create_response_from_buffer(0, NULL, MHD_RESPMEM_PERSISTENT);
  if (!response)
    return false;
  bool ready = add_common_response_headers(response) &&
               MHD_add_response_header(
                   response, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_METHODS,
                   "GET, POST, OPTIONS") == MHD_YES &&
               MHD_add_response_header(
                   response, MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_HEADERS,
                   "Content-Type") == MHD_YES &&
               MHD_add_response_header(
                   response, "Access-Control-Allow-Private-Network",
                   "true") == MHD_YES &&
               MHD_add_response_header(response, "Access-Control-Max-Age",
                                       "86400") == MHD_YES;
  enum MHD_Result result =
      ready ? MHD_queue_response(connection, MHD_HTTP_NO_CONTENT, response)
            : MHD_NO;
  MHD_destroy_response(response);
  return result == MHD_YES;
}

static bool send_json_object(struct MHD_Connection *connection,
                             int http_status,
                             struct json_object *response) {
  const char *body =
      json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN);
  if (!body)
    return false;
  return send_json_text(connection, http_status, body, strlen(body));
}

static ssize_t read_file_response(void *cls, uint64_t pos, char *buf,
                                  size_t max) {
  sm_file_response_t *file = cls;
  off_t offset = (off_t)pos;
  if (!file || file->fd < 0 || (uint64_t)offset != pos)
    return MHD_CONTENT_READER_END_WITH_ERROR;

  ssize_t read_size;
  do {
    read_size = pread(file->fd, buf, max, offset);
  } while (read_size < 0 && errno == EINTR);
  return read_size > 0 ? read_size : MHD_CONTENT_READER_END_WITH_ERROR;
}

static void free_file_response(void *cls) {
  sm_file_response_t *file = cls;
  if (!file)
    return;
  if (file->fd >= 0)
    (void)close(file->fd);
  free(file);
}

static bool send_file_response(struct MHD_Connection *connection, int fd,
                               uint64_t size, const char *content_type,
                               const char *cache_control) {
  sm_file_response_t *file = malloc(sizeof(*file));
  if (!file) {
    (void)close(fd);
    return false;
  }
  file->fd = fd;
  struct MHD_Response *response = MHD_create_response_from_callback(
      size, 64u * 1024u, read_file_response, file, free_file_response);
  if (!response) {
    free_file_response(file);
    return false;
  }
  bool ready = add_common_response_headers(response) &&
               MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE,
                                       content_type) == MHD_YES &&
               MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL,
                                       cache_control) == MHD_YES;
  enum MHD_Result result =
      ready ? MHD_queue_response(connection, MHD_HTTP_OK, response) : MHD_NO;
  MHD_destroy_response(response);
  return result == MHD_YES;
}

static bool add_json_value(struct json_object *object, const char *key,
                           struct json_object *value) {
  if (!value)
    return false;
  if (json_object_object_add_ex(object, key, value,
                                JSON_C_OBJECT_ADD_CONSTANT_KEY) == 0) {
    return true;
  }
  json_object_put(value);
  return false;
}

static bool add_json_int(struct json_object *object, const char *key,
                         int64_t value) {
  return add_json_value(object, key, json_object_new_int64(value));
}

static bool add_json_bool(struct json_object *object, const char *key,
                          bool value) {
  return add_json_value(object, key, json_object_new_boolean(value));
}

static bool add_json_string(struct json_object *object, const char *key,
                            const char *value) {
  return add_json_value(object, key, json_object_new_string(value));
}

static bool append_json_string(struct json_object *array, const char *value) {
  struct json_object *item = json_object_new_string(value);
  if (!item)
    return false;
  if (json_object_array_add(array, item) == 0)
    return true;
  json_object_put(item);
  return false;
}

static struct json_object *new_status_response(int status) {
  struct json_object *response = json_object_new_object();
  if (response && !add_json_int(response, "status", status)) {
    json_object_put(response);
    return NULL;
  }
  return response;
}

static bool send_out_of_memory_response(struct MHD_Connection *connection) {
  static const char body[] =
      "{\"status\":12,\"error\":\"out of memory\"}";
  return send_json_text(connection, 500, body, sizeof(body) - 1u);
}

static bool send_error_response(struct MHD_Connection *connection,
                                int http_status, int status,
                                const char *message) {
  struct json_object *response = new_status_response(status);
  if (!response)
    return send_out_of_memory_response(connection);
  if (!add_json_string(response, "error", message)) {
    json_object_put(response);
    return send_out_of_memory_response(connection);
  }
  bool sent = send_json_object(connection, http_status, response);
  json_object_put(response);
  return sent;
}

static bool is_json_content_type(const char *value) {
  static const char media_type[] = "application/json";
  size_t size = sizeof(media_type) - 1u;
  if (strncasecmp(value, media_type, size) != 0)
    return false;
  value += size;
  while (*value == ' ' || *value == '\t')
    value++;
  return *value == '\0' || *value == ';';
}

static bool parse_content_length(const char *value, size_t *size_out) {
  if (!value || !size_out || value[0] == '\0' || value[0] == '-')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > SIZE_MAX)
    return false;
  *size_out = (size_t)parsed;
  return true;
}

static struct json_object *parse_request_json(const sm_http_request_t *request) {
  struct json_tokener *tokener = json_tokener_new_ex(16);
  if (!tokener)
    return NULL;
  json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *root =
      json_tokener_parse_ex(tokener, request->body, (int)request->body_size);
  enum json_tokener_error error = json_tokener_get_error(tokener);
  size_t parsed_size = json_tokener_get_parse_end(tokener);
  while (parsed_size < request->body_size &&
         isspace((unsigned char)request->body[parsed_size])) {
    parsed_size++;
  }
  bool valid = error == json_tokener_success && root != NULL &&
               parsed_size == request->body_size &&
               json_object_is_type(root, json_type_object);
  json_tokener_free(tokener);
  if (!valid) {
    if (root)
      json_object_put(root);
    return NULL;
  }
  return root;
}

static const char *get_title_id(struct json_object *request) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "title_id", &value) ||
      !json_object_is_type(value, json_type_string)) {
    return NULL;
  }
  const char *title_id = json_object_get_string(value);
  return is_supported_game_title_id(title_id) ? title_id : NULL;
}

static bool get_manual_path(struct json_object *request,
                            char path_out[MAX_PATH]) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "path", &value) ||
      !json_object_is_type(value, json_type_string)) {
    return false;
  }
  return sm_manual_normalize_path(json_object_get_string(value), path_out);
}

static bool get_mount_mode(struct json_object *request, bool *present_out,
                           bool *read_only_out) {
  *present_out = false;
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "mode", &value))
    return true;
  if (!json_object_is_type(value, json_type_string))
    return false;

  const char *mode = json_object_get_string(value);
  if (strcmp(mode, "ro") == 0 || strcmp(mode, "r/o") == 0) {
    *read_only_out = true;
  } else if (strcmp(mode, "rw") == 0 || strcmp(mode, "r/w") == 0) {
    *read_only_out = false;
  } else {
    return false;
  }
  *present_out = true;
  return true;
}

static bool get_optional_bool(struct json_object *request, const char *key,
                              bool default_value, bool *value_out) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, key, &value)) {
    *value_out = default_value;
    return true;
  }
  if (!json_object_is_type(value, json_type_boolean))
    return false;
  *value_out = json_object_get_boolean(value) != 0;
  return true;
}

static const char *get_destination_dir(struct json_object *request) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "destination_dir", &value) ||
      !json_object_is_type(value, json_type_string)) {
    return NULL;
  }
  const char *path = json_object_get_string(value);
  size_t size = strlen(path);
  return size > 1u && size < MAX_PATH && path[0] == '/' ? path : NULL;
}

typedef struct {
  size_t size;
  bool too_large;
} http_header_size_ctx_t;

static enum MHD_Result count_http_header_size(void *ctx_ptr,
                                              enum MHD_ValueKind kind,
                                              const char *key,
                                              const char *value) {
  (void)kind;
  http_header_size_ctx_t *ctx = ctx_ptr;
  size_t key_size = key ? strlen(key) : 0;
  size_t value_size = value ? strlen(value) : 0;
  if (key_size > SM_API_MAX_HTTP_HEADER_SIZE - ctx->size ||
      value_size > SM_API_MAX_HTTP_HEADER_SIZE - ctx->size - key_size ||
      4u > SM_API_MAX_HTTP_HEADER_SIZE - ctx->size - key_size - value_size) {
    ctx->too_large = true;
    return MHD_NO;
  }
  ctx->size += key_size + value_size + 4u;
  return MHD_YES;
}

static bool http_headers_fit(struct MHD_Connection *connection,
                             const char *url, const char *method,
                             const char *version) {
  http_header_size_ctx_t ctx = {0};
  size_t request_line_size = strlen(method) + strlen(url) + strlen(version) + 4u;
  if (request_line_size > SM_API_MAX_HTTP_HEADER_SIZE - 2u)
    return false;
  ctx.size = request_line_size + 2u;
  (void)MHD_get_connection_values(connection, MHD_HEADER_KIND,
                                  count_http_header_size, &ctx);
  return !ctx.too_large && ctx.size <= SM_API_MAX_HTTP_HEADER_SIZE;
}

static void handle_version(struct MHD_Connection *fd) {
  struct json_object *response = new_status_response(0);
  struct json_object *capabilities = json_object_new_array();
  if (!response || !capabilities) {
    if (response)
      json_object_put(response);
    if (capabilities)
      json_object_put(capabilities);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_int(response, "api_version", SM_API_VERSION) ||
      !add_json_string(response, "shadowmount_version", SHADOWMOUNT_VERSION) ||
      !append_json_string(capabilities, "web_ui") ||
      !append_json_string(capabilities, "storage_space") ||
      !append_json_string(capabilities, "list_images") ||
      !append_json_string(capabilities, "list_games") ||
      !append_json_string(capabilities, "game_info") ||
      !append_json_string(capabilities, "game_icon") ||
      !append_json_string(capabilities, "mount_game") ||
      !append_json_string(capabilities, "unmount_game") ||
      !append_json_string(capabilities, "uninstall_game") ||
      !append_json_string(capabilities, "move_game_source") ||
      !append_json_string(capabilities, "copy_game_source") ||
      !append_json_string(capabilities, "delete_game_source") ||
      !append_json_string(capabilities, "list_manual_sources") ||
      !append_json_string(capabilities, "add_manual_source") ||
      !append_json_string(capabilities, "remove_manual_source") ||
      !append_json_string(capabilities, "rescan")) {
    json_object_put(capabilities);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_value(response, "capabilities", capabilities)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static int64_t filesystem_size_bytes(uint64_t blocks, uint64_t block_size) {
  if (block_size == 0)
    return 0;
  if (blocks > (uint64_t)INT64_MAX / block_size)
    return INT64_MAX;
  return (int64_t)(blocks * block_size);
}

static bool filesystem_has_capacity(const struct statfs *mount) {
  if (mount->f_bsize <= 0 || mount->f_blocks == 0 ||
      mount->f_mntonname[0] != '/') {
    return false;
  }

  static const char *const virtual_types[] = {
      "devfs", "fdescfs", "linprocfs", "nullfs", "procfs", "tmpfs",
  };
  for (size_t i = 0; i < sizeof(virtual_types) / sizeof(virtual_types[0]);
       ++i) {
    if (strcmp(mount->f_fstypename, virtual_types[i]) == 0)
      return false;
  }
  return true;
}

static struct json_object *filesystem_to_json(const struct statfs *mount) {
  uint64_t block_size = (uint64_t)mount->f_bsize;
  uint64_t total_blocks = (uint64_t)mount->f_blocks;
  uint64_t free_blocks = mount->f_bfree > 0 ? (uint64_t)mount->f_bfree : 0;
  uint64_t available_blocks =
      mount->f_bavail > 0 ? (uint64_t)mount->f_bavail : 0;
  if (free_blocks > total_blocks)
    free_blocks = total_blocks;
  if (available_blocks > free_blocks)
    available_blocks = free_blocks;

  int64_t total_bytes = filesystem_size_bytes(total_blocks, block_size);
  int64_t free_bytes = filesystem_size_bytes(free_blocks, block_size);
  int64_t available_bytes =
      filesystem_size_bytes(available_blocks, block_size);
  int64_t used_bytes = total_bytes - available_bytes;

  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;
  if (!add_json_string(item, "source", mount->f_mntfromname) ||
      !add_json_string(item, "mount_point", mount->f_mntonname) ||
      !add_json_string(item, "filesystem", mount->f_fstypename) ||
      !add_json_int(item, "total_bytes", total_bytes) ||
      !add_json_int(item, "free_bytes", free_bytes) ||
      !add_json_int(item, "available_bytes", available_bytes) ||
      !add_json_int(item, "used_bytes", used_bytes) ||
      !add_json_bool(item, "read_only", (mount->f_flags & MNT_RDONLY) != 0)) {
    json_object_put(item);
    return NULL;
  }
  return item;
}

static void handle_storage(struct MHD_Connection *connection) {
  struct statfs *mounts = NULL;
  int mount_count = getmntinfo(&mounts, MNT_NOWAIT);
  if (mount_count < 0 || !mounts) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  struct json_object *response = new_status_response(0);
  struct json_object *items = json_object_new_array_ext(mount_count);
  if (!response || !items) {
    if (response)
      json_object_put(response);
    if (items)
      json_object_put(items);
    send_out_of_memory_response(connection);
    return;
  }

  size_t count = 0;
  for (int i = 0; i < mount_count; ++i) {
    if (!filesystem_has_capacity(&mounts[i]))
      continue;
    struct json_object *item = filesystem_to_json(&mounts[i]);
    if (!item || json_object_array_add(items, item) != 0) {
      if (item)
        json_object_put(item);
      json_object_put(items);
      json_object_put(response);
      send_out_of_memory_response(connection);
      return;
    }
    count++;
  }

  if (!add_json_int(response, "count", (int64_t)count) ||
      !add_json_value(response, "mounts", items)) {
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static struct json_object *image_to_json(const void *entry) {
  const sm_image_index_snapshot_entry_t *source = entry;
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;

  char mount_point[MAX_PATH];
  get_image_mount_point_for_source(source->path, mount_point);
  bool source_available = path_exists(source->path);
  bool mapped = false;
  bool mounted = false;
  int unit_id = -1;
  attach_backend_t backend = ATTACH_BACKEND_NONE;

  image_cache_entry_t cached;
  if (find_image_cache_entry(source->path, &cached, NULL)) {
    mapped = true;
    (void)strlcpy(mount_point, cached.mount_point, sizeof(mount_point));
    unit_id = cached.unit_id;
    backend = cached.backend;
    mounted = cached.unit_id >= 0 && cached.backend != ATTACH_BACKEND_NONE &&
              is_active_image_mount_point(cached.mount_point);
  }

  if (!add_json_string(item, "path", source->path) ||
      !add_json_string(item, "mount_point", mount_point) ||
      !add_json_int(item, "size", source->size) ||
      !add_json_int(item, "mtime_sec", source->mtime_sec) ||
      !add_json_int(item, "mtime_nsec", source->mtime_nsec) ||
      !add_json_int(item, "unit_id", unit_id) ||
      !add_json_string(item, "backend", attach_backend_name(backend)) ||
      !add_json_bool(item, "complete", source->complete) ||
      !add_json_bool(item, "source_available", source_available) ||
      !add_json_bool(item, "mapped", mapped) ||
      !add_json_bool(item, "mounted", mounted)) {
    json_object_put(item);
    return NULL;
  }
  return item;
}

typedef struct {
  char physical_path[MAX_PATH];
  char runtime_path[MAX_PATH];
  const char *source_type;
  const char *image_type;
  bool image_backed;
} game_source_info_t;

static bool resolve_game_source(const sm_game_cache_snapshot_entry_t *source,
                                game_source_info_t *info) {
  memset(info, 0, sizeof(*info));
  (void)strlcpy(info->runtime_path, source->path,
                sizeof(info->runtime_path));

  char image_path[MAX_PATH];
  if (read_mount_image_link(source->title_id, image_path,
                            sizeof(image_path))) {
    (void)strlcpy(info->physical_path, image_path,
                  sizeof(info->physical_path));
    info->source_type = "image";
    info->image_type = image_fs_type_name_for_path(image_path);
    info->image_backed = true;
    return true;
  }

  (void)strlcpy(info->physical_path, source->path,
                sizeof(info->physical_path));
  info->source_type = "folder";
  info->image_type = "";
  return info->physical_path[0] != '\0';
}

static const char *game_platform_name(const char *title_id,
                                      const sm_app_db_game_info_t *metadata) {
  if (strncmp(title_id, "CUSA", 4u) == 0)
    return "ps4";
  if (strncmp(title_id, "PPSA", 4u) == 0)
    return "ps5";
  if (!metadata)
    return "unknown";
  if (metadata->platform == 0)
    return "ps4";
  if (metadata->platform == 1)
    return "ps5";
  return "unknown";
}

static struct json_object *game_to_json(
    const sm_game_cache_snapshot_entry_t *source,
    const sm_app_db_game_info_t *metadata, bool include_size) {
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;

  game_source_info_t source_info;
  if (!resolve_game_source(source, &source_info)) {
    json_object_put(item);
    return NULL;
  }

  char managed_path[MAX_PATH];
  bool installed = is_installed(source->title_id);
  bool mounted = is_data_mounted(source->title_id);
  bool managed =
      read_mount_link(source->title_id, managed_path, sizeof(managed_path));
  bool source_available = path_exists(source_info.physical_path);
  const char *title_name =
      metadata && metadata->title_name[0] != '\0' ? metadata->title_name
                                                  : source->title_name;
  const char *content_id = metadata ? metadata->content_id : "";
  const char *last_access_time = metadata ? metadata->last_access_time : "";
  const char *install_time = metadata ? metadata->install_time : "";
  char icon_url[SM_API_ROUTE_SIZE + MAX_TITLE_ID + 32u];
  icon_url[0] = '\0';
  if (metadata && metadata->icon_path[0] != '\0') {
    (void)snprintf(icon_url, sizeof(icon_url), "%s?title_id=%s",
                   SM_API_ROUTE_GAME_ICON, source->title_id);
  }

  if (!add_json_string(item, "path", source_info.physical_path) ||
      !add_json_string(item, "runtime_path", source_info.runtime_path) ||
      !add_json_string(item, "source_type", source_info.source_type) ||
      !add_json_string(item, "image_type", source_info.image_type) ||
      !add_json_string(item, "platform",
                       game_platform_name(source->title_id, metadata)) ||
      !add_json_string(item, "title_id", source->title_id) ||
      !add_json_string(item, "content_id", content_id) ||
      !add_json_string(item, "title_name", title_name) ||
      !add_json_string(item, "last_access_time", last_access_time) ||
      !add_json_string(item, "install_time", install_time) ||
      !add_json_string(item, "icon_url", icon_url) ||
      !add_json_int(item, "app_db_size_bytes",
                    metadata ? (int64_t)metadata->installed_size : 0) ||
      !add_json_bool(item, "installed", installed) ||
      !add_json_bool(item, "managed", managed) ||
      !add_json_bool(item, "mounted", mounted) ||
      !add_json_bool(item, "image_backed", source_info.image_backed) ||
      !add_json_bool(item, "source_available", source_available)) {
    json_object_put(item);
    return NULL;
  }

  if (include_size) {
    uint64_t size = 0;
    int size_status = sm_storage_measure_path(source_info.physical_path, &size);
    int size_error = size_status == 0 ? 0 : (errno != 0 ? errno : EIO);
    if (!add_json_int(item, "size_status", size_error) ||
        (size_status == 0 &&
         !add_json_int(item, "size_bytes", (int64_t)size))) {
      json_object_put(item);
      return NULL;
    }
  }
  return item;
}

typedef struct json_object *(*snapshot_to_json_fn)(const void *entry);

static void send_snapshot_response(struct MHD_Connection *fd, void *snapshot,
                                   size_t count,
                                   size_t entry_size, const char *array_key,
                                   snapshot_to_json_fn to_json) {
  struct json_object *response = new_status_response(0);
  struct json_object *items = json_object_new_array_ext((int)count);
  if (!response || !items) {
    if (response)
      json_object_put(response);
    if (items)
      json_object_put(items);
    free(snapshot);
    send_out_of_memory_response(fd);
    return;
  }

  const unsigned char *entries = snapshot;
  for (size_t i = 0; i < count; ++i) {
    struct json_object *item = to_json(entries + i * entry_size);
    if (!item || json_object_array_add(items, item) != 0) {
      if (item)
        json_object_put(item);
      json_object_put(items);
      json_object_put(response);
      free(snapshot);
      send_out_of_memory_response(fd);
      return;
    }
  }
  free(snapshot);

  if (!add_json_int(response, "count", (int64_t)count)) {
    json_object_put(items);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_value(response, array_key, items)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void handle_images(struct MHD_Connection *fd) {
  sm_image_index_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_image_index_snapshot(&snapshot, &count)) {
    send_out_of_memory_response(fd);
    return;
  }
  send_snapshot_response(fd, snapshot, count, sizeof(*snapshot), "images",
                         image_to_json);
}

static void handle_games(struct MHD_Connection *fd,
                         struct json_object *request) {
  bool include_size = false;
  if (!get_optional_bool(request, "include_size", false, &include_size)) {
    send_error_response(fd, 400, EINVAL, "include_size must be boolean");
    return;
  }
  if (include_size) {
    (void)MHD_set_connection_option(fd, MHD_CONNECTION_OPTION_TIMEOUT, 0u);
  }

  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count)) {
    send_out_of_memory_response(fd);
    return;
  }

  sm_app_db_game_info_t *metadata = NULL;
  size_t metadata_count = 0;
  if (!app_db_game_info_snapshot(&metadata, &metadata_count)) {
    int status = errno != 0 ? errno : EIO;
    free(snapshot);
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  struct json_object *response = new_status_response(0);
  struct json_object *games = json_object_new_array_ext((int)count);
  if (!response || !games) {
    if (response)
      json_object_put(response);
    if (games)
      json_object_put(games);
    free(metadata);
    free(snapshot);
    send_out_of_memory_response(fd);
    return;
  }

  bool built = true;
  for (size_t i = 0; i < count; ++i) {
    const sm_app_db_game_info_t *game_metadata = app_db_find_game_info(
        metadata, metadata_count, snapshot[i].title_id);
    struct json_object *game =
        game_to_json(&snapshot[i], game_metadata, include_size);
    if (!game || json_object_array_add(games, game) != 0) {
      if (game)
        json_object_put(game);
      built = false;
      break;
    }
  }
  free(metadata);
  free(snapshot);
  if (!built) {
    json_object_put(games);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_int(response, "count", (int64_t)count) ||
      !add_json_bool(response, "size_included", include_size)) {
    json_object_put(games);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_value(response, "games", games)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static bool find_game_snapshot_by_title(
    const char *title_id, sm_game_cache_snapshot_entry_t *entry_out) {
  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count)) {
    errno = ENOMEM;
    return false;
  }
  bool found = false;
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(snapshot[i].title_id, title_id) != 0)
      continue;
    *entry_out = snapshot[i];
    found = true;
    break;
  }
  free(snapshot);
  if (!found)
    errno = ENOENT;
  return found;
}

static void handle_game_info(struct MHD_Connection *connection,
                             struct json_object *request) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(connection, 400, EINVAL,
                        "title_id must be a valid PS4 or PS5 title ID");
    return;
  }
  (void)MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT,
                                  0u);

  sm_game_cache_snapshot_entry_t game_entry;
  if (!find_game_snapshot_by_title(title_id, &game_entry)) {
    int status = errno != 0 ? errno : ENOENT;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  sm_app_db_game_info_t *metadata = NULL;
  size_t metadata_count = 0;
  if (!app_db_game_info_snapshot(&metadata, &metadata_count)) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  const sm_app_db_game_info_t *game_metadata =
      app_db_find_game_info(metadata, metadata_count, title_id);
  struct json_object *response =
      game_to_json(&game_entry, game_metadata, true);
  free(metadata);
  if (!response) {
    send_out_of_memory_response(connection);
    return;
  }
  if (!add_json_int(response, "status", 0)) {
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static bool handle_game_icon(struct MHD_Connection *connection) {
  const char *title_id = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "title_id");
  if (!title_id || !is_supported_game_title_id(title_id)) {
    return send_error_response(
        connection, 400, EINVAL,
        "title_id query parameter must be a valid PS4 or PS5 title ID");
  }

  sm_app_db_game_info_t *metadata = NULL;
  size_t metadata_count = 0;
  if (!app_db_game_info_snapshot(&metadata, &metadata_count)) {
    int status = errno != 0 ? errno : EIO;
    return send_error_response(connection, operation_http_status(status),
                               status, strerror(status));
  }
  const sm_app_db_game_info_t *game_metadata =
      app_db_find_game_info(metadata, metadata_count, title_id);
  if (!game_metadata || game_metadata->icon_path[0] == '\0') {
    free(metadata);
    return send_error_response(connection, 404, ENOENT,
                               "game icon is not available");
  }

  char icon_path[MAX_PATH];
  (void)strlcpy(icon_path, game_metadata->icon_path, sizeof(icon_path));
  free(metadata);
  char *query = strchr(icon_path, '?');
  if (query)
    *query = '\0';

  char appmeta_root[MAX_PATH];
  char app_sce_sys_root[MAX_PATH];
  int appmeta_written = snprintf(appmeta_root, sizeof(appmeta_root), "%s/%s",
                                 APPMETA_BASE, title_id);
  int app_written = snprintf(app_sce_sys_root, sizeof(app_sce_sys_root),
                             "%s/%s/sce_sys", APP_BASE, title_id);
  if (appmeta_written < 0 ||
      (size_t)appmeta_written >= sizeof(appmeta_root) || app_written < 0 ||
      (size_t)app_written >= sizeof(app_sce_sys_root) ||
      (!path_matches_root_or_child(icon_path, appmeta_root) &&
       !path_matches_root_or_child(icon_path, app_sce_sys_root))) {
    return send_error_response(connection, 404, ENOENT,
                               "game icon path is not valid");
  }

  struct stat st;
  if (stat(icon_path, &st) != 0 || !S_ISREG(st.st_mode)) {
    return send_error_response(connection, 404, ENOENT,
                               "game icon is not available");
  }
  int fd = open(icon_path, O_RDONLY);
  if (fd < 0) {
    int status = errno != 0 ? errno : EIO;
    return send_error_response(connection, operation_http_status(status),
                               status, strerror(status));
  }
  return send_file_response(connection, fd, (uint64_t)st.st_size, "image/png",
                            "private, max-age=300");
}

static bool handle_index_page(struct MHD_Connection *connection) {
  int fd = open(WEB_INDEX_FILE, O_RDONLY);
  if (fd < 0) {
    int status = errno != 0 ? errno : EIO;
    if (status == ENOENT || status == ENOTDIR) {
      return send_error_response(connection, 404, ENOENT,
                                 "web UI is not installed");
    }
    return send_error_response(connection, operation_http_status(status),
                               status, strerror(status));
  }

  struct stat st;
  if (fstat(fd, &st) != 0) {
    int status = errno != 0 ? errno : EIO;
    (void)close(fd);
    return send_error_response(connection, operation_http_status(status),
                               status, strerror(status));
  }
  if (!S_ISREG(st.st_mode) || st.st_size < 0) {
    (void)close(fd);
    return send_error_response(connection, 404, ENOENT,
                               "web UI is not a regular file");
  }
  return send_file_response(connection, fd, (uint64_t)st.st_size,
                            "text/html; charset=utf-8", "no-cache");
}

static int operation_http_status(int status) {
  switch (status) {
  case 0:
    return 200;
  case EINVAL:
  case EISDIR:
  case ENOTDIR:
  case ENAMETOOLONG:
  case ELOOP:
    return 400;
  case ENOENT:
    return 404;
  case EACCES:
  case EPERM:
  case EROFS:
    return 403;
  case EBUSY:
  case EEXIST:
  case ECANCELED:
    return 409;
  case ENOSPC:
    return 507;
  case ENOTSUP:
    return 501;
  default:
    return 500;
  }
}

static void handle_mount_operation(struct MHD_Connection *fd,
                                   struct json_object *request,
                                   bool mount) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(fd, 400, EINVAL,
                        "title_id must be a valid PS4 or PS5 title ID");
    return;
  }

  bool mode_present = false;
  bool mount_read_only = false;
  if (mount &&
      !get_mount_mode(request, &mode_present, &mount_read_only)) {
    send_error_response(fd, 400, EINVAL,
                        "mode must be ro, rw, r/o or r/w");
    return;
  }

  const bool *mount_mode = mode_present ? &mount_read_only : NULL;
  int status = mount ? sm_shellcore_mount_title_runtime(title_id, mount_mode)
                     : sm_shellcore_unmount_title_runtime(title_id);
  log_debug("  [API] HTTP %s game: title=%s mode=%s status=%d",
            mount ? "mount" : "unmount", title_id,
            !mount ? "n/a" : mode_present
                                 ? (mount_read_only ? "ro" : "rw")
                                 : "default",
            status);
  if (status != 0) {
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_string(response, "title_id", title_id) ||
      !add_json_bool(response, "mounted", mount) ||
      (mount &&
       !add_json_string(response, "mode",
                        mode_present ? (mount_read_only ? "ro" : "rw")
                                     : "default"))) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void handle_scan(struct MHD_Connection *fd,
                        struct json_object *request) {
  if (runtime_sleep_mode_active()) {
    send_error_response(fd, 409, EBUSY, strerror(EBUSY));
    return;
  }

  bool reset_attempts = false;
  if (!get_optional_bool(request, "reset_attempts", false,
                         &reset_attempts)) {
    send_error_response(fd, 400, EINVAL, "reset_attempts must be boolean");
    return;
  }

  request_scan_now_with_options("HTTP API request", reset_attempts);

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_bool(response, "queued", true) ||
      !add_json_bool(response, "reset_attempts", reset_attempts)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

typedef struct {
  struct json_object *paths;
  size_t count;
  bool allocation_failed;
} manual_list_json_ctx_t;

static bool append_manual_list_path(const char *path, void *ctx_ptr) {
  manual_list_json_ctx_t *ctx = ctx_ptr;
  if (!append_json_string(ctx->paths, path)) {
    ctx->allocation_failed = true;
    return false;
  }
  ctx->count++;
  return true;
}

static void handle_manual_list(struct MHD_Connection *fd) {
  struct json_object *response = new_status_response(0);
  struct json_object *paths = json_object_new_array();
  if (!response || !paths) {
    if (response)
      json_object_put(response);
    if (paths)
      json_object_put(paths);
    send_out_of_memory_response(fd);
    return;
  }

  manual_list_json_ctx_t ctx = {
      .paths = paths,
  };
  errno = 0;
  bool completed = sm_manual_for_each_path(append_manual_list_path, &ctx);
  if (ctx.allocation_failed) {
    json_object_put(paths);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!completed && errno != ENOENT) {
    int status = errno != 0 ? errno : EIO;
    json_object_put(paths);
    json_object_put(response);
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  if (!add_json_int(response, "count", (int64_t)ctx.count)) {
    json_object_put(paths);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_value(response, "paths", paths)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void handle_manual_update(struct MHD_Connection *fd,
                                 struct json_object *request,
                                 bool add) {
  char path[MAX_PATH];
  if (!get_manual_path(request, path)) {
    send_error_response(fd, 400, EINVAL,
                        "path must be a non-root absolute path");
    return;
  }

  bool changed = false;
  bool updated = add ? sm_manual_add_path(path, &changed)
                     : sm_manual_remove_path(path, &changed);
  if (!updated) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  if (changed)
    request_scan_now(add ? "HTTP API manual source added"
                         : "HTTP API manual source removed");

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_string(response, "path", path) ||
      !add_json_bool(response, "present", add) ||
      !add_json_bool(response, "changed", changed)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static int request_game_uninstall(const char *title_id) {
  if (runtime_sleep_mode_active() ||
      sm_game_lifecycle_has_active_game() ||
      sm_install_has_pending_work())
    return EBUSY;
  if (!is_installed(title_id))
    return ENOENT;

  bool title_prepared = sm_shellcore_service_title_is_prepared(title_id);
  if (sm_shellcore_service_has_prepared_mount() && !title_prepared)
    return EBUSY;
  if (title_prepared || is_data_mounted(title_id)) {
    int release_status = sm_shellcore_unmount_title_runtime(title_id);
    if (release_status != 0)
      return release_status;
  }

  int platform_status = sceAppInstUtilAppUnInstall(title_id);
  if (platform_status != 0) {
    log_debug("  [API] uninstall request failed: title=%s code=0x%08X",
              title_id, (uint32_t)platform_status);
    return EIO;
  }

  invalidate_app_db_title_cache();
  reset_title_attempts(title_id, NULL, NULL);
  log_debug("  [API] uninstall requested: title=%s", title_id);
  return 0;
}

static void handle_uninstall(struct MHD_Connection *fd,
                             struct json_object *request) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(fd, 400, EINVAL,
                        "title_id must be a valid PS4 or PS5 title ID");
    return;
  }

  if (!sm_scanner_try_begin_external_mutation()) {
    send_error_response(fd, 409, EBUSY, strerror(EBUSY));
    return;
  }
  if (!sm_shellcore_try_begin_external_mutation()) {
    sm_scanner_end_external_mutation();
    send_error_response(fd, 409, EBUSY, strerror(EBUSY));
    return;
  }
  int status = request_game_uninstall(title_id);
  sm_shellcore_end_external_mutation();
  sm_scanner_end_external_mutation();
  if (status != 0) {
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_string(response, "title_id", title_id) ||
      !add_json_bool(response, "uninstall_requested", true)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void dispatch_request(struct MHD_Connection *connection,
                             const char *route,
                             struct json_object *json) {
  if (strcmp(route, SM_API_ROUTE_VERSION) == 0) {
    handle_version(connection);
  } else if (strcmp(route, SM_API_ROUTE_STORAGE) == 0) {
    handle_storage(connection);
  } else if (strcmp(route, SM_API_ROUTE_IMAGES) == 0) {
    handle_images(connection);
  } else if (strcmp(route, SM_API_ROUTE_GAMES) == 0) {
    handle_games(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_INFO) == 0) {
    handle_game_info(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_MOUNT) == 0) {
    handle_mount_operation(connection, json, true);
  } else if (strcmp(route, SM_API_ROUTE_UNMOUNT) == 0) {
    handle_mount_operation(connection, json, false);
  } else if (strcmp(route, SM_API_ROUTE_UNINSTALL) == 0) {
    handle_uninstall(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_MOVE) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_MOVE);
  } else if (strcmp(route, SM_API_ROUTE_GAME_COPY) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_COPY);
  } else if (strcmp(route, SM_API_ROUTE_GAME_DELETE) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_DELETE);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_LIST) == 0) {
    handle_manual_list(connection);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_ADD) == 0) {
    handle_manual_update(connection, json, true);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_REMOVE) == 0) {
    handle_manual_update(connection, json, false);
  } else if (strcmp(route, SM_API_ROUTE_SCAN) == 0) {
    handle_scan(connection, json);
  } else {
    send_error_response(connection, 404, ENOENT, "unknown API route");
  }
}

static const char *game_storage_operation_name(
    game_storage_operation_t operation) {
  switch (operation) {
  case GAME_STORAGE_MOVE:
    return "move";
  case GAME_STORAGE_COPY:
    return "copy";
  case GAME_STORAGE_DELETE:
    return "delete";
  }
  return "unknown";
}

static bool destination_is_managed(const char *destination) {
  if (is_under_image_mount_base(destination) ||
      path_matches_root_or_child(destination, APP_BASE) ||
      path_matches_root_or_child(destination, APPMETA_BASE) ||
      path_matches_root_or_child(destination, "/system_data")) {
    return false;
  }
  if (is_usb_storage_path(destination) &&
      !usb_storage_root_mounted(destination)) {
    return false;
  }
  for (int i = 0; i < get_scan_path_count(); ++i) {
    const char *scan_path = get_scan_path(i);
    if (!is_under_image_mount_base(scan_path) &&
        path_matches_root_or_child(destination, scan_path)) {
      return true;
    }
  }
  return false;
}

static int build_storage_destination(const char *requested_dir,
                                     const char *source,
                                     char destination[MAX_PATH]) {
  char resolved_dir[MAX_PATH];
  if (!realpath(requested_dir, resolved_dir))
    return errno != 0 ? errno : EINVAL;
  struct stat st;
  if (stat(resolved_dir, &st) != 0)
    return errno != 0 ? errno : EIO;
  if (!S_ISDIR(st.st_mode) || !destination_is_managed(resolved_dir))
    return EINVAL;

  const char *filename = get_filename_component(source);
  if (!filename || filename[0] == '\0' || strcmp(filename, ".") == 0 ||
      strcmp(filename, "..") == 0) {
    return EINVAL;
  }
  int written = snprintf(destination, MAX_PATH, "%s/%s", resolved_dir,
                         filename);
  if (written < 0 || (size_t)written >= MAX_PATH)
    return ENAMETOOLONG;
  if (strcmp(source, destination) == 0 ||
      path_matches_root_or_child(destination, source)) {
    return EINVAL;
  }
  if (lstat(destination, &st) == 0)
    return EEXIST;
  return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
}

static int release_storage_source_runtime(const char *physical_path,
                                          const char *title_id,
                                          size_t *affected_out) {
  *affected_out = 0;
  if (runtime_sleep_mode_active() || sm_game_lifecycle_has_active_game() ||
      sm_install_has_pending_work() ||
      sm_shellcore_service_has_prepared_mount()) {
    return EBUSY;
  }

  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count))
    return ENOMEM;

  int status = 0;
  runtime_mount_state_lock();
  for (size_t i = 0; i < count; ++i) {
    game_source_info_t info;
    if (!resolve_game_source(&snapshot[i], &info) ||
        strcmp(info.physical_path, physical_path) != 0) {
      continue;
    }
    (*affected_out)++;
    if (is_data_mounted(snapshot[i].title_id) &&
        !unmount_title_runtime_layers(snapshot[i].title_id)) {
      status = errno == EBUSY ? EBUSY : EIO;
      break;
    }
  }
  free(snapshot);
  if (status != 0) {
    runtime_mount_state_unlock();
    return status;
  }
  if (*affected_out == 0) {
    runtime_mount_state_unlock();
    return ENOENT;
  }

  char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
  size_t image_count = 0;
  if (read_mount_image_chain(title_id, image_chain, &image_count)) {
    for (size_t layer = image_count; layer > 0; --layer) {
      if (!release_runtime_image_mount(image_chain[layer - 1u])) {
        runtime_mount_state_unlock();
        return errno == EBUSY ? EBUSY : EIO;
      }
    }
  }
  runtime_mount_state_unlock();
  return 0;
}

static void update_moved_source_links(const char *source,
                                      const char *destination) {
  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count))
    return;

  for (size_t i = 0; i < count; ++i) {
    char image_chain[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH];
    size_t image_count = 0;
    if (read_mount_image_chain(snapshot[i].title_id, image_chain,
                               &image_count)) {
      bool changed = false;
      for (size_t layer = 0; layer < image_count; ++layer) {
        if (strcmp(image_chain[layer], source) == 0) {
          (void)strlcpy(image_chain[layer], destination,
                        sizeof(image_chain[layer]));
          changed = true;
        }
      }
      if (changed && !write_mount_image_chain(snapshot[i].title_id,
                                               image_chain, image_count)) {
        log_debug("  [API] failed to update moved image link: title=%s",
                  snapshot[i].title_id);
      }
      continue;
    }

    char tracked_path[MAX_PATH];
    if (read_mount_link(snapshot[i].title_id, tracked_path,
                        sizeof(tracked_path)) &&
        strcmp(tracked_path, source) == 0 &&
        !write_mount_link(snapshot[i].title_id, destination)) {
      log_debug("  [API] failed to update moved source link: title=%s",
                snapshot[i].title_id);
    }
  }
  free(snapshot);
}

static int perform_game_storage_operation(
    const char *title_id, const char *destination_dir,
    game_storage_operation_t operation, char source_out[MAX_PATH],
    char destination_out[MAX_PATH], const char **source_type_out,
    size_t *affected_out) {
  sm_game_cache_snapshot_entry_t game_entry;
  if (!find_game_snapshot_by_title(title_id, &game_entry))
    return errno != 0 ? errno : ENOENT;

  game_source_info_t source_info;
  if (!resolve_game_source(&game_entry, &source_info) ||
      !path_exists(source_info.physical_path)) {
    return ENOENT;
  }
  (void)strlcpy(source_out, source_info.physical_path, MAX_PATH);
  *source_type_out = source_info.source_type;
  destination_out[0] = '\0';

  if (operation != GAME_STORAGE_DELETE) {
    int destination_status = build_storage_destination(
        destination_dir, source_info.physical_path, destination_out);
    if (destination_status != 0)
      return destination_status;
  }

  int release_status = release_storage_source_runtime(
      source_info.physical_path, title_id, affected_out);
  if (release_status != 0)
    return release_status;

  int result = -1;
  switch (operation) {
  case GAME_STORAGE_MOVE:
    result = sm_storage_move_path(source_info.physical_path, destination_out);
    break;
  case GAME_STORAGE_COPY:
    result = sm_storage_copy_path(source_info.physical_path, destination_out);
    break;
  case GAME_STORAGE_DELETE:
    result = sm_storage_delete_path(source_info.physical_path);
    break;
  }
  if (result != 0) {
    request_scan_now("HTTP API storage operation recovery");
    return errno != 0 ? errno : EIO;
  }

  if (operation == GAME_STORAGE_MOVE)
    update_moved_source_links(source_info.physical_path, destination_out);
  request_scan_now(operation == GAME_STORAGE_MOVE
                       ? "HTTP API game source moved"
                       : operation == GAME_STORAGE_COPY
                             ? "HTTP API game source copied"
                             : "HTTP API game source deleted");
  return 0;
}

static void handle_game_storage_operation(
    struct MHD_Connection *connection, struct json_object *request,
    game_storage_operation_t operation) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(connection, 400, EINVAL,
                        "title_id must be a valid PS4 or PS5 title ID");
    return;
  }

  const char *destination_dir = NULL;
  if (operation != GAME_STORAGE_DELETE) {
    destination_dir = get_destination_dir(request);
    if (!destination_dir) {
      send_error_response(connection, 400, EINVAL,
                          "destination_dir must be an absolute directory");
      return;
    }
  } else {
    bool confirmed = false;
    if (!get_optional_bool(request, "confirm", false, &confirmed) ||
        !confirmed) {
      send_error_response(connection, 400, EINVAL,
                          "delete requires confirm=true");
      return;
    }
  }

  // Large cross-device copies can legitimately take much longer than the
  // normal idle-client timeout. Suspend/shutdown still cancel the copy loop.
  (void)MHD_set_connection_option(connection, MHD_CONNECTION_OPTION_TIMEOUT,
                                  0u);

  if (!sm_scanner_try_begin_external_mutation()) {
    send_error_response(connection, 409, EBUSY, strerror(EBUSY));
    return;
  }
  if (!sm_shellcore_try_begin_external_mutation()) {
    sm_scanner_end_external_mutation();
    send_error_response(connection, 409, EBUSY, strerror(EBUSY));
    return;
  }

  char source[MAX_PATH];
  char destination[MAX_PATH];
  const char *source_type = "unknown";
  size_t affected = 0;
  int status = perform_game_storage_operation(
      title_id, destination_dir, operation, source, destination, &source_type,
      &affected);
  sm_shellcore_end_external_mutation();
  sm_scanner_end_external_mutation();
  if (status != 0) {
    log_debug("  [API] game source %s failed: title=%s status=%d",
              game_storage_operation_name(operation), title_id, status);
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  log_debug("  [API] game source %s complete: title=%s source=%s destination=%s",
            game_storage_operation_name(operation), title_id, source,
            destination[0] != '\0' ? destination : "n/a");
  struct json_object *response = new_status_response(0);
  if (!response ||
      !add_json_string(response, "operation",
                       game_storage_operation_name(operation)) ||
      !add_json_string(response, "title_id", title_id) ||
      !add_json_string(response, "source_type", source_type) ||
      !add_json_string(response, "source", source) ||
      !add_json_string(response, "destination", destination) ||
      !add_json_int(response, "affected_titles", (int64_t)affected) ||
      !add_json_bool(response, "scan_queued", true)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static enum MHD_Result finish_request_error(sm_http_request_t *request,
                                            struct MHD_Connection *connection,
                                            int http_status, int status,
                                            const char *error) {
  bool queued = send_error_response(connection, http_status, status, error);
  request->response_queued = queued;
  return queued ? MHD_YES : MHD_NO;
}

static enum MHD_Result handle_http_request(
    void *cls, struct MHD_Connection *connection, const char *url,
    const char *method, const char *version, const char *upload_data,
    size_t *upload_data_size, void **con_cls) {
  (void)cls;

  sm_http_request_t *request = *con_cls;
  if (!request) {
    request = calloc(1, sizeof(*request));
    if (!request)
      return MHD_NO;
    *con_cls = request;
    return MHD_YES;
  }
  if (request->response_queued)
    return MHD_YES;

  if (!request->header_size_checked) {
    request->header_size_checked = true;
    if (!http_headers_fit(connection, url, method, version)) {
      return finish_request_error(request, connection, 431, EPROTO,
                                  "HTTP headers are too large");
    }
    if (strlen(url) >= SM_API_ROUTE_SIZE) {
      return finish_request_error(request, connection, 400, EPROTO,
                                  "HTTP route is too long");
    }
  }

  if (strcmp(method, MHD_HTTP_METHOD_OPTIONS) == 0) {
    request->response_queued = true;
    return send_preflight_response(connection) ? MHD_YES : MHD_NO;
  }
  if (strcmp(method, MHD_HTTP_METHOD_GET) == 0) {
    bool queued = false;
    if (strcmp(url, SM_API_ROUTE_INDEX) == 0)
      queued = handle_index_page(connection);
    else if (strcmp(url, SM_API_ROUTE_GAME_ICON) == 0)
      queued = handle_game_icon(connection);
    else
      queued = send_error_response(connection, 404, ENOENT,
                                   "unknown API route");
    request->response_queued = queued;
    return queued ? MHD_YES : MHD_NO;
  }
  if (strcmp(method, MHD_HTTP_METHOD_POST) != 0) {
    return finish_request_error(
        request, connection, 405, EPROTO,
        "only GET, POST and OPTIONS requests are supported");
  }

  if (!request->headers_validated) {
    const char *transfer_encoding = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_TRANSFER_ENCODING);
    if (transfer_encoding) {
      return finish_request_error(request, connection, 501, EPROTO,
                                  "Transfer-Encoding is not supported");
    }

    const char *content_length = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_LENGTH);
    if (!content_length) {
      return finish_request_error(request, connection, 411, EPROTO,
                                  "Content-Length is required");
    }
    if (!parse_content_length(content_length,
                              &request->expected_body_size)) {
      return finish_request_error(request, connection, 400, EPROTO,
                                  "invalid Content-Length header");
    }
    if (request->expected_body_size > SM_API_MAX_JSON_BODY_SIZE) {
      return finish_request_error(request, connection, 413, EPROTO,
                                  "JSON request body is too large");
    }

    const char *content_type = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
    if (!content_type || !is_json_content_type(content_type)) {
      return finish_request_error(
          request, connection, 415, EPROTO,
          "Content-Type must be application/json");
    }
    request->headers_validated = true;
  }

  if (*upload_data_size > 0) {
    if (*upload_data_size > request->expected_body_size - request->body_size) {
      *upload_data_size = 0;
      return finish_request_error(request, connection, 400, EPROTO,
                                  "request body exceeds Content-Length");
    }
    memcpy(request->body + request->body_size, upload_data,
           *upload_data_size);
    request->body_size += *upload_data_size;
    *upload_data_size = 0;
    return MHD_YES;
  }

  if (request->body_size != request->expected_body_size) {
    return finish_request_error(request, connection, 400, EPROTO,
                                "incomplete JSON request body");
  }
  request->body[request->body_size] = '\0';
  struct json_object *json = parse_request_json(request);
  if (!json) {
    return finish_request_error(
        request, connection, 400, EINVAL,
        "request body must be one valid JSON object");
  }

  dispatch_request(connection, url, json);
  json_object_put(json);
  request->response_queued = true;
  return MHD_YES;
}

static void request_completed(void *cls, struct MHD_Connection *connection,
                              void **con_cls,
                              enum MHD_RequestTerminationCode termination) {
  (void)cls;
  (void)connection;
  (void)termination;
  free(*con_cls);
  *con_cls = NULL;
}

static struct MHD_Daemon *start_http_daemon(const char *bind_address,
                                            uint16_t port) {
  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
    errno = EINVAL;
    return NULL;
  }

  errno = 0;
  struct MHD_Daemon *daemon = MHD_start_daemon(
      MHD_USE_POLL_INTERNAL_THREAD | MHD_USE_ITC, port, NULL, NULL,
      handle_http_request, NULL, MHD_OPTION_SOCK_ADDR_LEN,
      (socklen_t)sizeof(address), (const struct sockaddr *)&address,
      MHD_OPTION_THREAD_POOL_SIZE, (unsigned int)SM_API_WORKER_COUNT,
      MHD_OPTION_CONNECTION_LIMIT, (unsigned int)SM_API_CONNECTION_LIMIT,
      MHD_OPTION_CONNECTION_TIMEOUT,
      (unsigned int)SM_API_CONNECTION_TIMEOUT_SECONDS,
      MHD_OPTION_CONNECTION_MEMORY_LIMIT,
      (size_t)SM_API_CONNECTION_MEMORY_LIMIT,
      MHD_OPTION_LISTEN_BACKLOG_SIZE, (unsigned int)SM_API_LISTEN_BACKLOG,
      MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL, MHD_OPTION_END);
  if (!daemon && errno == 0)
    errno = EIO;
  return daemon;
}

static void *service_thread_main(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_api.mutex);
  char bind_address[MAX_API_BIND_ADDRESS];
  (void)strlcpy(bind_address, g_api.bind_address, sizeof(bind_address));
  uint16_t port = g_api.port;
  pthread_mutex_unlock(&g_api.mutex);

  uint32_t retry_delay_ms = SM_API_RETRY_INITIAL_MS;
  bool outage_logged = false;
  while (wait_until_runtime_awake()) {
    struct MHD_Daemon *daemon = start_http_daemon(bind_address, port);
    if (!daemon) {
      int start_error = errno != 0 ? errno : EIO;
      if (!outage_logged) {
        log_debug("  [API] HTTP listener unavailable at %s:%u: %s; "
                  "recovery scheduled",
                  bind_address, (unsigned int)port, strerror(start_error));
        outage_logged = true;
      }
      if (!wait_before_listener_retry(retry_delay_ms))
        break;
      retry_delay_ms = next_retry_delay_ms(retry_delay_ms);
      continue;
    }

    pthread_mutex_lock(&g_api.mutex);
    if (g_api.stop_requested || runtime_sleep_mode_active()) {
      bool stopping = g_api.stop_requested;
      pthread_mutex_unlock(&g_api.mutex);
      MHD_stop_daemon(daemon);
      if (stopping)
        break;
      continue;
    }
    g_api.daemon = daemon;
    pthread_mutex_unlock(&g_api.mutex);

    if (outage_logged) {
      log_debug("  [API] HTTP listener recovered: http://%s:%u/api/v1",
                bind_address, (unsigned int)port);
    } else {
      log_debug("  [API] HTTP/JSON ready: http://%s:%u/api/v1 (v%u, "
                "workers=%u, limit=%u)",
                bind_address, (unsigned int)port, SM_API_VERSION,
                SM_API_WORKER_COUNT, SM_API_CONNECTION_LIMIT);
    }
    outage_logged = false;
    retry_delay_ms = SM_API_RETRY_INITIAL_MS;

    pthread_mutex_lock(&g_api.mutex);
    while (!g_api.stop_requested && !runtime_sleep_mode_active()) {
      int rc = pthread_cond_wait(&g_api.cond, &g_api.mutex);
      if (rc != 0) {
        log_debug("  [API] service wait failed: %s", strerror(rc));
        g_api.stop_requested = true;
        break;
      }
    }
    if (g_api.daemon == daemon)
      g_api.daemon = NULL;
    bool stopping = g_api.stop_requested;
    pthread_mutex_unlock(&g_api.mutex);

    MHD_stop_daemon(daemon);
    if (stopping)
      break;
  }
  return NULL;
}

bool sm_api_service_start(void) {
  const runtime_config_t *cfg = runtime_config();
  char bind_address[MAX_API_BIND_ADDRESS];
  (void)strlcpy(bind_address, cfg->api_bind_address, sizeof(bind_address));
  uint16_t port = (uint16_t)cfg->api_port;

  pthread_mutex_lock(&g_api.mutex);
  if (g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return true;
  }
  g_api.daemon = NULL;
  g_api.stop_requested = false;
  (void)strlcpy(g_api.bind_address, bind_address,
                sizeof(g_api.bind_address));
  g_api.port = port;
  int rc = pthread_create(&g_api.thread, NULL, service_thread_main, NULL);
  if (rc != 0) {
    pthread_mutex_unlock(&g_api.mutex);
    errno = rc;
    return false;
  }
  g_api.started = true;
  pthread_mutex_unlock(&g_api.mutex);
  return true;
}

void sm_api_service_stop(void) {
  pthread_mutex_lock(&g_api.mutex);
  if (!g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return;
  }
  g_api.stop_requested = true;
  pthread_cond_broadcast(&g_api.cond);
  pthread_mutex_unlock(&g_api.mutex);

  pthread_join(g_api.thread, NULL);

  pthread_mutex_lock(&g_api.mutex);
  g_api.started = false;
  g_api.stop_requested = false;
  g_api.daemon = NULL;
  pthread_mutex_unlock(&g_api.mutex);
}

bool sm_api_service_reconfigure(void) {
  sm_api_service_stop();
  return sm_api_service_start();
}

void sm_api_service_on_sleep_change(bool active) {
  pthread_mutex_lock(&g_api.mutex);
  if (!g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return;
  }

  (void)active;
  pthread_cond_broadcast(&g_api.cond);
  pthread_mutex_unlock(&g_api.mutex);
}
