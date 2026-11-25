#include "sm_platform.h"

#include <json-c/json.h>
#include <pthread.h>
#include <sys/time.h>
#include <zlib.h>

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
#define AMPR_HTTP_HEADER_MAX_SIZE (32u * 1024u)
#define AMPR_IO_BUFFER_SIZE (64u * 1024u)
#define AMPR_MAX_DOWNLOAD_SIZE (16u * 1024u * 1024u)
#define AMPR_HTTP_TIMEOUT_US (30u * 1000u * 1000u)
#define AMPR_HTTP_VERSION_1_1 2
#define DLC_EMU_MAX_RELEASE_JSON_SIZE (1024u * 1024u)
#define DLC_EMU_MAX_FILE_SIZE (4u * 1024u * 1024u)
#define DLC_EMU_FILE_COUNT 3u
// Release zips carry log/ and nolog/ builds; games get the quiet one.
#define DLC_EMU_ZIP_DIRECTORY "nolog/"

static const char *const g_dlc_emu_files[DLC_EMU_FILE_COUNT] = {
    "libSceAppContent.sprx", "libSceNpEntitlementAccess.sprx",
    "libSceGameUpdate.sprx"};

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
int sceHttpInit(int net_pool_id, int ssl_id, size_t pool_size);
int sceHttpTerm(int http_id);
int sceHttpCreateTemplate(int http_id, const char *user_agent,
                           int http_version, int auto_proxy_config);
int sceHttpDeleteTemplate(int template_id);
int sceHttpCreateConnectionWithURL(int template_id, const char *url,
                                     int enable_keepalive);
int sceHttpDeleteConnection(int connection_id);
int sceHttpCreateRequestWithURL2(int connection_id, const char *method,
                                 const char *url, uint64_t content_length);
int sceHttpDeleteRequest(int request_id);
int sceHttpSendRequest(int request_id, const void *data, size_t size);
int sceHttpGetStatusCode(int request_id, int *status_code);
int sceHttpGetAllResponseHeaders(int request_id, char **headers,
                                  size_t *headers_size);
int sceHttpReadData(int request_id, void *buffer, size_t size);
int sceHttpAbortRequest(int request_id);
int sceHttpSetAutoRedirect(int id, int enabled);
int sceHttpSetResponseHeaderMaxSize(int id, size_t size);
int sceHttpSetResolveTimeOut(int id, uint32_t timeout_us);
int sceHttpSetConnectTimeOut(int id, uint32_t timeout_us);
int sceHttpSetSendTimeOut(int id, uint32_t timeout_us);
int sceHttpSetRecvTimeOut(int id, uint32_t timeout_us);

#include "sm_ampr_ca.inc"

typedef struct {
  bool enabled;
  bool dlc_emu_enabled;
  uint64_t generation;
  char url[MAX_PATH];
  char dlc_emu_url[MAX_PATH];
  char emulators_path[MAX_PATH];
} ampr_update_config_t;

typedef struct {
  int net_pool_id;
  int ssl_id;
  int http_id;
  int template_id;
  bool ca_certificates_loaded;
} http_session_t;

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
  const runtime_config_t cfg = runtime_config();
  snapshot->enabled = cfg.auto_update_ampr_enabled;
  snapshot->dlc_emu_enabled = cfg.auto_update_dlc_emu_enabled;
  snapshot->generation = g_ampr_updater.config_generation;
  (void)strlcpy(snapshot->url, cfg.ampr_update_url, sizeof(snapshot->url));
  (void)strlcpy(snapshot->dlc_emu_url, cfg.dlc_emu_update_url,
                sizeof(snapshot->dlc_emu_url));
  (void)strlcpy(snapshot->emulators_path, cfg.emulators_path,
                sizeof(snapshot->emulators_path));
}

static bool
update_snapshot_is_current_locked(const ampr_update_config_t *snapshot) {
  const runtime_config_t cfg = runtime_config();
  return !g_ampr_updater.stop_requested && !runtime_sleep_mode_active() &&
         snapshot->generation == g_ampr_updater.config_generation &&
         cfg.auto_update_ampr_enabled == snapshot->enabled &&
         cfg.auto_update_dlc_emu_enabled == snapshot->dlc_emu_enabled &&
         strcmp(snapshot->url, cfg.ampr_update_url) == 0 &&
         strcmp(snapshot->dlc_emu_url, cfg.dlc_emu_update_url) == 0 &&
         strcmp(snapshot->emulators_path, cfg.emulators_path) == 0;
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
    if (runtime_sleep_mode_active())
      break;
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

// Keep the ID pinned until abort completes: the worker clears it before delete.
// Call with g_ampr_updater.mutex held to exclude delete and ID reuse.
static void abort_active_request_locked(void) {
  int request_id = g_ampr_updater.active_request_id;
  if (request_id < 0)
    return;
  g_ampr_updater.active_request_id = -1;
  (void)sceHttpAbortRequest(request_id);
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
      sceHttpGetAllResponseHeaders(request_id, &headers, &headers_size);
  if (rc != 0) {
    log_sce_failure("sceHttpGetAllResponseHeaders", rc);
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
  int rc = sceHttpSetAutoRedirect(template_id, 1);
  if (rc != 0) {
    log_sce_failure("sceHttpSetAutoRedirect", rc);
    return false;
  }

  // GitHub redirect responses include large CSP and signed Location headers.
  rc = sceHttpSetResponseHeaderMaxSize(template_id, AMPR_HTTP_HEADER_MAX_SIZE);
  if (rc != 0) {
    log_sce_failure("sceHttpSetResponseHeaderMaxSize", rc);
    return false;
  }

  int timeout_error = 0;
  rc = sceHttpSetResolveTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttpSetConnectTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttpSetSendTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  rc = sceHttpSetRecvTimeOut(template_id, AMPR_HTTP_TIMEOUT_US);
  if (timeout_error == 0 && rc != 0)
    timeout_error = rc;
  if (timeout_error != 0) {
    log_sce_failure("HTTP timeout configuration", timeout_error);
    return false;
  }
  return true;
}

static bool create_and_send_request(
    int connection_id, const char *method, const char *url,
    const ampr_update_config_t *snapshot,
    int *request_id_out, int *status_code_out,
    ampr_update_result_t *result_out) {
  int request_id = sceHttpCreateRequestWithURL2(connection_id, method, url, 0);
  if (request_id < 0) {
    log_sce_failure("sceHttpCreateRequestWithURL2", request_id);
    return false;
  }
  *request_id_out = request_id;

  if (!publish_active_request(request_id, snapshot)) {
    *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }

  int rc = sceHttpSendRequest(request_id, NULL, 0);
  if (rc != 0) {
    if (update_snapshot_is_current(snapshot))
      log_sce_failure("sceHttpSendRequest", rc);
    else
      *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }

  rc = sceHttpGetStatusCode(request_id, status_code_out);
  if (rc != 0) {
    if (update_snapshot_is_current(snapshot))
      log_sce_failure("sceHttpGetStatusCode", rc);
    else
      *result_out = AMPR_UPDATE_CANCELLED;
    return false;
  }
  return true;
}

static void close_http_session(http_session_t *session) {
  if (session->template_id >= 0)
    (void)sceHttpDeleteTemplate(session->template_id);
  if (session->http_id >= 0)
    (void)sceHttpTerm(session->http_id);
  if (session->ca_certificates_loaded)
    (void)sceSslUnloadCert(session->ssl_id);
  if (session->ssl_id >= 0)
    (void)sceSslTerm(session->ssl_id);
  if (session->net_pool_id >= 0)
    (void)sceNetPoolDestroy(session->net_pool_id);
  session->net_pool_id = session->ssl_id = session->http_id =
      session->template_id = -1;
  session->ca_certificates_loaded = false;
}

static bool open_http_session(http_session_t *session, bool load_ca) {
  session->net_pool_id = session->ssl_id = session->http_id =
      session->template_id = -1;
  session->ca_certificates_loaded = false;
  if (!ensure_net_initialized())
    return false;

  session->net_pool_id = sceNetPoolCreate("sm_ampr", AMPR_NET_POOL_SIZE, 0);
  if (session->net_pool_id < 0) {
    log_sce_failure("sceNetPoolCreate", session->net_pool_id);
    return false;
  }
  session->ssl_id = sceSslInit(AMPR_SSL_POOL_SIZE);
  if (session->ssl_id < 0) {
    log_sce_failure("sceSslInit", session->ssl_id);
    return false;
  }
  if (load_ca) {
    if (!load_ampr_ca_certificates(session->ssl_id))
      return false;
    session->ca_certificates_loaded = true;
  }
  // Blocking HTTP/1.1 avoids libhttp2's internal event-thread affinity.
  session->http_id =
      sceHttpInit(session->net_pool_id, session->ssl_id, AMPR_HTTP_POOL_SIZE);
  if (session->http_id < 0) {
    log_sce_failure("sceHttpInit", session->http_id);
    return false;
  }
  session->template_id = sceHttpCreateTemplate(
      session->http_id, "ShadowMountPlus/1.0", AMPR_HTTP_VERSION_1_1, 1);
  if (session->template_id < 0) {
    log_sce_failure("sceHttpCreateTemplate", session->template_id);
    return false;
  }
  return configure_http_template(session->template_id);
}

static ampr_update_result_t
check_and_update_ampr(const ampr_update_config_t *snapshot) {
  http_session_t session = {-1, -1, -1, -1, false};
  int connection_id = -1;
  int request_id = -1;
  int output_fd = -1;
  unsigned char *buffer = NULL;
  bool temp_exists = false;
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

  if (!open_http_session(&session,
                         strncasecmp(snapshot->url, "https://", 8u) == 0))
    goto cleanup;
  connection_id =
      sceHttpCreateConnectionWithURL(session.template_id, snapshot->url, 1);
  if (connection_id < 0) {
    log_sce_failure("sceHttpCreateConnectionWithURL", connection_id);
    goto cleanup;
  }

  int status_code = 0;
  if (!destination_missing) {
    if (!create_and_send_request(connection_id, "HEAD", snapshot->url,
                                 snapshot, &request_id, &status_code,
                                 &result)) {
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
    (void)sceHttpDeleteRequest(request_id);
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
  if (!create_and_send_request(connection_id, "GET", snapshot->url, snapshot,
                               &request_id, &status_code, &result)) {
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
    int received = sceHttpReadData(request_id, buffer, AMPR_IO_BUFFER_SIZE);
    if (received < 0) {
      if (update_snapshot_is_current(snapshot))
        log_sce_failure("sceHttpReadData", received);
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
    (void)sceHttpDeleteRequest(request_id);
  }
  if (connection_id >= 0)
    (void)sceHttpDeleteConnection(connection_id);
  close_http_session(&session);
  if (temp_exists)
    (void)unlink(temp_path);
  return result;
}

static bool http_get_body(const http_session_t *session, const char *url,
                          const ampr_update_config_t *snapshot, size_t max_size,
                          unsigned char **body_out, size_t *size_out,
                          ampr_update_result_t *result_out) {
  *body_out = NULL;
  *size_out = 0;
  int connection_id =
      sceHttpCreateConnectionWithURL(session->template_id, url, 1);
  if (connection_id < 0) {
    log_sce_failure("sceHttpCreateConnectionWithURL", connection_id);
    return false;
  }

  int request_id = -1;
  int status_code = 0;
  unsigned char *body = NULL;
  size_t size = 0;
  size_t capacity = 0;
  bool ok = false;
  if (!create_and_send_request(connection_id, "GET", url, snapshot,
                               &request_id, &status_code, result_out))
    goto done;
  if (status_code != 200) {
    log_debug("  [DLC_EMU] download returned HTTP %d: %s", status_code, url);
    goto done;
  }
  for (;;) {
    if (!update_snapshot_is_current(snapshot)) {
      *result_out = AMPR_UPDATE_CANCELLED;
      goto done;
    }
    if (size == capacity) {
      if (capacity >= max_size) {
        log_debug("  [DLC_EMU] download exceeds the %zu-byte size limit: %s",
                  max_size, url);
        goto done;
      }
      size_t next = capacity ? capacity * 2u : AMPR_IO_BUFFER_SIZE;
      if (next > max_size)
        next = max_size;
      unsigned char *grown = realloc(body, next);
      if (!grown) {
        log_debug("  [DLC_EMU] download buffer allocation failed");
        goto done;
      }
      body = grown;
      capacity = next;
    }
    int received = sceHttpReadData(request_id, body + size, capacity - size);
    if (received < 0) {
      if (update_snapshot_is_current(snapshot))
        log_sce_failure("sceHttpReadData", received);
      else
        *result_out = AMPR_UPDATE_CANCELLED;
      goto done;
    }
    if (received == 0)
      break;
    size += (size_t)received;
  }
  ok = size > 0;

done:
  if (request_id >= 0) {
    clear_active_request(request_id);
    (void)sceHttpDeleteRequest(request_id);
  }
  (void)sceHttpDeleteConnection(connection_id);
  if (ok) {
    *body_out = body;
    *size_out = size;
  } else {
    free(body);
  }
  return ok;
}

static bool parse_iso8601_utc(const char *value, time_t *time_out) {
  unsigned year, month, day, hour, minute, second;
  char zone = 0;
  char extra;
  if (sscanf(value, "%4u-%2u-%2uT%2u:%2u:%2u%c%c", &year, &month, &day, &hour,
             &minute, &second, &zone, &extra) != 7 ||
      zone != 'Z' || year < 1970u || month < 1u || month > 12u || day < 1u ||
      day > 31u || hour > 23u || minute > 59u || second > 59u)
    return false;
  int64_t timestamp = days_from_civil((int)year, month, day) * 86400ll +
                      (int64_t)hour * 3600ll + (int64_t)minute * 60ll +
                      (int64_t)second;
  time_t converted = (time_t)timestamp;
  if ((int64_t)converted != timestamp)
    return false;
  *time_out = converted;
  return true;
}

static const char *json_string_field(struct json_object *object,
                                     const char *key) {
  struct json_object *value = NULL;
  if (!json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string))
    return NULL;
  return json_object_get_string(value);
}

// The first .zip asset of a GitHub release and the asset's update time.
static bool parse_dlc_emu_release(const unsigned char *json, size_t size,
                                  char tag[64], char zip_url[MAX_PATH],
                                  time_t *asset_time_out) {
  struct json_tokener *tokener = json_tokener_new();
  if (!tokener)
    return false;
  struct json_object *root =
      json_tokener_parse_ex(tokener, (const char *)json, (int)size);
  json_tokener_free(tokener);
  if (!root)
    return false;

  bool found = false;
  const char *tag_name = json_string_field(root, "tag_name");
  (void)strlcpy(tag, tag_name ? tag_name : "?", 64);
  struct json_object *assets = NULL;
  if (json_object_object_get_ex(root, "assets", &assets) &&
      json_object_is_type(assets, json_type_array)) {
    size_t count = json_object_array_length(assets);
    for (size_t i = 0; i < count && !found; ++i) {
      struct json_object *asset = json_object_array_get_idx(assets, i);
      const char *name = asset ? json_string_field(asset, "name") : NULL;
      const char *url =
          asset ? json_string_field(asset, "browser_download_url") : NULL;
      const char *updated = asset ? json_string_field(asset, "updated_at")
                                  : NULL;
      size_t name_length = name ? strlen(name) : 0;
      found = name_length > 4u &&
              strcasecmp(name + name_length - 4u, ".zip") == 0 && url &&
              strncasecmp(url, "http", 4u) == 0 &&
              strlcpy(zip_url, url, MAX_PATH) < MAX_PATH && updated &&
              parse_iso8601_utc(updated, asset_time_out);
    }
  }
  json_object_put(root);
  return found;
}

static uint16_t read_le16(const unsigned char *p) {
  return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const unsigned char *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}

// Stored or deflated entries of a non-zip64 archive, checked by CRC-32.
static bool zip_extract(const unsigned char *zip, size_t zip_size,
                        const char *name, unsigned char **data_out,
                        size_t *size_out) {
  *data_out = NULL;
  *size_out = 0;
  if (zip_size < 22u)
    return false;
  size_t eocd = zip_size - 22u;
  size_t lowest = zip_size > 22u + 65535u ? zip_size - 22u - 65535u : 0;
  while (read_le32(zip + eocd) != 0x06054b50u) {
    if (eocd == lowest)
      return false;
    eocd--;
  }
  size_t entries = read_le16(zip + eocd + 10u);
  size_t offset = read_le32(zip + eocd + 16u);
  size_t name_length = strlen(name);
  for (size_t i = 0; i < entries; ++i) {
    if (offset > zip_size || zip_size - offset < 46u ||
        read_le32(zip + offset) != 0x02014b50u)
      return false;
    const unsigned char *entry = zip + offset;
    size_t entry_name_length = read_le16(entry + 28u);
    size_t entry_size = 46u + entry_name_length + read_le16(entry + 30u) +
                        read_le16(entry + 32u);
    if (zip_size - offset < entry_size)
      return false;
    offset += entry_size;
    if (entry_name_length != name_length ||
        memcmp(entry + 46u, name, name_length) != 0)
      continue;

    uint16_t method = read_le16(entry + 10u);
    uint32_t crc = read_le32(entry + 16u);
    size_t packed_size = read_le32(entry + 20u);
    size_t size = read_le32(entry + 24u);
    size_t local = read_le32(entry + 42u);
    if (size == 0 || size > DLC_EMU_MAX_FILE_SIZE || local > zip_size ||
        zip_size - local < 30u || read_le32(zip + local) != 0x04034b50u)
      return false;
    size_t data_offset = local + 30u + read_le16(zip + local + 26u) +
                         read_le16(zip + local + 28u);
    if (data_offset > zip_size || zip_size - data_offset < packed_size)
      return false;
    unsigned char *data = malloc(size);
    if (!data)
      return false;
    bool ok = false;
    if (method == 0) {
      ok = packed_size == size;
      if (ok)
        memcpy(data, zip + data_offset, size);
    } else if (method == 8) {
      z_stream stream;
      memset(&stream, 0, sizeof(stream));
      if (inflateInit2(&stream, -MAX_WBITS) == Z_OK) {
        stream.next_in = (Bytef *)(uintptr_t)(zip + data_offset);
        stream.avail_in = (uInt)packed_size;
        stream.next_out = data;
        stream.avail_out = (uInt)size;
        ok = inflate(&stream, Z_FINISH) == Z_STREAM_END &&
             stream.total_out == size;
        (void)inflateEnd(&stream);
      }
    }
    if (ok)
      ok = crc32(crc32(0L, Z_NULL, 0), data, (uInt)size) == crc;
    if (!ok) {
      free(data);
      return false;
    }
    *data_out = data;
    *size_out = size;
    return true;
  }
  return false;
}

static bool write_file_with_time(const char *path, const unsigned char *data,
                                 size_t size, time_t modified_time) {
  (void)unlink(path);
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0777);
  if (fd < 0)
    return false;
  bool ok = write_all(fd, data, size) && fsync(fd) == 0;
  if (close(fd) != 0)
    ok = false;
  return ok && set_file_modified_time(path, modified_time);
}

// Like AMPR: a set older than the release asset is replaced, a current one kept.
static ampr_update_result_t
check_and_update_dlc_emu(const ampr_update_config_t *snapshot) {
  char destinations[DLC_EMU_FILE_COUNT][MAX_PATH];
  char temp_paths[DLC_EMU_FILE_COUNT][MAX_PATH];
  bool temp_exists[DLC_EMU_FILE_COUNT] = {false};
  bool all_present = true;
  time_t oldest_local_time = 0;
  for (size_t i = 0; i < DLC_EMU_FILE_COUNT; ++i) {
    int written = snprintf(destinations[i], MAX_PATH, "%s/%s",
                           snapshot->emulators_path, g_dlc_emu_files[i]);
    int temp_written = snprintf(temp_paths[i], MAX_PATH, "%s/.%s.download",
                                snapshot->emulators_path, g_dlc_emu_files[i]);
    if (written <= 0 || written >= MAX_PATH || temp_written <= 0 ||
        temp_written >= MAX_PATH) {
      log_debug("  [DLC_EMU] emulator path is too long: %s",
                snapshot->emulators_path);
      return AMPR_UPDATE_FAILED;
    }
    bool missing = false;
    time_t modified_time = 0;
    if (!inspect_local_emulator(destinations[i], &missing, &modified_time)) {
      log_debug("  [DLC_EMU] cannot inspect %s: %s", destinations[i],
                strerror(errno));
      return AMPR_UPDATE_FAILED;
    }
    if (missing)
      all_present = false;
    else if (i == 0 || modified_time < oldest_local_time)
      oldest_local_time = modified_time;
  }

  http_session_t session = {-1, -1, -1, -1, false};
  unsigned char *json = NULL;
  unsigned char *zip = NULL;
  size_t json_size = 0;
  size_t zip_size = 0;
  char tag[64];
  char zip_url[MAX_PATH];
  time_t release_time = 0;
  ampr_update_result_t result = AMPR_UPDATE_FAILED;

  // GitHub API and release assets are HTTPS, whatever the configured URL.
  if (!open_http_session(&session, true) ||
      !http_get_body(&session, snapshot->dlc_emu_url, snapshot,
                     DLC_EMU_MAX_RELEASE_JSON_SIZE, &json, &json_size,
                     &result))
    goto cleanup;
  if (!parse_dlc_emu_release(json, json_size, tag, zip_url, &release_time)) {
    log_debug("  [DLC_EMU] no zip asset in release: %s", snapshot->dlc_emu_url);
    goto cleanup;
  }
  if (all_present && release_time <= oldest_local_time) {
    result = AMPR_UPDATE_UNCHANGED;
    goto cleanup;
  }
  log_debug("  [DLC_EMU] downloading release %s: %s", tag, zip_url);
  if (!http_get_body(&session, zip_url, snapshot, AMPR_MAX_DOWNLOAD_SIZE, &zip,
                     &zip_size, &result))
    goto cleanup;
  if (!ensure_emulators_directory(snapshot->emulators_path)) {
    log_debug("  [DLC_EMU] cannot use emulator folder %s: %s",
              snapshot->emulators_path, strerror(errno));
    goto cleanup;
  }

  for (size_t i = 0; i < DLC_EMU_FILE_COUNT; ++i) {
    char entry[MAX_PATH];
    (void)snprintf(entry, sizeof(entry), "%s%s", DLC_EMU_ZIP_DIRECTORY,
                   g_dlc_emu_files[i]);
    unsigned char *data = NULL;
    size_t size = 0;
    if (!zip_extract(zip, zip_size, entry, &data, &size)) {
      log_debug("  [DLC_EMU] release %s has no valid %s", tag, entry);
      goto cleanup;
    }
    temp_exists[i] = true;
    bool written =
        write_file_with_time(temp_paths[i], data, size, release_time);
    free(data);
    if (!written || !downloaded_file_is_sprx(temp_paths[i])) {
      log_debug("  [DLC_EMU] cannot write %s: %s", temp_paths[i],
                written ? "invalid SELF/ELF header" : strerror(errno));
      goto cleanup;
    }
  }

  if (!update_snapshot_is_current(snapshot) || ampr_update_busy()) {
    result = AMPR_UPDATE_CANCELLED;
    goto cleanup;
  }
  bool replaced = false;
  for (size_t i = 0; i < DLC_EMU_FILE_COUNT; ++i) {
    bool unchanged = false;
    if (!regular_files_equal(destinations[i], temp_paths[i], &unchanged)) {
      log_debug("  [DLC_EMU] cannot compare %s: %s", destinations[i],
                strerror(errno));
      goto cleanup;
    }
    if (unchanged) {
      if (!set_file_modified_time(destinations[i], release_time)) {
        log_debug("  [DLC_EMU] cannot update modification time for %s: %s",
                  destinations[i], strerror(errno));
        goto cleanup;
      }
      continue;
    }
    if (rename(temp_paths[i], destinations[i]) != 0) {
      log_debug("  [DLC_EMU] cannot replace %s: %s", destinations[i],
                strerror(errno));
      goto cleanup;
    }
    temp_exists[i] = false;
    replaced = true;
  }
  log_debug("  [DLC_EMU] release %s %s in %s", tag,
            replaced ? "installed" : "already current",
            snapshot->emulators_path);
  result = replaced ? AMPR_UPDATE_REPLACED : AMPR_UPDATE_UNCHANGED;

cleanup:
  free(json);
  free(zip);
  close_http_session(&session);
  for (size_t i = 0; i < DLC_EMU_FILE_COUNT; ++i)
    if (temp_exists[i])
      (void)unlink(temp_paths[i]);
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

    if (!snapshot.enabled && !snapshot.dlc_emu_enabled) {
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

    if (runtime_sleep_mode_active()) {
      while (!g_ampr_updater.stop_requested &&
             runtime_sleep_mode_active()) {
        g_ampr_updater.wake_requested = false;
        int rc =
            pthread_cond_wait(&g_ampr_updater.cond, &g_ampr_updater.mutex);
        if (rc != 0) {
          log_debug("  [AMPR] sleep wait failed: %s", strerror(rc));
          pthread_mutex_unlock(&g_ampr_updater.mutex);
          return NULL;
        }
      }
      if (g_ampr_updater.stop_requested)
        break;
      g_ampr_updater.wake_requested = false;
      if (!wait_for_update_event_locked(AMPR_INITIAL_CHECK_DELAY_SECONDS,
                                        true)) {
        break;
      }
      continue;
    }

    if (ampr_update_busy()) {
      if (!wait_for_update_event_locked(AMPR_BUSY_RETRY_SECONDS, true))
        break;
      continue;
    }

    pthread_mutex_unlock(&g_ampr_updater.mutex);
    if (snapshot.enabled) {
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
    }
    if (snapshot.dlc_emu_enabled && update_snapshot_is_current(&snapshot)) {
      log_debug("  [DLC_EMU] checking latest release: %s",
                snapshot.dlc_emu_url);
      (void)check_and_update_dlc_emu(&snapshot);
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
  abort_active_request_locked();
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

void sm_ampr_updater_on_sleep_change(bool active) {
  pthread_mutex_lock(&g_ampr_updater.mutex);
  if (!g_ampr_updater.started) {
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    return;
  }

  const runtime_config_t cfg = runtime_config();
  bool update_enabled =
      cfg.auto_update_ampr_enabled || cfg.auto_update_dlc_emu_enabled;
  if (!update_enabled && g_ampr_updater.active_request_id < 0) {
    pthread_mutex_unlock(&g_ampr_updater.mutex);
    return;
  }

  if (active) {
    g_ampr_updater.config_generation++;
    abort_active_request_locked();
  }
  pthread_cond_broadcast(&g_ampr_updater.cond);
  pthread_mutex_unlock(&g_ampr_updater.mutex);
}

void sm_ampr_updater_on_config_reload(const runtime_config_t *old_cfg,
                                      const runtime_config_t *new_cfg) {
  if (!old_cfg || !new_cfg ||
      (old_cfg->auto_update_ampr_enabled ==
           new_cfg->auto_update_ampr_enabled &&
       strcmp(old_cfg->ampr_update_url, new_cfg->ampr_update_url) == 0 &&
       old_cfg->auto_update_dlc_emu_enabled ==
           new_cfg->auto_update_dlc_emu_enabled &&
       strcmp(old_cfg->dlc_emu_update_url, new_cfg->dlc_emu_update_url) == 0 &&
       strcmp(old_cfg->emulators_path, new_cfg->emulators_path) == 0)) {
    return;
  }

  pthread_mutex_lock(&g_ampr_updater.mutex);
  g_ampr_updater.config_generation++;
  g_ampr_updater.wake_requested = true;
  abort_active_request_locked();
  pthread_cond_broadcast(&g_ampr_updater.cond);
  pthread_mutex_unlock(&g_ampr_updater.mutex);
}
