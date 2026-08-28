#include "sm_platform.h"

#include <arpa/inet.h>
#include <json-c/json.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sys/socket.h>

#include "sm_api_protocol.h"
#include "sm_api_service.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_cache.h"
#include "sm_gameinfo.h"
#include "sm_image.h"
#include "sm_image_cache.h"
#include "sm_image_index.h"
#include "sm_log.h"
#include "sm_mount_device.h"
#include "sm_path_utils.h"
#include "sm_runtime.h"
#include "sm_shellcore_service.h"
#include "sm_socket_io.h"

#ifndef SHADOWMOUNT_VERSION
#define SHADOWMOUNT_VERSION "unknown"
#endif

#define SM_API_ROUTE_SIZE 128u
#define SM_API_RETRY_INITIAL_MS 250u
#define SM_API_RETRY_MAX_MS 5000u
#define SM_API_CORS_HEADERS                                                   \
  "Access-Control-Allow-Origin: *\r\n"                                      \
  "Access-Control-Allow-Methods: POST, OPTIONS\r\n"                         \
  "Access-Control-Allow-Headers: Content-Type\r\n"                          \
  "Access-Control-Allow-Private-Network: true\r\n"

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool stop_requested;
  int listen_fd;
  int client_fd;
  char bind_address[MAX_API_BIND_ADDRESS];
  uint16_t port;
} sm_api_service_state_t;

typedef struct {
  char route[SM_API_ROUTE_SIZE];
  char *storage;
  char *body;
  size_t body_size;
  bool preflight;
} sm_http_request_t;

static sm_api_service_state_t g_api = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .listen_fd = -1,
    .client_fd = -1,
};

static int open_http_listener(const char *bind_address, uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0)
    return -1;
  int reuse = 1;
  (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  struct sockaddr_in address;
  memset(&address, 0, sizeof(address));
  address.sin_len = sizeof(address);
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  if (inet_pton(AF_INET, bind_address, &address.sin_addr) != 1) {
    close(fd);
    errno = EINVAL;
    return -1;
  }
  if (bind(fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(fd, 4) != 0) {
    int saved_errno = errno;
    close(fd);
    errno = saved_errno;
    return -1;
  }
  return fd;
}

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

static bool accept_error_needs_wait(int error) {
  return error == EAGAIN || error == EWOULDBLOCK || error == EMFILE ||
         error == ENFILE || error == ENOBUFS || error == ENOMEM;
}

static const char *http_reason_phrase(int status) {
  switch (status) {
  case 200:
    return "OK";
  case 400:
    return "Bad Request";
  case 404:
    return "Not Found";
  case 405:
    return "Method Not Allowed";
  case 409:
    return "Conflict";
  case 411:
    return "Length Required";
  case 413:
    return "Payload Too Large";
  case 415:
    return "Unsupported Media Type";
  case 431:
    return "Request Header Fields Too Large";
  case 500:
    return "Internal Server Error";
  case 501:
    return "Not Implemented";
  default:
    return "Error";
  }
}

static bool send_json_text(int fd, int http_status, const char *body,
                           size_t body_size) {
  char header[512];
  int count = snprintf(header, sizeof(header),
                       "HTTP/1.1 %d %s\r\n"
                       "Content-Type: application/json\r\n"
                       SM_API_CORS_HEADERS
                       "Content-Length: %zu\r\n"
                       "Connection: close\r\n"
                       "Cache-Control: no-store\r\n\r\n",
                       http_status, http_reason_phrase(http_status), body_size);
  if (count < 0 || (size_t)count >= sizeof(header))
    return false;
  return sm_socket_write_full(fd, header, (size_t)count) &&
         sm_socket_write_full(fd, body, body_size);
}

static void send_preflight_response(int fd) {
  static const char response[] =
      "HTTP/1.1 204 No Content\r\n"
      SM_API_CORS_HEADERS
      "Access-Control-Max-Age: 86400\r\n"
      "Content-Length: 0\r\n"
      "Connection: close\r\n\r\n";
  (void)sm_socket_write_full(fd, response, sizeof(response) - 1u);
}

static bool send_json_object(int fd, int http_status,
                             struct json_object *response) {
  const char *body =
      json_object_to_json_string_ext(response, JSON_C_TO_STRING_PLAIN);
  if (!body)
    return false;
  return send_json_text(fd, http_status, body, strlen(body));
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

static void send_out_of_memory_response(int fd) {
  static const char body[] =
      "{\"status\":12,\"error\":\"out of memory\"}";
  (void)send_json_text(fd, 500, body, sizeof(body) - 1u);
}

static void send_error_response(int fd, int http_status, int status,
                                const char *message) {
  struct json_object *response = new_status_response(status);
  if (!response) {
    send_out_of_memory_response(fd);
    return;
  }
  if (!add_json_string(response, "error", message)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, http_status, response);
  json_object_put(response);
}

static char *find_http_header_end(char *buffer, size_t size) {
  if (size < 4u)
    return NULL;
  for (size_t i = 0; i + 3u < size; ++i) {
    if (buffer[i] == '\r' && buffer[i + 1u] == '\n' &&
        buffer[i + 2u] == '\r' && buffer[i + 3u] == '\n') {
      return &buffer[i];
    }
  }
  return NULL;
}

static char *trim_http_space(char *value) {
  while (*value == ' ' || *value == '\t')
    value++;
  size_t size = strlen(value);
  while (size > 0u && (value[size - 1u] == ' ' || value[size - 1u] == '\t'))
    value[--size] = '\0';
  return value;
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

static bool reject_http_request(char *buffer, int http_status,
                                const char *error, int *http_status_out,
                                const char **error_out) {
  free(buffer);
  *http_status_out = http_status;
  *error_out = error;
  return false;
}

static bool receive_http_request(int fd, sm_http_request_t *request,
                                 int *http_status_out,
                                 const char **error_out) {
  size_t capacity = SM_API_MAX_HTTP_HEADER_SIZE + SM_API_MAX_JSON_BODY_SIZE + 1u;
  char *buffer = malloc(capacity);
  if (!buffer)
    return reject_http_request(NULL, 500, "out of memory", http_status_out,
                               error_out);

  size_t received = 0;
  char *header_end = NULL;
  while (!header_end) {
    if (received >= SM_API_MAX_HTTP_HEADER_SIZE)
      return reject_http_request(buffer, 431, "HTTP headers are too large",
                                 http_status_out, error_out);
    ssize_t count = recv(fd, buffer + received,
                         SM_API_MAX_HTTP_HEADER_SIZE - received, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return reject_http_request(buffer, 400, "incomplete HTTP request",
                                 http_status_out, error_out);
    received += (size_t)count;
    header_end = find_http_header_end(buffer, received);
  }

  size_t header_size = (size_t)(header_end - buffer) + 4u;
  *header_end = '\0';
  char *line_end = strstr(buffer, "\r\n");
  if (!line_end)
    return reject_http_request(buffer, 400, "invalid HTTP request line",
                               http_status_out, error_out);
  *line_end = '\0';

  char method[8];
  char version[16];
  char extra;
  if (sscanf(buffer, "%7s %127s %15s %c", method, request->route, version,
             &extra) != 3 ||
      (strcmp(version, "HTTP/1.1") != 0 && strcmp(version, "HTTP/1.0") != 0))
    return reject_http_request(buffer, 400, "invalid HTTP request line",
                               http_status_out, error_out);
  if (strcmp(method, "POST") == 0) {
    request->preflight = false;
  } else if (strcmp(method, "OPTIONS") == 0) {
    request->preflight = true;
  } else {
    return reject_http_request(buffer, 405,
                               "only POST and OPTIONS requests are supported",
                               http_status_out, error_out);
  }

  bool has_content_length = false;
  bool has_json_content_type = false;
  bool has_transfer_encoding = false;
  size_t body_size = 0;
  char *line = line_end + 2;
  while (line < header_end && line[0] != '\0') {
    char *next = strstr(line, "\r\n");
    if (!next)
      next = header_end;
    *next = '\0';
    char *colon = strchr(line, ':');
    if (!colon)
      return reject_http_request(buffer, 400, "invalid HTTP header",
                                 http_status_out, error_out);
    *colon = '\0';
    char *name = trim_http_space(line);
    char *value = trim_http_space(colon + 1);
    if (strcasecmp(name, "Content-Length") == 0) {
      if (has_content_length || !parse_content_length(value, &body_size))
        return reject_http_request(buffer, 400,
                                   "invalid Content-Length header",
                                   http_status_out, error_out);
      has_content_length = true;
    } else if (strcasecmp(name, "Content-Type") == 0) {
      has_json_content_type = is_json_content_type(value);
    } else if (strcasecmp(name, "Transfer-Encoding") == 0) {
      has_transfer_encoding = true;
    }
    line = next + 2;
  }

  if (has_transfer_encoding)
    return reject_http_request(buffer, 501,
                               "Transfer-Encoding is not supported",
                               http_status_out, error_out);
  if (request->preflight) {
    request->storage = buffer;
    return true;
  }
  if (!has_content_length)
    return reject_http_request(buffer, 411, "Content-Length is required",
                               http_status_out, error_out);
  if (!has_json_content_type)
    return reject_http_request(buffer, 415,
                               "Content-Type must be application/json",
                               http_status_out, error_out);
  if (body_size > SM_API_MAX_JSON_BODY_SIZE)
    return reject_http_request(buffer, 413, "JSON request body is too large",
                               http_status_out, error_out);

  size_t total_size = header_size + body_size;
  while (received < total_size) {
    ssize_t count = recv(fd, buffer + received, total_size - received, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return reject_http_request(buffer, 400,
                                 "incomplete JSON request body",
                                 http_status_out, error_out);
    received += (size_t)count;
  }
  buffer[total_size] = '\0';
  request->storage = buffer;
  request->body = buffer + header_size;
  request->body_size = body_size;
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

static void handle_version(int fd) {
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
      !append_json_string(capabilities, "list_images") ||
      !append_json_string(capabilities, "list_games") ||
      !append_json_string(capabilities, "mount_game") ||
      !append_json_string(capabilities, "unmount_game")) {
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

static struct json_object *game_to_json(const void *entry) {
  const sm_game_cache_snapshot_entry_t *source = entry;
  struct json_object *item = json_object_new_object();
  if (!item)
    return NULL;

  char managed_path[MAX_PATH];
  char image_path[MAX_PATH] = {0};
  bool installed = is_installed(source->title_id);
  bool mounted = is_data_mounted(source->title_id);
  bool managed =
      read_mount_link(source->title_id, managed_path, sizeof(managed_path));
  bool image_backed = read_mount_image_link(source->title_id, image_path,
                                             sizeof(image_path));
  bool source_available = path_exists(source->path) ||
                          (image_path[0] != '\0' && path_exists(image_path));

  if (!add_json_string(item, "path", source->path) ||
      !add_json_string(item, "image_path", image_path) ||
      !add_json_string(item, "title_id", source->title_id) ||
      !add_json_string(item, "title_name", source->title_name) ||
      !add_json_bool(item, "installed", installed) ||
      !add_json_bool(item, "managed", managed) ||
      !add_json_bool(item, "mounted", mounted) ||
      !add_json_bool(item, "image_backed", image_backed) ||
      !add_json_bool(item, "source_available", source_available)) {
    json_object_put(item);
    return NULL;
  }
  return item;
}

typedef struct json_object *(*snapshot_to_json_fn)(const void *entry);

static void send_snapshot_response(int fd, void *snapshot, size_t count,
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

static void handle_images(int fd) {
  sm_image_index_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_image_index_snapshot(&snapshot, &count)) {
    send_out_of_memory_response(fd);
    return;
  }
  send_snapshot_response(fd, snapshot, count, sizeof(*snapshot), "images",
                         image_to_json);
}

static void handle_games(int fd) {
  sm_game_cache_snapshot_entry_t *snapshot = NULL;
  size_t count = 0;
  if (!sm_game_cache_snapshot(&snapshot, &count)) {
    send_out_of_memory_response(fd);
    return;
  }
  send_snapshot_response(fd, snapshot, count, sizeof(*snapshot), "games",
                         game_to_json);
}

static int operation_http_status(int status) {
  switch (status) {
  case 0:
    return 200;
  case EINVAL:
    return 400;
  case ENOENT:
    return 404;
  case EBUSY:
    return 409;
  case ENOTSUP:
    return 501;
  default:
    return 500;
  }
}

static void handle_mount_operation(int fd, struct json_object *request,
                                   bool mount) {
  const char *title_id = get_title_id(request);
  if (!title_id) {
    send_error_response(fd, 400, EINVAL,
                        "title_id must be a valid PS4 or PS5 title ID");
    return;
  }

  int status = mount ? sm_shellcore_mount_title_runtime(title_id)
                     : sm_shellcore_unmount_title_runtime(title_id);
  log_debug("  [API] HTTP %s game: title=%s status=%d",
            mount ? "mount" : "unmount", title_id, status);
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
      !add_json_bool(response, "mounted", mount)) {
    json_object_put(response);
    send_out_of_memory_response(fd);
    return;
  }
  (void)send_json_object(fd, 200, response);
  json_object_put(response);
}

static void handle_client(int fd) {
  sm_http_request_t request;
  memset(&request, 0, sizeof(request));
  int http_status = 400;
  const char *error = "invalid HTTP request";
  if (!receive_http_request(fd, &request, &http_status, &error)) {
    send_error_response(fd, http_status,
                        http_status == 500 ? ENOMEM : EPROTO, error);
    return;
  }

  if (request.preflight) {
    send_preflight_response(fd);
    free(request.storage);
    return;
  }

  struct json_object *json = parse_request_json(&request);
  if (!json) {
    send_error_response(fd, 400, EINVAL,
                        "request body must be one valid JSON object");
    free(request.storage);
    return;
  }

  if (strcmp(request.route, SM_API_ROUTE_VERSION) == 0) {
    handle_version(fd);
  } else if (strcmp(request.route, SM_API_ROUTE_IMAGES) == 0) {
    handle_images(fd);
  } else if (strcmp(request.route, SM_API_ROUTE_GAMES) == 0) {
    handle_games(fd);
  } else if (strcmp(request.route, SM_API_ROUTE_MOUNT) == 0) {
    handle_mount_operation(fd, json, true);
  } else if (strcmp(request.route, SM_API_ROUTE_UNMOUNT) == 0) {
    handle_mount_operation(fd, json, false);
  } else {
    send_error_response(fd, 404, ENOENT, "unknown API route");
  }

  json_object_put(json);
  free(request.storage);
}

static void *service_thread_main(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_api.mutex);
  char bind_address[MAX_API_BIND_ADDRESS];
  (void)strlcpy(bind_address, g_api.bind_address, sizeof(bind_address));
  uint16_t port = g_api.port;
  pthread_mutex_unlock(&g_api.mutex);

  int listen_fd = -1;
  uint32_t retry_delay_ms = SM_API_RETRY_INITIAL_MS;
  bool outage_logged = false;
  while (true) {
    if (!wait_until_runtime_awake())
      break;

    if (listen_fd >= 0) {
      pthread_mutex_lock(&g_api.mutex);
      bool owns_listener = g_api.listen_fd == listen_fd;
      pthread_mutex_unlock(&g_api.mutex);
      if (!owns_listener)
        listen_fd = -1;
    }

    if (listen_fd < 0) {
      listen_fd = open_http_listener(bind_address, port);
      if (listen_fd < 0) {
        int open_error = errno;
        if (!outage_logged) {
          log_debug("  [API] HTTP listener unavailable at %s:%u: %s; "
                    "recovery scheduled",
                    bind_address, (unsigned)port, strerror(open_error));
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
        close(listen_fd);
        listen_fd = -1;
        if (stopping)
          break;
        continue;
      }
      g_api.listen_fd = listen_fd;
      pthread_mutex_unlock(&g_api.mutex);

      if (outage_logged) {
        log_debug("  [API] HTTP listener recovered: http://%s:%u/api/v1",
                  bind_address, (unsigned)port);
      } else {
        log_debug("  [API] HTTP/JSON ready: http://%s:%u/api/v1 (v%u)",
                  bind_address, (unsigned)port, SM_API_VERSION);
      }
      outage_logged = false;
      retry_delay_ms = SM_API_RETRY_INITIAL_MS;
    }

    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      int accept_error = errno;
      pthread_mutex_lock(&g_api.mutex);
      bool stopping = g_api.stop_requested;
      bool sleeping = runtime_sleep_mode_active();
      bool owns_listener = g_api.listen_fd == listen_fd;
      pthread_mutex_unlock(&g_api.mutex);
      if (stopping)
        break;
      if (!owns_listener) {
        listen_fd = -1;
        continue;
      }
      if (sleeping)
        continue;
      if (accept_error == EINTR || accept_error == ECONNABORTED ||
          accept_error == EPROTO)
        continue;

      if (accept_error_needs_wait(accept_error)) {
        if (!outage_logged) {
          log_debug("  [API] HTTP accept paused: %s; recovery scheduled",
                    strerror(accept_error));
          outage_logged = true;
        }
        if (!wait_before_listener_retry(retry_delay_ms))
          break;
        retry_delay_ms = next_retry_delay_ms(retry_delay_ms);
        continue;
      }

      pthread_mutex_lock(&g_api.mutex);
      if (g_api.listen_fd == listen_fd)
        g_api.listen_fd = -1;
      pthread_mutex_unlock(&g_api.mutex);
      close(listen_fd);
      listen_fd = -1;
      if (!outage_logged) {
        log_debug("  [API] HTTP listener lost: %s; recovery scheduled",
                  strerror(accept_error));
        outage_logged = true;
      }
      if (!wait_before_listener_retry(retry_delay_ms))
        break;
      retry_delay_ms = next_retry_delay_ms(retry_delay_ms);
      continue;
    }

    if (outage_logged) {
      log_debug("  [API] HTTP accept recovered");
      outage_logged = false;
    }
    retry_delay_ms = SM_API_RETRY_INITIAL_MS;

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
    if (g_api.listen_fd != listen_fd)
      listen_fd = -1;
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
  const runtime_config_t *cfg = runtime_config();
  char bind_address[MAX_API_BIND_ADDRESS];
  (void)strlcpy(bind_address, cfg->api_bind_address, sizeof(bind_address));
  uint16_t port = (uint16_t)cfg->api_port;

  pthread_mutex_lock(&g_api.mutex);
  if (g_api.started) {
    pthread_mutex_unlock(&g_api.mutex);
    return true;
  }
  g_api.listen_fd = -1;
  g_api.client_fd = -1;
  g_api.stop_requested = false;
  (void)strlcpy(g_api.bind_address, bind_address,
                sizeof(g_api.bind_address));
  g_api.port = port;
  int rc = pthread_create(&g_api.thread, NULL, service_thread_main, NULL);
  if (rc != 0) {
    g_api.listen_fd = -1;
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
  int fd = g_api.listen_fd;
  int client_fd = g_api.client_fd;
  if (client_fd >= 0)
    (void)shutdown(client_fd, SHUT_RDWR);
  if (fd >= 0) {
    (void)shutdown(fd, SHUT_RDWR);
    close(fd);
  }
  g_api.listen_fd = -1;
  pthread_cond_broadcast(&g_api.cond);
  pthread_mutex_unlock(&g_api.mutex);

  pthread_join(g_api.thread, NULL);

  pthread_mutex_lock(&g_api.mutex);
  g_api.started = false;
  g_api.stop_requested = false;
  g_api.client_fd = -1;
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

  if (active) {
    if (g_api.listen_fd >= 0) {
      (void)shutdown(g_api.listen_fd, SHUT_RDWR);
      close(g_api.listen_fd);
    }
    g_api.listen_fd = -1;
  }
  pthread_cond_broadcast(&g_api.cond);
  pthread_mutex_unlock(&g_api.mutex);
}
