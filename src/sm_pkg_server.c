#define _GNU_SOURCE
#include "sm_platform.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>

#include "sm_pkg_server.h"
#include "sm_limits.h"
#include "sm_time.h"

#define SM_PKG_SERVER_MAX_CONNECTIONS 16
#define SM_PKG_SERVER_REQUEST_MAX 8192
#define SM_PKG_SERVER_SEND_CHUNK (1024u * 1024u)
#define SM_PKG_SERVER_IO_TIMEOUT_SECONDS 60
#define SM_PKG_SERVER_COVERAGE_MAX 256

struct sm_pkg_server {
  pthread_mutex_t mutex;
  volatile int running;
  int listen_fd;
  int pkg_fd;
  int connections[SM_PKG_SERVER_MAX_CONNECTIONS];
  int worker_count;
  pthread_t listener;
  char directory[MAX_PATH];
  char attempt[64];
  char basename[MAX_PATH];
  uint16_t port;
  uint64_t total;
  uint64_t last_request_us;
  struct {
    uint64_t start;
    uint64_t end;
  } intervals[SM_PKG_SERVER_COVERAGE_MAX];
  size_t interval_count;
};

typedef struct {
  sm_pkg_server_t *server;
  int fd;
} sm_pkg_server_worker_arg_t;

typedef struct {
  bool get;
  bool head;
  bool keep_alive;
  char target[MAX_PATH];
  char range[128];
  bool has_range;
} sm_pkg_http_request_t;

static bool send_all(int fd, const void *buffer, size_t size) {
  const char *data = buffer;
  while (size > 0) {
    ssize_t sent = send(fd, data, size, 0);
    if (sent < 0 && errno == EINTR)
      continue;
    if (sent <= 0)
      return false;
    data += sent;
    size -= (size_t)sent;
  }
  return true;
}

static int hex_digit(char value) {
  if (value >= '0' && value <= '9')
    return value - '0';
  if (value >= 'a' && value <= 'f')
    return value - 'a' + 10;
  if (value >= 'A' && value <= 'F')
    return value - 'A' + 10;
  return -1;
}

static bool decode_path(const char *encoded, char *decoded, size_t size) {
  size_t used = 0;
  for (size_t i = 0; encoded[i] != '\0'; i++) {
    unsigned char value = (unsigned char)encoded[i];
    if (value == '%') {
      if (encoded[i + 1] == '\0' || encoded[i + 2] == '\0')
        return false;
      int high = hex_digit(encoded[i + 1]);
      int low = hex_digit(encoded[i + 2]);
      if (high < 0 || low < 0)
        return false;
      value = (unsigned char)((high << 4) | low);
      i += 2;
    }
    if (value == '\0' || used + 1 >= size)
      return false;
    decoded[used++] = (char)value;
  }
  decoded[used] = '\0';
  return true;
}

static bool contains_ci(const char *value, const char *needle) {
  size_t needle_size = strlen(needle);
  if (needle_size == 0)
    return true;
  for (const char *cursor = value; cursor && *cursor; cursor++) {
    if (strncasecmp(cursor, needle, needle_size) == 0)
      return true;
  }
  return false;
}

static bool header_value(const char *request, const char *name, char *value,
                         size_t value_size) {
  size_t name_size = strlen(name);
  const char *line = request;
  value[0] = '\0';
  while (line && *line) {
    const char *end = strstr(line, "\r\n");
    size_t length = end ? (size_t)(end - line) : strlen(line);
    if (length > name_size && strncasecmp(line, name, name_size) == 0 &&
        line[name_size] == ':') {
      const char *start = line + name_size + 1;
      const char *limit = line + length;
      while (start < limit && (*start == ' ' || *start == '\t'))
        start++;
      while (limit > start && (limit[-1] == ' ' || limit[-1] == '\t'))
        limit--;
      size_t copy = (size_t)(limit - start);
      if (copy >= value_size)
        copy = value_size - 1;
      memcpy(value, start, copy);
      value[copy] = '\0';
      return true;
    }
    if (!end)
      break;
    line = end + 2;
  }
  return false;
}

static bool parse_request(const char *request, sm_pkg_http_request_t *out) {
  char method[16];
  char version[16];
  char connection[64];
  if (sscanf(request, "%15s %1023s %15s", method, out->target, version) != 3)
    return false;
  out->get = strcasecmp(method, "GET") == 0;
  out->head = strcasecmp(method, "HEAD") == 0;
  if (!out->get && !out->head)
    return false;
  out->keep_alive = strcasecmp(version, "HTTP/1.0") != 0;
  if (header_value(request, "Connection", connection, sizeof(connection))) {
    if (contains_ci(connection, "close"))
      out->keep_alive = false;
    else if (contains_ci(connection, "keep-alive"))
      out->keep_alive = true;
  }
  out->has_range = header_value(request, "Range", out->range,
                                sizeof(out->range));
  return true;
}

static bool parse_range(const char *value, uint64_t total, uint64_t *start,
                        uint64_t *end) {
  const char *dash;
  char *endptr;
  uint64_t first;
  uint64_t last;
  if (total == 0 || strncasecmp(value, "bytes=", 6) != 0)
    return false;
  dash = strchr(value + 6, '-');
  if (!dash || strchr(dash + 1, ',') != NULL)
    return false;
  if (dash == value + 6) {
    uint64_t suffix = strtoull(dash + 1, &endptr, 10);
    if (endptr == dash + 1 || *endptr != '\0' || suffix == 0)
      return false;
    if (suffix > total)
      suffix = total;
    *start = total - suffix;
    *end = total - 1;
    return true;
  }
  first = strtoull(value + 6, &endptr, 10);
  if (endptr != dash || first >= total)
    return false;
  if (dash[1] == '\0') {
    last = total - 1;
  } else {
    last = strtoull(dash + 1, &endptr, 10);
    if (*endptr != '\0' || last < first)
      return false;
    if (last >= total)
      last = total - 1;
  }
  *start = first;
  *end = last;
  return true;
}

static void add_coverage_locked(sm_pkg_server_t *server, uint64_t start,
                                uint64_t end) {
  size_t position = 0;
  if (end < start)
    return;
  while (position < server->interval_count &&
         server->intervals[position].start < start)
    position++;
  if (server->interval_count >= SM_PKG_SERVER_COVERAGE_MAX)
    return;
  for (size_t i = server->interval_count; i > position; i--)
    server->intervals[i] = server->intervals[i - 1];
  server->intervals[position].start = start;
  server->intervals[position].end = end;
  server->interval_count++;
  size_t write = 0;
  for (size_t read = 1; read < server->interval_count; read++) {
    if (server->intervals[read].start <= server->intervals[write].end + 1) {
      if (server->intervals[read].end > server->intervals[write].end)
        server->intervals[write].end = server->intervals[read].end;
    } else {
      server->intervals[++write] = server->intervals[read];
    }
  }
  server->interval_count = write + 1;
}

static uint64_t covered_bytes_locked(const sm_pkg_server_t *server) {
  uint64_t total = 0;
  for (size_t i = 0; i < server->interval_count; i++)
    total += server->intervals[i].end - server->intervals[i].start + 1;
  return total;
}

static bool complete_locked(const sm_pkg_server_t *server) {
  return server->total == 0 ||
         (server->interval_count == 1 && server->intervals[0].start == 0 &&
          server->intervals[0].end == server->total - 1);
}

static int header_404(char *buffer, size_t size, bool keep_alive) {
  return snprintf(buffer, size,
                  "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n"
                  "Connection: %s\r\n\r\n",
                  keep_alive ? "keep-alive" : "close");
}

static int header_416(char *buffer, size_t size, uint64_t total,
                      bool keep_alive) {
  return snprintf(buffer, size,
                  "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Length: 0\r\n"
                  "Content-Range: bytes */%llu\r\nConnection: %s\r\n\r\n",
                  (unsigned long long)total,
                  keep_alive ? "keep-alive" : "close");
}

static int header_200(char *buffer, size_t size, uint64_t length,
                      bool keep_alive) {
  return snprintf(buffer, size,
                  "HTTP/1.1 200 OK\r\nContent-Length: %llu\r\n"
                  "Accept-Ranges: bytes\r\nConnection: %s\r\n\r\n",
                  (unsigned long long)length,
                  keep_alive ? "keep-alive" : "close");
}

static int header_206(char *buffer, size_t size, uint64_t start, uint64_t end,
                      uint64_t total, bool keep_alive) {
  return snprintf(buffer, size,
                  "HTTP/1.1 206 Partial Content\r\nContent-Length: %llu\r\n"
                  "Content-Range: bytes %llu-%llu/%llu\r\n"
                  "Accept-Ranges: bytes\r\nConnection: %s\r\n\r\n",
                  (unsigned long long)(end - start + 1),
                  (unsigned long long)start, (unsigned long long)end,
                  (unsigned long long)total,
                  keep_alive ? "keep-alive" : "close");
}

static bool serve_file(sm_pkg_server_t *server, int fd,
                       const sm_pkg_http_request_t *request) {
  char decoded[MAX_PATH];
  char expected[MAX_PATH + 80];
  char sidecar[MAX_PATH];
  char prefix[80];
  char header[1024];
  uint64_t start = 0;
  uint64_t end;
  bool ranged = false;
  bool invalid_range = false;

  if (!decode_path(request->target, decoded, sizeof(decoded))) {
    int size = header_404(header, sizeof(header), false);
    return size > 0 && send_all(fd, header, (size_t)size) && false;
  }
  pthread_mutex_lock(&server->mutex);
  server->last_request_us = monotonic_time_us();
  snprintf(expected, sizeof(expected), "/%s/%s", server->attempt,
           server->basename);
  snprintf(prefix, sizeof(prefix), "/%s/", server->attempt);
  uint64_t total = server->total;
  pthread_mutex_unlock(&server->mutex);

  if (strcmp(decoded, expected) != 0) {
    if (strncmp(decoded, prefix, strlen(prefix)) != 0) {
      int size = header_404(header, sizeof(header), request->keep_alive);
      return size > 0 && send_all(fd, header, (size_t)size) &&
             request->keep_alive;
    }
    const char *name = decoded + strlen(prefix);
    size_t length = strlen(name);
    if (strchr(name, '/') != NULL || length <= 4 ||
        strcasecmp(name + length - 4, ".crc") != 0) {
      int size = header_404(header, sizeof(header), request->keep_alive);
      return size > 0 && send_all(fd, header, (size_t)size) &&
             request->keep_alive;
    }
    snprintf(sidecar, sizeof(sidecar), "%s/%s", server->directory, name);
    struct stat st;
    if (stat(sidecar, &st) != 0 || !S_ISREG(st.st_mode)) {
      int size = header_404(header, sizeof(header), request->keep_alive);
      return size > 0 && send_all(fd, header, (size_t)size) &&
             request->keep_alive;
    }
    int size = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Length: %llu\r\n"
                        "Connection: close\r\n\r\n",
                        (unsigned long long)st.st_size);
    if (size <= 0 || (size_t)size >= sizeof(header) ||
        !send_all(fd, header, (size_t)size))
      return false;
    if (!request->head) {
      int side_fd = open(sidecar, O_RDONLY);
      if (side_fd < 0)
        return false;
      char *buffer = malloc(SM_PKG_SERVER_SEND_CHUNK);
      bool ok = buffer != NULL;
      while (ok) {
        ssize_t count = read(side_fd, buffer, SM_PKG_SERVER_SEND_CHUNK);
        if (count == 0)
          break;
        if (count < 0 || !send_all(fd, buffer, (size_t)count))
          ok = false;
      }
      free(buffer);
      close(side_fd);
      return false;
    }
    return false;
  }

  if (request->has_range) {
    if (parse_range(request->range, total, &start, &end))
      ranged = true;
    else
      invalid_range = true;
  }
  int header_size;
  if (invalid_range) {
    header_size = header_416(header, sizeof(header), total,
                             request->keep_alive);
  } else if (ranged) {
    header_size = header_206(header, sizeof(header), start, end, total,
                             request->keep_alive);
  } else {
    end = total == 0 ? 0 : total - 1;
    header_size = header_200(header, sizeof(header), total,
                             request->keep_alive);
  }
  if (header_size <= 0 || (size_t)header_size >= sizeof(header) ||
      !send_all(fd, header, (size_t)header_size))
    return false;
  if (invalid_range || request->head || total == 0)
    return request->keep_alive && !invalid_range;

  char *buffer = malloc(SM_PKG_SERVER_SEND_CHUNK);
  if (!buffer)
    return false;
  uint64_t offset = start;
  bool ok = true;
  while (offset <= end) {
    uint64_t remaining = end - offset + 1;
    size_t want = remaining > SM_PKG_SERVER_SEND_CHUNK
                      ? SM_PKG_SERVER_SEND_CHUNK
                      : (size_t)remaining;
    ssize_t count = pread(server->pkg_fd, buffer, want, (off_t)offset);
    if (count <= 0 || !send_all(fd, buffer, (size_t)count)) {
      ok = false;
      break;
    }
    pthread_mutex_lock(&server->mutex);
    add_coverage_locked(server, offset, offset + (uint64_t)count - 1);
    pthread_mutex_unlock(&server->mutex);
    offset += (uint64_t)count;
  }
  free(buffer);
  return ok && request->keep_alive;
}

static void *worker(void *opaque) {
  sm_pkg_server_worker_arg_t *argument = opaque;
  sm_pkg_server_t *server = argument->server;
  int fd = argument->fd;
  int slot = -1;
  free(argument);
  pthread_mutex_lock(&server->mutex);
  for (int i = 0; i < SM_PKG_SERVER_MAX_CONNECTIONS; i++) {
    if (server->connections[i] < 0) {
      server->connections[i] = fd;
      slot = i;
      break;
    }
  }
  pthread_mutex_unlock(&server->mutex);
  if (slot < 0) {
    close(fd);
    __sync_sub_and_fetch(&server->worker_count, 1);
    return NULL;
  }
  struct timeval timeout = {SM_PKG_SERVER_IO_TIMEOUT_SECONDS, 0};
  (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  char request[SM_PKG_SERVER_REQUEST_MAX + 1];
  size_t used = 0;
  while (server->running && used < SM_PKG_SERVER_REQUEST_MAX) {
    ssize_t count = recv(fd, request + used,
                         SM_PKG_SERVER_REQUEST_MAX - used, 0);
    if (count <= 0)
      break;
    used += (size_t)count;
    request[used] = '\0';
    char *end = strstr(request, "\r\n\r\n");
    if (!end)
      continue;
    size_t request_size = (size_t)(end + 4 - request);
    sm_pkg_http_request_t parsed;
    bool keep_alive = false;
    if (parse_request(request, &parsed))
      keep_alive = serve_file(server, fd, &parsed) && parsed.keep_alive;
    else {
      char header[256];
      int header_size = header_404(header, sizeof(header), false);
      if (header_size > 0)
        (void)send_all(fd, header, (size_t)header_size);
    }
    if (!keep_alive)
      break;
    memmove(request, request + request_size, used - request_size);
    used -= request_size;
    request[used] = '\0';
  }

  pthread_mutex_lock(&server->mutex);
  if (slot >= 0)
    server->connections[slot] = -1;
  pthread_mutex_unlock(&server->mutex);
  close(fd);
  __sync_sub_and_fetch(&server->worker_count, 1);
  return NULL;
}

static void *listener(void *opaque) {
  sm_pkg_server_t *server = opaque;
  while (server->running) {
    struct sockaddr_in address;
    socklen_t address_size = sizeof(address);
    int fd = accept(server->listen_fd, (struct sockaddr *)&address,
                    &address_size);
    if (fd < 0) {
      if (server->running)
        continue;
      break;
    }
    int one = 1;
    (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (__sync_add_and_fetch(&server->worker_count, 1) >
        SM_PKG_SERVER_MAX_CONNECTIONS) {
      __sync_sub_and_fetch(&server->worker_count, 1);
      close(fd);
      continue;
    }
    sm_pkg_server_worker_arg_t *argument = malloc(sizeof(*argument));
    if (!argument) {
      __sync_sub_and_fetch(&server->worker_count, 1);
      close(fd);
      continue;
    }
    argument->server = server;
    argument->fd = fd;
    pthread_t thread;
    if (pthread_create(&thread, NULL, worker, argument) != 0) {
      __sync_sub_and_fetch(&server->worker_count, 1);
      free(argument);
      close(fd);
      continue;
    }
    (void)pthread_detach(thread);
  }
  return NULL;
}

int sm_pkg_server_start(sm_pkg_server_t **out, const char *pkg_path,
                        const char *attempt, uint16_t *port_out) {
  struct stat st;
  struct sockaddr_in address;
  socklen_t address_size = sizeof(address);
  sm_pkg_server_t *server;
  if (!out || !pkg_path || !attempt || !port_out ||
      stat(pkg_path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0)
    return EINVAL;
  server = calloc(1, sizeof(*server));
  if (!server)
    return ENOMEM;
  pthread_mutex_init(&server->mutex, NULL);
  server->listen_fd = -1;
  server->pkg_fd = open(pkg_path, O_RDONLY);
  if (server->pkg_fd < 0) {
    free(server);
    return errno;
  }
  server->total = (uint64_t)st.st_size;
  (void)strlcpy(server->attempt, attempt, sizeof(server->attempt));
  (void)strlcpy(server->basename, strrchr(pkg_path, '/')
                                  ? strrchr(pkg_path, '/') + 1
                                  : pkg_path,
                sizeof(server->basename));
  (void)strlcpy(server->directory, pkg_path, sizeof(server->directory));
  char *slash = strrchr(server->directory, '/');
  if (slash)
    *slash = '\0';
  else
    (void)strlcpy(server->directory, ".", sizeof(server->directory));
  for (int i = 0; i < SM_PKG_SERVER_MAX_CONNECTIONS; i++)
    server->connections[i] = -1;
  server->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (server->listen_fd < 0) {
    int error = errno;
    close(server->pkg_fd);
    pthread_mutex_destroy(&server->mutex);
    free(server);
    return error;
  }
  int one = 1;
  (void)setsockopt(server->listen_fd, SOL_SOCKET, SO_REUSEADDR, &one,
                   sizeof(one));
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  if (bind(server->listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
      listen(server->listen_fd, SM_PKG_SERVER_MAX_CONNECTIONS) != 0 ||
      getsockname(server->listen_fd, (struct sockaddr *)&address,
                  &address_size) != 0) {
    int error = errno;
    close(server->listen_fd);
    close(server->pkg_fd);
    pthread_mutex_destroy(&server->mutex);
    free(server);
    return error;
  }
  server->port = ntohs(address.sin_port);
  server->running = true;
  server->last_request_us = monotonic_time_us();
  int thread_result = pthread_create(&server->listener, NULL, listener, server);
  if (thread_result != 0) {
    close(server->listen_fd);
    close(server->pkg_fd);
    pthread_mutex_destroy(&server->mutex);
    free(server);
    return thread_result;
  }
  *out = server;
  *port_out = server->port;
  return 0;
}

uint64_t sm_pkg_server_bytes_served(sm_pkg_server_t *server) {
  if (!server)
    return 0;
  pthread_mutex_lock(&server->mutex);
  uint64_t result = covered_bytes_locked(server);
  pthread_mutex_unlock(&server->mutex);
  return result;
}

uint64_t sm_pkg_server_total(sm_pkg_server_t *server) {
  if (!server)
    return 0;
  pthread_mutex_lock(&server->mutex);
  uint64_t result = server->total;
  pthread_mutex_unlock(&server->mutex);
  return result;
}

bool sm_pkg_server_complete(sm_pkg_server_t *server) {
  if (!server)
    return false;
  pthread_mutex_lock(&server->mutex);
  bool result = complete_locked(server);
  pthread_mutex_unlock(&server->mutex);
  return result;
}

uint64_t sm_pkg_server_last_request_us(sm_pkg_server_t *server) {
  if (!server)
    return 0;
  pthread_mutex_lock(&server->mutex);
  uint64_t result = server->last_request_us;
  pthread_mutex_unlock(&server->mutex);
  return result;
}

void sm_pkg_server_stop(sm_pkg_server_t *server) {
  int listen_fd;
  int connections[SM_PKG_SERVER_MAX_CONNECTIONS];
  if (!server)
    return;

  pthread_mutex_lock(&server->mutex);
  server->running = 0;
  listen_fd = server->listen_fd;
  server->listen_fd = -1;
  pthread_mutex_unlock(&server->mutex);

  if (listen_fd >= 0) {
    (void)shutdown(listen_fd, SHUT_RDWR);
    close(listen_fd);
  }
  // Join the listener before taking the connection snapshot. This prevents
  // a just-accepted worker from escaping the shutdown socket sweep.
  (void)pthread_join(server->listener, NULL);

  pthread_mutex_lock(&server->mutex);
  for (int i = 0; i < SM_PKG_SERVER_MAX_CONNECTIONS; i++) {
    connections[i] = server->connections[i];
    server->connections[i] = -1;
  }
  pthread_mutex_unlock(&server->mutex);
  for (int i = 0; i < SM_PKG_SERVER_MAX_CONNECTIONS; i++) {
    if (connections[i] >= 0)
      (void)shutdown(connections[i], SHUT_RDWR);
  }
  while (__sync_val_compare_and_swap(&server->worker_count, 0, 0) != 0)
    usleep(1000);
  close(server->pkg_fd);
  pthread_mutex_destroy(&server->mutex);
  free(server);
}
