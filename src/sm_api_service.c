#include "sm_platform.h"

#include <arpa/inet.h>
#include <dirent.h>
#include <json-c/json.h>
#include <microhttpd.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>

#include "sm_api_protocol.h"
#include "sm_api_service.h"
#include "sm_appdb.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_game_lifecycle.h"
#include "sm_game_patches.h"
#include "sm_gameinfo.h"
#include "sm_icon_thumb.h"
#include "sm_image.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_install_queue.h"
#include "sm_log.h"
#include "sm_manual.h"
#include "sm_mdbg.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_pkg.h"
#include "sm_pkg_native.h"
#include "sm_pkg_server.h"
#include "sm_pkg_stream.h"
#include "sm_ps4_memory.h"
#include "sm_runtime.h"
#include "sm_scan.h"
#include "sm_scanner.h"
#include "sm_shellcore_service.h"
#include "sm_shellcore_hooks.h"
#include "sm_storage.h"
#include "sm_time.h"
#include "sm_title_state.h"
#include "sm_smb.h"
#include "sm_vfs.h"
#include "sm_usb.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

#define SM_API_RETRY_INITIAL_MS 250u
#define SM_API_RETRY_MAX_MS 5000u
#define SM_API_LISTENER_CHECK_MS 10000u
#define SM_API_ROUTE_SIZE 128u
#define SM_API_WORKER_COUNT 8u
#define SM_API_CONNECTION_LIMIT 32u
#define SM_API_CONNECTION_TIMEOUT_SECONDS 15u
#define SM_API_LISTEN_BACKLOG 32u
#define SM_API_CONNECTION_MEMORY_LIMIT 16384u
#define SM_API_STORAGE_STOP_LOG_SECONDS 5
#define SM_API_LOG_DEFAULT_BYTES (128u * 1024u)
#define SM_API_LOG_MAX_BYTES (256u * 1024u)
#define SM_API_PKG_START_TIMEOUT_US (30ull * 1000000ull)

extern unsigned char web_index_html[];
extern unsigned int web_index_html_len;
extern unsigned char smp_icon_png[];
extern unsigned int smp_icon_png_len;

int sceKernelConvertUtcToLocaltime(time_t utc, time_t *local, void *timezone,
                                  int32_t *dst_seconds);

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool stop_requested;
  bool restart_requested;
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
  unsigned char *binary_body;
} sm_http_request_t;

typedef struct {
  int fd;
} sm_file_response_t;

typedef enum {
  GAME_STORAGE_MOVE,
  GAME_STORAGE_COPY,
  GAME_STORAGE_DELETE,
  GAME_STORAGE_UNPACK,
  GAME_STORAGE_INSTALL,
  GAME_STORAGE_EJECT,
} game_storage_operation_t;

typedef enum {
  STORAGE_JOB_IDLE,
  STORAGE_JOB_PREPARING,
  STORAGE_JOB_MEASURING,
  STORAGE_JOB_TRANSFERRING,
  STORAGE_JOB_DELETING,
  STORAGE_JOB_FINALIZING,
  STORAGE_JOB_INSTALLING,
  STORAGE_JOB_COMPLETED,
  STORAGE_JOB_FAILED,
  STORAGE_JOB_CANCELLED,
} storage_job_state_t;

typedef struct {
  char source[MAX_PATH];
  char target[MAX_PATH];
} usb_eject_blocker_t;

typedef struct {
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool accepting;
  bool cancel_requested;
  uint64_t next_id;
  uint64_t id;
  game_storage_operation_t operation;
  storage_job_state_t state;
  char title_id[MAX_TITLE_ID];
  char source_type[16];
  char source[MAX_PATH];
  char runtime_source[MAX_PATH];
  uint64_t usb_mount_id;
  char destination[MAX_PATH];
  bool delete_source;
  uint64_t total_bytes;
  uint64_t processed_bytes;
  uint64_t total_files;
  uint64_t processed_files;
  uint64_t started_us;
  uint64_t transfer_started_us;
  uint64_t transfer_finished_us;
  uint64_t finished_us;
  size_t affected_titles;
  int result_status;
  char result_error_reason[32];
  usb_eject_blocker_t usb_blocker;
  bool scan_queued;
  char title_name[MAX_TITLE_NAME];
  int32_t native_status;
  uint64_t pkg_speed, pkg_sample_us;
  sm_pkg_stream_t *pkg_stream;
  sm_pkg_server_t *pkg_server;
  uint16_t pkg_server_port;
  char stream_token[33];
  sm_pkg_info_t pkg_queue[SM_PKG_QUEUE_MAX];
  size_t pkg_queue_count, pkg_queue_completed, pkg_queue_total;
  bool pkg_queue_running;
  pkg_kind_t pkg_current_kind;
} storage_job_t;

typedef struct {
  bool active;
  bool cancel_requested;
  uint64_t id;
  game_storage_operation_t operation;
  storage_job_state_t state;
  char title_id[MAX_TITLE_ID];
  char source_type[16];
  char source[MAX_PATH];
  char runtime_source[MAX_PATH];
  char destination[MAX_PATH];
  bool delete_source;
  uint64_t total_bytes;
  uint64_t processed_bytes;
  uint64_t total_files;
  uint64_t processed_files;
  uint64_t started_us;
  uint64_t transfer_started_us;
  uint64_t transfer_finished_us;
  uint64_t finished_us;
  size_t affected_titles;
  int result_status;
  char result_error_reason[32];
  usb_eject_blocker_t usb_blocker;
  bool scan_queued;
  char title_name[MAX_TITLE_NAME];
  int32_t native_status;
  uint64_t pkg_speed, pkg_sample_us;
  size_t pkg_queue_count, pkg_queue_completed, pkg_queue_total;
  bool pkg_queue_running;
  pkg_kind_t pkg_current_kind;
} storage_job_snapshot_t;

static sm_api_service_state_t g_api = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
};

static storage_job_t g_storage_job = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .state = STORAGE_JOB_IDLE,
};

static int operation_http_status(int status);
static bool storage_job_is_active(storage_job_state_t state) {
  return state == STORAGE_JOB_PREPARING || state == STORAGE_JOB_MEASURING ||
         state == STORAGE_JOB_TRANSFERRING ||
         state == STORAGE_JOB_DELETING || state == STORAGE_JOB_FINALIZING || state == STORAGE_JOB_INSTALLING;
}
static void handle_storage_eject(struct MHD_Connection *connection, struct json_object *request);
static void handle_file_listing(struct MHD_Connection *connection, struct json_object *request, bool packages);
static void handle_backport_operation(struct MHD_Connection *connection, struct json_object *request,
                                      game_storage_operation_t operation);
static void handle_package_install(struct MHD_Connection *connection, struct json_object *request, bool streaming, bool from_queue);
static void handle_package_queue(struct MHD_Connection *connection, struct json_object *request);
static void handle_source_install(struct MHD_Connection *connection, struct json_object *request);
static void handle_source_delete(struct MHD_Connection *connection, struct json_object *request);
static void handle_directory_create(struct MHD_Connection *connection, struct json_object *request);
static bool handle_package_icon(struct MHD_Connection *connection);
static void handle_package_range(struct MHD_Connection *connection, struct json_object *request);
static bool destination_is_managed(const char *destination);
static bool storage_job_is_active(storage_job_state_t state);
static void handle_game_storage_operation(
    struct MHD_Connection *connection, struct json_object *request,
    game_storage_operation_t operation);
static void handle_storage_job_status(struct MHD_Connection *connection,
                                      struct json_object *request);
static void handle_storage_job_cancel(struct MHD_Connection *connection,
                                      struct json_object *request);

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
  if (keep_running)
    g_api.restart_requested = false;
  pthread_mutex_unlock(&g_api.mutex);
  return keep_running;
}

static bool wait_for_listener_event(uint32_t delay_ms) {
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
  while (!g_api.stop_requested && !g_api.restart_requested &&
         !runtime_sleep_mode_active()) {
    wait_status =
        pthread_cond_timedwait(&g_api.cond, &g_api.mutex, &deadline);
    if (wait_status != 0)
      break;
  }
  if (wait_status != 0 && wait_status != ETIMEDOUT) {
    log_debug("  [API] listener wait failed: %s", strerror(wait_status));
    g_api.stop_requested = true;
  }
  bool keep_running = !g_api.stop_requested;
  pthread_mutex_unlock(&g_api.mutex);
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
                           const char *body, size_t body_size, bool keep_alive) {
  struct MHD_Response *response = MHD_create_response_from_buffer(
      body_size, (void *)body, MHD_RESPMEM_MUST_COPY);
  if (!response)
    return false;
  time_t now = time(NULL);
  time_t local = now;
  char local_time[32];
  if (sceKernelConvertUtcToLocaltime(now, &local, NULL, NULL) == 0) {
    // Shifted Unix seconds encode the console's wall clock, including DST.
    snprintf(local_time, sizeof(local_time), "%lld", (long long)local);
    (void)MHD_add_response_header(response, "X-ShadowMount-Local-Time",
                                  local_time);
  }
  bool ready = (keep_alive ? MHD_add_response_header(response,
                      MHD_HTTP_HEADER_ACCESS_CONTROL_ALLOW_ORIGIN, "*") == MHD_YES
                          : add_common_response_headers(response)) &&
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
  return send_json_text(connection, http_status, body, strlen(body), false);
}

static bool send_stream_json_object(struct MHD_Connection *connection,
                                    struct json_object *response) {
  const char *body = json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN);
  return body && send_json_text(connection, 200, body, strlen(body), true);
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

static bool send_embedded_response(struct MHD_Connection *connection,
                                    const void *data, size_t data_size,
                                    const char *content_type,
                                    const char *cache_control) {
  struct MHD_Response *response = MHD_create_response_from_buffer(
      data_size, (void *)data, MHD_RESPMEM_PERSISTENT);
  if (!response)
    return false;
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

static bool add_json_double(struct json_object *object, const char *key,
                            double value) {
  return add_json_value(object, key, json_object_new_double(value));
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
  return send_json_text(connection, 500, body, sizeof(body) - 1u, false);
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
      !json_object_is_type(value, json_type_string) ||
      json_object_get_string_len(value) != 9) {
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

static bool get_optional_job_id(struct json_object *request, bool required,
                                uint64_t *job_id_out) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "job_id", &value)) {
    *job_id_out = 0;
    return !required;
  }
  if (!json_object_is_type(value, json_type_int))
    return false;
  int64_t job_id = json_object_get_int64(value);
  if (job_id <= 0)
    return false;
  *job_id_out = (uint64_t)job_id;
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
  return size > 1u && size < MAX_PATH && (path[0] == '/' || sm_smb_path(path)) ? path : NULL;
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
      !append_json_string(capabilities, "storage_eject") ||
      !append_json_string(capabilities, "list_images") ||
      !append_json_string(capabilities, "list_games") ||
      !append_json_string(capabilities, "installed_pkg_games") ||
      !append_json_string(capabilities, "game_fakelib_settings") ||
      !append_json_string(capabilities, "game_ps4_profiles") ||
      !append_json_string(capabilities, "game_ps4_memory") ||
      !append_json_string(capabilities, "game_info") ||
      !append_json_string(capabilities, "game_data_sizes") ||
      !append_json_string(capabilities, "pkg_install") ||
      !append_json_string(capabilities, "pkg_browser_stream") ||
      !append_json_string(capabilities, "smb2_smb3") ||
      !append_json_string(capabilities, "game_icon") ||
      !append_json_string(capabilities, "mount_game") ||
      !append_json_string(capabilities, "unmount_game") ||
      !append_json_string(capabilities, "uninstall_game") ||
      !append_json_string(capabilities, "uninstall_game_content") ||
      !append_json_string(capabilities, "install_game_source") ||
      !append_json_string(capabilities, "move_game_source") ||
      !append_json_string(capabilities, "copy_game_source") ||
      !append_json_string(capabilities, "delete_game_source") ||
      !append_json_string(capabilities, "unpack_game_image") ||
      !append_json_string(capabilities, "storage_job_status") ||
      !append_json_string(capabilities, "storage_job_cancel") ||
      !append_json_string(capabilities, "list_manual_sources") ||
      !append_json_string(capabilities, "add_manual_source") ||
      !append_json_string(capabilities, "remove_manual_source") ||
      !append_json_string(capabilities, "manage_settings") ||
      !append_json_string(capabilities, "read_debug_log") ||
      !append_json_string(capabilities, "read_kernel_log") ||
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
      "devfs", "fdescfs", "linprocfs", "procfs", "tmpfs",
  };
  for (size_t i = 0; i < sizeof(virtual_types) / sizeof(virtual_types[0]);
       ++i) {
    if (strcmp(mount->f_fstypename, virtual_types[i]) == 0)
      return false;
  }
  return true;
}

static bool add_filesystem_capacity(struct json_object *item,
                                     uint64_t block_size, uint64_t total_blocks,
                                     uint64_t free_blocks, uint64_t available_blocks) {
  if (free_blocks > total_blocks)
    free_blocks = total_blocks;
  if (available_blocks > free_blocks)
    available_blocks = free_blocks;

  int64_t total_bytes = filesystem_size_bytes(total_blocks, block_size);
  int64_t free_bytes = filesystem_size_bytes(free_blocks, block_size);
  int64_t available_bytes =
      filesystem_size_bytes(available_blocks, block_size);
  int64_t used_bytes = total_bytes - available_bytes;

  return add_json_int(item, "total_bytes", total_bytes) &&
         add_json_int(item, "free_bytes", free_bytes) &&
         add_json_int(item, "available_bytes", available_bytes) &&
         add_json_int(item, "used_bytes", used_bytes);
}

static struct json_object *filesystem_to_json(const struct statfs *mount) {
  bool ejectable = sm_usb_mount_path(mount->f_mntonname);
  char mount_id[17];
  if (ejectable)
    snprintf(mount_id, sizeof(mount_id), "%016llx",
               (unsigned long long)sm_usb_filesystem_id(mount));
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;
  if (!add_json_string(item, "source", mount->f_mntfromname) ||
      !add_json_string(item, "mount_point", mount->f_mntonname) ||
      !add_json_string(item, "filesystem", mount->f_fstypename) ||
      !add_filesystem_capacity(item, (uint64_t)mount->f_bsize,
                              (uint64_t)mount->f_blocks,
                              mount->f_bfree > 0 ? (uint64_t)mount->f_bfree : 0,
                              mount->f_bavail > 0 ? (uint64_t)mount->f_bavail : 0) ||
      !add_json_bool(item, "read_only", (mount->f_flags & MNT_RDONLY) != 0) ||
      !add_json_bool(item, "ejectable", ejectable) ||
      (ejectable && !add_json_string(item, "mount_id", mount_id))) {
    json_object_put(item);
    return NULL;
  }
  return item;
}

static struct json_object *smb_storage_to_json(const char *path) {
  struct statvfs filesystem;
  if (statvfs(path, &filesystem) != 0) return NULL;
  struct json_object *item = new_status_response(0);
  if (!item || !add_json_string(item, "path", path) ||
      !add_json_string(item, "filesystem", "smb") ||
      !add_json_bool(item, "remote", true) ||
      !add_filesystem_capacity(item,
                              filesystem.f_frsize ? filesystem.f_frsize : filesystem.f_bsize,
                              filesystem.f_blocks, filesystem.f_bfree, filesystem.f_bavail) ||
      !add_json_bool(item, "read_only", (filesystem.f_flag & ST_RDONLY) != 0)) {
    if (item) json_object_put(item);
    errno = ENOMEM;
    return NULL;
  }
  return item;
}

static bool append_smb_paths(struct json_object *items, bool destinations) {
  sm_smb_connection_t *connections=calloc(SM_SMB_CONNECTIONS_MAX,sizeof(*connections));
  if(!connections) return false;
  size_t count=sm_smb_snapshot(connections);
  bool ok=true;
  for(size_t i=0;ok && i<count;++i) {
    if(!connections[i].active) continue;
    char path[MAX_PATH]; snprintf(path,sizeof(path),"smb://%s",connections[i].name);
    if(!destinations) { ok=append_json_string(items,path); continue; }
    struct json_object *item=json_object_new_object();
    if(!item || !add_json_string(item,"path",path) || !add_json_string(item,"filesystem","smb") ||
        !add_json_string(item,"source",connections[i].path) ||
        !add_json_string(item,"name",connections[i].name) || !add_json_bool(item,"remote",true) ||
        !add_json_bool(item,"read_only",connections[i].read_only) ||
        json_object_array_add(items,item)) {
      if (item) json_object_put(item);
      ok = false;
    }
  }
  memset(connections,0,SM_SMB_CONNECTIONS_MAX*sizeof(*connections)); free(connections); return ok;
}

static int append_storage_destination(struct json_object *items,
                                      const char *path) {
  char resolved[MAX_PATH];
  struct stat st;
  struct statfs filesystem;
  bool missing_homebrew = false;
  if (!realpath(path, resolved)) {
    if (errno != ENOENT || strcmp(path, "/mnt/ext1/homebrew") != 0 ||
        !realpath("/mnt/ext1", resolved))
      return 0;
    missing_homebrew = true;
  }
  if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode) ||
      !destination_is_managed(missing_homebrew ? path : resolved) ||
      statfs(resolved, &filesystem) != 0 ||
      !filesystem_has_capacity(&filesystem) ||
      (filesystem.f_flags & MNT_RDONLY) != 0 ||
      (missing_homebrew &&
       strcmp(filesystem.f_mntonname, "/mnt/ext1") != 0)) {
    return 0;
  }

  struct json_object *item = filesystem_to_json(&filesystem);
  if (!item || !add_json_string(item, "path", path) ||
      json_object_array_add(items, item) != 0) {
    if (item)
      json_object_put(item);
    return -1;
  }
  return 1;
}

static void handle_storage(struct MHD_Connection *connection, struct json_object *request) {
  struct json_object *path_value;
  if (json_object_object_get_ex(request, "path", &path_value)) {
    if (!json_object_is_type(path_value, json_type_string) ||
        !sm_smb_path(json_object_get_string(path_value)) ||
        strlen(json_object_get_string(path_value)) != (size_t)json_object_get_string_len(path_value)) {
      send_error_response(connection, 400, EINVAL, "invalid SMB storage path");
      return;
    }
    struct json_object *response = smb_storage_to_json(json_object_get_string(path_value));
    if (!response) {
      int status = errno != 0 ? errno : EIO;
      send_error_response(connection, operation_http_status(status), status, strerror(status));
      return;
    }
    (void)send_json_object(connection, 200, response);
    json_object_put(response);
    return;
  }
  struct statfs *mounts = NULL;
  int mount_count = sm_mount_table_snapshot(&mounts);
  if (mount_count < 0) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  struct json_object *response = new_status_response(0);
  struct json_object *items = json_object_new_array_ext(mount_count);
  struct json_object *destinations = json_object_new_array();
  if (!response || !items || !destinations) {
    free(mounts);
    if (response)
      json_object_put(response);
    if (items)
      json_object_put(items);
    if (destinations)
      json_object_put(destinations);
    send_out_of_memory_response(connection);
    return;
  }

  size_t count = 0;
  for (int i = 0; i < mount_count; ++i) {
    if (!filesystem_has_capacity(&mounts[i]) ||
        strcmp(mounts[i].f_fstypename, "nullfs") == 0)
      continue;
    struct json_object *item = filesystem_to_json(&mounts[i]);
    if (!item || json_object_array_add(items, item) != 0) {
      free(mounts);
      if (item)
        json_object_put(item);
      json_object_put(items);
      json_object_put(destinations);
      json_object_put(response);
      send_out_of_memory_response(connection);
      return;
    }
    count++;
  }
  free(mounts);

  static const char *const destination_paths[] = {
      "/data/homebrew",
      "/mnt/ext0",          "/mnt/ext0/homebrew",
      "/mnt/ext1",          "/mnt/ext1/homebrew",
      "/mnt/usb0",          "/mnt/usb0/homebrew",
      "/mnt/usb1",          "/mnt/usb1/homebrew",
      "/mnt/usb2",          "/mnt/usb2/homebrew",
      "/mnt/usb3",          "/mnt/usb3/homebrew",
      "/mnt/usb4",          "/mnt/usb4/homebrew",
      "/mnt/usb5",          "/mnt/usb5/homebrew",
      "/mnt/usb6",          "/mnt/usb6/homebrew",
      "/mnt/usb7",          "/mnt/usb7/homebrew",
  };
  size_t destination_count = 0;
  for (size_t i = 0;
       i < sizeof(destination_paths) / sizeof(destination_paths[0]); ++i) {
    int result = append_storage_destination(destinations,
                                            destination_paths[i]);
    if (result < 0) {
      json_object_put(destinations);
      json_object_put(items);
      json_object_put(response);
      send_out_of_memory_response(connection);
      return;
    }
    destination_count += (size_t)result;
  }

  if (!append_smb_paths(destinations,true)) {
    json_object_put(destinations); json_object_put(items); json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  destination_count=json_object_array_length(destinations);
  if (!add_json_int(response, "count", (int64_t)count) ||
      !add_json_int(response, "destination_count",
                    (int64_t)destination_count)) {
    json_object_put(destinations);
    json_object_put(items);
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  if (!add_json_value(response, "mounts", items)) {
    json_object_put(destinations);
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  if (!add_json_value(response, "destinations", destinations)) {
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
  uint64_t usb_mount_id;
} game_source_info_t;

static int resolve_browser_delete_source(const char *path, game_source_info_t *source);

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

static bool api_listener_stop_requested(void *ctx) {
  (void)ctx;
  pthread_mutex_lock(&g_api.mutex);
  bool stopping = g_api.stop_requested || g_api.restart_requested ||
                  runtime_sleep_mode_active();
  pthread_mutex_unlock(&g_api.mutex);
  return stopping;
}

static bool copy_regular_file_path(const char *path, char out[MAX_PATH]) {
  struct stat st;
  if (!path || path[0] == '\0' || stat(path, &st) != 0 ||
      !S_ISREG(st.st_mode) || st.st_size <= 0) {
    return false;
  }
  (void)strlcpy(out, path, MAX_PATH);
  return true;
}

static bool resolve_game_icon_path(const char *title_id,
                                   const char *recorded_path,
                                   char out[MAX_PATH]) {
  static const char *const storage_prefixes[] = {
      "", "/mnt/ext0", "/mnt/ext0/ps5", "/mnt/ext1", "/mnt/ext1/ps5"};
  static const struct {
    const char *base;
    const char *suffix;
  } icon_dirs[] = {
      {APP_BASE, ""}, {APPMETA_BASE, ""}, {APP_BASE, "/sce_sys"},
      {APPMETA_BASE "/external", ""}};
  char recorded_icon[MAX_PATH] = {0};
  if (recorded_path) {
    size_t length = strcspn(recorded_path, "?");
    if (length < sizeof(recorded_icon))
      memcpy(recorded_icon, recorded_path, length);
  }

  for (size_t i = 0;
       i < sizeof(storage_prefixes) / sizeof(storage_prefixes[0]); ++i) {
    for (size_t j = 0; j < sizeof(icon_dirs) / sizeof(icon_dirs[0]); ++j) {
      char root[MAX_PATH];
      int written = snprintf(root, sizeof(root), "%s%s/%s%s",
                             storage_prefixes[i], icon_dirs[j].base,
                             title_id, icon_dirs[j].suffix);
      if (written <= 0 || (size_t)written >= sizeof(root))
        continue;

      char candidate[MAX_PATH];
      written = snprintf(candidate, sizeof(candidate), "%s/icon0.png", root);
      if (written > 0 && (size_t)written < sizeof(candidate) &&
          copy_regular_file_path(candidate, out)) {
        return true;
      }
      // Recorded icons belong to appmeta or sce_sys, never the app root.
      if (j != 0 && path_matches_root_or_child(recorded_icon, root) &&
          copy_regular_file_path(recorded_icon, out)) {
        return true;
      }
    }
  }
  return false;
}

static void resolve_game_icon_url(const char *title_id, const char *recorded_path,
                                   bool thumbnail, char *out, size_t size) {
  char path[MAX_PATH];
  struct stat st;
  out[0] = 0;
  if (!resolve_game_icon_path(title_id, recorded_path, path) || stat(path, &st) != 0)
    return;
  (void)snprintf(out, size, "%s?title_id=%s%s&v=%u-%016llx",
                 SM_API_ROUTE_GAME_ICON, title_id, thumbnail ? "&size=thumb" : "",
                 SM_ICON_CACHE_VERSION,
                 (unsigned long long)sm_icon_source_fingerprint(path, &st));
}

static bool resolve_installed_pkg_game(
    const char *title_id, const sm_app_db_game_info_t *metadata,
    sm_game_cache_snapshot_entry_t *entry) {
  if (!title_id || !entry || !is_supported_game_title_id(title_id)) {
    errno = EINVAL;
    return false;
  }
  char managed_path[MAX_PATH];
  if (read_mount_link(title_id, managed_path, sizeof(managed_path)) ||
      read_mount_image_link(title_id, managed_path, sizeof(managed_path))) {
    errno = ENOENT;
    return false;
  }

  static const char *const roots[] = {
      APP_BASE, "/mnt/ext0/user/app", "/mnt/ext0/ps5/user/app",
      "/mnt/ext1/user/app", "/mnt/ext1/ps5/user/app"};
  char package_path[MAX_PATH];
  for (size_t i = 0; i <= sizeof(roots) / sizeof(roots[0]); ++i) {
    if (i == 0) {
      const char *path = metadata ? metadata->metadata_path : NULL;
      size_t length = path ? strlen(path) : 0;
      static const char suffix[] = "/sce_sys";
      if (length <= sizeof(suffix) - 1u ||
          strcmp(path + length - (sizeof(suffix) - 1u), suffix) != 0)
        continue;
      int written = snprintf(package_path, sizeof(package_path),
                              "%.*s/app.pkg",
                              (int)(length - (sizeof(suffix) - 1u)), path);
      if (written <= 0 || (size_t)written >= sizeof(package_path))
        continue;
    } else {
      int written = snprintf(package_path, sizeof(package_path),
                              "%s/%s/app.pkg", roots[i - 1u], title_id);
      if (written <= 0 || (size_t)written >= sizeof(package_path))
        continue;
    }
    struct stat st;
    if (stat(package_path, &st) != 0 || !S_ISREG(st.st_mode))
      continue;
    memset(entry, 0, sizeof(*entry));
    (void)strlcpy(entry->path, package_path, sizeof(entry->path));
    (void)strlcpy(entry->title_id, title_id, sizeof(entry->title_id));
    (void)strlcpy(entry->title_name,
                  metadata ? metadata->title_name : title_id,
                  sizeof(entry->title_name));
    return true;
  }
  errno = ENOENT;
  return false;
}

static bool find_installed_pkg_game(const char *title_id,
                                     sm_game_cache_snapshot_entry_t *entry) {
  char managed_path[MAX_PATH];
  if (read_mount_link(title_id, managed_path, sizeof(managed_path)) ||
      read_mount_image_link(title_id, managed_path, sizeof(managed_path))) {
    errno = ENOENT;
    return false;
  }
  if (resolve_installed_pkg_game(title_id, NULL, entry))
    return true;
  sm_app_db_game_info_t *metadata = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&metadata, &count))
    return false;
  const sm_app_db_game_info_t *info = app_db_find_game_info(metadata, count,
                                                          title_id);
  bool found = info && resolve_installed_pkg_game(title_id, info, entry);
  free(metadata);
  if (!found)
    errno = ENOENT;
  return found;
}

static const char *const game_patch_roots[] = {
    "/user/patch", "/mnt/ext0/user/patch", "/mnt/ext0/ps5/user/patch",
    "/mnt/ext1/user/patch", "/mnt/ext1/ps5/user/patch"};

static int resolve_game_patch_file(const char *title_id, char path[MAX_PATH]) {
  for (size_t i = 0; i < sizeof(game_patch_roots) / sizeof(game_patch_roots[0]); ++i) {
    int length = snprintf(path, MAX_PATH, "%s/%s/patch.pkg", game_patch_roots[i], title_id);
    if (length < 0 || length >= MAX_PATH) return ENAMETOOLONG;
    struct stat st;
    if (lstat(path, &st) != 0) {
      if (errno == ENOENT || errno == ENOTDIR) continue;
      return errno != 0 ? errno : EIO;
    }
    if (!S_ISREG(st.st_mode)) return S_ISLNK(st.st_mode) ? ELOOP : EINVAL;
    if (st.st_size > 0) return 0;
  }
  path[0] = 0;
  return ENOENT;
}

static int read_game_patch_status(const char *title_id, const char *content_id,
                                  sm_pkg_native_status_t *patch, bool *installed) {
  *installed = false;
  memset(patch, 0, sizeof(*patch));
  if (api_listener_stop_requested(NULL)) return ECANCELED;
  int status = content_id && content_id[0] ?
      sceAppInstUtilGetPatchInstallStatus(content_id, patch) : EINVAL;
  patch->status[sizeof(patch->status) - 1] = 0;
  patch->patch_version[sizeof(patch->patch_version) - 1] = 0;
  if (!status && patch->error.code) status = patch->error.code;
  if (!status && patch->total > INT64_MAX) status = EOVERFLOW;
  if (!status && !strcmp(patch->status, "error")) status = EIO;
  *installed = !status && (!strcmp(patch->status, "playable") || !strcmp(patch->status, "completed"));
  if (!*installed) {
    // Download task history can be empty for an installed patch, including on ext drives.
    char path[MAX_PATH];
    int file_status = resolve_game_patch_file(title_id, path);
    if (!file_status) {
      *installed = true;
      patch->total = 0;
      patch->patch_version[0] = 0;
      return 0;
    }
    if (!status && file_status != ENOENT) status = file_status;
  }
  return status;
}

static bool add_game_content_summary(struct json_object *item, const char *title_id,
                                      const char *content_id, bool installed_pkg) {
  sm_pkg_native_status_t patch;
  bool installed = false;
  int patch_status = installed_pkg ? read_game_patch_status(title_id, content_id, &patch, &installed) : 0;
  uint64_t dlc_count = 0;
  int dlc_status = api_listener_stop_requested(NULL) ? ECANCELED :
      sceAppInstUtilAppGetAddcontNum(title_id, &dlc_count);
  if (!dlc_status && dlc_count > INT64_MAX) dlc_status = EOVERFLOW;
  return add_json_int(item, "patch_info_status", patch_status) &&
      (patch_status || add_json_bool(item, "has_patch", installed)) &&
      add_json_int(item, "dlc_info_status", dlc_status) &&
      (dlc_status || add_json_int(item, "dlc_count", (int64_t)dlc_count));
}

static bool add_ps4_profile_fields(struct json_object *object,
                                   const runtime_config_t *cfg,
                                   const char *title_id) {
  static const char *const keys[] = {"ps4_profile", "ps4_profile_effective", "ps4_profile_default"};
  static const char *const fields[] = {"pro", "cpu", "gpu", "memory", "video_120hz"};
  static const uint8_t bits[] = {4, 1, 2, 8, 32};
  sm_ps4_settings_t setting = sm_config_title_ps4_override(cfg, title_id);
  const sm_ps4_setting_t values[] = {setting.pro, setting.cpu, setting.gpu, setting.memory, setting.video_120hz};
  const bool defaults[] = {cfg->ps4_force_pro || setting.memory == SM_PS4_ON,
                            cfg->ps4_cpu_boost, cfg->ps4_gpu_boost, false, false};
  uint8_t policy = sm_config_title_ps4_policy(cfg, title_id);
  for (size_t kind = 0; kind < 3; ++kind) {
    struct json_object *value = json_object_new_object();
    if (!value)
      return false;
    for (size_t field = 0; field < sizeof(fields) / sizeof(fields[0]); ++field) {
      bool ok = kind == 0
          ? add_json_string(value, fields[field], sm_config_ps4_setting_name(values[field]))
          : add_json_bool(value, fields[field], kind == 1 ? (policy & bits[field]) != 0 : defaults[field]);
      if (!ok) {
        json_object_put(value);
        return false;
      }
    }
    bool clock_ok = kind == 0
        ? add_json_string(value, "gpu_clock", setting.gpu_mhz ? "2350" : "default")
        : add_json_int(value, "gpu_clock", kind == 1 && (policy & 2u)
            ? ((policy & 16u) ? 2350 : 2200) : 0);
    if (!clock_ok) { json_object_put(value); return false; }
    if (!add_json_value(object, keys[kind], value))
      return false;
  }
  return add_json_bool(object, "ps4_memory_supported", sm_ps4_memory_supported()) &&
         add_json_string(object, "ps4_memory_status", sm_ps4_memory_status(title_id));
}

static struct json_object *game_to_json(
    const sm_game_cache_snapshot_entry_t *source,
    const sm_app_db_game_info_t *metadata, const runtime_config_t *cfg,
    bool installed_pkg, bool include_size,
    bool library_entry) {
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;

  game_source_info_t source_info;
  if (installed_pkg) {
    memset(&source_info, 0, sizeof(source_info));
    (void)strlcpy(source_info.physical_path, source->path,
                  sizeof(source_info.physical_path));
    source_info.source_type = "pkg";
    source_info.image_type = "";
  } else if (!resolve_game_source(source, &source_info)) {
    json_object_put(item);
    return NULL;
  }

  char managed_path[MAX_PATH];
  bool installed = installed_pkg || is_installed(source->title_id);
  bool mounted = !installed_pkg && is_data_mounted(source->title_id);
  bool managed = !installed_pkg &&
      read_mount_link(source->title_id, managed_path, sizeof(managed_path));
  bool source_available = path_exists(source_info.physical_path);
  const char *title_name =
      metadata && metadata->title_name[0] != '\0' ? metadata->title_name
                                                  : source->title_name;
  const char *content_id = metadata ? metadata->content_id : "";
  const char *last_access_time = metadata ? metadata->last_access_time : "";
  const char *install_time = metadata ? metadata->install_time : "";
  const char *platform = game_platform_name(source->title_id, metadata);
  bool can_toggle_fakelib = strcmp(platform, "ps5") == 0;
  sm_fakelib_mode_t fakelib_mode =
      sm_config_title_fakelib_mode(cfg, source->title_id);
  bool fakelib_enabled = fakelib_mode != SM_FAKELIB_DISABLED;
  char icon_url[SM_API_ROUTE_SIZE + MAX_TITLE_ID + 32u];
  resolve_game_icon_url(source->title_id, metadata ? metadata->icon_path : NULL,
                         library_entry, icon_url, sizeof(icon_url));
  char backport_path[MAX_PATH];
  (void)resolve_backport_path_for_title(source->title_id, NULL, backport_path);

  if (!add_json_string(item, "path", source_info.physical_path) ||
      !add_json_string(item, "runtime_path", source_info.runtime_path) ||
      !add_json_string(item, "backport_path", backport_path) ||
      !add_json_string(item, "source_type", source_info.source_type) ||
      !add_json_string(item, "image_type", source_info.image_type) ||
      !add_json_string(item, "platform", platform) ||
      !add_json_string(item, "title_id", source->title_id) ||
      !add_json_string(item, "content_id", content_id) ||
      !add_json_string(item, "title_name", title_name) ||
      !add_json_string(item, "version", metadata ? metadata->version : "") ||
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
  if (!add_json_bool(item, "installed_pkg", installed_pkg) ||
      !add_json_bool(item, "can_uninstall", installed) ||
      !add_json_bool(item, "can_manage_source", !installed_pkg) ||
      !add_json_bool(item, "can_toggle_fakelib", can_toggle_fakelib) ||
      !add_json_bool(item, "ignore_global_fakelib",
                     sm_config_title_global_fakelib_excluded(cfg, source->title_id)) ||
      !add_json_bool(item, "fakelib_enabled", fakelib_enabled) ||
      !add_json_string(item, "fakelib_mode",
                       sm_config_fakelib_mode_name(fakelib_mode)) ||
      !add_json_string(item, "fakelib_default_mode",
                       sm_config_fakelib_mode_name(cfg->fakelib_default_mode)) ||
      !add_json_string(item, "fakelib_mode_override",
                       sm_config_fakelib_mode_name(
                           sm_config_title_fakelib_override(cfg, source->title_id))) ||
      !add_json_bool(item, "fakelib_effective_enabled",
                     fakelib_enabled && cfg->backport_fakelib_enabled)) {
    json_object_put(item);
    return NULL;
  }

  bool ps4_profile = sm_config_is_ps4_title(source->title_id);
  if (metadata && metadata->play_time_known &&
      !add_json_int(item, "play_time_seconds", (int64_t)metadata->play_time_seconds)) {
    json_object_put(item);
    return NULL;
  }
  if (!add_json_bool(item, "can_configure_ps4_profile", ps4_profile) ||
      (ps4_profile && !add_ps4_profile_fields(item, cfg, source->title_id))) {
    json_object_put(item);
    return NULL;
  }

  if (library_entry && !add_game_content_summary(item, source->title_id, content_id, installed_pkg)) {
    json_object_put(item);
    return NULL;
  }
  if (include_size) {
    uint64_t size = 0;
    uint64_t file_count = 0;
    int size_status = 0;
    // The library uses the recorded total. Details measure the base package
    // separately so patch and DLC figures do not overlap this source size.
    if (library_entry && installed_pkg && metadata && metadata->installed_size > 0)
      size = metadata->installed_size;
    else
      size_status = sm_storage_measure_path_progress(
          source_info.physical_path, &size, &file_count,
          api_listener_stop_requested, NULL);
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

static bool game_snapshot_contains_title(
    const sm_game_cache_snapshot_entry_t *snapshot, size_t count,
    const char *title_id) {
  for (size_t i = 0; i < count; ++i) {
    if (strcmp(snapshot[i].title_id, title_id) == 0)
      return true;
  }
  return false;
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
  bool cancelled = false;
  const runtime_config_t cfg = runtime_config();
  char active_title[MAX_TITLE_ID];
  (void)sm_game_lifecycle_active_title(active_title);
  for (size_t i = 0; i < count; ++i) {
    if (api_listener_stop_requested(NULL)) {
      cancelled = true;
      break;
    }
    const sm_app_db_game_info_t *game_metadata = app_db_find_game_info(
        metadata, metadata_count, snapshot[i].title_id);
    sm_game_cache_snapshot_entry_t package;
    bool installed_pkg = resolve_installed_pkg_game(snapshot[i].title_id,
                                                    game_metadata, &package);
    struct json_object *game = game_to_json(
        installed_pkg ? &package : &snapshot[i], game_metadata, &cfg,
        installed_pkg, include_size, true);
    if (!game || !add_json_bool(game, "active", !strcmp(snapshot[i].title_id, active_title)) ||
        json_object_array_add(games, game) != 0) {
      if (game)
        json_object_put(game);
      built = false;
      break;
    }
  }
  for (size_t i = 0; built && !cancelled && i < metadata_count; ++i) {
    if (api_listener_stop_requested(NULL)) {
      cancelled = true;
      break;
    }
    if (game_snapshot_contains_title(snapshot, count, metadata[i].title_id) ||
        (i > 0 && strcmp(metadata[i - 1u].title_id, metadata[i].title_id) == 0))
      continue;
    sm_game_cache_snapshot_entry_t package;
    if (!resolve_installed_pkg_game(metadata[i].title_id, &metadata[i], &package))
      continue;
    struct json_object *game = game_to_json(&package, &metadata[i], &cfg, true,
                                           include_size, true);
    if (!game || !add_json_bool(game, "active", !strcmp(package.title_id, active_title)) ||
        json_object_array_add(games, game) != 0) {
      if (game)
        json_object_put(game);
      built = false;
    }
  }
  free(metadata);
  free(snapshot);
  if (cancelled) {
    json_object_put(games);
    json_object_put(response);
    send_error_response(fd, 409, ECANCELED, strerror(ECANCELED));
    return;
  }
  if (!built) {
    json_object_put(games);
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_int(response, "count", (int64_t)json_object_array_length(games)) ||
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

static int measure_optional_title_directory(const char *parent,
                                            const char *title_id,
                                           uint64_t *size_out) {
  *size_out = 0;
  if (api_listener_stop_requested(NULL))
    return ECANCELED;
  struct stat st;
  if (lstat(parent, &st) != 0)
    return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
  if (!S_ISDIR(st.st_mode))
    return S_ISLNK(st.st_mode) ? ELOOP : ENOTDIR;
  char path[MAX_PATH];
  int length = snprintf(path, sizeof(path), "%s/%s", parent, title_id);
  if (length < 0 || (size_t)length >= sizeof(path))
    return ENAMETOOLONG;
  if (lstat(path, &st) != 0)
    return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
  if (!S_ISDIR(st.st_mode))
    return S_ISLNK(st.st_mode) ? ELOOP : ENOTDIR;
  uint64_t size = 0, files = 0;
  if (sm_storage_measure_path_progress(path, &size, &files,
                                      api_listener_stop_requested, NULL) != 0)
    return errno != 0 ? errno : EIO;
  if (size > INT64_MAX)
    return EOVERFLOW;
  *size_out = size;
  return 0;
}

static int measure_title_savedata(const char *home_root, const char *title_id,
                                 uint64_t *size_out) {
  *size_out = 0;
  if (api_listener_stop_requested(NULL))
    return ECANCELED;
  struct stat st;
  if (lstat(home_root, &st) != 0)
    return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
  if (!S_ISDIR(st.st_mode))
    return S_ISLNK(st.st_mode) ? ELOOP : ENOTDIR;
  DIR *homes = opendir(home_root);
  if (!homes)
    return errno != 0 ? errno : EIO;
  const char *family = strncmp(title_id, "PPSA", 4) == 0
                           ? "savedata_prospero" : "savedata";
  uint64_t total = 0;
  int status = 0;
  for (;;) {
    if (api_listener_stop_requested(NULL)) {
      status = ECANCELED;
      break;
    }
    errno = 0;
    struct dirent *entry = readdir(homes);
    if (!entry) {
      status = errno;
      break;
    }
    if (strlen(entry->d_name) != 8 ||
        strspn(entry->d_name, "0123456789abcdefABCDEF") != 8)
      continue;
    char parent[MAX_PATH];
    int length = snprintf(parent, sizeof(parent), "%s/%s", home_root,
                            entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(parent)) {
      status = ENAMETOOLONG;
      break;
    }
    if (lstat(parent, &st) != 0) {
      if (errno == ENOENT)
        continue;
      status = errno != 0 ? errno : EIO;
      break;
    }
    if (!S_ISDIR(st.st_mode))
      continue;
    length = snprintf(parent, sizeof(parent), "%s/%s/%s", home_root,
                        entry->d_name, family);
    if (length < 0 || (size_t)length >= sizeof(parent)) {
      status = ENAMETOOLONG;
      break;
    }
    uint64_t size = 0;
    status = measure_optional_title_directory(parent, title_id, &size);
    if (status != 0)
      break;
    if (total > (uint64_t)INT64_MAX - size) {
      status = EOVERFLOW;
      break;
    }
    total += size;
  }
  if (closedir(homes) != 0 && status == 0)
    status = errno != 0 ? errno : EIO;
  if (status == 0)
    *size_out = total;
  return status;
}

static int measure_title_content(const char *const *roots, size_t root_count,
                                  const char *title_id, uint64_t *size_out) {
  *size_out = 0;
  if (root_count == 0)
    return 0;
  // Callers provide small, fixed root lists; identity tracking stays local.
  struct {
    dev_t device;
    ino_t inode;
  } seen[root_count];
  size_t seen_count = 0;
  uint64_t total = 0;
  for (size_t i = 0; i < root_count; ++i) {
    if (api_listener_stop_requested(NULL))
      return ECANCELED;
    struct stat st;
    if (lstat(roots[i], &st) != 0) {
      if (errno == ENOENT)
        continue;
      return errno != 0 ? errno : EIO;
    }
    if (!S_ISDIR(st.st_mode))
      return S_ISLNK(st.st_mode) ? ELOOP : ENOTDIR;
    char path[MAX_PATH];
    int length = snprintf(path, sizeof(path), "%s/%s", roots[i], title_id);
    if (length < 0 || (size_t)length >= sizeof(path))
      return ENAMETOOLONG;
    if (lstat(path, &st) != 0) {
      if (errno == ENOENT)
        continue;
      return errno != 0 ? errno : EIO;
    }
    if (!S_ISDIR(st.st_mode))
      return S_ISLNK(st.st_mode) ? ELOOP : ENOTDIR;
    bool duplicate = false;
    for (size_t j = 0; j < seen_count; ++j) {
      if (seen[j].device == st.st_dev && seen[j].inode == st.st_ino) {
        duplicate = true;
        break;
      }
    }
    if (duplicate)
      continue;
    seen[seen_count].device = st.st_dev;
    seen[seen_count++].inode = st.st_ino;
    uint64_t size = 0;
    int status = measure_optional_title_directory(roots[i], title_id, &size);
    if (status != 0)
      return status;
    if (total > (uint64_t)INT64_MAX - size)
      return EOVERFLOW;
    total += size;
  }
  *size_out = total;
  return 0;
}

static bool add_game_data_sizes(struct json_object *item,
                               const char *title_id) {
  static const char *const dlc_roots[] = {
      "/user/addcont", "/mnt/ext0/user/addcont", "/mnt/ext0/ps5/user/addcont",
      "/mnt/ext1/user/addcont", "/mnt/ext1/ps5/user/addcont"};
  uint64_t saves = 0, downloads = 0, patches = 0, dlc = 0;
  int save_status = measure_title_savedata("/user/home", title_id, &saves);
  int download_status = measure_optional_title_directory(
      "/user/download", title_id, &downloads);
  int patch_status = measure_title_content(game_patch_roots,
      sizeof(game_patch_roots) / sizeof(game_patch_roots[0]), title_id, &patches);
  int dlc_status = measure_title_content(dlc_roots,
      sizeof(dlc_roots) / sizeof(dlc_roots[0]), title_id, &dlc);
  return add_json_int(item, "save_data_size_status", save_status) &&
         (save_status != 0 ||
          add_json_int(item, "save_data_size_bytes", (int64_t)saves)) &&
         add_json_int(item, "download_size_status", download_status) &&
         (download_status != 0 ||
          add_json_int(item, "download_size_bytes", (int64_t)downloads)) &&
         add_json_int(item, "patch_size_status", patch_status) &&
         (patch_status != 0 ||
          add_json_int(item, "patch_size_bytes", (int64_t)patches)) &&
         add_json_int(item, "dlc_size_status", dlc_status) &&
         (dlc_status != 0 ||
          add_json_int(item, "dlc_size_bytes", (int64_t)dlc));
}

static bool valid_game_dlc_content_id(const char *content_id, const char *title_id) {
  if (!content_id || !title_id) return false;
  size_t length = strnlen(content_id, 48);
  if (length < 21 || length >= 48 || content_id[6] != '-' ||
      strncmp(content_id + 7, title_id, 9) || content_id[16] != '_' ||
      content_id[17] < '0' || content_id[17] > '9' ||
      content_id[18] < '0' || content_id[18] > '9' || content_id[19] != '-')
    return false;
  for (size_t i = 0; i < length; ++i) {
    unsigned char c = (unsigned char)content_id[i];
    if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
          (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  }
  return true;
}

static bool add_game_content_info(struct json_object *item, const char *title_id,
                                  const char *content_id, bool installed_pkg) {
  uint64_t system_size = 0;
  int system_status = api_listener_stop_requested(NULL) ? ECANCELED :
      sceAppInstUtilAppGetSize(title_id, &system_size);
  if (!system_status && system_size > INT64_MAX) system_status = EOVERFLOW;
  if (!add_json_int(item, "system_content_size_status", system_status) ||
      (!system_status && !add_json_int(item, "system_content_size_bytes", (int64_t)system_size)))
    return false;

  sm_pkg_native_status_t native_patch = {0};
  bool installed = false;
  int patch_status = installed_pkg ? read_game_patch_status(title_id, content_id, &native_patch, &installed) : 0;
  const char *patch_version = native_patch.patch_version[0] ? native_patch.patch_version :
      json_object_get_string(json_object_object_get(item, "version"));
  int64_t patch_size = native_patch.total ? (int64_t)native_patch.total :
      json_object_get_int64(json_object_object_get(item, "patch_size_bytes"));
  struct json_object *patch = json_object_new_object();
  if (!add_json_value(item, "patch", patch) ||
      !add_json_int(item, "patch_info_status", patch_status) ||
      !add_json_bool(patch, "installed", installed) ||
      !add_json_string(patch, "version", installed && patch_version ? patch_version : "") ||
      !add_json_int(patch, "size_bytes", installed ? patch_size : 0))
    return false;
  char base_version[MAX_GAME_VERSION] = {0};
  if (installed && !api_listener_stop_requested(NULL)) {
    const char *path = json_object_get_string(json_object_object_get(item, "path"));
    sm_pkg_info_t base;
    if (path && path[0] && sm_pkg_parse_file(path, NULL, &base) &&
        base.pkg_kind == PKG_KIND_BASE && !strcmp(base.title_id, title_id))
      (void)strlcpy(base_version, base.app_ver, sizeof(base_version));
  }
  if (!add_json_string(item, "base_version", base_version)) return false;

  struct json_object *dlc = json_object_new_array();
  if (!add_json_value(item, "dlc", dlc)) return false;
  enum { PAGE = 16, LIMIT = 256 };
  sm_pkg_native_addcont_t page[PAGE];
  uint64_t total = 0;
  size_t offset = 0;
  int dlc_status = 0;
  while (offset < LIMIT) {
    if (api_listener_stop_requested(NULL)) { dlc_status = ECANCELED; break; }
    memset(page, 0, sizeof(page));
    dlc_status = sceAppInstUtilGetAddcontInfoList2(title_id, 0, (int)offset, PAGE, page, &total);
    if (dlc_status) break;
    if (total > INT64_MAX) { dlc_status = EOVERFLOW; break; }
    size_t count = total > offset ? (size_t)(total - offset) : 0;
    if (count > PAGE) count = PAGE;
    for (size_t i = 0; i < count; ++i) {
      sm_pkg_native_addcont_t *entry = &page[i];
      // Validate the parent title and all string boundaries before publishing.
      if (!valid_game_dlc_content_id(entry->content_id, title_id)) {
        dlc_status = EPROTO;
        break;
      }
      entry->title_name[sizeof(entry->title_name) - 1] = 0;
      entry->version[sizeof(entry->version) - 1] = 0;
      bool duplicate = false;
      for (size_t j = 0; j < json_object_array_length(dlc); ++j) {
        struct json_object *old = json_object_array_get_idx(dlc, j);
        if (!strcmp(json_object_get_string(json_object_object_get(old, "content_id")), entry->content_id)) {
          duplicate = true;
          break;
        }
      }
      if (duplicate) continue;
      struct json_object *record = json_object_new_object();
      if (!record) return false;
      bool built = add_json_string(record, "content_id", entry->content_id) &&
          add_json_string(record, "title_name", entry->title_name) &&
          add_json_string(record, "version", entry->version) &&
          add_json_int(record, "size_status", entry->size > INT64_MAX ? EOVERFLOW : 0) &&
          (entry->size > INT64_MAX || add_json_int(record, "size_bytes", (int64_t)entry->size));
      if (!built || json_object_array_add(dlc, record) != 0) {
        json_object_put(record);
        return false;
      }
    }
    if (dlc_status) break;
    offset += count;
    if (offset >= total) break;
  }
  return add_json_int(item, "dlc_info_status", dlc_status) &&
      add_json_bool(item, "dlc_info_truncated", !dlc_status && offset < total);
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
  sm_game_cache_snapshot_entry_t game_entry;
  bool installed_pkg = game_metadata &&
                       resolve_installed_pkg_game(title_id, game_metadata,
                                                   &game_entry);
  if (!installed_pkg && !find_game_snapshot_by_title(title_id, &game_entry)) {
    int status = errno != 0 ? errno : ENOENT;
    free(metadata);
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  const runtime_config_t cfg = runtime_config();
  struct json_object *response =
      game_to_json(&game_entry, game_metadata, &cfg, installed_pkg, true, false);
  free(metadata);
  if (!response) {
    send_out_of_memory_response(connection);
    return;
  }
  char active_title[MAX_TITLE_ID];
  (void)sm_game_lifecycle_active_title(active_title);
  if (!add_json_bool(response, "active", !strcmp(title_id, active_title)) ||
      !add_game_data_sizes(response, title_id) ||
      !add_game_content_info(response, title_id, json_object_get_string(
          json_object_object_get(response, "content_id")), installed_pkg) ||
      !add_json_int(response, "status", 0)) {
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
  const char *size = MHD_lookup_connection_value(
      connection, MHD_GET_ARGUMENT_KIND, "size");
  uint32_t edge = 0;
  if (size && size[0] != '\0') {
    if (strcmp(size, "thumb") == 0)
      edge = SM_ICON_THUMB_SIZE;
    else if (strcmp(size, "card") == 0)
      edge = SM_ICON_CARD_SIZE;
    else if (strcmp(size, "cover") == 0)
      edge = SM_ICON_COVER_SIZE;
    else {
      return send_error_response(connection, 400, EINVAL,
                                 "size query parameter must be thumb, card or cover");
    }
  }

  char icon_path[MAX_PATH];
  if (!resolve_game_icon_path(title_id, NULL, icon_path)) {
    sm_app_db_game_info_t *metadata = NULL;
    size_t metadata_count = 0;
    if (!app_db_game_info_snapshot(&metadata, &metadata_count)) {
      int status = errno != 0 ? errno : EIO;
      return send_error_response(connection, operation_http_status(status),
                                 status, strerror(status));
    }
    const sm_app_db_game_info_t *game_metadata =
        app_db_find_game_info(metadata, metadata_count, title_id);
    char recorded_path[MAX_PATH];
    (void)strlcpy(recorded_path,
                  game_metadata ? game_metadata->icon_path : "",
                  sizeof(recorded_path));
    free(metadata);
    if (!resolve_game_icon_path(title_id, recorded_path, icon_path)) {
      return send_error_response(connection, 404, ENOENT,
                                 "game icon is not available");
    }
  }
  int fd = open(icon_path, O_RDONLY);
  if (fd < 0) {
    int status = errno != 0 ? errno : EIO;
    return send_error_response(connection, operation_http_status(status),
                               status, strerror(status));
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) {
    (void)close(fd);
    return send_error_response(connection, 404, ENOENT,
                               "game icon is not available");
  }
  const char *content_type = "image/png";
  if (edge != 0) {
    char thumbnail_path[MAX_PATH];
    if (sm_icon_thumbnail_path(title_id, icon_path, &st, edge, thumbnail_path)) {
      int thumbnail_fd = open(thumbnail_path, O_RDONLY);
      struct stat thumbnail_st;
      if (thumbnail_fd >= 0 && fstat(thumbnail_fd, &thumbnail_st) == 0 &&
          S_ISREG(thumbnail_st.st_mode) && thumbnail_st.st_size > 0) {
        (void)close(fd);
        fd = thumbnail_fd;
        st = thumbnail_st;
        const char *extension = strrchr(thumbnail_path, '.');
        if (extension && strcmp(extension, ".jpg") == 0)
          content_type = "image/jpeg";
      } else if (thumbnail_fd >= 0) {
        (void)close(thumbnail_fd);
      }
    }
  }
  return send_file_response(connection, fd, (uint64_t)st.st_size, content_type,
                            "public, max-age=86400, immutable");
}

static bool handle_index_page(struct MHD_Connection *connection) {
  int fd = open(WEB_INDEX_FILE, O_RDONLY);
  if (fd < 0) {
    int status = errno != 0 ? errno : EIO;
    if (status == ENOENT || status == ENOTDIR)
      return send_embedded_response(connection, web_index_html,
                                     (size_t)web_index_html_len,
                                     "text/html; charset=utf-8", "no-cache");
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
  case ESTALE:
  case EEXIST:
  case EALREADY:
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

  sm_game_cache_snapshot_entry_t package;
  if (find_installed_pkg_game(title_id, &package)) {
    send_error_response(fd, 403, EPERM,
                        "installed PKGs only support information and uninstall");
    return;
  }
  if (errno != ENOENT) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(fd, operation_http_status(status), status,
                        strerror(status));
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
  if (!sm_scanner_try_begin_external_mutation()) {
    send_error_response(fd, 409, EBUSY, "scanner or storage operation is busy");
    return;
  }
  int status = mount ? sm_shellcore_mount_title_runtime(title_id, mount_mode)
                     : sm_shellcore_unmount_title_runtime(title_id);
  sm_scanner_end_external_mutation();
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

static const char *operation_error_message(int status, const char *reason) {
  static const struct {
    const char *reason;
    const char *message;
  } messages[] = {
      {"rest_mode", "console is entering or in rest mode"},
      {"game_active", "a game is active or awaiting exit; close the game and try again"},
      {"install_pending", "game installation is pending; wait for it to finish"},
      {"runtime_prepared", "a title has a prepared runtime mount; close or unmount it first"},
      {"scanner_busy", "scanner or another storage operation is busy; try again when it finishes"},
      {"runtime_busy", "title runtime is being prepared or released; try again when it finishes"},
      {"title_unmount_busy", "title runtime could not be released; its mount or sandbox is still busy"},
      {"image_unmount_busy", "backing image could not be released; its mount is still busy"},
      {"unpack_mounted", "image runtime is already mounted or prepared; unmount it before unpacking"},
      {"storage_busy", "a storage operation is already running; wait for it to finish"},
      {"storage_unavailable", "storage service is stopping or unavailable"},
      {"usb_disk_busy", "the drive has other mounted partitions, game links or attached images; unmount them first"},
      {"usb_mount_busy", "a dependent mount uses this drive; unmount its game or application and retry"},
      {"usb_partition_busy", "another partition of this drive is mounted; unmount that partition and retry"},
      {"usb_image_busy", "an attached image uses this drive; unmount its game and retry"},
      {"usb_mount_changed", "the drive changed or was disconnected; refresh storage before trying again"},
      {"usb_eject_unconfirmed", "unmount completion was not confirmed; keep the drive connected and refresh storage"},
      {"usb_eject_failed", "the system could not safely unmount this drive"},
      {"pkg_invalid", "invalid or unsupported PKG"},
      {"pkg_base_missing", "install the base game before this patch or DLC"},
      {"pkg_stream_lost", "the browser streaming source stopped responding"},
      {"pkg_source_changed", "the PKG changed during installation"},
      {"pkg_already_installed", "the base game is already installed"},
      {"pkg_queued", "the package is already queued"},
      {"pkg_queue_full", "the installation queue is full (32 packages)"},
  };
  if (reason && reason[0] != '\0') {
    for (size_t i = 0; i < sizeof(messages) / sizeof(messages[0]); ++i) {
      if (strcmp(reason, messages[i].reason) == 0)
        return messages[i].message;
    }
  }
  return strerror(status);
}

static void send_operation_error_response(struct MHD_Connection *connection,
                                           int status, const char *reason) {
  struct json_object *response = new_status_response(status);
  if (!response ||
      !add_json_string(response, "error", operation_error_message(status, reason)) ||
      !add_json_string(response, "error_reason", reason ? reason : "")) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, operation_http_status(status), response);
  json_object_put(response);
}

static bool add_scan_queue_fields(struct json_object *response, bool queued) {
  const char *reason = "";
  if (queued) {
    if (runtime_sleep_mode_active())
      reason = "rest_mode";
    else if (sm_shellcore_background_work_blocked())
      reason = sm_game_lifecycle_has_active_game() ? "game_active"
                                                  : "runtime_prepared";
  }
  return add_json_bool(response, "scan_queued", queued) &&
         add_json_bool(response, "scan_deferred", reason[0] != '\0') &&
         add_json_string(response, "scan_deferred_reason", reason);
}

static void handle_scan(struct MHD_Connection *fd,
                        struct json_object *request) {
  if (runtime_sleep_mode_active()) {
    send_operation_error_response(fd, EBUSY, "rest_mode");
    return;
  }

  bool reset_attempts = false;
  if (!get_optional_bool(request, "reset_attempts", false,
                         &reset_attempts)) {
    send_error_response(fd, 400, EINVAL, "reset_attempts must be boolean");
    return;
  }

  if (!request_scan_now_with_options("HTTP API request", reset_attempts)) {
    send_operation_error_response(fd, EBUSY, "rest_mode");
    return;
  }

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_bool(response, "queued", true) ||
      !add_scan_queue_fields(response, true) ||
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

static bool get_required_bool(struct json_object *request, const char *key,
                              bool *value_out) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, key, &value) ||
      !json_object_is_type(value, json_type_boolean)) {
    return false;
  }
  *value_out = json_object_get_boolean(value) != 0;
  return true;
}

static bool get_log_max_bytes(struct MHD_Connection *connection,
                              struct json_object *request,
                              size_t *max_bytes_out) {
  *max_bytes_out = SM_API_LOG_DEFAULT_BYTES;
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(request, "max_bytes", &value))
    return true;
  if (!json_object_is_type(value, json_type_int)) {
    send_error_response(connection, 400, EINVAL,
                        "max_bytes must be an integer");
    return false;
  }
  int64_t requested = json_object_get_int64(value);
  if (requested < 4096 || requested > SM_API_LOG_MAX_BYTES) {
    send_error_response(connection, 400, EINVAL,
                        "max_bytes must be in range 4096..262144");
    return false;
  }
  *max_bytes_out = (size_t)requested;
  return true;
}

static void send_log_tail_response(struct MHD_Connection *connection,
                                   char *content, size_t returned_bytes,
                                   size_t total_bytes,
                                   const char *total_bytes_key) {
  struct json_object *response = new_status_response(0);
  struct json_object *content_value =
      json_object_new_string_len(content, (int)returned_bytes);
  free(content);
  if (!response || !content_value) {
    if (content_value)
      json_object_put(content_value);
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  if (!add_json_value(response, "content", content_value) ||
      !add_json_int(response, total_bytes_key, (int64_t)total_bytes) ||
      !add_json_int(response, "returned_bytes", (int64_t)returned_bytes) ||
      !add_json_bool(response, "truncated", total_bytes > returned_bytes)) {
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static bool smb_request_string(struct json_object *request, const char *key,
                                 char *out, size_t size, bool required) {
  struct json_object *value;
  if(!json_object_object_get_ex(request,key,&value)) return !required;
  if(!json_object_is_type(value,json_type_string)) return false;
  const char *text=json_object_get_string(value);
  if(strlen(text) != (size_t)json_object_get_string_len(value) || strlen(text) >= size) return false;
  strlcpy(out,text,size); return true;
}

static void handle_smb(struct MHD_Connection *connection, struct json_object *request) {
  char action[16]="list", previous[64]="", name[64]="";
  if(!smb_request_string(request,"action",action,sizeof(action),false) ||
      !smb_request_string(request,"previous_name",previous,sizeof(previous),false) ||
      !smb_request_string(request,"name",name,sizeof(name),false)) {
    send_error_response(connection,400,EINVAL,"invalid SMB fields"); return;
  }
  int status=0;
  uint16_t dialect=0;
  if(strcmp(action,"list")) {
    if(strcmp(action,"save") && strcmp(action,"remove") && strcmp(action,"check")) {
      send_error_response(connection,400,EINVAL,"invalid SMB action"); return;
    }
    sm_smb_connection_t c={0};
    if(!strcmp(action,"remove")) {
      if(!name[0]) status=EINVAL;
    } else {
      struct json_object *value;
      if(previous[0]) status=sm_smb_get(previous,&c);
      if(!status && (!smb_request_string(request,"name",c.name,sizeof(c.name),true) ||
          !smb_request_string(request,"path",c.path,sizeof(c.path),true) ||
          !smb_request_string(request,"username",c.username,sizeof(c.username),false) ||
          !smb_request_string(request,"password",c.password,sizeof(c.password),false))) status=EINVAL;
      if(!previous[0]) c.active=true;
      if(!status && json_object_object_get_ex(request,"active",&value)) {
        if(!json_object_is_type(value,json_type_boolean)) status=EINVAL;
        else c.active=json_object_get_boolean(value);
      }
      if(!status && json_object_object_get_ex(request,"read_only",&value)) {
        if(!json_object_is_type(value,json_type_boolean)) status=EINVAL;
        else c.read_only=json_object_get_boolean(value);
      }
      if(!status) status=sm_smb_validate(&c);
    }
    if(!status && !strcmp(action,"check")) status=sm_smb_check(&c,&dialect);
    else if(!status) {
      pthread_mutex_lock(&g_storage_job.mutex);
      if(storage_job_is_active(g_storage_job.state) || g_storage_job.pkg_queue_running || g_storage_job.pkg_queue_count) status=EBUSY;
      else status=!strcmp(action,"remove") ? sm_smb_remove(name) : sm_smb_save(previous,&c);
      pthread_mutex_unlock(&g_storage_job.mutex);
    }
    memset(&c,0,sizeof(c));
    if(status) {
      if(status == EBUSY) send_operation_error_response(connection,status,"storage_busy");
      else send_error_response(connection,operation_http_status(status),status,strerror(status));
      return;
    }
  }
  struct json_object *response=new_status_response(0), *items=json_object_new_array();
  sm_smb_connection_t *connections=calloc(SM_SMB_CONNECTIONS_MAX,sizeof(*connections));
  if(!response || !items || !connections) goto oom;
  size_t count=sm_smb_snapshot(connections);
  for(size_t i=0;i<count;++i) {
    struct json_object *item=json_object_new_object();
    if(!item || !add_json_string(item,"name",connections[i].name) ||
        !add_json_string(item,"path",connections[i].path) || !add_json_string(item,"username",connections[i].username) ||
        !add_json_bool(item,"active",connections[i].active) ||
        !add_json_bool(item,"read_only",connections[i].read_only) ||
        !add_json_bool(item,"password_set",connections[i].password[0] != 0) || json_object_array_add(items,item)) {
      if (item) json_object_put(item);
      goto oom;
    }
  }
  memset(connections,0,SM_SMB_CONNECTIONS_MAX*sizeof(*connections)); free(connections); connections=NULL;
  if(!add_json_int(response,"dialect",dialect)) goto oom;
  if(!add_json_value(response,"connections",items)) { items=NULL; goto oom; }
  (void)send_json_object(connection,200,response); json_object_put(response); return;
oom:
  if(connections) { memset(connections,0,SM_SMB_CONNECTIONS_MAX*sizeof(*connections)); free(connections); }
  if(response) json_object_put(response);
  if(items) json_object_put(items);
  send_out_of_memory_response(connection);
}

static void handle_settings(struct MHD_Connection *connection) {
  runtime_config_t cfg = runtime_config();
  struct json_object *response = new_status_response(0);
  struct json_object *scan_paths = json_object_new_array();
  if (!response || !scan_paths) {
    if (response)
      json_object_put(response);
    if (scan_paths)
      json_object_put(scan_paths);
    send_out_of_memory_response(connection);
    return;
  }

  size_t count = 0;
  int custom_path_count = get_custom_scan_path_count();
  for (int i = 0; i < custom_path_count; ++i) {
    char path[MAX_PATH];
    if (!get_custom_scan_path(i, path))
      continue;
    if (!append_json_string(scan_paths, path)) {
      json_object_put(scan_paths);
      json_object_put(response);
      send_out_of_memory_response(connection);
      return;
    }
    count++;
  }

  if (!add_json_value(response, "scan_paths", scan_paths) ||
      !add_json_bool(response, "api_enabled", cfg.api_enabled) ||
      !add_json_bool(response, "allow_lan_access",
                     strncmp(cfg.api_bind_address, "127.", 4) != 0) ||
      !add_json_bool(response, "debug", cfg.debug_enabled) ||
      !add_json_bool(response, "quiet_mode", cfg.quiet_mode) ||
      !add_json_bool(response, "update_emulators",
                     cfg.update_emulators_enabled) ||
      !add_json_string(response, "fakelib_default_mode",
                       sm_config_fakelib_mode_name(cfg.fakelib_default_mode)) ||
      !add_json_bool(response, "ps4_force_pro", cfg.ps4_force_pro) ||
      !add_json_bool(response, "ps4_cpu_boost", cfg.ps4_cpu_boost) ||
      !add_json_bool(response, "ps4_gpu_boost", cfg.ps4_gpu_boost) ||
      !add_json_bool(response, "auto_update_ampr",
                     cfg.auto_update_ampr_enabled) ||
      !add_json_bool(response, "auto_remove_missing_games",
                     cfg.auto_remove_missing_games) ||
      !add_json_int(response, "auto_remove_missing_delay_seconds",
                    cfg.auto_remove_missing_delay_seconds) ||
      !add_json_int(response, "fan_target_temperature",
                    cfg.fan_target_temperature_c) ||
      !add_json_int(response, "scan_path_count", (int64_t)count)) {
    json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_settings_update(struct MHD_Connection *connection,
                                   struct json_object *request) {
  runtime_config_t current_cfg = runtime_config();
  bool debug_enabled = false;
  bool quiet_mode = false;
  bool update_emulators = false;
  bool auto_update_ampr = current_cfg.auto_update_ampr_enabled;
  bool ps4_force_pro = current_cfg.ps4_force_pro;
  bool ps4_cpu_boost = current_cfg.ps4_cpu_boost;
  bool ps4_gpu_boost = current_cfg.ps4_gpu_boost;
  bool auto_remove_missing_games = current_cfg.auto_remove_missing_games;
  bool allow_lan_access = false;
  sm_fakelib_mode_t fakelib_default_mode = current_cfg.fakelib_default_mode;
  struct json_object *mode_value = NULL;
  struct json_object *remove_delay_value = NULL;
  struct json_object *fan_value = NULL;
  struct json_object *paths_value = NULL;
  if (!get_required_bool(request, "debug", &debug_enabled) ||
      !get_required_bool(request, "quiet_mode", &quiet_mode) ||
      !get_required_bool(request, "update_emulators", &update_emulators) ||
      !get_optional_bool(request, "auto_update_ampr",
                         current_cfg.auto_update_ampr_enabled,
                         &auto_update_ampr) ||
      !get_optional_bool(request, "auto_remove_missing_games",
                         current_cfg.auto_remove_missing_games,
                         &auto_remove_missing_games) ||
      !get_optional_bool(request, "ps4_force_pro", current_cfg.ps4_force_pro,
                         &ps4_force_pro) ||
      !get_optional_bool(request, "ps4_cpu_boost", current_cfg.ps4_cpu_boost,
                         &ps4_cpu_boost) ||
      !get_optional_bool(request, "ps4_gpu_boost", current_cfg.ps4_gpu_boost,
                         &ps4_gpu_boost) ||
      !get_required_bool(request, "allow_lan_access", &allow_lan_access) ||
      !json_object_object_get_ex(request, "fan_target_temperature",
                                 &fan_value) ||
      !json_object_is_type(fan_value, json_type_int) ||
      !json_object_object_get_ex(request, "scan_paths", &paths_value) ||
      !json_object_is_type(paths_value, json_type_array)) {
    send_error_response(connection, 400, EINVAL,
                        "settings fields have invalid types");
    return;
  }

  if (json_object_object_get_ex(request, "fakelib_default_mode", &mode_value) &&
      (!json_object_is_type(mode_value, json_type_string) ||
       !sm_config_parse_fakelib_mode(json_object_get_string(mode_value),
                                    &fakelib_default_mode) ||
       fakelib_default_mode == SM_FAKELIB_DEFAULT)) {
    send_error_response(connection, 400, EINVAL,
                        "fakelib_default_mode must be full, emulators or disabled");
    return;
  }

  int64_t remove_delay_int = current_cfg.auto_remove_missing_delay_seconds;
  if (json_object_object_get_ex(request,
                                "auto_remove_missing_delay_seconds",
                                &remove_delay_value)) {
    if (!json_object_is_type(remove_delay_value, json_type_int)) {
      send_error_response(connection, 400, EINVAL,
                          "settings fields have invalid types");
      return;
    }
    remove_delay_int = json_object_get_int64(remove_delay_value);
  }
  if (remove_delay_int < MIN_AUTO_REMOVE_MISSING_DELAY_SECONDS ||
      remove_delay_int > MAX_AUTO_REMOVE_MISSING_DELAY_SECONDS) {
    send_error_response(
        connection, 400, EINVAL,
        "auto_remove_missing_delay_seconds must be 1..86400");
    return;
  }

  int64_t fan_value_int = json_object_get_int64(fan_value);
  if (fan_value_int < 0 || fan_value_int > UINT32_MAX ||
      (fan_value_int != FAN_TARGET_TEMPERATURE_SYSTEM &&
       (fan_value_int < MIN_FAN_TARGET_TEMPERATURE_C ||
        fan_value_int > MAX_FAN_TARGET_TEMPERATURE_C))) {
    send_error_response(connection, 400, EINVAL,
                        "fan_target_temperature must be 0 or 50..91");
    return;
  }

  size_t path_count = json_object_array_length(paths_value);
  if (path_count > MAX_SCAN_PATHS) {
    send_error_response(connection, 400, EINVAL, "too many scan paths");
    return;
  }
  const char **paths = path_count > 0 ? calloc(path_count, sizeof(*paths))
                                      : NULL;
  if (path_count > 0 && !paths) {
    send_out_of_memory_response(connection);
    return;
  }
  bool valid = true;
  for (size_t i = 0; i < path_count; ++i) {
    struct json_object *item = json_object_array_get_idx(paths_value, i);
    if (!item || !json_object_is_type(item, json_type_string)) {
      valid = false;
      break;
    }
    paths[i] = json_object_get_string(item);
  }
  if (!valid || !sm_config_write_web_settings(
                    debug_enabled, quiet_mode, update_emulators,
                    auto_update_ampr, auto_remove_missing_games,
                    (uint32_t)remove_delay_int,
                    allow_lan_access,
                    (uint32_t)fan_value_int, paths, path_count,
                    fakelib_default_mode, ps4_force_pro, ps4_cpu_boost,
                    ps4_gpu_boost)) {
    int status = valid && errno != 0 ? errno : EINVAL;
    free(paths);
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  free(paths);

  // The config vnode event reloads the new topology first and then schedules
  // its full scan. Queuing an immediate scan here would race that reload and
  // scan the previous (often default) roots once more.

  struct json_object *response = new_status_response(0);
  if (!response || !add_json_bool(response, "saved", true) ||
      !add_json_bool(response, "reload_queued", true)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_debug_log(struct MHD_Connection *connection,
                             struct json_object *request) {
  size_t max_bytes;
  if (!get_log_max_bytes(connection, request, &max_bytes))
    return;

  int fd = open(LOG_FILE, O_RDONLY);
  struct stat st;
  if (fd < 0 && errno != ENOENT) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  memset(&st, 0, sizeof(st));
  if (fd >= 0 && (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode))) {
    int status = errno != 0 ? errno : EIO;
    (void)close(fd);
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  uint64_t file_size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
  size_t read_size = file_size > max_bytes ? max_bytes : (size_t)file_size;
  char *content = malloc(read_size + 1u);
  if (!content) {
    if (fd >= 0)
      (void)close(fd);
    send_out_of_memory_response(connection);
    return;
  }
  size_t offset = 0;
  int read_status = 0;
  off_t file_offset = (off_t)(file_size - read_size);
  while (fd >= 0 && offset < read_size) {
    ssize_t count = pread(fd, content + offset, read_size - offset,
                          file_offset + (off_t)offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count < 0) {
      read_status = errno != 0 ? errno : EIO;
      break;
    }
    if (count == 0)
      break;
    offset += (size_t)count;
  }
  if (fd >= 0)
    (void)close(fd);
  if (read_status != 0) {
    free(content);
    send_error_response(connection, operation_http_status(read_status),
                        read_status, strerror(read_status));
    return;
  }
  if (file_size > read_size && offset > 0) {
    char *newline = memchr(content, '\n', offset);
    if (newline) {
      size_t discard = (size_t)(newline - content) + 1u;
      memmove(content, content + discard, offset - discard);
      offset -= discard;
    }
  }
  content[offset] = '\0';

  send_log_tail_response(connection, content, offset, (size_t)file_size,
                         "file_size");
}

static void handle_kernel_log(struct MHD_Connection *connection,
                              struct json_object *request) {
  size_t max_bytes;
  if (!get_log_max_bytes(connection, request, &max_bytes))
    return;

  char *content = NULL;
  size_t returned_bytes = 0;
  size_t total_bytes = 0;
  int status = sm_mdbg_get_log_tail(max_bytes, &content, &returned_bytes,
                                    &total_bytes);
  if (status != 0) {
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }

  send_log_tail_response(connection, content, returned_bytes, total_bytes,
                         "total_bytes");
}

static const char *runtime_mutation_block_reason(void) {
  if (runtime_sleep_mode_active())
    return "rest_mode";
  if (sm_game_lifecycle_has_active_game() &&
      sm_shellcore_background_work_blocked())
    return "game_active";
  if (sm_install_has_pending_work())
    return "install_pending";
  return NULL;
}

static int resolve_storage_source(const char *title_id,
                                   game_storage_operation_t operation,
                                   game_source_info_t *source_info,
                                   const char **error_out) {
  *error_out = NULL;
  sm_game_cache_snapshot_entry_t entry;
  if (!find_game_snapshot_by_title(title_id, &entry))
    return errno != 0 ? errno : ENOENT;
  if (!resolve_game_source(&entry, source_info))
    return ENOENT;

  struct stat st;
  if (lstat(source_info->physical_path, &st) == 0)
    return 0;
  int status = errno != 0 ? errno : EIO;
  if (operation == GAME_STORAGE_DELETE &&
      (status == ENOENT || status == ENOTDIR))
    *error_out = "source_missing";
  return status;
}

static int request_game_uninstall(const char *title_id,
                                  const char **error_out,
                                  bool *scan_queued_out) {
  *scan_queued_out = false;
  *error_out = runtime_mutation_block_reason();
  if (*error_out)
    return EBUSY;
  if (sm_shellcore_title_is_protected(title_id)) {
    *error_out = "game_active";
    return EBUSY;
  }
  sm_game_cache_snapshot_entry_t package;
  bool installed_pkg = find_installed_pkg_game(title_id, &package);
  if (!installed_pkg && errno != ENOENT)
    return errno != 0 ? errno : EIO;
  if (!installed_pkg && !is_installed(title_id))
    return ENOENT;

  bool title_prepared = sm_shellcore_service_title_is_prepared(title_id);
  if (sm_shellcore_service_has_prepared_mount() && !title_prepared &&
      sm_shellcore_background_work_blocked()) {
    *error_out = "runtime_prepared";
    return EBUSY;
  }
  if (!installed_pkg && (title_prepared || is_data_mounted(title_id))) {
    int release_status;
    if (!title_prepared && sm_game_lifecycle_has_active_game()) {
      runtime_mount_state_lock();
      bool released = unmount_title_runtime_layers(title_id);
      release_status = released ? 0 : (errno != 0 ? errno : EIO);
      runtime_mount_state_unlock();
    } else {
      release_status = sm_shellcore_unmount_title_runtime(title_id);
    }
    if (release_status != 0) {
      if (release_status == EBUSY)
        *error_out = "title_unmount_busy";
      return release_status;
    }
  }

  bool source_missing = false;
  if (!installed_pkg) {
    game_source_info_t info;
    const char *source_error = NULL;
    (void)resolve_storage_source(title_id, GAME_STORAGE_DELETE, &info,
                                 &source_error);
    source_missing = source_error && strcmp(source_error, "source_missing") == 0;
  }

  int platform_status = sceAppInstUtilAppUnInstall(title_id);
  if (platform_status != 0) {
    log_debug("  [API] uninstall request failed: title=%s code=0x%08X",
              title_id, (uint32_t)platform_status);
    return EIO;
  }

  invalidate_app_db_title_cache();
  reset_title_attempts(title_id, NULL, NULL);
  if (source_missing)
    *scan_queued_out = request_scan_now("HTTP API missing source uninstall requested");
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
    send_operation_error_response(fd, EBUSY, "scanner_busy");
    return;
  }
  if (!sm_shellcore_try_begin_external_mutation()) {
    sm_scanner_end_external_mutation();
    send_operation_error_response(fd, EBUSY, "runtime_busy");
    return;
  }
  const char *error = NULL;
  bool scan_queued = false;
  int status = request_game_uninstall(title_id, &error, &scan_queued);
  sm_shellcore_end_external_mutation();
  sm_scanner_end_external_mutation();
  if (status != 0) {
    log_debug("  [API] uninstall blocked or failed: title=%s status=%d reason=%s",
              title_id, status, operation_error_message(status, error));
    send_operation_error_response(fd, status, error);
    return;
  }

  struct json_object *response = new_status_response(0);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_string(response, "title_id", title_id) ||
      !add_scan_queue_fields(response, scan_queued) ||
      !add_json_bool(response, "uninstall_requested", true)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void handle_content_uninstall(struct MHD_Connection *connection,
                                      struct json_object *request) {
  const char *title_id = get_title_id(request);
  struct json_object *kind_value = NULL, *cid_value = NULL;
  const char *kind = NULL, *cid = NULL;
  if (json_object_object_get_ex(request, "kind", &kind_value) &&
      json_object_is_type(kind_value, json_type_string))
    kind = json_object_get_string(kind_value);
  bool patch = kind && json_object_get_string_len(kind_value) == 5 && !strcmp(kind, "patch");
  bool dlc = kind && json_object_get_string_len(kind_value) == 3 && !strcmp(kind, "dlc");
  char dir_name[32] = {0};
  if (dlc && json_object_object_get_ex(request, "content_id", &cid_value) &&
      json_object_is_type(cid_value, json_type_string)) {
    cid = json_object_get_string(cid_value);
    size_t length = strlen(cid);
    if ((size_t)json_object_get_string_len(cid_value) != length ||
        !valid_game_dlc_content_id(cid, title_id)) {
      cid = NULL;
    }
    if (cid) strlcpy(dir_name, cid + 20, sizeof(dir_name));
  }
  if (!title_id || (!patch && !dlc) || (dlc && !cid)) {
    send_error_response(connection, 400, EINVAL,
                        "valid title_id, kind (patch/dlc), and matching DLC content_id required");
    return;
  }
  if (!sm_scanner_try_begin_external_mutation()) {
    send_operation_error_response(connection, EBUSY, "scanner_busy");
    return;
  }
  if (!sm_shellcore_try_begin_external_mutation()) {
    sm_scanner_end_external_mutation();
    send_operation_error_response(connection, EBUSY, "runtime_busy");
    return;
  }
  const char *error = runtime_mutation_block_reason();
  int status = error ? EBUSY : 0;
  int native_status = 0;
  // Serialize with storage submissions, including the PKG installation queue.
  pthread_mutex_lock(&g_storage_job.mutex);
  if (!status && (!g_storage_job.accepting ||
      storage_job_is_active(g_storage_job.state) || g_storage_job.pkg_queue_running)) {
    status = EBUSY; error = "storage_busy";
  }
  if (!status && sm_shellcore_title_is_protected(title_id)) {
    status = EBUSY; error = "game_active";
  }
  if (!status && (sm_shellcore_service_title_is_prepared(title_id) ||
      sm_shellcore_background_work_blocked() || is_data_mounted(title_id))) {
    status = EBUSY; error = "runtime_prepared";
  }
  if (!status && !is_installed(title_id)) status = ENOENT;
  if (!status && patch) {
    sm_game_cache_snapshot_entry_t package;
    if (!find_installed_pkg_game(title_id, &package))
      status = errno != 0 ? errno : ENOENT;
  }
  if (!status && dlc) {
    int installed = 0;
    native_status = sceAppInstUtilGetAddcontInstalledStatus(cid, &installed);
    if (native_status) status = EIO;
    else if (installed != SM_PKG_ADDCONT_INSTALLED) status = ENOENT;
  }
  if (!status) {
    native_status = patch ? sceAppInstUtilAppUnInstallPat(title_id) :
        sceAppInstUtilAppUnInstallAddcont(title_id, dir_name);
    if (native_status) status = EIO;
    else invalidate_app_db_title_cache();
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
  sm_shellcore_end_external_mutation();
  sm_scanner_end_external_mutation();
  log_debug("  [API] content uninstall: title=%s kind=%s content=%s status=%d native=0x%08X",
            title_id, kind, cid ? cid : "", status, (uint32_t)native_status);
  if (status) {
    send_operation_error_response(connection, status, error);
    return;
  }
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_string(response, "title_id", title_id) ||
      !add_json_string(response, "kind", kind) ||
      !add_json_bool(response, "uninstall_requested", true) ||
      (dlc && !add_json_string(response, "content_id", cid))) {
    if (response) json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_game_fakelib(struct MHD_Connection *connection,
                                 struct json_object *request) {
  const char *title_id = get_title_id(request);
  sm_fakelib_mode_t mode = SM_FAKELIB_DEFAULT;
  struct json_object *mode_value = NULL;
  struct json_object *global_value = NULL;
  bool update_global = json_object_object_get_ex(request, "ignore_global_fakelib",
                                                 &global_value);
  bool global_excluded = false;
  bool valid_policy;
  if (update_global) {
    valid_policy = json_object_is_type(global_value, json_type_boolean) &&
                   !json_object_object_get_ex(request, "mode", &mode_value) &&
                   !json_object_object_get_ex(request, "enabled", &mode_value);
    global_excluded = json_object_get_boolean(global_value);
  } else if (json_object_object_get_ex(request, "mode", &mode_value)) {
    valid_policy = json_object_is_type(mode_value, json_type_string) &&
                   sm_config_parse_fakelib_mode(
                       json_object_get_string(mode_value), &mode);
  } else {
    bool enabled = false;
    valid_policy = get_required_bool(request, "enabled", &enabled);
    mode = enabled ? SM_FAKELIB_FULL : SM_FAKELIB_DISABLED;
  }
  if (!title_id || !valid_policy) {
    send_error_response(connection, 400, EINVAL,
                        "title_id and a valid fakelib setting are required; "
                        "ignore_global_fakelib cannot be combined with mode or enabled");
    return;
  }
  if (strncmp(title_id, "PPSA", 4u) != 0) {
    sm_app_db_game_info_t *metadata = NULL;
    size_t count = 0;
    if (!app_db_game_info_snapshot(&metadata, &count)) {
      int status = errno != 0 ? errno : EIO;
      send_error_response(connection, operation_http_status(status), status,
                          strerror(status));
      return;
    }
    const sm_app_db_game_info_t *info = app_db_find_game_info(metadata, count,
                                                            title_id);
    bool ps5 = strcmp(game_platform_name(title_id, info), "ps5") == 0;
    free(metadata);
    if (!ps5) {
      send_error_response(connection, 400, EINVAL,
                          "fakelib settings are only available for PS5 games");
      return;
    }
  }
  sm_game_cache_snapshot_entry_t entry;
  if (!find_game_snapshot_by_title(title_id, &entry) &&
      (errno != ENOENT || !find_installed_pkg_game(title_id, &entry))) {
    int status = errno != 0 ? errno : ENOENT;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  bool saved = update_global
                   ? sm_config_set_title_global_fakelib_excluded(title_id, global_excluded)
                   : sm_config_set_title_fakelib_mode(title_id, mode);
  if (!saved) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
    return;
  }
  const runtime_config_t cfg = runtime_config();
  sm_fakelib_mode_t effective_mode = sm_config_title_fakelib_mode(&cfg, title_id);
  bool enabled = effective_mode != SM_FAKELIB_DISABLED;
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_string(response, "title_id", title_id) ||
      !add_json_bool(response, "fakelib_enabled", enabled) ||
      !add_json_bool(response, "ignore_global_fakelib",
                     sm_config_title_global_fakelib_excluded(&cfg, title_id)) ||
      !add_json_string(response, "fakelib_mode",
                       sm_config_fakelib_mode_name(effective_mode)) ||
      !add_json_string(response, "fakelib_default_mode",
                       sm_config_fakelib_mode_name(cfg.fakelib_default_mode)) ||
      !add_json_string(response, "fakelib_mode_override",
                       sm_config_fakelib_mode_name(
                           sm_config_title_fakelib_override(&cfg, title_id))) ||
      !add_json_bool(response, "fakelib_effective_enabled",
                     enabled && cfg.backport_fakelib_enabled) ||
      !add_json_bool(response, "saved", true) ||
      !add_json_bool(response, "applies_on_next_launch", true)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_game_ps4_profile(struct MHD_Connection *connection,
                                    struct json_object *request) {
  const char *title_id = get_title_id(request);
  sm_ps4_settings_t settings;
  struct json_object *pro, *cpu, *gpu, *memory_value;
  runtime_config_t cfg = runtime_config();
  settings = sm_config_title_ps4_override(&cfg, title_id);
  if (!sm_config_is_ps4_title(title_id) ||
      !json_object_object_get_ex(request, "pro", &pro) || !json_object_is_type(pro, json_type_string) ||
      !json_object_object_get_ex(request, "cpu", &cpu) || !json_object_is_type(cpu, json_type_string) ||
      !json_object_object_get_ex(request, "gpu", &gpu) || !json_object_is_type(gpu, json_type_string) ||
      !sm_config_parse_ps4_setting(json_object_get_string(pro), &settings.pro) ||
      !sm_config_parse_ps4_setting(json_object_get_string(cpu), &settings.cpu) ||
      !sm_config_parse_ps4_setting(json_object_get_string(gpu), &settings.gpu)) {
    send_error_response(connection, 400, EINVAL,
                        "PS4 title_id and pro/cpu/gpu values default, on or off are required");
    return;
  }
  if ((json_object_object_get_ex(request, "memory", &memory_value) &&
       (!json_object_is_type(memory_value, json_type_string) ||
        !sm_config_parse_ps4_setting(json_object_get_string(memory_value), &settings.memory))) ||
      (settings.memory == SM_PS4_ON &&
       (settings.pro == SM_PS4_OFF || !sm_ps4_memory_supported()))) {
    send_error_response(connection, 400, EINVAL,
                        "8 GB requires a supported memory profile and Pro mode; memory accepts default, on or off");
    return;
  }
  struct json_object *video, *clock;
  if ((json_object_object_get_ex(request, "video_120hz", &video) &&
       (!json_object_is_type(video, json_type_string) ||
        !sm_config_parse_ps4_setting(json_object_get_string(video), &settings.video_120hz))) ||
      (json_object_object_get_ex(request, "gpu_clock", &clock) &&
       (!json_object_is_type(clock, json_type_string) ||
        (strcmp(json_object_get_string(clock), "default") &&
         strcmp(json_object_get_string(clock), "2350"))))) {
    send_error_response(connection, 400, EINVAL,
                        "video_120hz accepts default/on/off; gpu_clock accepts default/2350");
    return;
  }
  if (json_object_object_get_ex(request, "gpu_clock", &clock))
    settings.gpu_mhz = !strcmp(json_object_get_string(clock), "2350") ? 2350 : 0;
  sm_game_cache_snapshot_entry_t entry;
  if (!find_game_snapshot_by_title(title_id, &entry) &&
      (errno != ENOENT || !find_installed_pkg_game(title_id, &entry))) {
    int status = errno != 0 ? errno : ENOENT;
    send_error_response(connection, operation_http_status(status), status, strerror(status));
    return;
  }
  if (!sm_config_set_title_ps4_profile(title_id, &settings)) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status, strerror(status));
    return;
  }
  // The title writer publishes this rule immediately. Prepare newly required
  // hooks now; the launch callback selects the effective policy without ptrace.
  (void)sm_shellcore_hooks_refresh();
  cfg = runtime_config();
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_string(response, "title_id", title_id) ||
      !add_ps4_profile_fields(response, &cfg, title_id) ||
      !add_json_bool(response, "saved", true) ||
      !add_json_bool(response, "applies_on_next_launch", true)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_game_patches(struct MHD_Connection *connection,
                                struct json_object *request) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(connection, 400, EINVAL, "valid title_id is required");
    return;
  }
  char version[MAX_GAME_VERSION] = {0};
  sm_app_db_game_info_t *metadata = NULL;
  size_t count = 0;
  if (!app_db_game_info_snapshot(&metadata, &count)) {
    int error = errno ? errno : EIO;
    send_error_response(connection, operation_http_status(error), error, strerror(error));
    return;
  }
  const sm_app_db_game_info_t *info = app_db_find_game_info(metadata, count, title_id);
  bool found = info != NULL;
  if (info) strlcpy(version, info->version, sizeof(version));
  free(metadata);
  if (!found || !*version) {
    sm_game_cache_snapshot_entry_t game;
    if (find_game_snapshot_by_title(title_id, &game)) {
      found = true;
      sm_pkg_info_t source;
      if (sm_pkg_read_directory_metadata(game.path, &source))
        strlcpy(version, source.app_ver, sizeof(version));
    }
  }
  if (!found) {
    send_error_response(connection, 404, ENOENT, "game not found");
    return;
  }
  sm_game_patch_list_t list;
  int error = sm_game_patches_list(title_id, version, &list);
  if (error) {
    send_error_response(connection, operation_http_status(error), error, "XML patch catalog could not be read");
    return;
  }
  struct json_object *id = NULL, *enabled = NULL;
  bool update_id = json_object_object_get_ex(request, "id", &id);
  bool update_enabled = json_object_object_get_ex(request, "enabled", &enabled);
  bool update = update_id || update_enabled;
  if (update && (!update_id || !update_enabled ||
      !json_object_is_type(id, json_type_string) || !json_object_is_type(enabled, json_type_boolean) ||
      (size_t)json_object_get_string_len(id) != strlen(json_object_get_string(id)))) error = EINVAL;
  if (update && !error) {
    sm_game_patch_t *selected = NULL;
    for (size_t i = 0; i < list.count; ++i)
      if (!strcmp(list.entries[i].id, json_object_get_string(id))) selected = &list.entries[i];
    if (!selected) error = ENOENT;
    else if (json_object_get_boolean(enabled) && (!selected->compatible || !selected->supported)) error = EINVAL;
    else {
      error = sm_game_patches_set_enabled(title_id, &list, selected->id, json_object_get_boolean(enabled));
      if (!error) selected->enabled = json_object_get_boolean(enabled);
    }
  }
  if (error) {
    sm_game_patches_free(&list);
    send_error_response(connection, operation_http_status(error), error, "invalid or incompatible patch selection");
    return;
  }
  struct json_object *response = new_status_response(0), *patches = json_object_new_array();
  bool ok = response && patches;
  for (size_t i = 0; ok && i < list.count; ++i) {
    const sm_game_patch_t *p = &list.entries[i];
    struct json_object *item = json_object_new_object();
    ok = item && add_json_string(item, "id", p->id) && add_json_string(item, "name", p->name) &&
         add_json_string(item, "author", p->author) && add_json_string(item, "note", p->note) &&
         add_json_string(item, "app_version", p->app_ver) && add_json_string(item, "module", p->app_elf) &&
         add_json_string(item, "requires", p->requires) && add_json_string(item, "error", p->error) &&
         add_json_bool(item, "enabled", p->enabled) && add_json_bool(item, "compatible", p->compatible) &&
         add_json_bool(item, "supported", p->supported) && add_json_int(item, "line_count", p->line_count);
    if (ok) ok = json_object_array_add(patches, item) == 0;
    if (!ok && item) json_object_put(item);
  }
  sm_game_patches_free(&list);
  if (ok) {
    ok = add_json_value(response, "patches", patches);
    patches = NULL;
  }
  if (ok) ok = add_json_string(response, "title_id", title_id) && add_json_string(response, "version", version) &&
                add_json_string(response, "directory", SM_PATCH_DIRECTORY) &&
                add_json_bool(response, "applies_on_next_launch", true) && add_json_bool(response, "saved", update);
  if (patches) json_object_put(patches);
  if (ok) (void)send_json_object(connection, 200, response);
  else send_out_of_memory_response(connection);
  if (response) json_object_put(response);
}

static void dispatch_request(struct MHD_Connection *connection,
                             const char *route,
                             struct json_object *json) {
  if (strcmp(route, SM_API_ROUTE_VERSION) == 0) {
    handle_version(connection);
  } else if (strcmp(route, SM_API_ROUTE_STORAGE) == 0) {
    handle_storage(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_STORAGE_EJECT) == 0) {
    handle_storage_eject(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_IMAGES) == 0) {
    handle_images(connection);
  } else if (strcmp(route, SM_API_ROUTE_GAMES) == 0) {
    handle_games(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_INFO) == 0) {
    handle_game_info(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_PS4_PROFILE) == 0) {
    handle_game_ps4_profile(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_PATCHES) == 0) {
    handle_game_patches(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_FAKELIB) == 0) {
    handle_game_fakelib(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_MOUNT) == 0) {
    handle_mount_operation(connection, json, true);
  } else if (strcmp(route, SM_API_ROUTE_UNMOUNT) == 0) {
    handle_mount_operation(connection, json, false);
  } else if (strcmp(route, SM_API_ROUTE_UNINSTALL) == 0) {
    handle_uninstall(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_CONTENT_UNINSTALL) == 0) {
    handle_content_uninstall(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_MOVE) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_MOVE);
  } else if (strcmp(route, SM_API_ROUTE_GAME_COPY) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_COPY);
  } else if (strcmp(route, SM_API_ROUTE_GAME_DELETE) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_DELETE);
  } else if (strcmp(route, SM_API_ROUTE_GAME_UNPACK) == 0) {
    handle_game_storage_operation(connection, json, GAME_STORAGE_UNPACK);
  } else if (strcmp(route, SM_API_ROUTE_GAME_STORAGE_STATUS) == 0) {
    handle_storage_job_status(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_GAME_STORAGE_CANCEL) == 0) {
    handle_storage_job_cancel(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_LIST) == 0) {
    handle_manual_list(connection);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_ADD) == 0) {
    handle_manual_update(connection, json, true);
  } else if (strcmp(route, SM_API_ROUTE_MANUAL_REMOVE) == 0) {
    handle_manual_update(connection, json, false);
  } else if (strcmp(route, SM_API_ROUTE_SETTINGS) == 0) {
    handle_settings(connection);
  } else if (strcmp(route, SM_API_ROUTE_SETTINGS_UPDATE) == 0) {
    handle_settings_update(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_SMB) == 0) {
    handle_smb(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_DEBUG_LOG) == 0) {
    handle_debug_log(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_KERNEL_LOG) == 0) {
    handle_kernel_log(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_SCAN) == 0) {
    handle_scan(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_PACKAGES) == 0) {
    handle_file_listing(connection, json, true);
  } else if (strcmp(route, SM_API_ROUTE_FILES) == 0) {
    handle_file_listing(connection, json, false);
  } else if (strcmp(route, SM_API_ROUTE_DIRECTORY_CREATE) == 0) {
    handle_directory_create(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_SOURCE_INSTALL) == 0) {
    handle_source_install(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_SOURCE_DELETE) == 0) {
    handle_source_delete(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_BACKPORT_INSTALL) == 0) {
    handle_backport_operation(connection, json, GAME_STORAGE_COPY);
  } else if (strcmp(route, SM_API_ROUTE_BACKPORT_DELETE) == 0) {
    handle_backport_operation(connection, json, GAME_STORAGE_DELETE);
  } else if (strcmp(route, SM_API_ROUTE_PACKAGE_INSTALL) == 0) {
    handle_package_install(connection, json, false, false);
  } else if (strcmp(route, SM_API_ROUTE_PACKAGE_QUEUE) == 0) {
    handle_package_queue(connection, json);
  } else if (strcmp(route, SM_API_ROUTE_PACKAGE_STREAM) == 0) {
    handle_package_install(connection, json, true, false);
  } else if (strcmp(route, SM_API_ROUTE_PACKAGE_RANGE) == 0) {
    handle_package_range(connection, json);
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
  case GAME_STORAGE_UNPACK:
    return "unpack";
  case GAME_STORAGE_INSTALL:
    return "install";
  case GAME_STORAGE_EJECT:
    return "eject";
  }
  return "unknown";
}

static bool destination_is_managed(const char *destination) {
  if (sm_smb_path(destination)) {
    char canonical[MAX_PATH];
    return sm_smb_normalize(destination,canonical) == 0;
  }
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
    char scan_path[MAX_PATH];
    if (!get_scan_path(i, scan_path))
      continue;
    if (!is_under_image_mount_base(scan_path) &&
        path_matches_root_or_child(destination, scan_path)) {
      return true;
    }
  }
  return false;
}

static int resolve_storage_destination_dir(const char *requested_dir,
                                            char resolved_dir[MAX_PATH]) {
  if (sm_smb_path(requested_dir)) {
    sm_smb_address_t address;
    if (sm_smb_parse_address(requested_dir, &address)) return errno;
    if (address.read_only) return EROFS;
  }
  if (!realpath(requested_dir, resolved_dir)) {
    if (errno != ENOENT ||
        strcmp(requested_dir, "/mnt/ext1/homebrew") != 0)
      return errno != 0 ? errno : EINVAL;
    struct statfs filesystem;
    if (statfs("/mnt/ext1", &filesystem) != 0)
      return errno != 0 ? errno : EIO;
    if (strcmp(filesystem.f_mntonname, "/mnt/ext1") != 0)
      return ENODEV;
    if ((filesystem.f_flags & MNT_RDONLY) != 0)
      return EROFS;
    if (!destination_is_managed(requested_dir))
      return EINVAL;
    if (mkdir(requested_dir, 0777) != 0 && errno != EEXIST)
      return errno != 0 ? errno : EIO;
    if (!realpath(requested_dir, resolved_dir))
      return errno != 0 ? errno : EINVAL;
  }
  struct stat st;
  if (stat(resolved_dir, &st) != 0)
    return errno != 0 ? errno : EIO;
  if (!S_ISDIR(st.st_mode) || !destination_is_managed(resolved_dir))
    return EINVAL;
  return 0;
}

static int build_storage_destination_path(const char *resolved_dir,
                                          const char *source,
                                          char destination[MAX_PATH]) {
  struct stat st;

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

static int build_storage_destination(const char *requested_dir,
                                     const char *source,
                                     char destination[MAX_PATH]) {
  char resolved_dir[MAX_PATH];
  int status = resolve_storage_destination_dir(requested_dir, resolved_dir);
  return status ? status : build_storage_destination_path(resolved_dir, source, destination);
}

static int build_unpack_destination(const char *requested_dir,
                                    const char *title_id,
                                    char destination[MAX_PATH]) {
  char resolved_dir[MAX_PATH];
  int status = resolve_storage_destination_dir(requested_dir, resolved_dir);
  if (status != 0)
    return status;
  struct stat st;
  int written = snprintf(destination, MAX_PATH, "%s/%s", resolved_dir,
                         title_id);
  if (written < 0 || (size_t)written >= MAX_PATH)
    return ENAMETOOLONG;
  if (lstat(destination, &st) == 0)
    return EEXIST;
  return errno == ENOENT ? 0 : (errno != 0 ? errno : EIO);
}

static int count_titles_for_source(const char *physical_path,
                                   size_t *count_out) {
  *count_out = 0;
  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count))
    return ENOMEM;
  for (size_t i = 0; i < count; ++i) {
    game_source_info_t info;
    if (resolve_game_source(&snapshot[i], &info) &&
        strcmp(info.physical_path, physical_path) == 0) {
      (*count_out)++;
    }
  }
  free(snapshot);
  return *count_out > 0 ? 0 : ENOENT;
}

static int release_storage_source_runtime(const char *physical_path,
                                          const char *title_id,
                                          size_t *affected_out,
                                          const char **error_out) {
  *affected_out = 0;
  *error_out = runtime_mutation_block_reason();
  if (*error_out)
    return EBUSY;
  if (sm_shellcore_background_work_blocked()) {
    *error_out = "runtime_prepared";
    return EBUSY;
  }
  if (sm_shellcore_title_is_protected(title_id) ||
      sm_shellcore_source_is_protected(physical_path)) {
    *error_out = "game_active";
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
      if (status == EBUSY)
        *error_out = "title_unmount_busy";
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
        int status = errno == EBUSY ? EBUSY : EIO;
        if (status == EBUSY)
          *error_out = "image_unmount_busy";
        runtime_mount_state_unlock();
        return status;
      }
    }
  }
  runtime_mount_state_unlock();
  return 0;
}

static int release_browser_source_runtime(const char *path, size_t *affected_out,
                                           const char **error_out) {
  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  *affected_out = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count)) return ENOMEM;
  int status = 0;
  for (size_t i = 0; i < count; ++i) {
    game_source_info_t source;
    char canonical[PATH_MAX];
    if (!resolve_game_source(&snapshot[i], &source)) continue;
    const char *physical = realpath(source.physical_path, canonical) ? canonical : source.physical_path;
    if (!path_matches_root_or_child(physical, path)) continue;
    size_t affected = 0;
    status = release_storage_source_runtime(source.physical_path, snapshot[i].title_id, &affected, error_out);
    if (status == ENOENT && affected == 0) status = 0;
    if (status) break;
    ++*affected_out;
  }
  free(snapshot);
  return status;
}

static void update_moved_source_links(const char *source,
                                      const char *destination) {
  if (sm_smb_path(destination)) {
    cleanup_mount_links_for_source_unmount(source);
    return;
  }
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

static const char *storage_job_state_name(storage_job_state_t state) {
  switch (state) {
  case STORAGE_JOB_IDLE:
    return "idle";
  case STORAGE_JOB_PREPARING:
    return "preparing";
  case STORAGE_JOB_MEASURING:
    return "measuring";
  case STORAGE_JOB_TRANSFERRING:
    return "transferring";
  case STORAGE_JOB_DELETING:
    return "deleting";
  case STORAGE_JOB_FINALIZING:
    return "finalizing";
  case STORAGE_JOB_INSTALLING:
    return "installing";
  case STORAGE_JOB_COMPLETED:
    return "completed";
  case STORAGE_JOB_FAILED:
    return "failed";
  case STORAGE_JOB_CANCELLED:
    return "cancelled";
  }
  return "unknown";
}

static bool storage_job_is_cancellable(storage_job_state_t state) {
  return state == STORAGE_JOB_PREPARING || state == STORAGE_JOB_MEASURING ||
         state == STORAGE_JOB_TRANSFERRING || state == STORAGE_JOB_INSTALLING;
}
static void copy_storage_job_snapshot_locked(
    storage_job_snapshot_t *snapshot) {
  snapshot->active = storage_job_is_active(g_storage_job.state);
  snapshot->cancel_requested = g_storage_job.cancel_requested;
  snapshot->id = g_storage_job.id;
  snapshot->operation = g_storage_job.operation;
  snapshot->state = g_storage_job.state;
  (void)strlcpy(snapshot->title_id, g_storage_job.title_id,
                sizeof(snapshot->title_id));
  (void)strlcpy(snapshot->source_type, g_storage_job.source_type,
                sizeof(snapshot->source_type));
  (void)strlcpy(snapshot->source, g_storage_job.source,
                sizeof(snapshot->source));
  (void)strlcpy(snapshot->runtime_source, g_storage_job.runtime_source,
                sizeof(snapshot->runtime_source));
  (void)strlcpy(snapshot->destination, g_storage_job.destination,
                sizeof(snapshot->destination));
  snapshot->delete_source = g_storage_job.delete_source;
  snapshot->total_bytes = g_storage_job.total_bytes;
  snapshot->processed_bytes = g_storage_job.processed_bytes;
  snapshot->total_files = g_storage_job.total_files;
  snapshot->processed_files = g_storage_job.processed_files;
  snapshot->started_us = g_storage_job.started_us;
  snapshot->transfer_started_us = g_storage_job.transfer_started_us;
  snapshot->transfer_finished_us = g_storage_job.transfer_finished_us;
  snapshot->finished_us = g_storage_job.finished_us;
  snapshot->affected_titles = g_storage_job.affected_titles;
  snapshot->result_status = g_storage_job.result_status;
  (void)strlcpy(snapshot->result_error_reason, g_storage_job.result_error_reason,
                sizeof(snapshot->result_error_reason));
  snapshot->usb_blocker = g_storage_job.usb_blocker;
  snapshot->scan_queued = g_storage_job.scan_queued;
  strlcpy(snapshot->title_name, g_storage_job.title_name, sizeof(snapshot->title_name));
  snapshot->native_status = g_storage_job.native_status;
  snapshot->pkg_speed = g_storage_job.pkg_speed;
  snapshot->pkg_sample_us = g_storage_job.pkg_sample_us;
  snapshot->pkg_queue_count = g_storage_job.pkg_queue_count;
  snapshot->pkg_queue_completed = g_storage_job.pkg_queue_completed;
  snapshot->pkg_queue_total = g_storage_job.pkg_queue_total;
  snapshot->pkg_queue_running = g_storage_job.pkg_queue_running;
  snapshot->pkg_current_kind = g_storage_job.pkg_current_kind;
}

static bool get_storage_job_snapshot(uint64_t requested_id,
                                     storage_job_snapshot_t *snapshot) {
  pthread_mutex_lock(&g_storage_job.mutex);
  bool found = requested_id == 0 || requested_id == g_storage_job.id;
  if (found)
    copy_storage_job_snapshot_locked(snapshot);
  pthread_mutex_unlock(&g_storage_job.mutex);
  return found;
}

static double storage_job_progress_percent(
    const storage_job_snapshot_t *snapshot) {
  if (snapshot->state == STORAGE_JOB_COMPLETED)
    return 100.0;
  if (snapshot->total_bytes > 0) {
    double percent =
        (double)snapshot->processed_bytes * 100.0 /
        (double)snapshot->total_bytes;
    return percent > 100.0 ? 100.0 : percent;
  }
  if (snapshot->total_files > 0) {
    double percent =
        (double)snapshot->processed_files * 100.0 /
        (double)snapshot->total_files;
    return percent > 100.0 ? 100.0 : percent;
  }
  return 0.0;
}

static uint64_t storage_job_elapsed_ms(
    const storage_job_snapshot_t *snapshot, uint64_t now_us) {
  if (snapshot->started_us == 0)
    return 0;
  uint64_t end_us = snapshot->finished_us != 0 ? snapshot->finished_us : now_us;
  if (end_us < snapshot->started_us)
    return 0;
  return (end_us - snapshot->started_us) / 1000u;
}

static uint64_t storage_job_speed_bytes_per_second(
    const storage_job_snapshot_t *snapshot, uint64_t now_us) {
  if (snapshot->operation == GAME_STORAGE_INSTALL &&
      (!strcmp(snapshot->source_type, "pkg") || !strcmp(snapshot->source_type, "stream")))
    return snapshot->active && now_us >= snapshot->pkg_sample_us &&
                   now_us - snapshot->pkg_sample_us <= 3000000
               ? snapshot->pkg_speed : 0;
  if (snapshot->transfer_started_us == 0 || snapshot->processed_bytes == 0)
    return 0;
  uint64_t end_us = snapshot->transfer_finished_us != 0
                        ? snapshot->transfer_finished_us
                        : now_us;
  if (end_us <= snapshot->transfer_started_us)
    return 0;
  uint64_t elapsed_us = end_us - snapshot->transfer_started_us;
  double speed = (double)snapshot->processed_bytes * 1000000.0 /
                 (double)elapsed_us;
  return speed >= (double)INT64_MAX ? (uint64_t)INT64_MAX : (uint64_t)speed;
}

static bool add_storage_job_fields(struct json_object *response,
                                   const storage_job_snapshot_t *snapshot) {
  uint64_t now_us = monotonic_time_us();
  return add_json_int(response, "job_id", (int64_t)snapshot->id) &&
         add_json_string(response, "operation",
                         snapshot->id == 0
                             ? ""
                             : game_storage_operation_name(
                                   snapshot->operation)) &&
         add_json_string(response, "state",
                         storage_job_state_name(snapshot->state)) &&
         add_json_bool(response, "active", snapshot->active) &&
         add_json_bool(response, "cancellable",
                       snapshot->active &&
                           storage_job_is_cancellable(snapshot->state)) &&
         add_json_bool(response, "cancel_requested",
                       snapshot->cancel_requested) &&
         add_json_string(response, "title_id", snapshot->title_id) &&
         add_json_string(response, "title_name", snapshot->title_name) &&
         add_json_int(response, "native_status", snapshot->native_status) &&
         add_json_int(response, "queue_count", (int64_t)snapshot->pkg_queue_count) &&
         add_json_int(response, "queue_completed", (int64_t)snapshot->pkg_queue_completed) &&
         add_json_int(response, "queue_total", (int64_t)snapshot->pkg_queue_total) &&
         add_json_bool(response, "queue_running", snapshot->pkg_queue_running) &&
         add_json_string(response, "package_kind", sm_pkg_kind_name(snapshot->pkg_current_kind)) &&
         add_json_string(response, "source_type", snapshot->source_type) &&
         add_json_string(response, "source", snapshot->source) &&
         add_json_string(response, "runtime_source",
                         snapshot->runtime_source) &&
         add_json_string(response, "destination", snapshot->destination) &&
         add_json_bool(response, "delete_source", snapshot->delete_source) &&
         add_json_int(response, "total_bytes",
                      (int64_t)snapshot->total_bytes) &&
         add_json_int(response, "processed_bytes",
                      (int64_t)snapshot->processed_bytes) &&
         add_json_int(response, "total_files",
                      (int64_t)snapshot->total_files) &&
         add_json_int(response, "processed_files",
                      (int64_t)snapshot->processed_files) &&
         add_json_double(response, "progress_percent",
                         storage_job_progress_percent(snapshot)) &&
         add_json_int(response, "speed_bytes_per_second",
                      (int64_t)storage_job_speed_bytes_per_second(snapshot,
                                                                  now_us)) &&
         add_json_int(response, "elapsed_ms",
                      (int64_t)storage_job_elapsed_ms(snapshot, now_us)) &&
         add_json_int(response, "affected_titles",
                      (int64_t)snapshot->affected_titles) &&
         add_json_int(response, "result_status", snapshot->result_status) &&
         add_json_string(response, "result_error",
                         snapshot->result_status == 0
                             ? ""
                             : operation_error_message(snapshot->result_status,
                                                       snapshot->result_error_reason)) &&
         add_json_string(response, "result_error_reason",
                         snapshot->result_error_reason) &&
         add_json_string(response, "blocking_source", snapshot->usb_blocker.source) &&
         add_json_string(response, "blocking_target", snapshot->usb_blocker.target) &&
         add_json_bool(response, "scan_queued", snapshot->scan_queued);
}

static void send_storage_job_response(
    struct MHD_Connection *connection, int http_status,
    const storage_job_snapshot_t *snapshot) {
  struct json_object *response = new_status_response(0);
  if (!response || !add_storage_job_fields(response, snapshot)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  (void)send_json_object(connection, http_status, response);
  json_object_put(response);
}

static bool storage_job_cancelled(void *ctx) {
  uint64_t job_id = (uint64_t)(uintptr_t)ctx;
  pthread_mutex_lock(&g_storage_job.mutex);
  bool cancelled = g_storage_job.id != job_id ||
                   g_storage_job.cancel_requested;
  pthread_mutex_unlock(&g_storage_job.mutex);
  return cancelled;
}

static void update_storage_job_progress(uint64_t bytes_delta,
                                        uint64_t files_delta, void *ctx) {
  uint64_t job_id = (uint64_t)(uintptr_t)ctx;
  pthread_mutex_lock(&g_storage_job.mutex);
  if (storage_job_is_active(g_storage_job.state) &&
      g_storage_job.id == job_id) {
    if (UINT64_MAX - g_storage_job.processed_bytes < bytes_delta)
      g_storage_job.processed_bytes = UINT64_MAX;
    else
      g_storage_job.processed_bytes += bytes_delta;
    if (UINT64_MAX - g_storage_job.processed_files < files_delta)
      g_storage_job.processed_files = UINT64_MAX;
    else
      g_storage_job.processed_files += files_delta;
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
}

static bool begin_storage_job_finalizing(void *ctx) {
  uint64_t job_id = (uint64_t)(uintptr_t)ctx;
  pthread_mutex_lock(&g_storage_job.mutex);
  bool allowed = storage_job_is_active(g_storage_job.state) &&
                 g_storage_job.id == job_id &&
                 !g_storage_job.cancel_requested;
  if (allowed) {
    g_storage_job.state = STORAGE_JOB_FINALIZING;
    g_storage_job.transfer_finished_us = monotonic_time_us();
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
  return allowed;
}

static void finish_storage_job(uint64_t job_id, int status,
                               bool scan_queued, const char *error) {
  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.id == job_id) {
    if (status == 0) {
      g_storage_job.processed_bytes = g_storage_job.total_bytes;
      g_storage_job.processed_files = g_storage_job.total_files;
      g_storage_job.state = STORAGE_JOB_COMPLETED;
    } else if (status == ECANCELED) {
      g_storage_job.state = STORAGE_JOB_CANCELLED;
    } else {
      g_storage_job.state = STORAGE_JOB_FAILED;
    }
    g_storage_job.result_status = status;
    (void)strlcpy(g_storage_job.result_error_reason,
                  status == 0 || !error ? "" : error,
                  sizeof(g_storage_job.result_error_reason));
    g_storage_job.scan_queued = scan_queued;
    g_storage_job.finished_us = monotonic_time_us();
    pthread_cond_broadcast(&g_storage_job.cond);
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
}

static int storage_operation_result_status(game_storage_operation_t operation,
                                            const char *source, int result) {
  if (result == 0)
    return 0;
  int status = errno != 0 ? errno : EIO;
  if (operation == GAME_STORAGE_DELETE &&
      (status == ENOENT || status == ENOTDIR)) {
    struct stat st;
    // FTP or disconnected storage may remove the root after preflight. Only
    // an absent root is harmless; a missing child in an existing tree is not.
    if (lstat(source, &st) != 0 && (errno == ENOENT || errno == ENOTDIR))
      return 0;
  }
  return status;
}

static bool resolve_backport_source(const char *path, char title_id[MAX_TITLE_ID]) {
  title_id[0] = 0;
  const char *name = path ? strrchr(path, '/') : NULL;
  if (!name || strlen(++name) < 9u) return false;
  if (strncmp(name, "PPSA", 4u) && strncmp(name, "CUSA", 4u)) return false;
  for (size_t i = 4u; i < 9u; ++i)
    if (!isdigit((unsigned char)name[i])) return false;
  struct stat st;
  if (lstat(path, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
  char child[MAX_PATH];
  int written = snprintf(child, sizeof(child), "%s/fakelib", path);
  if (written < 0 || (size_t)written >= sizeof(child) ||
      lstat(child, &st) != 0 || !S_ISDIR(st.st_mode)) return false;
  written = snprintf(child, sizeof(child), "%s/sce_sys/keystone", path);
  if (written < 0 || (size_t)written >= sizeof(child) ||
      lstat(child, &st) == 0 || errno != ENOENT) return false;
  memcpy(title_id, name, 9u);
  title_id[9] = 0;
  return true;
}

static int build_backport_destination(const char *source, char destination[MAX_PATH]) {
  const char *name = strrchr(source, '/');
  if (!name || !name[1]) return EINVAL;
  int written = snprintf(destination, MAX_PATH, "%s/%s", INSTALLED_BACKPORTS_PATH, name + 1);
  if (written < 0 || written >= MAX_PATH) return ENAMETOOLONG;
  struct stat st;
  if (lstat(destination, &st) == 0) return S_ISDIR(st.st_mode) ? EEXIST : ENOTDIR;
  if (path_matches_root_or_child(destination, source)) return EINVAL;
  return errno == ENOENT ? 0 : (errno ? errno : EIO);
}

static bool read_install_source_info(const char *path, const struct stat *st,
                                      game_source_info_t *source,
                                      char title_id[MAX_TITLE_ID],
                                      char title_name[MAX_TITLE_NAME]) {
  title_id[0] = title_name[0] = 0;
  source->source_type = NULL;
  if (S_ISREG(st->st_mode) && st->st_size > 0 &&
      is_supported_image_file_path(path, get_filename_component(path))) {
    source->source_type = "image";
    strlcpy(title_name, get_filename_component(path), MAX_TITLE_NAME);
  } else if (S_ISDIR(st->st_mode)) {
    if (sm_smb_path(path)) {
      sm_pkg_info_t info;
      char executable[MAX_PATH];
      struct stat executable_stat;
      bool has_executable = false;
      const char *names[] = {"eboot.bin", "eboot2.bin"};
      for (size_t i=0;i<2 && !has_executable;++i) {
        int size=snprintf(executable,sizeof(executable),"%s/%s",path,names[i]);
        if (size > 0 && (size_t)size < sizeof(executable) &&
            lstat(executable,&executable_stat) == 0 && S_ISREG(executable_stat.st_mode)) has_executable=true;
      }
      if (!has_executable || !sm_pkg_read_directory_metadata(path,&info) || !is_supported_game_title_id(info.title_id)) return false;
      strlcpy(title_id,info.title_id,MAX_TITLE_ID);
      strlcpy(title_name,info.title_name[0] ? info.title_name : info.title_id,MAX_TITLE_NAME);
      source->source_type="folder";
      if(path != source->physical_path) strlcpy(source->physical_path,path,sizeof(source->physical_path));
      return true;
    }
    struct stat param;
    char backport_title[MAX_TITLE_ID];
    if (resolve_backport_source(path, backport_title) ||
        !directory_has_game_param(path, &param) || !directory_has_game_executable(path) ||
        !get_game_info(path, &param, title_id, title_name)) return false;
    source->source_type = "folder";
  } else return false;
  if (path != source->physical_path)
    strlcpy(source->physical_path, path, sizeof(source->physical_path));
  return true;
}

// Compare whole da units, not da1 against da10 or partition text prefixes.
static size_t usb_disk_source_length(const char *source) {
  if (strncmp(source, "/dev/da", 7) || !isdigit((unsigned char)source[7]))
    return 0;
  size_t size = 8;
  while (isdigit((unsigned char)source[size]))
    ++size;
  return source[size] == '\0' || source[size] == 'p' || source[size] == 's'
             ? size : 0;
}

static bool usb_shadowmount_title_mount(const struct statfs *mount) {
  static const char prefix[] = "/system_ex/app/";
  const char *target = mount->f_mntonname;
  size_t prefix_size = sizeof(prefix) - 1;
  if (strcmp(mount->f_fstypename, "nullfs") ||
      strncmp(target, prefix, prefix_size) ||
      !is_supported_game_title_id(target + prefix_size))
    return false;
  // Protect layers owned by ShadowMount using its persisted title registry.
  // AutoMounter/LncService own the application and child sandbox registries;
  // their USB mappings must reach the native eject request without guessing
  // ownership from NPXS/CUSA/PPSA names or sandbox directory layouts.
  char source[MAX_PATH];
  return read_mount_link(target + prefix_size, source, sizeof(source));
}

static int check_usb_eject_mount(const char *path, const char *source,
                                  uint64_t mount_id, bool check_dependencies,
                                  usb_eject_blocker_t *blocker) {
  if (blocker)
    memset(blocker, 0, sizeof(*blocker));
  if (!check_dependencies)
    return sm_usb_check_mount(path, source, mount_id);
  struct statfs *mounts = NULL;
  int count = sm_mount_table_snapshot(&mounts);
  if (count < 0)
    return errno ? errno : EIO;
  bool found = false;
  int status = 0;
  size_t disk_size = usb_disk_source_length(source);
  for (int i = 0; i < count; ++i) {
    const struct statfs *mount = &mounts[i];
    if (!strcmp(mount->f_mntonname, path)) {
      found = true;
      if (!sm_usb_mount_matches(mount, path, source, mount_id))
        status = ESTALE;
      break;
    }
  }
  if (!found)
    status = ENOENT;
  for (int i = 0; i < count && !status; ++i) {
    const struct statfs *mount = &mounts[i];
    if (!strcmp(mount->f_mntonname, path))
      continue;
    if ((path_matches_root_or_child(mount->f_mntfromname, path) &&
         usb_shadowmount_title_mount(mount)) ||
        (disk_size && usb_disk_source_length(mount->f_mntfromname) == disk_size &&
         !strncmp(mount->f_mntfromname, source, disk_size))) {
      status = EBUSY;
      if (blocker) {
        strlcpy(blocker->source, mount->f_mntfromname, sizeof(blocker->source));
        strlcpy(blocker->target, mount->f_mntonname, sizeof(blocker->target));
      }
    }
  }
  free(mounts);
  if (status)
    return status;
  return 0;
}

static const char *usb_eject_block_reason(const char *path) {
  const char *reason = runtime_mutation_block_reason();
  if (reason)
    return reason;
  if (sm_shellcore_source_is_protected(path))
    return "game_active";
  if (sm_shellcore_background_work_blocked())
    return "runtime_prepared";
  char title_id[MAX_TITLE_ID], runtime_path[MAX_PATH];
  if (!sm_game_lifecycle_background_title(title_id))
    return "game_active";
  if (title_id[0] && !read_mount_link(title_id, runtime_path, sizeof(runtime_path))) {
    // Native PKGs have no mount.lnk. Resolve their installed storage before
    // allowing an unrelated volume to be ejected; unknown storage fails closed.
    sm_game_cache_snapshot_entry_t entry;
    if (!find_installed_pkg_game(title_id, &entry))
      return "game_active";
    if (paths_overlap(entry.path, path))
      return "game_active";
    // A native patch or DLC can live on a different volume from the base.
    static const char *const trees[] = {
        "user/app", "ps5/user/app", "user/patch", "ps5/user/patch",
        "user/addcont", "ps5/user/addcont"};
    for (size_t i = 0; i < sizeof(trees) / sizeof(trees[0]); ++i) {
      char content[MAX_PATH];
      int length = snprintf(content, sizeof(content), "%s/%s/%s", path,
                            trees[i], title_id);
      struct stat st;
      if (length < 0 || (size_t)length >= sizeof(content) ||
          stat(content, &st) == 0 || (errno != ENOENT && errno != ENOTDIR))
        return "game_active";
    }
  }
  return NULL;
}

static const char *usb_mount_error_reason(int status,
                                            const usb_eject_blocker_t *blocker) {
  if (status == EBUSY) {
    if (!blocker || !blocker->source[0])
      return "usb_disk_busy";
    return usb_disk_source_length(blocker->source)
               ? "usb_partition_busy" : "usb_mount_busy";
  }
  if (status == ESTALE || status == ENOENT)
    return "usb_mount_changed";
  return NULL;
}

static void run_usb_eject_job(uint64_t job_id, const char *path,
                               const char *source, uint64_t mount_id) {
  void *ctx = (void *)(uintptr_t)job_id;
  int status = 0;
  const char *error = NULL;
  usb_eject_blocker_t blocker = {0};
  bool scanner = false, shellcore = false, paused = false;
  if (storage_job_cancelled(ctx))
    status = ECANCELED;
  else if (!(scanner = sm_scanner_try_begin_external_mutation())) {
    status = EBUSY;
    error = "scanner_busy";
  } else if (!(shellcore = sm_shellcore_try_begin_external_mutation())) {
    status = EBUSY;
    error = "runtime_busy";
  }
  if (!status) {
    error = usb_eject_block_reason(path);
    if (error)
      status = EBUSY;
  }
  if (!status) {
    status = check_usb_eject_mount(path, source, mount_id, true, &blocker);
    error = usb_mount_error_reason(status, &blocker);
  }
  if (!status && image_cache_find_active_source_under(path, blocker.source,
                                                       sizeof(blocker.source))) {
    status = EBUSY;
    error = "usb_image_busy";
  }
  if (!status && !begin_storage_job_finalizing(ctx))
    status = ECANCELED;
  if (!status && !(paused = sm_scanner_suspend_usb_for_eject())) {
    status = EBUSY;
    error = "scanner_busy";
  }
  if (!status && (should_stop_requested() || runtime_sleep_mode_active())) {
    status = ECANCELED;
  }
  if (!status && (error = usb_eject_block_reason(path)) != NULL)
    status = EBUSY;
  if (!status) {
    // Recheck after the watcher handshake, before resolving a live device ID.
    status = check_usb_eject_mount(path, source, mount_id, true, &blocker);
    error = usb_mount_error_reason(status, &blocker);
  }
  if (!status) {
    int32_t native_status = 0;
    status = sm_usb_request_eject(path, source, mount_id, &native_status);
    if (status)
      error = status == ESTALE || status == ENOENT
                  ? "usb_mount_changed" : "usb_eject_failed";
    // A successful request can enqueue an extended-storage operation. Only
    // publish completed after the kernel mount has actually disappeared.
    for (unsigned i = 0; !status && i < 50; ++i) {
      int mounted = check_usb_eject_mount(path, source, mount_id, false, NULL);
      if (mounted == ENOENT)
        break;
      if (mounted || i == 49 || sleep_with_stop_check(100000)) {
        status = mounted ? mounted : ETIMEDOUT;
        error = "usb_eject_unconfirmed";
      }
    }
  }
  if (paused)
    sm_scanner_resume_usb_after_eject();
  if (shellcore)
    sm_shellcore_end_external_mutation();
  if (scanner)
    sm_scanner_end_external_mutation();
  log_debug("  [API] USB eject path=%s status=%d reason=%s", path, status,
            status ? operation_error_message(status, error) : "complete");
  if (status && blocker.source[0]) {
    log_debug("  [API] USB eject blocked by source=%s target=%s",
              blocker.source, blocker.target);
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.id == job_id)
      g_storage_job.usb_blocker = blocker;
    pthread_mutex_unlock(&g_storage_job.mutex);
  }
  finish_storage_job(job_id, status, false, error);
}

static void *storage_job_thread_main(void *arg) {
  uint64_t job_id = (uint64_t)(uintptr_t)arg;
  char title_id[MAX_TITLE_ID];
  char source[MAX_PATH];
  char runtime_source[MAX_PATH];
  char destination[MAX_PATH];
  bool delete_source;
  bool backport;
  bool browser_delete;
  game_storage_operation_t operation;
  uint64_t usb_mount_id;

  pthread_mutex_lock(&g_storage_job.mutex);
  (void)strlcpy(title_id, g_storage_job.title_id, sizeof(title_id));
  (void)strlcpy(source, g_storage_job.source, sizeof(source));
  (void)strlcpy(runtime_source, g_storage_job.runtime_source,
                sizeof(runtime_source));
  (void)strlcpy(destination, g_storage_job.destination,
                sizeof(destination));
  delete_source = g_storage_job.delete_source;
  operation = g_storage_job.operation;
  usb_mount_id = g_storage_job.usb_mount_id;
  backport = strcmp(g_storage_job.source_type, "backport") == 0;
  browser_delete = operation == GAME_STORAGE_DELETE && !title_id[0];
  pthread_mutex_unlock(&g_storage_job.mutex);

  if (operation == GAME_STORAGE_EJECT) {
    run_usb_eject_job(job_id, source, runtime_source, usb_mount_id);
    return NULL;
  }

  uint64_t total_bytes = 0;
  uint64_t total_files = 0;
  int status = 0;
  const char *error = NULL;
  bool scanner_mutation = false;
  bool shellcore_mutation = false;
  bool unpack_mounted = false;
  bool recovery_scan_needed = false;
  void *job_ctx = (void *)(uintptr_t)job_id;

  // pthread mutex ownership cannot be transferred from the HTTP request
  // thread. The storage worker owns both mutation gates for the whole job.
  if (storage_job_cancelled(job_ctx)) {
    status = ECANCELED;
  } else if (!sm_scanner_try_begin_external_mutation()) {
    status = EBUSY;
    error = "scanner_busy";
  } else {
    scanner_mutation = true;
  }
  if (status == 0 && operation == GAME_STORAGE_UNPACK) {
    const bool read_only = true;
    status = sm_shellcore_mount_title_runtime(title_id, &read_only);
    if (status == 0) {
      unpack_mounted = true;
      recovery_scan_needed = true;
    }
  }
  if (status == 0) {
    if (!sm_shellcore_try_begin_external_mutation()) {
      status = EBUSY;
      error = "runtime_busy";
    } else
      shellcore_mutation = true;
  }
  if (status == 0 && storage_job_cancelled(job_ctx))
    status = ECANCELED;

  if (status == 0 &&
      (sm_shellcore_title_is_protected(title_id) ||
       sm_shellcore_source_is_protected(source) ||
       sm_shellcore_source_is_protected(destination))) {
    status = EBUSY;
    error = "game_active";
  }

  if (status == 0) {
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.id != job_id || g_storage_job.cancel_requested)
      status = ECANCELED;
    else
      g_storage_job.state = STORAGE_JOB_MEASURING;
    pthread_mutex_unlock(&g_storage_job.mutex);
  }

  if (status == 0 && backport) {
    char checked_title[MAX_TITLE_ID];
    if (!resolve_backport_source(source, checked_title) || strcmp(title_id, checked_title))
      status = EINVAL;
    const char *roots[] = {DEFAULT_BACKPORT_SCAN_PATH, INSTALLED_BACKPORTS_PATH};
    for (size_t i = 0; status == 0 && operation != GAME_STORAGE_DELETE &&
                       i < sizeof(roots) / sizeof(roots[0]); ++i) {
      struct stat st;
      if (mkdir(roots[i], 0777) != 0 && errno != EEXIST) status = errno ? errno : EIO;
      else if (lstat(roots[i], &st) != 0) status = errno ? errno : EIO;
      else if (!S_ISDIR(st.st_mode)) status = EINVAL;
    }
  }

  if (status == 0 && operation == GAME_STORAGE_INSTALL) {
    struct stat st;
    game_source_info_t checked = {0};
    char checked_title[MAX_TITLE_ID], checked_name[MAX_TITLE_NAME];
    if (lstat(source, &st) != 0) status = errno ? errno : EIO;
    else if (!read_install_source_info(source, &st, &checked, checked_title, checked_name) ||
             strcmp(title_id, checked_title)) status = EINVAL;
  }

  if (status == 0 && browser_delete) {
    game_source_info_t checked = {0};
    status = resolve_browser_delete_source(source, &checked);
    if (!status && !sm_smb_path(source)) {
      size_t affected = 0;
      status = release_browser_source_runtime(source, &affected, &error);
      recovery_scan_needed = affected > 0 || status != 0;
      pthread_mutex_lock(&g_storage_job.mutex);
      g_storage_job.affected_titles = affected;
      pthread_mutex_unlock(&g_storage_job.mutex);
    }
  }

  if (status == 0 && operation != GAME_STORAGE_UNPACK && operation != GAME_STORAGE_INSTALL && !backport && !browser_delete) {
    size_t affected = 0;
    recovery_scan_needed = true;
    status = release_storage_source_runtime(source, title_id, &affected,
                                             &error);
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.id == job_id)
      g_storage_job.affected_titles = affected;
    pthread_mutex_unlock(&g_storage_job.mutex);
  }

  if (status == 0 && operation == GAME_STORAGE_UNPACK) {
    size_t affected = 0;
    status = count_titles_for_source(source, &affected);
    if (status == 0 && delete_source && affected > 1u)
      status = EBUSY;
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.id == job_id)
      g_storage_job.affected_titles = affected;
    pthread_mutex_unlock(&g_storage_job.mutex);
    if (status == 0 && !path_exists(runtime_source))
      status = ENOENT;
  }

  if (status == 0) {
    const char *measure_source = operation == GAME_STORAGE_UNPACK
                                     ? runtime_source
                                     : source;
    int measure_result = operation == GAME_STORAGE_DELETE
                             ? sm_storage_measure_delete_path_progress(
                                   source, &total_bytes, &total_files,
                                   storage_job_cancelled, job_ctx)
                             : sm_storage_measure_path_progress(
                                   measure_source, &total_bytes, &total_files,
                                   storage_job_cancelled, job_ctx);
    status = storage_operation_result_status(operation, source, measure_result);
  }

  if (status == 0 && browser_delete) {
    game_source_info_t checked = {0};
    status = resolve_browser_delete_source(source, &checked);
  }

  if (status == 0) {
    bool runtime_cancelled =
        should_stop_requested() || runtime_sleep_mode_active();
    pthread_mutex_lock(&g_storage_job.mutex);
    if (operation == GAME_STORAGE_DELETE &&
        (runtime_cancelled || g_storage_job.cancel_requested)) {
      status = ECANCELED;
    } else {
      g_storage_job.total_bytes = total_bytes;
      g_storage_job.total_files = total_files;
      g_storage_job.transfer_started_us = monotonic_time_us();
      g_storage_job.state = operation == GAME_STORAGE_DELETE
                                ? STORAGE_JOB_DELETING
                                : STORAGE_JOB_TRANSFERRING;
    }
    pthread_mutex_unlock(&g_storage_job.mutex);
  }

  if (status == 0) {
    bool renamed = false;
    const char *copy_source = operation == GAME_STORAGE_UNPACK
                                  ? runtime_source
                                  : source;
    int result = operation == GAME_STORAGE_DELETE
                     ? sm_storage_delete_path_progress(
                           source, update_storage_job_progress, job_ctx)
                 : operation == GAME_STORAGE_MOVE
                     ? sm_storage_move_path_progress(
                           source, destination, update_storage_job_progress,
                           storage_job_cancelled,
                           begin_storage_job_finalizing, job_ctx, &renamed)
                 : backport
                     ? sm_storage_replace_directory_progress(
                           source, destination, update_storage_job_progress,
                           storage_job_cancelled, begin_storage_job_finalizing, job_ctx)
                     : sm_storage_copy_path_progress(
                           copy_source, destination,
                           update_storage_job_progress,
                           storage_job_cancelled, job_ctx);
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.transfer_finished_us == 0)
      g_storage_job.transfer_finished_us = monotonic_time_us();
    pthread_mutex_unlock(&g_storage_job.mutex);
    status = storage_operation_result_status(operation, source, result);
    if (status == 0) {
      if (renamed) {
        pthread_mutex_lock(&g_storage_job.mutex);
        g_storage_job.processed_bytes = total_bytes;
        g_storage_job.processed_files = total_files;
        pthread_mutex_unlock(&g_storage_job.mutex);
      }
      if (operation == GAME_STORAGE_MOVE)
        update_moved_source_links(source, destination);
    }
  }

  if (status == 0 && (operation == GAME_STORAGE_UNPACK || operation == GAME_STORAGE_INSTALL) &&
      !begin_storage_job_finalizing(job_ctx)) {
    status = ECANCELED;
    if (sm_storage_delete_path(destination) != 0 && errno != ENOENT)
      status = errno != 0 ? errno : EIO;
  }

  if (unpack_mounted) {
    if (!sm_shellcore_release_title_runtime(title_id) && status == 0)
      status = errno == EBUSY ? EBUSY : EIO;
  }
  if (status == 0 && operation == GAME_STORAGE_UNPACK && delete_source &&
      sm_storage_delete_path(source) != 0) {
    status = errno != 0 ? errno : EIO;
  }

  if (status == 0 && operation == GAME_STORAGE_INSTALL) {
    bool changed = false;
    recovery_scan_needed = true;
    if (!sm_manual_add_path(destination, &changed)) status = errno ? errno : EIO;
  }

  if (shellcore_mutation)
    sm_shellcore_end_external_mutation();
  if (scanner_mutation)
    sm_scanner_end_external_mutation();

  bool scan_queued = false;
  if (recovery_scan_needed) {
    scan_queued = request_scan_now(
        status != 0
            ? "HTTP API async storage operation recovery"
            : operation == GAME_STORAGE_INSTALL
                  ? "HTTP API game source installed"
            : operation == GAME_STORAGE_MOVE
                  ? "HTTP API async game source moved"
                  : operation == GAME_STORAGE_COPY
                        ? "HTTP API async game source copied"
                        : operation == GAME_STORAGE_UNPACK
                              ? "HTTP API game image unpacked"
                              : "HTTP API async game source deleted");
  }
  log_debug("  [API] async game source %s %s: job=%llu title=%s status=%d reason=%s",
            game_storage_operation_name(operation),
            status == 0 ? "complete" : "stopped",
            (unsigned long long)job_id, title_id, status,
            status == 0 ? "complete" : operation_error_message(status, error));
  finish_storage_job(job_id, status, scan_queued, error);
  return NULL;
}

static int launch_storage_job(const char *title_id,
                              const game_source_info_t *source_info,
                              const char *destination,
                              game_storage_operation_t operation,
                              bool delete_source,
                              storage_job_snapshot_t *accepted_out,
                              const char **error_out) {
  pthread_mutex_lock(&g_storage_job.mutex);
  if (!g_storage_job.accepting || storage_job_is_active(g_storage_job.state) || g_storage_job.pkg_queue_running ||
      (operation == GAME_STORAGE_DELETE && !title_id[0] && g_storage_job.pkg_queue_count)) {
    *error_out = g_storage_job.accepting
                     ? "storage_busy"
                     : "storage_unavailable";
    pthread_mutex_unlock(&g_storage_job.mutex);
    return EBUSY;
  }
  uint64_t job_id = ++g_storage_job.next_id;
  if (job_id == 0)
    job_id = ++g_storage_job.next_id;
  g_storage_job.cancel_requested = false;
  g_storage_job.id = job_id;
  g_storage_job.title_name[0] = 0;
  g_storage_job.native_status = 0;
  g_storage_job.pkg_speed = g_storage_job.pkg_sample_us = 0;
  g_storage_job.pkg_current_kind = PKG_KIND_UNKNOWN;
  g_storage_job.operation = operation;
  g_storage_job.usb_mount_id = operation == GAME_STORAGE_EJECT
                                  ? source_info->usb_mount_id : 0;
  g_storage_job.state = STORAGE_JOB_PREPARING;
  (void)strlcpy(g_storage_job.title_id, title_id,
                sizeof(g_storage_job.title_id));
  (void)strlcpy(g_storage_job.source_type, source_info->source_type,
                sizeof(g_storage_job.source_type));
  (void)strlcpy(g_storage_job.source, source_info->physical_path,
                sizeof(g_storage_job.source));
  (void)strlcpy(g_storage_job.runtime_source, source_info->runtime_path,
                sizeof(g_storage_job.runtime_source));
  (void)strlcpy(g_storage_job.destination, destination,
                sizeof(g_storage_job.destination));
  g_storage_job.delete_source =
      operation == GAME_STORAGE_UNPACK && delete_source;
  g_storage_job.total_bytes = 0;
  g_storage_job.processed_bytes = 0;
  g_storage_job.total_files = 0;
  g_storage_job.processed_files = 0;
  g_storage_job.started_us = monotonic_time_us();
  g_storage_job.transfer_started_us = 0;
  g_storage_job.transfer_finished_us = 0;
  g_storage_job.finished_us = 0;
  g_storage_job.affected_titles = 0;
  g_storage_job.result_status = 0;
  g_storage_job.result_error_reason[0] = '\0';
  memset(&g_storage_job.usb_blocker, 0, sizeof(g_storage_job.usb_blocker));
  g_storage_job.scan_queued = false;
  copy_storage_job_snapshot_locked(accepted_out);
  pthread_mutex_unlock(&g_storage_job.mutex);

  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  bool attr_initialized = rc == 0;
  if (attr_initialized)
    rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  if (rc == 0)
    rc = pthread_create(&thread, &attr, storage_job_thread_main,
                        (void *)(uintptr_t)job_id);
  if (attr_initialized)
    (void)pthread_attr_destroy(&attr);
  if (rc != 0) {
    finish_storage_job(job_id, rc, false, NULL);
    return rc;
  }
  return 0;
}

static void handle_storage_eject(struct MHD_Connection *connection,
                                  struct json_object *request) {
  game_source_info_t source = {0};
  char mount_id[17];
  if (!smb_request_string(request, "path", source.physical_path,
                           sizeof(source.physical_path), true) ||
      !sm_usb_mount_path(source.physical_path) ||
      !smb_request_string(request, "source", source.runtime_path,
                           sizeof(source.runtime_path), true) ||
      !source.runtime_path[0] ||
      !smb_request_string(request, "mount_id", mount_id, sizeof(mount_id), true) ||
      strlen(mount_id) != 16 || strspn(mount_id, "0123456789abcdefABCDEF") != 16) {
    send_error_response(connection, 400, EINVAL, "invalid USB mount");
    return;
  }
  source.usb_mount_id = strtoull(mount_id, NULL, 16);
  int status = check_usb_eject_mount(source.physical_path, source.runtime_path,
                                     source.usb_mount_id, false, NULL);
  if (status) {
    send_operation_error_response(connection, status, usb_mount_error_reason(status, NULL));
    return;
  }
  source.source_type = "usb";
  storage_job_snapshot_t accepted;
  const char *error = NULL;
  status = launch_storage_job("", &source, "", GAME_STORAGE_EJECT, false,
                               &accepted, &error);
  if (status) {
    send_operation_error_response(connection, status, error);
    return;
  }
  send_storage_job_response(connection, 202, &accepted);
}

static int start_storage_job(const char *title_id,
                             const char *destination_dir,
                             game_storage_operation_t operation,
                             bool delete_source,
                             storage_job_snapshot_t *accepted_out,
                             const char **error_out) {
  game_source_info_t source_info;
  int status = resolve_storage_source(title_id, operation, &source_info,
                                      error_out);
  if (status != 0)
    return status;
  if (operation == GAME_STORAGE_UNPACK &&
      (!source_info.image_backed || is_data_mounted(title_id) ||
       sm_shellcore_service_title_is_prepared(title_id))) {
    if (source_info.image_backed)
      *error_out = "unpack_mounted";
    return source_info.image_backed ? EBUSY : ENOTSUP;
  }
  char destination[MAX_PATH];
  destination[0] = '\0';
  if (operation != GAME_STORAGE_DELETE) {
    int destination_status = operation == GAME_STORAGE_UNPACK
                                 ? build_unpack_destination(
                                       destination_dir, title_id, destination)
                                 : build_storage_destination(
                                       destination_dir,
                                       source_info.physical_path, destination);
    if (destination_status != 0)
      return destination_status;
  }
  if (sm_shellcore_title_is_protected(title_id) ||
      sm_shellcore_source_is_protected(source_info.physical_path) ||
      sm_shellcore_source_is_protected(destination)) {
    *error_out = "game_active";
    return EBUSY;
  }

  return launch_storage_job(title_id, &source_info, destination, operation,
                            delete_source, accepted_out, error_out);
}

static const char *package_string(struct json_object *request, const char *key) {
  struct json_object *value;
  if (!json_object_object_get_ex(request, key, &value) ||
      !json_object_is_type(value, json_type_string)) return NULL;
  const char *text = json_object_get_string(value);
  return strlen(text) == (size_t)json_object_get_string_len(value) ? text : NULL;
}

static bool package_root(const char *path, char root[MAX_PATH]) {
  if (sm_smb_path(path)) {
    char canonical[MAX_PATH];
    if (sm_smb_normalize(path,canonical)) return false;
    const char *slash=strchr(canonical+6,'/');
    size_t length=slash ? (size_t)(slash-canonical) : strlen(canonical);
    memcpy(root,canonical,length); root[length]=0; return true;
  }
  if (path_matches_root_or_child(path, "/data")) {
    strlcpy(root, "/data", MAX_PATH);
    return true;
  }
  for (int i = 0; i < 10; ++i) {
    snprintf(root, MAX_PATH, i < 8 ? "/mnt/usb%d" : "/mnt/ext%d", i < 8 ? i : i - 8);
    if (!path_matches_root_or_child(path, root)) continue;
    struct statfs fs;
    return statfs(root, &fs) == 0 && strcmp(fs.f_mntonname, root) == 0 && filesystem_has_capacity(&fs);
  }
  return false;
}

static int resolve_package_path(const char *path, char resolved[MAX_PATH], bool directory) {
  if (!path || (path[0] != '/' && !sm_smb_path(path)) || strlen(path) >= MAX_PATH) return EINVAL;
  if (sm_smb_path(path)) {
    int status = sm_smb_normalize(path, resolved);
    if (status) return status;
  } else {
    char canonical[PATH_MAX];
    if (!realpath(path, canonical)) return errno ? errno : EINVAL;
    if (strlcpy(resolved, canonical, MAX_PATH) >= MAX_PATH) return ENAMETOOLONG;
  }
  char root[MAX_PATH];
  if (!package_root(resolved, root) || sm_pkg_is_installed_content_path(resolved)) return EPERM;
  struct stat st;
  if (lstat(resolved, &st) != 0) return errno;
  if (directory ? !S_ISDIR(st.st_mode) : !S_ISREG(st.st_mode)) return EINVAL;
  return 0;
}

static void handle_source_install(struct MHD_Connection *connection, struct json_object *request) {
  const char *error = runtime_mutation_block_reason();
  if (error) { send_operation_error_response(connection, EBUSY, error); return; }
  const char *path = package_string(request, "path");
  const char *target = package_string(request, "destination_dir");
  struct stat st;
  int status = !path || (path[0] != '/' && !sm_smb_path(path)) || strlen(path) >= MAX_PATH ||
      !target || target[0] != '/' || strlen(target) >= MAX_PATH ? EINVAL : 0;
  if (!status && stat(path, &st) != 0) status = errno ? errno : EIO;
  game_source_info_t source = {0};
  char title_id[MAX_TITLE_ID] = "", title_name[MAX_TITLE_NAME] = "";
  char destination[MAX_PATH], resolved_dir[MAX_PATH];
  if (!status) status = resolve_package_path(path, source.physical_path, S_ISDIR(st.st_mode));
  if (!status && lstat(source.physical_path, &st) != 0) status = errno ? errno : EIO;
  if (!status && !read_install_source_info(source.physical_path, &st, &source, title_id, title_name)) status = EINVAL;
  if (!status) {
    status = resolve_package_path(target, resolved_dir, true);
    if (status == ENOENT && !strcmp(target, "/mnt/ext1/homebrew"))
      status = resolve_storage_destination_dir(target, resolved_dir);
  }
  if (!status && is_under_image_mount_base(resolved_dir)) status = EPERM;
  if (!status) status = build_storage_destination_path(resolved_dir, source.physical_path, destination);
  if (!status && (sm_shellcore_title_is_protected(title_id) ||
      sm_shellcore_source_is_protected(source.physical_path) || sm_shellcore_source_is_protected(destination))) {
    status = EBUSY; error = "game_active";
  }
  storage_job_snapshot_t accepted;
  if (!status) status = launch_storage_job(title_id, &source, destination, GAME_STORAGE_INSTALL, false, &accepted, &error);
  if (status) { send_operation_error_response(connection, status, error); return; }
  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.id == accepted.id) {
    strlcpy(g_storage_job.title_name, title_name, sizeof(g_storage_job.title_name));
    strlcpy(accepted.title_name, title_name, sizeof(accepted.title_name));
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
  send_storage_job_response(connection, 202, &accepted);
}

static bool browser_source_writable(const char *path, const struct stat *st) {
  char parent[MAX_PATH];
  if (strlcpy(parent, path, sizeof(parent)) >= sizeof(parent)) return false;
  char *slash = strrchr(parent, '/');
  if (!slash) return false;
  if (slash == parent) slash[1] = 0;
  else *slash = 0;
  struct stat parent_stat;
  struct statvfs filesystem;
  return (!S_ISDIR(st->st_mode) || (st->st_mode & 0222) != 0) &&
         stat(parent, &parent_stat) == 0 && (parent_stat.st_mode & 0222) != 0 &&
         access(parent, W_OK | X_OK) == 0 &&
         (!S_ISDIR(st->st_mode) || access(path, W_OK | X_OK) == 0) &&
         statvfs(parent, &filesystem) == 0 && (filesystem.f_flag & ST_RDONLY) == 0;
}

static bool browser_path_is_system(const char *path, bool include_ancestors) {
  static const char *const system_paths[] = {
      "/system", "/system_ex", "/system_data", "/user", "/preinst", "/update",
      LOG_DIR, IMAGE_MOUNT_BASE, "/mnt/ext0/user", "/mnt/ext1/user",
      "/mnt/ext0/ps5", "/mnt/ext1/ps5", "/mnt/ext0/system_data", "/mnt/ext1/system_data",
  };
  for (size_t i = 0; i < sizeof(system_paths) / sizeof(system_paths[0]); ++i)
    if (path_matches_root_or_child(path, system_paths[i]) ||
        (include_ancestors && path_matches_root_or_child(system_paths[i], path))) return true;
  return sm_pkg_is_installed_content_path(path);
}

static bool browser_directory_creatable(const char *path) {
  if (sm_smb_path(path)) return sm_vfs_mkdir_access(path) == 0;
  char root[MAX_PATH];
  struct stat st;
  struct statvfs filesystem;
  return package_root(path, root) && !browser_path_is_system(path, false) &&
         stat(path, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0222) != 0 &&
         access(path, W_OK | X_OK) == 0 && statvfs(path, &filesystem) == 0 &&
         (filesystem.f_flag & ST_RDONLY) == 0;
}

static bool browser_source_deletable(const char *path, const struct stat *st) {
  const char *name = get_filename_component(path);
  const char *extension = strrchr(name, '.');
  bool pkg = S_ISREG(st->st_mode) && extension && !strcasecmp(extension, ".pkg");
  bool directory = S_ISDIR(st->st_mode);
  if (!directory && !pkg && !(S_ISREG(st->st_mode) && is_supported_image_file_path(path, name))) return false;
  if (sm_smb_path(path)) return strchr(path + 6, '/') != NULL && sm_vfs_delete_access(path, directory) == 0;
  char root[MAX_PATH];
  if (!package_root(path, root) || !strcmp(path, root) || browser_path_is_system(path, directory)) return false;
  // PKGs may be removed from scan roots. Folders must not contain one either.
  if (pkg) return browser_source_writable(path, st);
  for (int i = 0; i < get_scan_path_count(); ++i) {
    char scan_path[MAX_PATH], canonical[PATH_MAX];
    if (!get_scan_path(i, scan_path)) return false;
    const char *scan = realpath(scan_path, canonical) ? canonical : scan_path;
    if (path_matches_root_or_child(path, scan) ||
        (directory && path_matches_root_or_child(scan, path))) return false;
  }
  return browser_source_writable(path, st);
}

static int resolve_browser_delete_source(const char *path, game_source_info_t *source) {
  if (!path || (!sm_smb_path(path) && path[0] != '/') || strlen(path) >= MAX_PATH) return EINVAL;
  char resolved[PATH_MAX];
  if (!realpath(path, resolved)) return errno ? errno : EINVAL;
  if (strlcpy(source->physical_path, resolved, sizeof(source->physical_path)) >= sizeof(source->physical_path)) return ENAMETOOLONG;
  struct stat st;
  if (lstat(source->physical_path, &st) != 0) return errno ? errno : EIO;
  if (!browser_source_deletable(source->physical_path, &st)) return EPERM;
  if (S_ISDIR(st.st_mode)) source->source_type = "folder";
  else if (S_ISREG(st.st_mode)) {
    const char *name = get_filename_component(source->physical_path);
    const char *extension = strrchr(name, '.');
    if (is_supported_image_file_path(source->physical_path, name)) source->source_type = "image";
    else if (extension && !strcasecmp(extension, ".pkg")) source->source_type = "pkg";
    else return EINVAL;
  } else return EINVAL;
  return 0;
}

static void handle_source_delete(struct MHD_Connection *connection, struct json_object *request) {
  struct json_object *confirm;
  if (!json_object_object_get_ex(request, "confirm", &confirm) ||
      !json_object_is_type(confirm, json_type_boolean) || !json_object_get_boolean(confirm)) {
    send_operation_error_response(connection, EINVAL, NULL);
    return;
  }
  const char *error = runtime_mutation_block_reason();
  if (error) { send_operation_error_response(connection, EBUSY, error); return; }
  game_source_info_t source = {0};
  int status = resolve_browser_delete_source(package_string(request, "path"), &source);
  storage_job_snapshot_t accepted;
  if (!status) status = launch_storage_job("", &source, "", GAME_STORAGE_DELETE, false, &accepted, &error);
  if (status) { send_operation_error_response(connection, status, error); return; }
  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.id == accepted.id) {
    strlcpy(g_storage_job.title_name, get_filename_component(source.physical_path), sizeof(g_storage_job.title_name));
    strlcpy(accepted.title_name, g_storage_job.title_name, sizeof(accepted.title_name));
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
  send_storage_job_response(connection, 202, &accepted);
}

static void handle_directory_create(struct MHD_Connection *connection, struct json_object *request) {
  const char *name = package_string(request, "name");
  if (!name || !name[0] || strlen(name) > 255 || !strcmp(name, ".") || !strcmp(name, "..") ||
      strpbrk(name, "/\\:*?\"<>|")) {
    send_operation_error_response(connection, EINVAL, NULL); return;
  }
  for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
    if (*p < 32 || *p == 127) { send_operation_error_response(connection, EINVAL, NULL); return; }
  const char *error = runtime_mutation_block_reason();
  int status = error ? EBUSY : 0;
  bool scanner_mutation = false, shellcore_mutation = false;
  char parent[MAX_PATH], child[MAX_PATH];
  if (!status) status = resolve_package_path(package_string(request, "path"), parent, true);
  if (!status && sm_smb_path(parent) && (name[strlen(name) - 1] == '.' || name[strlen(name) - 1] == ' ')) status = EINVAL;
  if (!status) {
    int length = snprintf(child, sizeof(child), "%s/%s", parent, name);
    if (length < 0 || (size_t)length >= sizeof(child)) status = ENAMETOOLONG;
  }
  if (!status) {
    pthread_mutex_lock(&g_storage_job.mutex);
    bool busy = !g_storage_job.accepting || storage_job_is_active(g_storage_job.state) || g_storage_job.pkg_queue_running;
    pthread_mutex_unlock(&g_storage_job.mutex);
    if (busy) { status = EBUSY; error = "storage_busy"; }
  }
  if (!status) {
    scanner_mutation = sm_scanner_try_begin_external_mutation();
    if (!scanner_mutation) { status = EBUSY; error = "scanner_busy"; }
  }
  if (!status) {
    shellcore_mutation = sm_shellcore_try_begin_external_mutation();
    if (!shellcore_mutation) { status = EBUSY; error = "runtime_busy"; }
  }
  // A launch may have completed between path resolution and taking the gate.
  if (!status) {
    error = runtime_mutation_block_reason();
    if (error) status = EBUSY;
    else if (sm_shellcore_source_is_protected(child)) {
      status = EBUSY; error = "game_active";
    }
  }
  if (!status && !browser_directory_creatable(parent))
    status = should_stop_requested() || runtime_sleep_mode_active() ? ECANCELED : EACCES;
  if (!status && (should_stop_requested() || runtime_sleep_mode_active())) status = ECANCELED;
  if (!status && mkdir(child, 0777) != 0) status = errno ? errno : EIO;
  if (shellcore_mutation) sm_shellcore_end_external_mutation();
  if (scanner_mutation) sm_scanner_end_external_mutation();
  if (status) { send_operation_error_response(connection, status, error); return; }
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_string(response, "path", child)) {
    if (response) json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
}

static void handle_backport_operation(struct MHD_Connection *connection, struct json_object *request,
                                      game_storage_operation_t operation) {
  const char *error = runtime_mutation_block_reason();
  if (error) { send_operation_error_response(connection, EBUSY, error); return; }
  game_source_info_t source = {.source_type = "backport"};
  int status = resolve_package_path(package_string(request, "path"), source.physical_path, true);
  char title_id[MAX_TITLE_ID], destination[MAX_PATH] = "";
  if (!status && !resolve_backport_source(source.physical_path, title_id)) status = EINVAL;
  if (!status && operation == GAME_STORAGE_COPY) {
    status = build_backport_destination(source.physical_path, destination);
    if (status == EEXIST) status = 0;
  }
  if (!status && operation == GAME_STORAGE_COPY &&
                (path_matches_root_or_child(destination, source.physical_path) ||
                 path_matches_root_or_child(source.physical_path, destination))) status = EINVAL;
  if (!status && (sm_shellcore_title_is_protected(title_id) ||
      sm_shellcore_source_is_protected(source.physical_path) || sm_shellcore_source_is_protected(destination))) {
    status = EBUSY; error = "game_active";
  }
  storage_job_snapshot_t accepted;
  if (!status) status = launch_storage_job(title_id, &source, destination, operation, false, &accepted, &error);
  if (status) { send_operation_error_response(connection, status, error); return; }
  struct json_object *response = new_status_response(0);
  if (!response || !add_storage_job_fields(response, &accepted)) {
    if (response) json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  send_json_object(connection, 202, response);
  json_object_put(response);
}

static bool add_package_info(struct json_object *item, const sm_pkg_info_t *info) {
  return add_json_string(item, "title_id", info->title_id) &&
    add_json_string(item, "title_name", info->title_name) &&
    add_json_string(item, "content_id", info->content_id) &&
    add_json_string(item, "version", info->app_ver) &&
    add_json_int(item, "sdk_version", info->sdk_version) &&
    add_json_string(item, "kind", sm_pkg_kind_name(info->pkg_kind)) &&
    add_json_bool(item, "replaceable", info->pkg_kind == PKG_KIND_BASE && sm_pkg_is_media_remaster(info));
}

static bool package_base_installed(const char *title_id, bool *pkg_installed_out) {
  bool installed = is_installed(title_id);
  if (installed && !pkg_installed_out) return true;
  sm_game_cache_snapshot_entry_t entry;
  bool pkg_installed = resolve_installed_pkg_game(title_id, NULL, &entry);
  if (pkg_installed_out) *pkg_installed_out = pkg_installed;
  return installed || pkg_installed;
}

static bool package_base_queued_locked(const char *title_id) {
  if (g_storage_job.operation == GAME_STORAGE_INSTALL &&
      (!strcmp(g_storage_job.source_type, "pkg") || !strcmp(g_storage_job.source_type, "stream")) &&
      storage_job_is_active(g_storage_job.state) &&
      g_storage_job.pkg_current_kind == PKG_KIND_BASE &&
      !strcmp(g_storage_job.title_id, title_id) && !g_storage_job.cancel_requested)
    return true;
  for (size_t i = 0; i < g_storage_job.pkg_queue_count; ++i)
    if (g_storage_job.pkg_queue[i].pkg_kind == PKG_KIND_BASE &&
        !strcmp(g_storage_job.pkg_queue[i].title_id, title_id)) return true;
  return false;
}

static int enqueue_package_locked(const sm_pkg_info_t *info, bool installed) {
  if (info->pkg_kind == PKG_KIND_BASE && installed && !sm_pkg_is_media_remaster(info)) return EALREADY;
  if (info->pkg_kind != PKG_KIND_BASE && !installed &&
      !package_base_queued_locked(info->title_id)) return ENOENT;
  if (g_storage_job.operation == GAME_STORAGE_INSTALL && storage_job_is_active(g_storage_job.state) &&
      !strcmp(g_storage_job.source, info->path)) return EEXIST;
  for (size_t i = 0; i < g_storage_job.pkg_queue_count; ++i)
    if (!strcmp(g_storage_job.pkg_queue[i].path, info->path) ||
        (g_storage_job.pkg_queue[i].pkg_kind == info->pkg_kind &&
         !strcmp(g_storage_job.pkg_queue[i].content_id, info->content_id) &&
         !strcmp(g_storage_job.pkg_queue[i].app_ver, info->app_ver))) return EEXIST;
  if (g_storage_job.pkg_queue_count == SM_PKG_QUEUE_MAX) return ENOSPC;
  g_storage_job.pkg_queue[g_storage_job.pkg_queue_count++] = *info;
  if (g_storage_job.pkg_queue_running) ++g_storage_job.pkg_queue_total;
  return 0;
}

static int clear_package_queue_locked(bool only_if_idle) {
  if (only_if_idle && (!g_storage_job.accepting || storage_job_is_active(g_storage_job.state) || g_storage_job.pkg_queue_running))
    return EBUSY;
  size_t keep = g_storage_job.pkg_queue_running ? 1 : 0;
  if (g_storage_job.pkg_queue_running) g_storage_job.pkg_queue_total -= g_storage_job.pkg_queue_count - keep;
  g_storage_job.pkg_queue_count = keep;
  return 0;
}

static bool prepare_next_package_locked(void) {
  if (!g_storage_job.pkg_queue_count) return false;
  const sm_pkg_info_t *next = &g_storage_job.pkg_queue[0];
  strlcpy(g_storage_job.source, next->path, sizeof(g_storage_job.source));
  strlcpy(g_storage_job.title_id, next->title_id, sizeof(g_storage_job.title_id));
  strlcpy(g_storage_job.title_name, next->title_name, sizeof(g_storage_job.title_name));
  g_storage_job.pkg_current_kind = next->pkg_kind;
  g_storage_job.total_bytes = next->source_size;
  g_storage_job.processed_bytes = g_storage_job.processed_files = 0;
  g_storage_job.transfer_started_us = g_storage_job.transfer_finished_us = 0;
  g_storage_job.pkg_speed = g_storage_job.pkg_sample_us = 0;
  g_storage_job.native_status = 0;
  g_storage_job.state = STORAGE_JOB_PREPARING;
  return true;
}

static bool add_package_icon_info(struct json_object *item, const sm_pkg_info_t *info) {
  char revision[100];
  snprintf(revision, sizeof(revision), "%llx-%llx-%x-%llx", (unsigned long long)info->source_inode,
      (unsigned long long)info->source_mtime, info->source_mtime_ns, (unsigned long long)info->source_size);
  return add_json_bool(item, "has_icon", info->icon_size != 0) && add_json_string(item, "icon_revision", revision);
}

static bool add_package_state(struct json_object *item, const sm_pkg_info_t *info) {
  bool pkg_installed;
  bool base = package_base_installed(info->title_id, &pkg_installed);
  pthread_mutex_lock(&g_storage_job.mutex);
  bool queued_base = package_base_queued_locked(info->title_id);
  pthread_mutex_unlock(&g_storage_job.mutex);
  return add_json_bool(item, "installed", info->pkg_kind == PKG_KIND_BASE && pkg_installed) &&
    add_json_bool(item, "base_installed", base) && add_json_bool(item, "base_queued", queued_base) &&
    add_package_icon_info(item, info);
}

static bool handle_package_icon(struct MHD_Connection *connection) {
  const char *path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
  const char *source_type = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "source_type");
  bool folder = source_type && !strcmp(source_type, "folder");
  char resolved[MAX_PATH];
  int status = source_type && !folder ? EINVAL : resolve_package_path(path, resolved, folder);
  unsigned char *bytes = NULL;
  size_t size = 0;
  if (!status) status = folder ? sm_pkg_read_directory_icon(resolved, &bytes, &size) : sm_pkg_read_icon(resolved, &bytes, &size);
  if (status) return send_error_response(connection, operation_http_status(status), status, "package icon is not available");
  struct MHD_Response *response = MHD_create_response_from_buffer(size, bytes, MHD_RESPMEM_MUST_FREE);
  if (!response) { free(bytes); return false; }
  bool ready = add_common_response_headers(response) &&
      MHD_add_response_header(response, MHD_HTTP_HEADER_CONTENT_TYPE, "image/png") == MHD_YES &&
      MHD_add_response_header(response, MHD_HTTP_HEADER_CACHE_CONTROL, "public, max-age=3600") == MHD_YES;
  bool queued = ready && MHD_queue_response(connection, 200, response) == MHD_YES;
  MHD_destroy_response(response);
  return queued;
}

static bool add_backport_info(struct json_object *item, const char *path,
                              const char *title_id, bool installed,
                              const sm_app_db_game_info_t *game) {
  char icon_url[SM_API_ROUTE_SIZE + MAX_TITLE_ID + 32u] = "";
  if (game) resolve_game_icon_url(title_id, game->icon_path, true, icon_url, sizeof(icon_url));
  char executable[MAX_PATH];
  int length = snprintf(executable, sizeof(executable), "%s/eboot.bin", path);
  uint32_t sdk = length > 0 && (size_t)length < sizeof(executable)
                     ? sm_pkg_read_executable_sdk(executable) : 0;
  return add_json_bool(item, "backport", true) &&
    add_json_bool(item, "backport_installed", installed) &&
    add_json_string(item, "title_id", title_id) &&
    add_json_string(item, "title_name", game ? game->title_name : "") &&
    add_json_string(item, "version", game ? game->version : "") &&
    add_json_string(item, "icon_url", icon_url) &&
    add_json_int(item, "sdk_version", sdk);
}

static bool add_folder_install_info(struct json_object *item, const char *path, const char *title_id) {
  sm_pkg_info_t info = {0};
  if (!sm_pkg_read_directory_metadata(path, &info) ||
      (info.title_id[0] && strcmp(info.title_id, title_id))) memset(&info, 0, sizeof(info));
  char icon[MAX_PATH], revision[32] = "";
  struct stat st;
  int length = snprintf(icon, sizeof(icon), "%s/sce_sys/icon0.png", path);
  bool has_icon = length > 0 && (size_t)length < sizeof(icon) && lstat(icon, &st) == 0 &&
      S_ISREG(st.st_mode) && st.st_size >= 8 && st.st_size <= SM_PKG_ICON_MAX;
  if (has_icon) snprintf(revision, sizeof(revision), "%llx", (unsigned long long)sm_icon_source_fingerprint(icon, &st));
  return add_json_string(item, "version", info.app_ver) && add_json_int(item, "sdk_version", info.sdk_version) &&
      add_json_bool(item, "has_icon", has_icon) && add_json_string(item, "icon_revision", revision);
}

static void handle_file_listing(struct MHD_Connection *connection, struct json_object *request, bool packages) {
  const char *path = package_string(request, "path");
  struct json_object *path_value;
  if (!path && json_object_object_get_ex(request, "path", &path_value)) {
    send_error_response(connection, 400, EINVAL, "path must be a string"); return;
  }
  size_t offset = 0;
  bool include_files = false;
  struct json_object *files_value;
  if (!packages && json_object_object_get_ex(request, "include_files", &files_value)) {
    if (!json_object_is_type(files_value, json_type_boolean)) {
      send_error_response(connection, 400, EINVAL, "include_files must be a boolean"); return;
    }
    include_files = json_object_get_boolean(files_value);
  }
  struct json_object *offset_value;
  if (json_object_object_get_ex(request, "offset", &offset_value)) {
    if (!json_object_is_type(offset_value, json_type_int) || json_object_get_int64(offset_value) < 0 ||
        json_object_get_int64(offset_value) > 1000000) {
      send_error_response(connection, 400, EINVAL, "invalid listing offset"); return;
    }
    offset = (size_t)json_object_get_int64(offset_value);
  }
  struct json_object *response = new_status_response(0), *roots = json_object_new_array(), *entries = json_object_new_array();
  sm_app_db_game_info_t *metadata = NULL;
  size_t metadata_count = 0;
  bool metadata_loaded = false;
  if (!response || !roots || !entries) goto oom;
  struct json_object *root_storage = json_object_new_array();
  if (!root_storage || !add_json_value(response, "root_storage", root_storage)) goto oom;
  for (int i = 0; i < 12; ++i) {
    char root[MAX_PATH], managed[MAX_PATH];
    if (i < 2) strlcpy(root, i == 0 ? "/data/pkgs" : "/data", sizeof(root));
    else snprintf(root, sizeof(root), i <= 9 ? "/mnt/usb%d" : "/mnt/ext%d", i <= 9 ? i - 2 : i - 10);
    struct stat st;
    if (package_root(root, managed) && stat(root, &st) == 0 && S_ISDIR(st.st_mode)) {
      if (!append_json_string(roots, root)) goto oom;
      struct statfs filesystem;
      if (statfs(root, &filesystem) == 0 && filesystem_has_capacity(&filesystem)) {
        struct json_object *item = filesystem_to_json(&filesystem);
        if (!item || !add_json_string(item, "path", root) || json_object_array_add(root_storage, item) != 0) {
          if (item) json_object_put(item);
          goto oom;
        }
      }
    }
  }
  size_t smb_root_offset = json_object_array_length(root_storage);
  if(!append_smb_paths(root_storage,true)) goto oom;
  for (size_t i = smb_root_offset; i < json_object_array_length(root_storage); ++i) {
    struct json_object *item = json_object_array_get_idx(root_storage, i);
    const char *root = json_object_get_string(json_object_object_get(item, "path"));
    if (!append_json_string(roots, root)) goto oom;
  }
  char resolved[MAX_PATH] = "", parent[MAX_PATH] = "";
  size_t page_limit = 256;
  bool truncated = false;
  bool can_create_directory = false;
  if (path && path[0]) {
    int status = resolve_package_path(path, resolved, true);
    if (status) {
      json_object_put(response); json_object_put(roots); json_object_put(entries);
      send_error_response(connection, operation_http_status(status), status, strerror(status)); return;
    }
    if(sm_smb_path(resolved)) page_limit=32;
    DIR *directory = opendir(resolved);
    if (!directory) {
      int error = errno;
      json_object_put(response); json_object_put(roots); json_object_put(entries);
      send_error_response(connection, operation_http_status(error), error, strerror(error)); return;
    }
    if (!packages) can_create_directory = browser_directory_creatable(resolved);
    struct dirent *entry;
    size_t count = 0;
    while (!api_listener_stop_requested(NULL) && (entry = readdir(directory))) {
      if (entry->d_name[0] == '.') continue;
      char child[MAX_PATH];
      int length = snprintf(child, sizeof(child), "%s/%s", resolved, entry->d_name);
      struct stat st;
      if (length < 0 || (size_t)length >= sizeof(child) || lstat(child, &st) != 0 ||
          sm_pkg_is_installed_content_path(child) || (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))) continue;
      bool folder = S_ISDIR(st.st_mode);
      size_t name_length = strlen(entry->d_name);
      bool image = packages && !folder && is_supported_image_file_path(child, entry->d_name);
      if (!folder && (packages ? (!image && (name_length < 4 || strcasecmp(entry->d_name + name_length - 4, ".pkg"))) : !include_files)) continue;
      if (count++ < offset) continue;
      if (count > offset + page_limit) { truncated = true; break; }
      sm_pkg_info_t info;
      bool valid = packages && !folder && sm_pkg_parse_file(child, &st, &info);
      char backport_title[MAX_TITLE_ID], backport_destination[MAX_PATH];
      bool backport = packages && folder && resolve_backport_source(child, backport_title);
      bool backport_installed = backport && build_backport_destination(child, backport_destination) == EEXIST;
      game_source_info_t game_source = {0};
      char game_title[MAX_TITLE_ID], game_name[MAX_TITLE_NAME];
      bool game = packages && !backport && (folder || image) &&
          read_install_source_info(child, &st, &game_source, game_title, game_name);
      if (backport && !metadata_loaded) {
        if (!app_db_game_info_snapshot(&metadata, &metadata_count)) {
          closedir(directory); goto oom;
        }
        metadata_loaded = true;
      }
      struct json_object *item = json_object_new_object();
      if (!item || !add_json_string(item, "name", entry->d_name) ||
          !add_json_string(item, "path", child) || !add_json_bool(item, "directory", folder) ||
          !add_json_bool(item, "can_delete", browser_source_deletable(child, &st)) ||
          !add_json_bool(item, "installable", valid || game) || !add_json_int(item, "size_bytes", folder ? 0 : st.st_size) ||
          (game && (!add_json_string(item, "source_type", game_source.source_type) ||
                    !add_json_string(item, "title_id", game_title) || !add_json_string(item, "title_name", game_name))) ||
          (game && folder && !add_folder_install_info(item, child, game_title)) ||
          (backport && !add_backport_info(item, child, backport_title, backport_installed,
                         app_db_find_game_info(metadata, metadata_count, backport_title))) ||
          (valid && (!add_package_info(item, &info) || !add_package_state(item, &info))) || json_object_array_add(entries, item) != 0) {
        if (item) json_object_put(item);
        closedir(directory); goto oom;
      }
    }
    #ifdef SM_VFS_ENABLED
    int directory_error=sm_vfs_dir_error(directory);
    #else
    int directory_error=0;
    #endif
    closedir(directory);
    if(directory_error) {
      json_object_put(response); json_object_put(roots); json_object_put(entries); free(metadata);
      send_error_response(connection,operation_http_status(directory_error),directory_error,strerror(directory_error)); return;
    }
    char root[MAX_PATH];
    if (package_root(resolved, root) && strcmp(root, resolved)) {
      strlcpy(parent, resolved, sizeof(parent));
      char *slash = strrchr(parent, '/');
      if (slash) *slash = 0;
    }
  }
  if (!add_json_string(response, "path", resolved) || !add_json_string(response, "parent", parent) ||
      !add_json_bool(response, "can_create_directory", can_create_directory) ||
      !add_json_bool(response, "truncated", truncated) || !add_json_int(response, "offset", offset) ||
      !add_json_int(response,"limit",page_limit)) goto oom;
  json_object_object_add(response, "roots", roots);
  json_object_object_add(response, "entries", entries);
  send_json_object(connection, 200, response);
  json_object_put(response);
  free(metadata);
  return;
oom:
  free(metadata);
  if (response) json_object_put(response);
  if (roots) json_object_put(roots);
  if (entries) json_object_put(entries);
  send_out_of_memory_response(connection);
}

typedef struct {
  uint64_t id, bytes, activity_us, source_check_us;
  uint64_t sample_us, sample_bytes, sample_total;
  bool native_progress;
  sm_pkg_server_t *server;
  sm_pkg_stream_t *stream;
  const sm_pkg_info_t *expected;
  int failure;
} pkg_job_context_t;

static void publish_package_progress(pkg_job_context_t *ctx, uint64_t total,
                                     uint64_t bytes, uint64_t now) {
  if (!total || total > INT64_MAX) return;
  if (bytes > total) bytes = total;
  // Native counters can briefly reset during delta promotion. Keep the last
  // usable display sample, not a false 0%.
  if (ctx->sample_bytes && (!bytes || (ctx->sample_total == total &&
                                      bytes < ctx->sample_bytes))) {
    pthread_mutex_lock(&g_storage_job.mutex);
    if (g_storage_job.id == ctx->id) {
      g_storage_job.pkg_speed = 0;
      g_storage_job.pkg_sample_us = now;
    }
    pthread_mutex_unlock(&g_storage_job.mutex);
    return;
  }
  uint64_t speed = 0;
  bool sample = ctx->sample_us && ctx->sample_total == total &&
                bytes >= ctx->sample_bytes && now > ctx->sample_us;
  if (sample && now - ctx->sample_us < 1000000) return;
  if (sample) {
    double rate = (double)(bytes - ctx->sample_bytes) * 1000000.0 / (double)(now - ctx->sample_us);
    speed = rate >= (double)INT64_MAX ? INT64_MAX : (uint64_t)rate;
  }
  if (sample && bytes > ctx->sample_bytes) ctx->activity_us = now;
  ctx->sample_us = now;
  ctx->sample_bytes = bytes;
  ctx->sample_total = total;
  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.id == ctx->id) {
    g_storage_job.total_bytes = total;
    g_storage_job.processed_bytes = bytes;
    g_storage_job.pkg_speed = speed;
    g_storage_job.pkg_sample_us = now;
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
}

static bool package_install_cancelled(void *opaque) {
  pkg_job_context_t *ctx = opaque;
  uint64_t now = monotonic_time_us();
  if (ctx->stream) {
    int status = sm_pkg_stream_status(ctx->stream);
    if (status && status != ECANCELED) ctx->failure = status;
  }
  if (ctx->expected && now >= ctx->source_check_us) {
    ctx->source_check_us = now + 1000000;
    struct stat st;
    bool matches=ctx->server ? sm_pkg_server_source_matches(ctx->server,ctx->expected) :
        lstat(ctx->expected->path,&st) == 0 && sm_pkg_source_matches(ctx->expected,&st);
    if (!matches)
      ctx->failure = ESTALE;
  }
  if (ctx->server) {
    uint64_t bytes = sm_pkg_server_bytes_served(ctx->server);
    if (bytes > ctx->bytes) {
      ctx->bytes = bytes;
      ctx->activity_us = now;
    }
    uint64_t request_time = sm_pkg_server_last_request_us(ctx->server);
    if (request_time > ctx->activity_us) ctx->activity_us = request_time;
  }
  if (now > ctx->activity_us && now - ctx->activity_us > 900ull * 1000000ull) ctx->failure = ETIMEDOUT;
  bool cancelled = storage_job_cancelled((void *)(uintptr_t)ctx->id) || should_stop_requested() || runtime_sleep_mode_active();
  if ((cancelled || ctx->failure) && ctx->stream) sm_pkg_stream_stop(ctx->stream);
  return cancelled || ctx->failure;
}

static int wait_for_package_poll(pkg_job_context_t *ctx) {
  for (int i = 0; i < 10; ++i) {
    if (package_install_cancelled(ctx))
      return ctx->failure ? ctx->failure : ECANCELED;
    usleep(100000);
  }
  return 0;
}

typedef struct {
  atomic_bool done;
  int result;
  sm_pkg_native_metadata_t metadata;
  sm_pkg_native_info_t info;
  sm_pkg_native_playgo_t playgo;
} pkg_submit_t;

static void *submit_native_package(void *opaque) {
  pkg_submit_t *submission = opaque;
  submission->result = sceAppInstUtilInstallByPackage(&submission->metadata, &submission->info, &submission->playgo);
  atomic_store(&submission->done, true);
  return NULL;
}

typedef struct {
  struct stat file;
  int status;
} pkg_result_snapshot_t;

static void snapshot_package_result(const char *title_id,
                                    pkg_result_snapshot_t result[2], char paths[2][MAX_PATH]) {
  memset(result, 0, sizeof(*result) * 2);
  memset(paths, 0, MAX_PATH * 2);
  sm_game_cache_snapshot_entry_t base;
  if (resolve_installed_pkg_game(title_id, NULL, &base))
    strlcpy(paths[0], base.path, MAX_PATH);
  else
    result[0].status = errno ? errno : EIO;
  result[1].status = resolve_game_patch_file(title_id, paths[1]);
  for (size_t i = 0; i < 2; ++i) {
    if (result[i].status) continue;
    if (lstat(paths[i], &result[i].file) != 0) result[i].status = errno ? errno : EIO;
    else if (!S_ISREG(result[i].file.st_mode) || result[i].file.st_size <= 0) result[i].status = EINVAL;
  }
}

static bool package_patch_result_installed(const sm_pkg_info_t *source,
                                           const pkg_result_snapshot_t before[2]) {
  pkg_result_snapshot_t current[2];
  char paths[2][MAX_PATH];
  snapshot_package_result(source->title_id, current, paths);
  for (size_t i = 0; i < 2; ++i) {
    if (current[i].status || (before[i].status && before[i].status != ENOENT && before[i].status != ENOTDIR)) continue;
    // Skip unchanged files before opening/parsing their package metadata.
    const struct stat *old = &before[i].file;
    sm_pkg_info_t installed = {
      .source_size = (uint64_t)old->st_size, .source_device = (uint64_t)old->st_dev,
      .source_inode = (uint64_t)old->st_ino, .source_ctime = (uint64_t)old->st_ctime,
      .source_mtime = old->st_mtime, .source_mtime_ns = (uint32_t)old->st_mtim.tv_nsec,
      .source_ctime_ns = (uint32_t)old->st_ctim.tv_nsec,
    };
    if (!before[i].status && sm_pkg_source_matches(&installed, &current[i].file)) continue;
    if (!sm_pkg_parse_file(paths[i], NULL, &installed) ||
        !sm_pkg_source_matches(&installed, &current[i].file) ||
        (installed.pkg_kind != PKG_KIND_BASE && installed.pkg_kind != PKG_KIND_PATCH) ||
        strcmp(installed.title_id, source->title_id) || strcmp(installed.content_id, source->content_id) ||
        (source->app_ver[0] && strcmp(installed.app_ver, source->app_ver))) continue;
    struct stat stable;
    if (lstat(paths[i], &stable) == 0 && sm_pkg_source_matches(&installed, &stable)) return true;
  }
  return false;
}

static void *package_install_thread(void *opaque) {
  uint64_t id = (uint64_t)(uintptr_t)opaque;
  bool scanner = false, shellcore = false;
  bool installed_any = false;
  for (;;) {
    char source[MAX_PATH], resolved[MAX_PATH], token[33];
    uint64_t size;
    pkg_job_context_t ctx = {.id = id, .activity_us = monotonic_time_us()};
    pthread_mutex_lock(&g_storage_job.mutex);
    strlcpy(source, g_storage_job.source, sizeof(source));
    strlcpy(token, g_storage_job.stream_token, sizeof(token));
    size = g_storage_job.total_bytes;
    ctx.stream = g_storage_job.pkg_stream;
    ctx.server = g_storage_job.pkg_server;
    uint16_t port = g_storage_job.pkg_server_port;
    pthread_mutex_unlock(&g_storage_job.mutex);
    int status = 0;
    int native_result = 0;
    sm_pkg_native_status_t native_status = {0};
    sm_pkg_native_status_t previous_general = {0};
    bool general_snapshot = false, replacement_observed = false;
    uint64_t patch_completed_total = 0;
    pkg_result_snapshot_t previous_result[2] = {0};
    pkg_submit_t submission = {0};
    atomic_init(&submission.done, false);
    bool submitted = false;
    pthread_t submitter;
    char uri[3072];
    const char *phase = "prepare";
    const char *error = runtime_mutation_block_reason();
    if (error) status = EBUSY;
    if (!status && package_install_cancelled(&ctx)) status = ECANCELED;
    if (!status && !scanner) {
      // Each browser file has its own job. Let the preceding file's requested
      // scan finish before submitting the next native install.
      uint64_t deadline = 0;
      while (!(scanner = sm_scanner_try_begin_external_mutation())) {
        uint64_t now = monotonic_time_us();
        if (!ctx.stream || (deadline && now >= deadline)) {
          status = EBUSY;
          error = "scanner_busy";
          break;
        }
        if (!now) { status = EIO; break; }
        if (!deadline) deadline = now + SM_API_PKG_START_TIMEOUT_US;
        status = wait_for_package_poll(&ctx);
        if (status) break;
      }
      if (!status) {
        error = runtime_mutation_block_reason();
        if (error) status = EBUSY;
      }
    }
    if (!status && !shellcore && !(shellcore = sm_shellcore_try_begin_external_mutation())) { status = EBUSY; error = "runtime_busy"; }
    sm_pkg_info_t info = {0};
    if (!status) {
      if (ctx.stream) {
        errno = 0;
        if (!sm_pkg_parse_stream(source, size, sm_pkg_stream_read, ctx.stream, &info)) status = errno ? errno : EINVAL;
      } else {
        status = resolve_package_path(source, resolved, false);
        if (!status && !sm_pkg_parse_file(resolved, NULL, &info)) status = EINVAL;
      }
    }
    if (!status) {
      bool pkg_installed;
      bool base = package_base_installed(info.title_id, &pkg_installed);
      if (info.pkg_kind == PKG_KIND_BASE && pkg_installed) {
        // Full media remasters install standalone, but ShellCore treats them
        // as patch tasks when a native base is already present.
        if (sm_pkg_is_media_remaster(&info)) info.pkg_kind = PKG_KIND_PATCH;
        else { status = EEXIST; error = "pkg_already_installed"; }
      }
      else if (info.pkg_kind != PKG_KIND_BASE && !base) status = ENOENT;
    }
    if (!status && sm_shellcore_title_is_protected(info.title_id)) { status = EBUSY; error = "game_active"; }
    if (!status) {
      pthread_mutex_lock(&g_storage_job.mutex);
      strlcpy(g_storage_job.title_id, info.title_id, sizeof(g_storage_job.title_id));
      strlcpy(g_storage_job.title_name, info.title_name, sizeof(g_storage_job.title_name));
      g_storage_job.pkg_current_kind = info.pkg_kind;
      g_storage_job.total_bytes = info.source_size;
      g_storage_job.total_files = 1;
      g_storage_job.state = STORAGE_JOB_INSTALLING;
      g_storage_job.transfer_started_us = monotonic_time_us();
      pthread_mutex_unlock(&g_storage_job.mutex);
      if (ctx.stream && !ctx.server)
        status = sm_pkg_server_start_reader(&ctx.server, source, size, sm_pkg_stream_read, ctx.stream, token, &port);
      // Updates share the base's ID. A managed HTTP source lets cancellation
      // interrupt their reads without CancelInstall removing the base title.
      else if (!ctx.server && (sm_smb_path(resolved) || info.pkg_kind != PKG_KIND_BASE))
        status = sm_pkg_server_start(&ctx.server,resolved,token,&port);
      if (!ctx.stream) ctx.expected = &info;
    }
    if (!status && package_install_cancelled(&ctx)) status = ctx.failure ? ctx.failure : ECANCELED;
    if (!status) {
      int length;
      if (ctx.server) {
        char encoded[MAX_PATH * 3];
        const char *name = strrchr(source, '/'); name = name ? name + 1 : source;
        static const char hex[] = "0123456789ABCDEF";
        size_t used = 0;
        for (const unsigned char *p = (const unsigned char *)name; *p; ++p) {
          if (isalnum(*p) || *p == '.' || *p == '-' || *p == '_') encoded[used++] = *p;
          else { encoded[used++] = '%'; encoded[used++] = hex[*p >> 4]; encoded[used++] = hex[*p & 15]; }
        }
        encoded[used] = 0;
        length = snprintf(uri, sizeof(uri), "http://%s:%u/%s/%s", sm_pkg_server_host(ctx.server), port, token, encoded);
      } else {
        // AppInstUtil resolves /data through its system-visible /user/data alias.
        length = snprintf(uri, sizeof(uri), "%s%s",
            path_matches_root_or_child(resolved, "/data") ? "/user" : "", resolved);
      }
      if (length < 0 || (size_t)length >= sizeof(uri)) status = ENAMETOOLONG;
      else {
        phase = "submit";
        if (info.pkg_kind != PKG_KIND_BASE) {
          int result = sceAppInstUtilGetInstallStatus(info.content_id, &previous_general);
          previous_general.status[sizeof(previous_general.status) - 1] = 0;
          general_snapshot = !result && !previous_general.error.code &&
              (!strcmp(previous_general.status, "playable") || !strcmp(previous_general.status, "completed"));
        }
        if (info.pkg_kind == PKG_KIND_PATCH) {
          char paths[2][MAX_PATH];
          snapshot_package_result(info.title_id, previous_result, paths);
        }
        submission.metadata.uri = uri;
        submission.metadata.ex_uri = submission.metadata.scenario = submission.metadata.content_id = submission.metadata.icon = "";
        submission.metadata.name = info.title_name;
        status = pthread_create(&submitter, NULL, submit_native_package, &submission);
        submitted = status == 0;
      }
    }
    while (!status && !atomic_load(&submission.done)) {
      if (package_install_cancelled(&ctx)) status = ctx.failure ? ctx.failure : ECANCELED;
      else usleep(100000);
    }
    if (!status) {
      native_result = submission.result;
      if (native_result) status = EIO;
      submission.info.content_id[sizeof(submission.info.content_id) - 1] = 0;
      if (!status && !submission.info.content_id[0]) status = EPROTO;
      if (!status && strcmp(submission.info.content_id, info.content_id)) status = EPROTO;
      if (!status && (submission.info.type < SM_PKG_NATIVE_BASE ||
                      submission.info.type > SM_PKG_NATIVE_PATCH)) status = EPROTO;
      if (!status)
        log_debug("  [PKG] native task accepted: job=%llu title=%s kind=%u type=%d platform=%d content_id=%s",
                  (unsigned long long)id, info.title_id, (unsigned)info.pkg_kind,
                  submission.info.type, submission.info.platform, submission.info.content_id);
    }
    uint64_t task_start_deadline_us = 0;
    if (!status) {
      phase = "poll";
      uint64_t now = monotonic_time_us();
      if (!now) status = EIO;
      else task_start_deadline_us = now + SM_API_PKG_START_TIMEOUT_US;
    }
    while (!status) {
      if (package_install_cancelled(&ctx)) { status = ctx.failure ? ctx.failure : ECANCELED; break; }
      phase = "poll";
      memset(&native_status, 0, sizeof(native_status));
      // Follow the task actually submitted by AppInstUtil, including delta-PKGs,
      // instead of assuming its type from the source parser's category.
      // True patch tasks must not read the already playable base's status.
      native_result = submission.info.type == SM_PKG_NATIVE_PATCH
          ? sceAppInstUtilGetPatchInstallStatus(submission.info.content_id, &native_status)
          : sceAppInstUtilGetInstallStatus(submission.info.content_id, &native_status);
      native_status.status[sizeof(native_status.status) - 1] = 0;
      if (native_result || native_status.error.code || !strcmp(native_status.status, "error")) { status = EIO; break; }
      if (!native_status.status[0]) { status = EPROTO; break; }
      int installed = 0;
      if (submission.info.type == SM_PKG_NATIVE_ADDCONT &&
          (!strcmp(native_status.status, "none") || !strcmp(native_status.status, "completed") ||
           !strcmp(native_status.status, "playable"))) {
        phase = "poll_addcont";
        native_result = sceAppInstUtilGetAddcontInstalledStatus(submission.info.content_id, &installed);
        // A new DLC need not have an installed-content record yet. Keep its
        // source alive and follow the install task until promotion is confirmed.
        if (native_result == SM_PKG_NATIVE_CONTENT_NOT_FOUND) {
          installed = 0;
          native_result = 0;
        }
        if (native_result) { status = EIO; break; }
        phase = "poll";
      }
      if (!strcmp(native_status.status, "none")) {
        // Entitlement-only DLC can have no general download record.
        if (submission.info.type == SM_PKG_NATIVE_ADDCONT && installed == SM_PKG_ADDCONT_INSTALLED) break;
        if (submission.info.type == SM_PKG_NATIVE_PATCH && patch_completed_total) {
          sm_pkg_native_status_t general = {0};
          native_result = sceAppInstUtilGetInstallStatus(submission.info.content_id, &general);
          general.status[sizeof(general.status) - 1] = 0;
          if (native_result || general.error.code || !strcmp(general.status, "error")) {
            native_status = general; status = EIO; break;
          }
          bool ready = !strcmp(general.status, "playable") || !strcmp(general.status, "completed");
          if (ready && (!general.copy_only || general.local_copy_percent >= 100) &&
              package_patch_result_installed(&info, previous_result)) {
            log_debug("  [PKG] patch completion verified after task retirement: job=%llu title=%s",
                      (unsigned long long)id, info.title_id);
            break;
          }
        }
        phase = "wait_task";
        // A missing record is not a native I/O failure. Keep the source alive
        // for one bounded visibility gap without resubmitting the package.
        uint64_t now = monotonic_time_us();
        if (!now) { status = EIO; break; }
        if (!task_start_deadline_us) task_start_deadline_us = now + SM_API_PKG_START_TIMEOUT_US;
        if (now >= task_start_deadline_us) { status = ETIMEDOUT; break; }
        status = wait_for_package_poll(&ctx);
        continue;
      }
      if (info.pkg_kind != PKG_KIND_BASE && submission.info.type == SM_PKG_NATIVE_BASE &&
          !replacement_observed) {
        // An unchanged terminal result can still describe the previous base.
        // Observe the replacement before allowing that shared record to finish it.
        bool terminal = !strcmp(native_status.status, "playable") || !strcmp(native_status.status, "completed");
        replacement_observed = !terminal || (general_snapshot &&
            (native_status.total != previous_general.total ||
             native_status.downloaded != previous_general.downloaded ||
             memcmp(native_status.patch_version, previous_general.patch_version, sizeof(native_status.patch_version))));
        if (!replacement_observed) {
          phase = "wait_task";
          uint64_t now = monotonic_time_us();
          if (!now) { status = EIO; break; }
          if (now >= task_start_deadline_us) { status = ETIMEDOUT; break; }
          status = wait_for_package_poll(&ctx);
          continue;
        }
      }
      if (submission.info.type == SM_PKG_NATIVE_PATCH && native_status.total > 0 &&
          native_status.total <= INT64_MAX) {
        if (native_status.downloaded >= native_status.total) patch_completed_total = native_status.total;
        else if (patch_completed_total != native_status.total) patch_completed_total = 0;
      }
      if (native_status.total > 0 && native_status.total <= INT64_MAX) {
        ctx.native_progress = true;
        publish_package_progress(&ctx, native_status.total, native_status.downloaded, monotonic_time_us());
      }
      // PlayGo can omit padding/unused ranges. HTTP coverage is not installed size.
      bool copied = !native_status.copy_only || native_status.local_copy_percent >= 100;
      bool downloaded = native_status.total > 0 && native_status.total <= INT64_MAX
          ? native_status.downloaded >= native_status.total
          : info.pkg_kind == PKG_KIND_BASE && !ctx.native_progress && ctx.server && sm_pkg_server_complete(ctx.server);
      bool completed = copied && (!strcmp(native_status.status, "completed") ||
          (!strcmp(native_status.status, "playable") && downloaded));
      // Completion can precede the installed DLC record. Reuse the bounded
      // visibility window, without limiting a transfer or local copy in progress.
      if (completed && submission.info.type == SM_PKG_NATIVE_ADDCONT && installed != SM_PKG_ADDCONT_INSTALLED) {
        phase = "wait_addcont";
        uint64_t now = monotonic_time_us();
        if (!now) { status = EIO; break; }
        if (!task_start_deadline_us) task_start_deadline_us = now + SM_API_PKG_START_TIMEOUT_US;
        if (now >= task_start_deadline_us) { status = ETIMEDOUT; break; }
      } else task_start_deadline_us = 0;
      // An installed DLC must not complete its replacement while transferring.
      if (submission.info.type == SM_PKG_NATIVE_ADDCONT) completed = completed && installed == SM_PKG_ADDCONT_INSTALLED;
      if (completed) break;
      status = wait_for_package_poll(&ctx);
    }
    // A confirmed native completion wins over a late stop/source disconnect.
    // Otherwise CancelInstall could remove the base that just finished.
    if (status && package_install_cancelled(&ctx)) status = ctx.failure ? ctx.failure : ECANCELED;
    if (status && !error) {
      if (status == EINVAL) error = "pkg_invalid";
      else if (status == ENOENT && info.title_id[0] && info.pkg_kind != PKG_KIND_BASE) error = "pkg_base_missing";
      else if (status == ESTALE) error = "pkg_source_changed";
      else if (ctx.stream && (ctx.failure == ETIMEDOUT || ctx.failure == ECONNRESET ||
                              ctx.failure == EPIPE || ctx.failure == EPROTO)) error = "pkg_stream_lost";
    }
    if (status && status != ECANCELED) {
      // The submission thread may still be filling its output during abort.
      int task_type = -1, task_platform = -1;
      if (atomic_load(&submission.done)) {
        task_type = submission.info.type;
        task_platform = submission.info.platform;
      }
      log_debug("  [PKG] install failure detail: job=%llu title=%s kind=%u phase=%s "
                "source=%s status=%d source_error=%d native=0x%08X "
                "state='%.16s' error=0x%08X type='%.9s' detail='%.160s' "
                "downloaded=%llu total=%llu served=%llu copy_only=%u copy_percent=%d task_type=%d platform=%d "
                "last_downloaded=%llu last_total=%llu source_size=%llu",
                (unsigned long long)id, info.title_id, (unsigned)info.pkg_kind, phase,
                source, status, ctx.failure, (unsigned)native_result,
                native_status.status, (unsigned)native_status.error.code,
                native_status.error.type, native_status.error.description,
                (unsigned long long)native_status.downloaded,
                (unsigned long long)native_status.total,
                (unsigned long long)ctx.bytes, (unsigned)native_status.copy_only,
                native_status.local_copy_percent, task_type, task_platform,
                (unsigned long long)ctx.sample_bytes, (unsigned long long)ctx.sample_total,
                (unsigned long long)info.source_size);
    }
    if (ctx.stream) sm_pkg_stream_stop(ctx.stream);
    // Close streamed sources and cancel native work before joining submission.
    sm_pkg_server_stop(ctx.server);
    // CancelInstall addresses the base application, not an individual patch or
    // DLC task sharing its content ID. Never use it to clean up an update.
    bool submission_done = atomic_load(&submission.done);
    bool cancel_base = !submission_done || (!submission.result && submission.info.type == SM_PKG_NATIVE_BASE &&
        submission.info.content_id[0] && !strcmp(submission.info.content_id, info.content_id));
    if (status && submitted && info.pkg_kind == PKG_KIND_BASE && cancel_base) {
      int cancelled = sceAppInstUtilCancelInstall(info.content_id);
      if (cancelled)
        log_debug("  [PKG] native cancel failed: job=%llu code=0x%08X", (unsigned long long)id, (unsigned)cancelled);
    }
    if (submitted) pthread_join(submitter, NULL);
    if (!status) {
      installed_any = true;
      invalidate_app_db_title_cache();
    }
    pthread_mutex_lock(&g_storage_job.mutex);
    g_storage_job.native_status = native_status.error.code ? native_status.error.code : native_result;
    g_storage_job.pkg_stream = NULL;
    g_storage_job.pkg_server = NULL;
    g_storage_job.pkg_server_port = 0;
    // Keep the current entry at the head until native completion is confirmed.
    // Failed/cancelled entries remain available for an explicit restart.
    if (!status && g_storage_job.pkg_queue_running) {
      --g_storage_job.pkg_queue_count;
      memmove(g_storage_job.pkg_queue, g_storage_job.pkg_queue + 1,
          g_storage_job.pkg_queue_count * sizeof(g_storage_job.pkg_queue[0]));
      ++g_storage_job.pkg_queue_completed;
    }
    if (!status && g_storage_job.pkg_queue_running && g_storage_job.pkg_queue_count &&
        (g_storage_job.cancel_requested || !g_storage_job.accepting ||
         should_stop_requested() || runtime_sleep_mode_active())) status = ECANCELED;
    bool next = !status && g_storage_job.pkg_queue_running && prepare_next_package_locked();
    if (!next) {
      g_storage_job.stream_token[0] = 0;
      g_storage_job.pkg_queue_running = false;
    }
    pthread_mutex_unlock(&g_storage_job.mutex);
    sm_pkg_stream_release(ctx.stream);
    if (next) continue;
    if (shellcore) sm_shellcore_end_external_mutation();
    if (scanner) sm_scanner_end_external_mutation();
    bool queued = installed_any && request_scan_now("HTTP API PKG batch finished");
    log_debug("  [PKG] install finished: job=%llu title=%s status=%d native=0x%08X", (unsigned long long)id, info.title_id, status,
              (unsigned)(native_status.error.code ? native_status.error.code : native_result));
    finish_storage_job(id, status, queued, error);
    return NULL;
  }
}

static void handle_package_install(struct MHD_Connection *connection, struct json_object *request, bool streaming, bool from_queue) {
  const runtime_config_t cfg = runtime_config();
  char source[MAX_PATH];
  const char *path = package_string(request, streaming ? "name" : "path");
  uint64_t size = 0;
  int status = 0;
  sm_pkg_info_t info = {0};
  const char *error = runtime_mutation_block_reason();
  if (error) { send_operation_error_response(connection, EBUSY, error); return; }
  if (streaming) {
    struct json_object *value;
    if (!path || strchr(path, '/') || strchr(path, '\\') || strlen(path) >= MAX_TITLE_NAME || strlen(path) < 5 ||
        strcasecmp(path + strlen(path) - 4, ".pkg") || !json_object_object_get_ex(request, "size", &value) ||
        !json_object_is_type(value, json_type_int) || json_object_get_int64(value) <= 0) status = EINVAL;
    else { size = (uint64_t)json_object_get_int64(value); strlcpy(source, path, sizeof(source)); }
  } else {
    status = resolve_package_path(path, source, false);
    if (!status && !sm_pkg_parse_file(source, NULL, &info)) status = EINVAL;
    size = info.source_size;
  }
  if (status) { send_error_response(connection, operation_http_status(status), status, strerror(status)); return; }
  bool pkg_installed = false;
  bool base = !streaming && package_base_installed(info.title_id, &pkg_installed);
  if (!streaming && !base && info.pkg_kind != PKG_KIND_BASE) {
    send_operation_error_response(connection, ENOENT, "pkg_base_missing"); return;
  }
  if (!streaming && pkg_installed && info.pkg_kind == PKG_KIND_BASE && !sm_pkg_is_media_remaster(&info)) {
    send_operation_error_response(connection, EEXIST, "pkg_already_installed"); return;
  }
  pthread_mutex_lock(&g_storage_job.mutex);
  if (!g_storage_job.accepting || storage_job_is_active(g_storage_job.state)) {
    pthread_mutex_unlock(&g_storage_job.mutex);
    send_operation_error_response(connection, EBUSY, "storage_busy"); return;
  }
  if (from_queue && (!g_storage_job.pkg_queue_count || strcmp(g_storage_job.pkg_queue[0].path, source))) {
    pthread_mutex_unlock(&g_storage_job.mutex);
    send_operation_error_response(connection, EBUSY, "storage_busy"); return;
  }
  if (!streaming && !from_queue) {
    for (size_t i = 0; i < g_storage_job.pkg_queue_count; ++i) {
      if (strcmp(g_storage_job.pkg_queue[i].path, source)) continue;
      pthread_mutex_unlock(&g_storage_job.mutex);
      send_operation_error_response(connection, EEXIST, "pkg_queued"); return;
    }
  }
  sm_pkg_stream_t *stream = streaming ? sm_pkg_stream_create(size) : NULL;
  if (streaming && !stream) { pthread_mutex_unlock(&g_storage_job.mutex); send_out_of_memory_response(connection); return; }
  uint64_t id = ++g_storage_job.next_id;
  if (!id) id = ++g_storage_job.next_id;
  g_storage_job.id = id;
  g_storage_job.operation = GAME_STORAGE_INSTALL;
  g_storage_job.state = STORAGE_JOB_PREPARING;
  g_storage_job.cancel_requested = g_storage_job.delete_source = false;
  g_storage_job.total_bytes = size;
  g_storage_job.processed_bytes = g_storage_job.processed_files = 0;
  g_storage_job.total_files = 1;
  g_storage_job.started_us = monotonic_time_us();
  g_storage_job.transfer_started_us = g_storage_job.transfer_finished_us = g_storage_job.finished_us = 0;
  g_storage_job.affected_titles = 0;
  g_storage_job.result_status = g_storage_job.native_status = 0;
  g_storage_job.pkg_speed = g_storage_job.pkg_sample_us = 0;
  g_storage_job.result_error_reason[0] = 0;
  memset(&g_storage_job.usb_blocker, 0, sizeof(g_storage_job.usb_blocker));
  g_storage_job.scan_queued = false;
  strlcpy(g_storage_job.title_id, info.title_id, sizeof(g_storage_job.title_id));
  strlcpy(g_storage_job.title_name, streaming ? source : info.title_name, sizeof(g_storage_job.title_name));
  strlcpy(g_storage_job.source_type, streaming ? "stream" : "pkg", sizeof(g_storage_job.source_type));
  strlcpy(g_storage_job.source, source, sizeof(g_storage_job.source));
  g_storage_job.pkg_current_kind = info.pkg_kind;
  g_storage_job.pkg_queue_running = from_queue;
  if (from_queue) {
    g_storage_job.pkg_queue_completed = 0;
    g_storage_job.pkg_queue_total = g_storage_job.pkg_queue_count;
    (void)prepare_next_package_locked();
  }
  g_storage_job.runtime_source[0] = g_storage_job.destination[0] = 0;
  g_storage_job.pkg_stream = stream;
  g_storage_job.pkg_server = NULL;
  g_storage_job.pkg_server_port = 0;
  unsigned char random[16];
  arc4random_buf(random, sizeof(random));
  for (size_t i = 0; i < sizeof(random); ++i) snprintf(g_storage_job.stream_token + i * 2, 3, "%02x", random[i]);
  char token[33]; strlcpy(token, g_storage_job.stream_token, sizeof(token));
  uint16_t stream_port = 0;
  if (streaming) {
    status = sm_pkg_server_start_browser(&g_storage_job.pkg_server, source, size, stream,
        cfg.api_bind_address, token, &stream_port);
    g_storage_job.pkg_server_port = stream_port;
    if (status) {
      g_storage_job.pkg_stream = NULL;
      g_storage_job.stream_token[0] = 0;
      pthread_mutex_unlock(&g_storage_job.mutex);
      sm_pkg_stream_release(stream);
      finish_storage_job(id, status, false, NULL);
      send_error_response(connection, 500, status, strerror(status)); return;
    }
  }
  storage_job_snapshot_t accepted;
  copy_storage_job_snapshot_locked(&accepted);
  pthread_mutex_unlock(&g_storage_job.mutex);
  pthread_attr_t attr;
  int rc = pthread_attr_init(&attr);
  bool initialized = rc == 0;
  if (!rc) rc = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  pthread_t thread;
  if (!rc) rc = pthread_create(&thread, &attr, package_install_thread, (void *)(uintptr_t)id);
  if (initialized) pthread_attr_destroy(&attr);
  if (rc) {
    pthread_mutex_lock(&g_storage_job.mutex);
    sm_pkg_server_t *server = g_storage_job.pkg_server;
    g_storage_job.pkg_server = NULL;
    g_storage_job.pkg_server_port = 0;
    g_storage_job.pkg_stream = NULL;
    g_storage_job.stream_token[0] = 0;
    g_storage_job.pkg_queue_running = false;
    pthread_mutex_unlock(&g_storage_job.mutex);
    if (stream) sm_pkg_stream_stop(stream);
    sm_pkg_server_stop(server);
    sm_pkg_stream_release(stream);
    finish_storage_job(id, rc, false, NULL);
    send_error_response(connection, 500, rc, strerror(rc)); return;
  }
  struct json_object *response = new_status_response(0);
  if (!response || !add_storage_job_fields(response, &accepted) ||
      (streaming && (!add_json_string(response, "token", token) ||
                     !add_json_int(response, "stream_port", stream_port) ||
                     !add_json_int(response, "chunk_bytes", SM_PKG_STREAM_CHUNK)))) {
    if (response) json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  send_json_object(connection, 202, response); json_object_put(response);
}

static void handle_package_queue(struct MHD_Connection *connection, struct json_object *request) {
  const char *action = package_string(request, "action");
  if (!action) action = "status";
  bool only_if_idle;
  if (!get_optional_bool(request, "only_if_idle", false, &only_if_idle)) {
    send_error_response(connection, 400, EINVAL, "only_if_idle must be a boolean"); return;
  }
  const char *path = package_string(request, "path");
  sm_pkg_info_t info = {0};
  bool installed = false;
  if (!strcmp(action, "add")) {
    char resolved[MAX_PATH];
    int status = resolve_package_path(path, resolved, false);
    if (!status && !sm_pkg_parse_file(resolved, NULL, &info)) status = EINVAL;
    if (status) { send_operation_error_response(connection, status, "pkg_invalid"); return; }
    bool pkg_installed;
    bool base = package_base_installed(info.title_id, &pkg_installed);
    installed = info.pkg_kind == PKG_KIND_BASE ? pkg_installed : base;
  } else if (strcmp(action, "status") && strcmp(action, "start") &&
             strcmp(action, "remove") && strcmp(action, "clear")) {
    send_error_response(connection, 400, EINVAL, "unknown queue action"); return;
  }
  pthread_mutex_lock(&g_storage_job.mutex);
  if (!strcmp(action, "start")) {
    if (!g_storage_job.pkg_queue_count || storage_job_is_active(g_storage_job.state)) {
      pthread_mutex_unlock(&g_storage_job.mutex);
      send_operation_error_response(connection, EBUSY, "storage_busy"); return;
    }
    char first[MAX_PATH];
    strlcpy(first, g_storage_job.pkg_queue[0].path, sizeof(first));
    pthread_mutex_unlock(&g_storage_job.mutex);
    struct json_object *start = json_object_new_object();
    if (!start || !add_json_string(start, "path", first)) {
      if (start) json_object_put(start);
      send_out_of_memory_response(connection); return;
    }
    handle_package_install(connection, start, false, true);
    json_object_put(start);
    return;
  }
  int status = 0;
  const char *reason = NULL;
  if (!strcmp(action, "add")) {
    if (!g_storage_job.accepting || (storage_job_is_active(g_storage_job.state) &&
        (g_storage_job.cancel_requested || (g_storage_job.operation == GAME_STORAGE_DELETE && !g_storage_job.title_id[0])))) status = EBUSY;
    else status = enqueue_package_locked(&info, installed);
    reason = status == ENOENT ? "pkg_base_missing" : status == EALREADY ? "pkg_already_installed" :
             status == EEXIST ? "pkg_queued" : status == ENOSPC ? "pkg_queue_full" : "storage_busy";
  } else if (!strcmp(action, "clear")) {
    status = clear_package_queue_locked(only_if_idle);
    if (status) reason = "storage_busy";
  } else if (!strcmp(action, "remove")) {
    size_t index = 0;
    while (index < g_storage_job.pkg_queue_count && (!path || strcmp(path, g_storage_job.pkg_queue[index].path))) ++index;
    if (index == g_storage_job.pkg_queue_count) status = ENOENT;
    else if (index == 0 && g_storage_job.pkg_queue_running) { status = EBUSY; reason = "storage_busy"; }
    else {
      sm_pkg_info_t removed = g_storage_job.pkg_queue[index];
      bool remove_dependents = removed.pkg_kind == PKG_KIND_BASE && !package_base_installed(removed.title_id, NULL);
      size_t kept = 0, count = g_storage_job.pkg_queue_count;
      for (size_t i = 0; i < count; ++i) {
        if (i == index || (remove_dependents && !(i == 0 && g_storage_job.pkg_queue_running) &&
            !strcmp(g_storage_job.pkg_queue[i].title_id, removed.title_id))) continue;
        g_storage_job.pkg_queue[kept++] = g_storage_job.pkg_queue[i];
      }
      g_storage_job.pkg_queue_count = kept;
      if (g_storage_job.pkg_queue_running) g_storage_job.pkg_queue_total -= count - kept;
    }
  }
  struct json_object *response = status ? NULL : new_status_response(0);
  struct json_object *entries = status ? NULL : json_object_new_array();
  bool ready = response && entries && add_json_bool(response, "queue_running", g_storage_job.pkg_queue_running) &&
      add_json_int(response, "queue_completed", (int64_t)g_storage_job.pkg_queue_completed) &&
      add_json_int(response, "queue_total", (int64_t)g_storage_job.pkg_queue_total);
  for (size_t i = 0; ready && i < g_storage_job.pkg_queue_count; ++i) {
    struct json_object *item = json_object_new_object();
    ready = item && add_package_info(item, &g_storage_job.pkg_queue[i]) &&
        add_package_icon_info(item, &g_storage_job.pkg_queue[i]) &&
        add_json_int(item, "size_bytes", (int64_t)g_storage_job.pkg_queue[i].source_size) &&
        add_json_string(item, "path", g_storage_job.pkg_queue[i].path) && json_object_array_add(entries, item) == 0;
    if (!ready && item) json_object_put(item);
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
  if (status) { send_operation_error_response(connection, status, reason); return; }
  if (!ready) {
    if (entries) json_object_put(entries);
    if (response) json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  json_object_object_add(response, "entries", entries);
  send_json_object(connection, 200, response);
  json_object_put(response);
}

static sm_pkg_stream_t *find_package_stream(const char *token) {
  pthread_mutex_lock(&g_storage_job.mutex);
  sm_pkg_stream_t *stream = token && strlen(token) == 32 && !strcmp(token, g_storage_job.stream_token) ? g_storage_job.pkg_stream : NULL;
  if (stream) sm_pkg_stream_retain(stream);
  pthread_mutex_unlock(&g_storage_job.mutex);
  return stream;
}

static void handle_package_range(struct MHD_Connection *connection, struct json_object *request) {
  unsigned client = 0;
  struct json_object *value;
  if (json_object_object_get_ex(request, "client", &value)) {
    int64_t requested = json_object_get_int64(value);
    if (!json_object_is_type(value, json_type_int) || requested < 1 || requested > SM_PKG_STREAM_CLIENTS) {
      send_error_response(connection, 400, EINVAL, "invalid stream client"); return;
    }
    client = (unsigned)requested;
  }
  sm_pkg_stream_t *stream = find_package_stream(package_string(request, "token"));
  if (!stream) { send_error_response(connection, 404, ENOENT, "stream session is unavailable"); return; }
  sm_pkg_range_t range = {0};
  int status = sm_pkg_stream_try_request(stream, client, &range);
  sm_pkg_stream_release(stream);
  if (status && status != EAGAIN) { send_error_response(connection, 410, status, "stream session ended"); return; }
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_int(response, "id", (int64_t)range.id) || !add_json_int(response, "offset", (int64_t)range.offset) ||
      !add_json_int(response, "size", (int64_t)range.size)) {
    if (response) json_object_put(response);
    send_out_of_memory_response(connection); return;
  }
  send_stream_json_object(connection, response); json_object_put(response);
}

static void handle_package_chunk(struct MHD_Connection *connection, const void *data, size_t size) {
  const char *token = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "token");
  const char *id_text = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "id");
  size_t id = 0;
  sm_pkg_stream_t *stream = find_package_stream(token);
  int status = !id_text || !parse_content_length(id_text, &id) || !id ? EINVAL : !stream ? ENOENT : 0;
  if (!status) status = sm_pkg_stream_reply(stream, id, data, size);
  bool available = stream != NULL;
  sm_pkg_stream_release(stream);
  if (status) {
    // The range may have been consumed after a lost browser acknowledgement.
    unsigned http_status = available && status == ENOENT ? 409 : operation_http_status(status);
    send_error_response(connection, http_status, status, strerror(status)); return;
  }
  struct json_object *response = new_status_response(0);
  if (!response) { send_out_of_memory_response(connection); return; }
  send_stream_json_object(connection, response); json_object_put(response);
}

static void handle_storage_job_status(struct MHD_Connection *connection,
                                      struct json_object *request) {
  uint64_t requested_id = 0;
  if (!get_optional_job_id(request, false, &requested_id)) {
    send_error_response(connection, 400, EINVAL,
                        "job_id must be a positive integer");
    return;
  }
  storage_job_snapshot_t snapshot;
  memset(&snapshot, 0, sizeof(snapshot));
  if (!get_storage_job_snapshot(requested_id, &snapshot)) {
    send_error_response(connection, 404, ENOENT, "storage job not found");
    return;
  }
  send_storage_job_response(connection, 200, &snapshot);
}

static void handle_storage_job_cancel(struct MHD_Connection *connection,
                                      struct json_object *request) {
  uint64_t job_id = 0;
  if (!get_optional_job_id(request, true, &job_id)) {
    send_error_response(connection, 400, EINVAL,
                        "job_id must be a positive integer");
    return;
  }

  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.id != job_id) {
    pthread_mutex_unlock(&g_storage_job.mutex);
    send_error_response(connection, 404, ENOENT, "storage job not found");
    return;
  }
  if (!storage_job_is_active(g_storage_job.state) ||
      !storage_job_is_cancellable(g_storage_job.state)) {
    pthread_mutex_unlock(&g_storage_job.mutex);
    send_error_response(connection, 409, EBUSY,
                        "storage job can no longer be cancelled");
    return;
  }
  g_storage_job.cancel_requested = true;
  if (g_storage_job.pkg_stream) sm_pkg_stream_stop(g_storage_job.pkg_stream);
  storage_job_snapshot_t snapshot;
  copy_storage_job_snapshot_locked(&snapshot);
  pthread_mutex_unlock(&g_storage_job.mutex);
  send_storage_job_response(connection, 200, &snapshot);
}

static void stop_storage_job(void) {
  pthread_mutex_lock(&g_storage_job.mutex);
  if (g_storage_job.pkg_stream) sm_pkg_stream_stop(g_storage_job.pkg_stream);
  g_storage_job.accepting = false;
  if (storage_job_is_active(g_storage_job.state) &&
      storage_job_is_cancellable(g_storage_job.state)) {
    g_storage_job.cancel_requested = true;
  }
  while (storage_job_is_active(g_storage_job.state)) {
    struct timespec deadline;
    int rc = clock_gettime(CLOCK_REALTIME, &deadline);
    if (rc == 0) {
      deadline.tv_sec += SM_API_STORAGE_STOP_LOG_SECONDS;
      rc = pthread_cond_timedwait(&g_storage_job.cond,
                                  &g_storage_job.mutex, &deadline);
    } else {
      rc = pthread_cond_wait(&g_storage_job.cond, &g_storage_job.mutex);
    }
    if (rc == ETIMEDOUT) {
      log_debug("  [API] waiting for storage job during shutdown: "
                "job=%llu state=%s bytes=%llu/%llu files=%llu/%llu",
                (unsigned long long)g_storage_job.id,
                storage_job_state_name(g_storage_job.state),
                (unsigned long long)g_storage_job.processed_bytes,
                (unsigned long long)g_storage_job.total_bytes,
                (unsigned long long)g_storage_job.processed_files,
                (unsigned long long)g_storage_job.total_files);
    } else if (rc != 0) {
      log_debug("  [API] storage job shutdown wait failed: %s",
                strerror(rc));
      pthread_mutex_unlock(&g_storage_job.mutex);
      sceKernelUsleep(100000u);
      pthread_mutex_lock(&g_storage_job.mutex);
    }
  }
  pthread_mutex_unlock(&g_storage_job.mutex);
}

static void send_missing_source_response(struct MHD_Connection *connection,
                                          const char *title_id) {
  // Absence can also mean disconnected storage. Reconcile through the scanner
  // when runtime is safe; do not unlink title state or uninstall here.
  if (!request_scan_now("HTTP API missing game source reconciliation")) {
    send_operation_error_response(connection, EBUSY, "rest_mode");
    return;
  }
  struct json_object *response = new_status_response(0);
  if (!response || !add_json_string(response, "title_id", title_id) ||
      !add_json_bool(response, "source_missing", true) ||
      !add_scan_queue_fields(response, true)) {
    if (response)
      json_object_put(response);
    send_out_of_memory_response(connection);
    return;
  }
  log_debug("  [API] source unavailable; reconciliation queued: title=%s",
            title_id);
  (void)send_json_object(connection, 200, response);
  json_object_put(response);
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

  sm_game_cache_snapshot_entry_t package;
  if (find_installed_pkg_game(title_id, &package)) {
    send_error_response(connection, 403, EPERM,
                        "installed PKG sources cannot be copied, moved or deleted");
    return;
  }
  if (errno != ENOENT) {
    int status = errno != 0 ? errno : EIO;
    send_error_response(connection, operation_http_status(status), status,
                        strerror(status));
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

  bool delete_source = false;
  if (operation == GAME_STORAGE_UNPACK &&
      !get_optional_bool(request, "delete_source", false, &delete_source)) {
    send_error_response(connection, 400, EINVAL,
                        "delete_source must be a boolean");
    return;
  }

  storage_job_snapshot_t accepted;
  memset(&accepted, 0, sizeof(accepted));
  const char *error = NULL;
  int status = start_storage_job(title_id, destination_dir, operation,
                                 delete_source, &accepted, &error);
  if (status != 0) {
    if (operation == GAME_STORAGE_DELETE && error &&
        strcmp(error, "source_missing") == 0) {
      send_missing_source_response(connection, title_id);
      return;
    }
    log_debug("  [API] async game source %s start failed: title=%s "
              "status=%d reason=%s",
              game_storage_operation_name(operation), title_id, status,
              operation_error_message(status, error));
    send_operation_error_response(connection, status, error);
    return;
  }
  log_debug("  [API] async game source %s accepted: job=%llu title=%s",
            game_storage_operation_name(operation),
            (unsigned long long)accepted.id, title_id);
  send_storage_job_response(connection, 202, &accepted);
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
    else if (strcmp(url, SM_API_ROUTE_FAVICON) == 0)
      queued = send_embedded_response(connection, smp_icon_png,
                                       (size_t)smp_icon_png_len,
                                       "image/png", "public, max-age=86400");
    else if (strcmp(url, SM_API_ROUTE_GAME_ICON) == 0)
      queued = handle_game_icon(connection);
    else if (strcmp(url, SM_API_ROUTE_PACKAGE_ICON) == 0)
      queued = handle_package_icon(connection);
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

  bool package_chunk = strcmp(url, SM_API_ROUTE_PACKAGE_CHUNK) == 0;
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
    size_t limit = package_chunk ? SM_PKG_STREAM_CHUNK : SM_API_MAX_JSON_BODY_SIZE;
    if (request->expected_body_size > limit) {
      return finish_request_error(request, connection, 413, EPROTO,
                                  "JSON request body is too large");
    }

    const char *content_type = MHD_lookup_connection_value(
        connection, MHD_HEADER_KIND, MHD_HTTP_HEADER_CONTENT_TYPE);
    if (!content_type || (package_chunk ? strcmp(content_type, "application/octet-stream") != 0 : !is_json_content_type(content_type))) {
      return finish_request_error(
          request, connection, 415, EPROTO,
          "Content-Type must be application/json");
    }
    if (package_chunk) {
      request->binary_body = malloc(request->expected_body_size ? request->expected_body_size : 1);
      if (!request->binary_body) return MHD_NO;
    }
    request->headers_validated = true;
  }

  if (*upload_data_size > 0) {
    if (*upload_data_size > request->expected_body_size - request->body_size) {
      *upload_data_size = 0;
      return finish_request_error(request, connection, 400, EPROTO,
                                  "request body exceeds Content-Length");
    }
    memcpy((package_chunk ? request->binary_body : (unsigned char *)request->body) + request->body_size, upload_data,
           *upload_data_size);
    request->body_size += *upload_data_size;
    *upload_data_size = 0;
    return MHD_YES;
  }

  if (request->body_size != request->expected_body_size) {
    return finish_request_error(request, connection, 400, EPROTO,
                                "incomplete JSON request body");
  }
  if (package_chunk) {
    handle_package_chunk(connection, request->binary_body, request->body_size);
    request->response_queued = true;
    return MHD_YES;
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
  sm_http_request_t *request = *con_cls;
  if (request) free(request->binary_body);
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

static int http_listener_status(struct MHD_Daemon *daemon) {
  const union MHD_DaemonInfo *info =
      MHD_get_daemon_info(daemon, MHD_DAEMON_INFO_LISTEN_FD);
  if (!info || info->listen_fd == MHD_INVALID_SOCKET)
    return EBADF;

  int accepting = 0;
  socklen_t size = sizeof(accepting);
  if (getsockopt(info->listen_fd, SOL_SOCKET, SO_ACCEPTCONN,
                 &accepting, &size) != 0) {
    // A periodic probe can defer an interrupted read until the next check.
    // Do not keep the service thread in a retry loop during shutdown.
    return errno == EINTR ? 0 : (errno != 0 ? errno : EIO);
  }
  return accepting ? 0 : ENOTCONN;
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
      if (!wait_for_listener_event(retry_delay_ms))
        break;
      retry_delay_ms = next_retry_delay_ms(retry_delay_ms);
      continue;
    }

    pthread_mutex_lock(&g_api.mutex);
    if (g_api.stop_requested || g_api.restart_requested ||
        runtime_sleep_mode_active()) {
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

    int listener_error = 0;
    while (wait_for_listener_event(SM_API_LISTENER_CHECK_MS)) {
      if (api_listener_stop_requested(NULL))
        break;
      listener_error = http_listener_status(daemon);
      if (listener_error != 0)
        break;
    }
    pthread_mutex_lock(&g_api.mutex);
    // Resume/stop may have arrived while the socket was being checked. Its
    // invalidation is then expected, rather than an independent API outage.
    if (g_api.stop_requested || g_api.restart_requested ||
        runtime_sleep_mode_active()) {
      listener_error = 0;
    }
    if (g_api.daemon == daemon)
      g_api.daemon = NULL;
    // Let long-running request handlers cancel before MHD_stop_daemon joins
    // its workers. Background storage jobs have their own cancellation state.
    g_api.restart_requested = true;
    bool stopping = g_api.stop_requested;
    pthread_mutex_unlock(&g_api.mutex);

    MHD_stop_daemon(daemon);
    if (stopping)
      break;
    if (listener_error != 0) {
      log_debug("  [API] HTTP listener lost at %s:%u: %s; recovery scheduled",
                bind_address, (unsigned int)port, strerror(listener_error));
      outage_logged = true;
    }
  }
  return NULL;
}

bool sm_api_service_start(void) {
  runtime_config_t cfg = runtime_config();
  if (!cfg.api_enabled) {
    pthread_mutex_lock(&g_storage_job.mutex);
    g_storage_job.accepting = false;
    pthread_mutex_unlock(&g_storage_job.mutex);
    log_debug("  [API] HTTP API disabled by configuration");
    return true;
  }

  char bind_address[MAX_API_BIND_ADDRESS];
  (void)strlcpy(bind_address, cfg.api_bind_address, sizeof(bind_address));
  uint16_t port = (uint16_t)cfg.api_port;

  pthread_mutex_lock(&g_api.mutex);
  if (g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return true;
  }
  g_api.daemon = NULL;
  g_api.stop_requested = false;
  g_api.restart_requested = false;
  (void)strlcpy(g_api.bind_address, bind_address,
                sizeof(g_api.bind_address));
  g_api.port = port;
  pthread_mutex_lock(&g_storage_job.mutex);
  g_storage_job.accepting = true;
  pthread_mutex_unlock(&g_storage_job.mutex);
  int rc = pthread_create(&g_api.thread, NULL, service_thread_main, NULL);
  if (rc != 0) {
    pthread_mutex_lock(&g_storage_job.mutex);
    g_storage_job.accepting = false;
    pthread_mutex_unlock(&g_storage_job.mutex);
    pthread_mutex_unlock(&g_api.mutex);
    errno = rc;
    return false;
  }
  g_api.started = true;
  pthread_mutex_unlock(&g_api.mutex);
  return true;
}

static void stop_api_listener(void) {
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
  g_api.restart_requested = false;
  g_api.daemon = NULL;
  pthread_mutex_unlock(&g_api.mutex);
}

void sm_api_service_stop(void) {
  stop_storage_job();
  stop_api_listener();
}

bool sm_api_service_reconfigure(void) {
  // Storage jobs are independent from the listener. Waiting for a slow USB
  // operation here would make a bind/port config reload look hung.
  pthread_mutex_lock(&g_storage_job.mutex);
  g_storage_job.accepting = false;
  pthread_mutex_unlock(&g_storage_job.mutex);
  stop_api_listener();
  return sm_api_service_start();
}

void sm_api_service_on_sleep_change(bool active) {
  if (active) {
    pthread_mutex_lock(&g_storage_job.mutex);
    // Install listeners belong to one attempt and must not survive sleep.
    // Latch disk installs too: a coalesced sleep/resume can otherwise leave
    // their worker using pre-sleep sockets without observing the sleep flag.
    if (g_storage_job.operation == GAME_STORAGE_INSTALL &&
        storage_job_is_active(g_storage_job.state)) {
      g_storage_job.cancel_requested = true;
      if (g_storage_job.pkg_stream) sm_pkg_stream_stop(g_storage_job.pkg_stream);
    }
    pthread_mutex_unlock(&g_storage_job.mutex);
  }
  pthread_mutex_lock(&g_api.mutex);
  if (!g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return;
  }

  // Preserve resume across a coalesced sleep/wake notification: the service
  // may never have observed sleep=true, but its old socket still needs reset.
  if (!active)
    g_api.restart_requested = true;
  pthread_cond_broadcast(&g_api.cond);
  pthread_mutex_unlock(&g_api.mutex);
}
