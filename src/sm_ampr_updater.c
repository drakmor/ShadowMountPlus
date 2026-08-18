#include "sm_platform.h"

#include <pthread.h>
#include <sys/time.h>

#include "sm_ampr_updater.h"
#include "sm_config_mount.h"
#include "sm_game_lifecycle.h"
#include "sm_log.h"
#include "sm_runtime.h"
#include "sm_shellcore_service.h"

#define AMPR_FILE_NAME "libSceAmpr.sprx"
#define AMPR_TEMP_FILE_NAME ".libSceAmpr.sprx.download"
#define AMPR_INITIAL_CHECK_DELAY_SECONDS 30u
#define AMPR_UPDATE_INTERVAL_SECONDS (4u * 60u * 60u)
#define AMPR_BUSY_RETRY_SECONDS (10u * 60u)
#define AMPR_NET_POOL_SIZE (32u * 1024u)
#define AMPR_PRIVATE_CA_COUNT 4u
#define AMPR_SSL_POOL_SIZE                                                \
  ((256u + 4u * AMPR_PRIVATE_CA_COUNT) * 1024u)
#define AMPR_HTTP_POOL_SIZE (256u * 1024u)
#define AMPR_IO_BUFFER_SIZE (64u * 1024u)
#define AMPR_MAX_DOWNLOAD_SIZE (16u * 1024u * 1024u)
#define AMPR_HTTP_TIMEOUT_US (30u * 1000u * 1000u)
#define AMPR_HTTP_VERSION_1_1 2

int sceNetInit(void);
int sceNetPoolCreate(const char *name, int size, int flags);
int sceNetPoolDestroy(int pool_id);
int sceSslInit(size_t pool_size);
int sceSslTerm(int ssl_id);
typedef struct {
  char *ptr;
  size_t size;
} SceSslData;
int sceSslLoadCert(int ssl_id, int ca_count, const SceSslData **ca_list,
                   const SceSslData *cert, const SceSslData *private_key);
int sceSslUnloadCert(int ssl_id);
int sceHttp2Init(int net_pool_id, int ssl_id, size_t pool_size,
                 int max_concurrent_requests);
int sceHttp2Term(int http_id);
int sceHttp2CreateTemplate(int http_id, const char *user_agent,
                           int http_version, int auto_proxy_config);
int sceHttp2DeleteTemplate(int template_id);
int sceHttp2CreateRequestWithURL(int template_id, const char *method,
                                 const char *url, uint64_t content_length);
int sceHttp2DeleteRequest(int request_id);
int sceHttp2SendRequest(int request_id, const void *data, size_t size);
int sceHttp2GetStatusCode(int request_id, int *status_code);
int sceHttp2GetAllResponseHeaders(int request_id, char **headers,
                                  size_t *headers_size);
int sceHttp2ReadData(int request_id, void *buffer, size_t size);
int sceHttp2AbortRequest(int request_id);
int sceHttp2SetAutoRedirect(int id, int enabled);
int sceHttp2SetTimeOut(int id, uint32_t timeout_us);
int sceHttp2SetResolveTimeOut(int id, uint32_t timeout_us);
int sceHttp2SetConnectTimeOut(int id, uint32_t timeout_us);
int sceHttp2SetSendTimeOut(int id, uint32_t timeout_us);
int sceHttp2SetRecvTimeOut(int id, uint32_t timeout_us);

#include "sm_ampr_ca.inc"

typedef struct {
  bool enabled;
  uint64_t generation;
  char url[MAX_PATH];
  char emulators_path[MAX_PATH];
} ampr_update_config_t;

typedef enum {
  AMPR_UPDATE_FAILED = 0,
  AMPR_UPDATE_UNCHANGED,
  AMPR_UPDATE_REPLACED,
  AMPR_UPDATE_CANCELLED,
} ampr_update_result_t;

typedef struct {
  pthread_t thread;
  pthread_mutex_t mutex;
  pthread_cond_t cond;
  bool started;
  bool stop_requested;
  bool wake_requested;
  bool net_initialized;
  uint64_t config_generation;
  int active_request_id;
} ampr_updater_state_t;

static ampr_updater_state_t g_ampr_updater = {
    .mutex = PTHREAD_MUTEX_INITIALIZER,
    .cond = PTHREAD_COND_INITIALIZER,
    .active_request_id = -1,
};

static void log_sce_failure(const char *operation, int error) {
  log_debug("  [AMPR] %s failed: 0x%08x", operation, (unsigned int)error);
}

static bool ampr_update_busy(void) {
  return runtime_sleep_mode_active() || sm_game_lifecycle_has_active_game() ||
         sm_shellcore_service_has_prepared_mount();
}

static void snapshot_update_config(ampr_update_config_t *snapshot) {
  const runtime_config_t *cfg = runtime_config();
  snapshot->enabled = cfg->auto_update_ampr_enabled;
  snapshot->generation = g_ampr_updater.config_generation;
  (void)strlcpy(snapshot->url, cfg->ampr_update_url, sizeof(snapshot->url));
  (void)strlcpy(snapshot->emulators_path, cfg->emulators_path,
                sizeof(snapshot->emulators_path));
}

static bool
update_snapshot_is_current_locked(const ampr_update_config_t *snapshot) {
  const runtime_config_t *cfg = runtime_config();
  return !g_ampr_updater.stop_requested &&
         snapshot->generation == g_ampr_updater.config_generation &&
         cfg->auto_update_ampr_enabled &&
         strcmp(snapshot->url, cfg->ampr_update_url) == 0 &&
         strcmp(snapshot->emulators_path, cfg->emulators_path) == 0;
}

static bool update_snapshot_is_current(const ampr_update_config_t *snapshot) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  bool current = update_snapshot_is_current_locked(snapshot);
  pthread_mutex_unlock(&g_ampr_updater.mutex);
  return current;
}

static bool wait_for_update_event_locked(uint32_t seconds,
                                         bool wake_early) {
  struct timespec deadline;
  if (clock_gettime(CLOCK_REALTIME, &deadline) != 0) {
    log_debug("  [AMPR] cannot create update deadline: %s", strerror(errno));
    return false;
  }
  deadline.tv_sec += (time_t)seconds;

  bool wait_failed = false;
  while (!g_ampr_updater.stop_requested) {
    if (wake_early && g_ampr_updater.wake_requested)
      break;
    g_ampr_updater.wake_requested = false;
    int rc = pthread_cond_timedwait(&g_ampr_updater.cond,
                                    &g_ampr_updater.mutex, &deadline);
    if (rc == ETIMEDOUT)
      break;
    if (rc != 0) {
      log_debug("  [AMPR] update wait failed: %s", strerror(rc));
      wait_failed = true;
      break;
    }
  }

  bool keep_running = !g_ampr_updater.stop_requested;
  g_ampr_updater.wake_requested = false;
  return keep_running && !wait_failed;
}

static bool publish_active_request(int request_id,
                                   const ampr_update_config_t *snapshot) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  bool current = update_snapshot_is_current_locked(snapshot);
  if (current)
    g_ampr_updater.active_request_id = request_id;
  pthread_mutex_unlock(&g_ampr_updater.mutex);
  return current;
}

static void clear_active_request(int request_id) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  if (g_ampr_updater.active_request_id == request_id)
    g_ampr_updater.active_request_id = -1;
  pthread_mutex_unlock(&g_ampr_updater.mutex);
}

static bool ensure_net_initialized(void) {
  if (g_ampr_updater.net_initialized)
    return true;

  int rc = sceNetInit();
  if (rc != 0) {
    log_sce_failure("sceNetInit", rc);
    return false;
  }
  g_ampr_updater.net_initialized = true;
  return true;
}

static bool ensure_emulators_directory(const char *path) {
  struct stat st;
  if (stat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      return true;
    errno = ENOTDIR;
    return false;
  }
  if (errno != ENOENT)
    return false;
  if (mkdir(path, 0777) == 0)
    return true;
  if (errno != EEXIST || stat(path, &st) != 0 || !S_ISDIR(st.st_mode))
    return false;
  return true;
}

static bool load_ampr_ca_certificates(int ssl_id) {
  SceSslData certificates[AMPR_PRIVATE_CA_COUNT] = {
      {
          .ptr = g_ampr_ca_sectigo_e46_usertrust,
          .size = sizeof(g_ampr_ca_sectigo_e46_usertrust) - 1u,
      },
      {
          .ptr = g_ampr_ca_isrg_root_yr_by_x1,
          .size = sizeof(g_ampr_ca_isrg_root_yr_by_x1) - 1u,
      },
      {
          .ptr = g_ampr_ca_usertrust_ecc,
          .size = sizeof(g_ampr_ca_usertrust_ecc) - 1u,
      },
      {
          .ptr = g_ampr_ca_isrg_root_x1,
          .size = sizeof(g_ampr_ca_isrg_root_x1) - 1u,
      },
  };
  const SceSslData *ca_list[AMPR_PRIVATE_CA_COUNT];
  for (unsigned i = 0; i < AMPR_PRIVATE_CA_COUNT; ++i)
    ca_list[i] = &certificates[i];
  int rc = sceSslLoadCert(ssl_id, (int)AMPR_PRIVATE_CA_COUNT, ca_list, NULL,
                          NULL);
  if (rc != 0) {
    log_sce_failure("sceSslLoadCert", rc);
    return false;
  }
  return true;
}

static bool write_all(int fd, const void *data, size_t size) {
  const unsigned char *cursor = data;
  while (size > 0) {
    ssize_t written = write(fd, cursor, size);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return false;
    cursor += (size_t)written;
    size -= (size_t)written;
  }
  return true;
}

static bool downloaded_file_is_sprx(const char *path) {
  static const unsigned char self_magic[4] = {0x4f, 0x15, 0x3d, 0x1d};
  static const unsigned char elf_magic[4] = {0x7f, 'E', 'L', 'F'};
  unsigned char magic[sizeof(self_magic)];

  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  ssize_t length;
  do {
    length = read(fd, magic, sizeof(magic));
  } while (length < 0 && errno == EINTR);
  (void)close(fd);
  return length == (ssize_t)sizeof(magic) &&
         (memcmp(magic, self_magic, sizeof(magic)) == 0 ||
          memcmp(magic, elf_magic, sizeof(magic)) == 0);
}

static bool find_http_header(const char *headers, size_t headers_size,
                             const char *name, char *value,
                             size_t value_size) {
  if (!headers || !name || !value || value_size == 0)
    return false;

  size_t name_size = strlen(name);
  size_t offset = 0;
  while (offset < headers_size) {
    size_t line_end = offset;
    while (line_end < headers_size && headers[line_end] != '\0' &&
           headers[line_end] != '\r' && headers[line_end] != '\n') {
      line_end++;
    }

    size_t colon = offset;
    while (colon < line_end && headers[colon] != ':')
      colon++;
    if (colon - offset == name_size && colon < line_end &&
        strncasecmp(headers + offset, name, name_size) == 0) {
      size_t begin = colon + 1u;
      while (begin < line_end &&
             (headers[begin] == ' ' || headers[begin] == '\t')) {
        begin++;
      }
      size_t length = line_end - begin;
      while (length > 0 &&
             (headers[begin + length - 1u] == ' ' ||
              headers[begin + length - 1u] == '\t')) {
        length--;
      }
      if (length == 0 || length >= value_size)
        return false;
      memcpy(value, headers + begin, length);
      value[length] = '\0';
      return true;
    }

    if (line_end < headers_size && headers[line_end] == '\0')
      break;
    offset = line_end;
    while (offset < headers_size &&
           (headers[offset] == '\r' || headers[offset] == '\n')) {
      offset++;
    }
  }
  return false;
}

static bool is_leap_year(unsigned year) {
  return year % 4u == 0 && (year % 100u != 0 || year % 400u == 0);
}

static int month_number(const char *name) {
  static const char *const months[] = {
      "Jan", "Feb", "Mar", "Apr", "May", "Jun",
      "Jul", "Aug", "Sep", "Oct", "Nov", "Dec",
  };
  for (int i = 0; i < 12; ++i) {
    if (strcasecmp(name, months[i]) == 0)
      return i + 1;
  }
  return 0;
}

static int64_t days_from_civil(int year, unsigned month, unsigned day) {
  year -= month <= 2u;
  int era = (year >= 0 ? year : year - 399) / 400;
  unsigned year_of_era = (unsigned)(year - era * 400);
  unsigned adjusted_month =
      (unsigned)((int)month + (month > 2u ? -3 : 9));
  unsigned day_of_year =
      (153u * adjusted_month + 2u) / 5u + day - 1u;
  unsigned day_of_era = year_of_era * 365u + year_of_era / 4u -
                        year_of_era / 100u + day_of_year;
  return (int64_t)era * 146097ll + (int64_t)day_of_era - 719468ll;
}

static bool parse_http_modified_time(const char *value, time_t *time_out) {
  if (!value || !time_out)
    return false;

  char weekday[5] = {0};
  char month_name[4] = {0};
  char zone[4] = {0};
  char extra;
  unsigned day = 0;
  unsigned year = 0;
  unsigned hour = 0;
  unsigned minute = 0;
  unsigned second = 0;
  int fields = sscanf(value, "%4s %u %3s %u %u:%u:%u %3s %c", weekday,
                      &day, month_name, &year, &hour, &minute, &second, zone,
                      &extra);
  int month = month_number(month_name);
  static const unsigned days_per_month[] = {31u, 28u, 31u, 30u, 31u, 30u,
                                             31u, 31u, 30u, 31u, 30u, 31u};
  if (fields != 8 || strlen(weekday) != 4u || weekday[3] != ',' ||
      strcmp(zone, "GMT") != 0 || year < 1970u || year > 9999u ||
      month == 0 || hour > 23u || minute > 59u || second > 59u) {
    return false;
  }

  unsigned max_day = days_per_month[month - 1];
  if (month == 2 && is_leap_year(year))
    max_day++;
  if (day == 0 || day > max_day)
    return false;

  int64_t timestamp =
      days_from_civil((int)year, (unsigned)month, day) * 86400ll +
      (int64_t)hour * 3600ll + (int64_t)minute * 60ll + (int64_t)second;
  time_t converted = (time_t)timestamp;
  if ((int64_t)converted != timestamp)
    return false;
  *time_out = converted;
  return true;
}

static bool try_read_remote_modified_time(int request_id, time_t *time_out) {
  char *headers = NULL;
  size_t headers_size = 0;
  int rc =
      sceHttp2GetAllResponseHeaders(request_id, &headers, &headers_size);
  if (rc != 0) {
    log_sce_failure("sceHttp2GetAllResponseHeaders", rc);
    return false;
  }

  char value[64];
  return find_http_header(headers, headers_size, "Last-Modified", value,
                          sizeof(value)) &&
         parse_http_modified_time(value, time_out);
}

static bool inspect_local_emulator(const char *path, bool *missing_out,
                                   time_t *modified_time_out) {
  *missing_out = false;
  *modified_time_out = 0;
  struct stat st;
  if (lstat(path, &st) != 0) {
    if (errno == ENOENT) {
      *missing_out = true;
      return true;
    }
    return false;
  }
  if (!S_ISREG(st.st_mode)) {
    errno = EINVAL;
    return false;
  }
  *modified_time_out = st.st_mtime;
  return true;
}

static bool set_file_modified_time(const char *path, time_t modified_time) {
  struct timeval times[2] = {
      {.tv_sec = modified_time, .tv_usec = 0},
      {.tv_sec = modified_time, .tv_usec = 0},
  };
  return utimes(path, times) == 0;
}

static bool regular_files_equal(const char *first, const char *second,
                                bool *equal_out) {
  *equal_out = false;

  struct stat first_st;
  if (lstat(first, &first_st) != 0) {
    if (errno == ENOENT)
      return true;
    return false;
  }
  if (!S_ISREG(first_st.st_mode)) {
    errno = EINVAL;
    return false;
  }

  struct stat second_st;
  if (lstat(second, &second_st) != 0)
    return false;
  if (!S_ISREG(second_st.st_mode)) {
    errno = EINVAL;
    return false;
  }
  if (first_st.st_size != second_st.st_size)
    return true;

  int first_fd = open(first, O_RDONLY);
  if (first_fd < 0)
    return false;
  int second_fd = open(second, O_RDONLY);
  if (second_fd < 0) {
    int saved_errno = errno;
    (void)close(first_fd);
    errno = saved_errno;
    return false;
  }

  unsigned char *buffers = malloc(AMPR_IO_BUFFER_SIZE * 2u);
  if (!buffers) {
    (void)close(second_fd);
    (void)close(first_fd);
    errno = ENOMEM;
    return false;
  }

  bool io_ok = true;
  bool equal = true;
  while (io_ok && equal) {
    ssize_t first_size;
    do {
      first_size = read(first_fd, buffers, AMPR_IO_BUFFER_SIZE);
    } while (first_size < 0 && errno == EINTR);
    if (first_size < 0) {
      io_ok = false;
      break;
    }

    size_t received = 0;
    while (received < (size_t)first_size) {
      ssize_t second_size =
          read(second_fd, buffers + AMPR_IO_BUFFER_SIZE + received,
               (size_t)first_size - received);
      if (second_size < 0 && errno == EINTR)
        continue;
      if (second_size <= 0) {
        if (second_size == 0)
          errno = EIO;
        io_ok = false;
        break;
      }
      received += (size_t)second_size;
    }
    if (!io_ok ||
        memcmp(buffers, buffers + AMPR_IO_BUFFER_SIZE,
               (size_t)first_size) != 0) {
      equal = false;
      break;
    }
    if (first_size == 0)
      break;
  }

  int saved_errno = io_ok ? 0 : errno;
  free(buffers);
  (void)close(second_fd);
  (void)close(first_fd);
  if (!io_ok)
    errno = saved_errno;
  *equal_out = equal;
  return io_ok;
}

static bool configure_http_template(int template_id) {
  int rc = sceHttp2SetAutoRedirect(template_id, 1);
  if (rc != 0) {
    log_sce_failure("sceHttp2SetAutoRedirect", rc);
    return false;
  }

  int timeout_error = 0;
  rc = sceHttp2SetTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (rc != 0)
    timeout_error = rc;
  rc = sceHttp2SetResolveTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttp2SetConnectTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttp2SetSendTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttp2SetRecvTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  if (timeout_error != 0) {
    log_sce_failure("HTTP timeout configuration", timeout_error);
    return false;
  }
  return true;
}

static bool create_and_send_request(
    int template_id, const char *method, const ampr_update_config_t *snapshot,
    int *request_id_out, int *status_code_out,
    ampr_update_result_t *result_out) {
  int request_id = sceHttp2CreateRequestWithURL(template_id, method,
                                                snapshot->url, 0);
  if (request_id < 0) {
    log_sce_failure("sceHttp2CreateRequestWithURL", request_id);
    return false;
  }
  *request_id_out = request_id;

  if (!publish_active_request(request_id, snapshot)) {
    *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }

  int rc = sceHttp2SendRequest(request_id, NULL, 0);
  if (rc != 0) {
    if (update_snapshot_is_current(snapshot))
      log_sce_failure("sceHttp2SendRequest", rc);
    else
      *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }

  rc = sceHttp2GetStatusCode(request_id, status_code_out);
  if (rc != 0) {
    if (update_snapshot_is_current(snapshot))
      log_sce_failure("sceHttp2GetStatusCode", rc);
    else
      *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }
  return true;
}

static ampr_update_result_t
check_and_update_ampr(const ampr_update_config_t *snapshot) {
  int net_pool_id = -1;
  int ssl_id = -1;
  int http_id = -1;
  int template_id = -1;
  int request_id = -1;
  int output_fd = -1;
  unsigned char *buffer = NULL;
  bool temp_exists = false;
  bool ca_certificates_loaded = false;
  bool destination_missing = false;
  time_t local_modified_time = 0;
  time_t remote_modified_time = 0;
  ampr_update_result_t result = AMPR_UPDATE_FAILED;
  char destination[MAX_PATH];
  char temp_path[MAX_PATH];

  int written = snprintf(destination, sizeof(destination), "%s/%s",
                         snapshot->emulators_path, AMPR_FILE_NAME);
  int temp_written = snprintf(temp_path, sizeof(temp_path), "%s/%s",
                              snapshot->emulators_path, AMPR_TEMP_FILE_NAME);
  if (written <= 0 || (size_t)written >= sizeof(destination) ||
      temp_written <= 0 || (size_t)temp_written >= sizeof(temp_path)) {
    log_debug("  [AMPR] emulator destination path is too long: %s",
              snapshot->emulators_path);
    return AMPR_UPDATE_FAILED;
  }

  if (!inspect_local_emulator(destination, &destination_missing,
                              &local_modified_time)) {
    log_debug("  [AMPR] cannot inspect %s: %s", destination,
              strerror(errno));
    return AMPR_UPDATE_FAILED;
  }

  if (!ensure_net_initialized())
    return AMPR_UPDATE_FAILED;

  net_pool_id = sceNetPoolCreate("sm_ampr", AMPR_NET_POOL_SIZE, 0);
  if (net_pool_id < 0) {
    log_sce_failure("sceNetPoolCreate", net_pool_id);
    goto cleanup;
  }
  ssl_id = sceSslInit(AMPR_SSL_POOL_SIZE);
  if (ssl_id < 0) {
    log_sce_failure("sceSslInit", ssl_id);
    goto cleanup;
  }
  if (strncasecmp(snapshot->url, "https://", 8u) == 0) {
    if (!load_ampr_ca_certificates(ssl_id))
      goto cleanup;
    ca_certificates_loaded = true;
  }
  http_id = sceHttp2Init(net_pool_id, ssl_id, AMPR_HTTP_POOL_SIZE, 1);
  if (http_id < 0) {
    log_sce_failure("sceHttp2Init", http_id);
    goto cleanup;
  }
  template_id = sceHttp2CreateTemplate(http_id, "ShadowMountPlus/1.0",
                                       AMPR_HTTP_VERSION_1_1, 1);
  if (template_id < 0) {
    log_sce_failure("sceHttp2CreateTemplate", template_id);
    goto cleanup;
  }
  if (!configure_http_template(template_id))
    goto cleanup;

  int status_code = 0;
  if (!destination_missing) {
    if (!create_and_send_request(template_id, "HEAD", snapshot, &request_id,
                                 &status_code, &result)) {
      goto cleanup;
    }
    if (status_code != 200) {
      log_debug("  [AMPR] metadata check returned HTTP %d: %s", status_code,
                snapshot->url);
      goto cleanup;
    }
    if (!try_read_remote_modified_time(request_id, &remote_modified_time)) {
      log_debug("  [AMPR] response has no valid Last-Modified header");
      goto cleanup;
    }
    if (remote_modified_time <= local_modified_time) {
      result = AMPR_UPDATE_UNCHANGED;
      goto cleanup;
    }

    clear_active_request(request_id);
    (void)sceHttp2DeleteRequest(request_id);
    request_id = -1;
  } else {
    log_debug("  [AMPR] emulator is missing; downloading: %s", destination);
  }

  if (!update_snapshot_is_current(snapshot)) {
    result = AMPR_UPDATE_CANCELLED;
    goto cleanup;
  }
  if (!ensure_emulators_directory(snapshot->emulators_path)) {
    log_debug("  [AMPR] cannot use emulator folder %s: %s",
              snapshot->emulators_path, strerror(errno));
    goto cleanup;
  }

  status_code = 0;
  if (!create_and_send_request(template_id, "GET", snapshot, &request_id,
                               &status_code, &result)) {
    goto cleanup;
  }
  if (status_code != 200) {
    log_debug("  [AMPR] update download returned HTTP %d: %s", status_code,
              snapshot->url);
    goto cleanup;
  }

  if (destination_missing &&
      !try_read_remote_modified_time(request_id, &remote_modified_time)) {
    remote_modified_time = time(NULL);
    if (remote_modified_time == (time_t)-1) {
      log_debug("  [AMPR] cannot determine download modification time");
      goto cleanup;
    }
  }

  (void)unlink(temp_path);
  output_fd = open(temp_path, O_WRONLY | O_CREAT | O_EXCL, 0777);
  if (output_fd < 0) {
    log_debug("  [AMPR] cannot create temporary download %s: %s", temp_path,
              strerror(errno));
    goto cleanup;
  }
  temp_exists = true;

  buffer = malloc(AMPR_IO_BUFFER_SIZE);
  if (!buffer) {
    log_debug("  [AMPR] download buffer allocation failed");
    goto cleanup;
  }

  size_t total_size = 0;
  for (;;) {
    if (!update_snapshot_is_current(snapshot)) {
      result = AMPR_UPDATE_CANCELLED;
      goto cleanup;
    }
    int received = sceHttp2ReadData(request_id, buffer, AMPR_IO_BUFFER_SIZE);
    if (received < 0) {
      if (update_snapshot_is_current(snapshot))
        log_sce_failure("sceHttp2ReadData", received);
      else
        result = AMPR_UPDATE_CANCELLED;
      goto cleanup;
    }
    if (received == 0)
      break;
    if ((size_t)received > AMPR_MAX_DOWNLOAD_SIZE - total_size) {
      log_debug("  [AMPR] update exceeds the %u-byte size limit",
                (unsigned)AMPR_MAX_DOWNLOAD_SIZE);
      goto cleanup;
    }
    if (!write_all(output_fd, buffer, (size_t)received)) {
      log_debug("  [AMPR] write failed for %s: %s", temp_path,
                strerror(errno));
      goto cleanup;
    }
    total_size += (size_t)received;
  }

  if (total_size < 4u || fsync(output_fd) != 0) {
    log_debug("  [AMPR] incomplete download for %s: %s", temp_path,
              total_size < 4u ? "file is empty" : strerror(errno));
    goto cleanup;
  }
  if (close(output_fd) != 0) {
    output_fd = -1;
    log_debug("  [AMPR] close failed for %s: %s", temp_path, strerror(errno));
    goto cleanup;
  }
  output_fd = -1;
  free(buffer);
  buffer = NULL;

  if (!set_file_modified_time(temp_path, remote_modified_time)) {
    log_debug("  [AMPR] cannot preserve server modification time for %s: %s",
              temp_path, strerror(errno));
    goto cleanup;
  }

  if (!downloaded_file_is_sprx(temp_path)) {
    log_debug("  [AMPR] downloaded file has an invalid SELF/ELF header");
    goto cleanup;
  }

  bool unchanged = false;
  if (!regular_files_equal(destination, temp_path, &unchanged)) {
    log_debug("  [AMPR] cannot compare %s: %s", destination, strerror(errno));
    goto cleanup;
  }
  if (!update_snapshot_is_current(snapshot) || ampr_update_busy()) {
    result = AMPR_UPDATE_CANCELLED;
    goto cleanup;
  }
  if (unchanged) {
    if (!set_file_modified_time(destination, remote_modified_time)) {
      log_debug("  [AMPR] cannot update modification time for %s: %s",
                destination, strerror(errno));
      goto cleanup;
    }
    result = AMPR_UPDATE_UNCHANGED;
    goto cleanup;
  }
  if (rename(temp_path, destination) != 0) {
    log_debug("  [AMPR] cannot replace %s: %s", destination, strerror(errno));
    goto cleanup;
  }
  temp_exists = false;
  result = AMPR_UPDATE_REPLACED;

cleanup:
  free(buffer);
  if (output_fd >= 0)
    (void)close(output_fd);
  if (request_id >= 0) {
    clear_active_request(request_id);
    (void)sceHttp2DeleteRequest(request_id);
  }
  if (template_id >= 0)
    (void)sceHttp2DeleteTemplate(template_id);
  if (http_id >= 0)
    (void)sceHttp2Term(http_id);
  if (ca_certificates_loaded)
    (void)sceSslUnloadCert(ssl_id);
  if (ssl_id >= 0)
    (void)sceSslTerm(ssl_id);
  if (net_pool_id >= 0)
    (void)sceNetPoolDestroy(net_pool_id);
  if (temp_exists)
    (void)unlink(temp_path);
  return result;
}

static void *ampr_updater_thread_main(void *unused) {
  (void)unused;

  pthread_mutex_lock(&g_ampr_updater.mutex);
  g_ampr_updater.wake_requested = false;
  if (!wait_for_update_event_locked(AMPR_INITIAL_CHECK_DELAY_SECONDS, false)) {
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    return NULL;
  }

  while (!g_ampr_updater.stop_requested) {
    ampr_update_config_t snapshot;
    snapshot_update_config(&snapshot);
    g_ampr_updater.wake_requested = false;

    if (!snapshot.enabled) {
      while (!g_ampr_updater.stop_requested &&
             !g_ampr_updater.wake_requested) {
        int rc =
            pthread_cond_wait(&g_ampr_updater.cond, &g_ampr_updater.mutex);
        if (rc != 0) {
          log_debug("  [AMPR] disabled wait failed: %s", strerror(rc));
          pthread_mutex_unlock(&g_ampr_updater.mutex);
          return NULL;
        }
      }
      continue;
    }

    if (ampr_update_busy()) {
      if (!wait_for_update_event_locked(AMPR_BUSY_RETRY_SECONDS, true))
        break;
      continue;
    }

    pthread_mutex_unlock(&g_ampr_updater.mutex);
    log_debug("  [AMPR] checking emulator modification time: %s",
              snapshot.url);
    ampr_update_result_t result = check_and_update_ampr(&snapshot);
    if (result == AMPR_UPDATE_REPLACED) {
      log_debug("  [AMPR] emulator updated: %s/%s", snapshot.emulators_path,
                AMPR_FILE_NAME);
      notify_system_l10n(SM_L10N_AMPR_EMULATOR_UPDATED);
    } else if (result == AMPR_UPDATE_UNCHANGED) {
      log_debug("  [AMPR] emulator is already current");
    }
    pthread_mutex_lock(&g_ampr_updater.mutex);

    if (!g_ampr_updater.stop_requested &&
        !g_ampr_updater.wake_requested &&
        !wait_for_update_event_locked(AMPR_UPDATE_INTERVAL_SECONDS, true)) {
      break;
    }
  }
  pthread_mutex_unlock(&g_ampr_updater.mutex);
  return NULL;
}

bool sm_ampr_updater_start(void) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  if (g_ampr_updater.started) {
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    return true;
  }

  g_ampr_updater.stop_requested = false;
  g_ampr_updater.wake_requested = true;
  g_ampr_updater.config_generation++;
  g_ampr_updater.active_request_id = -1;
  int rc = pthread_create(&g_ampr_updater.thread, NULL,
                          ampr_updater_thread_main, NULL);
  if (rc != 0) {
    g_ampr_updater.wake_requested = false;
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    errno = rc;
    return false;
  }
  g_ampr_updater.started = true;
  pthread_mutex_unlock(&g_ampr_updater.mutex);
  return true;
}

void sm_ampr_updater_stop(void) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  if (!g_ampr_updater.started) {
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    return;
  }
  g_ampr_updater.stop_requested = true;
  g_ampr_updater.wake_requested = true;
  g_ampr_updater.config_generation++;
  if (g_ampr_updater.active_request_id >= 0)
    (void)sceHttp2AbortRequest(g_ampr_updater.active_request_id);
  pthread_cond_broadcast(&g_ampr_updater.cond);
  pthread_mutex_unlock(&g_ampr_updater.mutex);

  (void)pthread_join(g_ampr_updater.thread, NULL);

  pthread_mutex_lock(&g_ampr_updater.mutex);
  g_ampr_updater.started = false;
  g_ampr_updater.stop_requested = false;
  g_ampr_updater.wake_requested = false;
  g_ampr_updater.active_request_id = -1;
  pthread_mutex_unlock(&g_ampr_updater.mutex);
}

void sm_ampr_updater_on_config_reload(const runtime_config_t *old_cfg,
                                      const runtime_config_t *new_cfg) {
  if (!old_cfg || !new_cfg ||
      (old_cfg->auto_update_ampr_enabled ==
           new_cfg->auto_update_ampr_enabled &&
       strcmp(old_cfg->ampr_update_url, new_cfg->ampr_update_url) == 0 &&
       strcmp(old_cfg->emulators_path, new_cfg->emulators_path) == 0)) {
    return;
  }

  pthread_mutex_lock(&g_ampr_updater.mutex);
  g_ampr_updater.config_generation++;
  g_ampr_updater.wake_requested = true;
  if (g_ampr_updater.active_request_id >= 0)
    (void)sceHttp2AbortRequest(g_ampr_updater.active_request_id);
  pthread_cond_broadcast(&g_ampr_updater.cond);
  pthread_mutex_unlock(&g_ampr_updater.mutex);
}
