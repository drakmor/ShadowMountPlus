#include "sm_platform.h"
#include "sm_log.h"
#include "sm_config_mount.h"
#include "sm_types.h"
#include "sm_paths.h"

static sm_error_t g_last_error;
static bool g_notifications_initialized = false;

extern unsigned char smp_icon_png[];
extern unsigned int smp_icon_png_len;

#define SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM 0xFE

int sceNotificationSend(int userId, bool isLogged, const char *payload);
int sceNotificationSendById(int userid, bool logged_in, const char *useCaseId,
                            const char *message);

static void notify_system_plain_message(const char *message) {
  notify_request_t req;
  memset(&req, 0, sizeof(req));
  (void)strlcpy(req.message, message, sizeof(req.message));
  sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
  log_debug("NOTIFY: %s", req.message);
}

static bool ensure_notification_icon_present(void) {
  struct stat st;
  if (stat(NOTIFY_ICON_FILE, &st) == 0 && st.st_size > 0)
    return true;

  (void)mkdir("/user/data", 0777);
  (void)mkdir(NOTIFY_ICON_DIR, 0777);
  FILE *fp = fopen(NOTIFY_ICON_FILE, "wb");
  if (!fp) {
    log_debug("  [NOTIFY] icon create failed: %s (%s)", NOTIFY_ICON_FILE,
              strerror(errno));
    return false;
  }

  size_t written = fwrite(smp_icon_png, 1, smp_icon_png_len, fp);
  int saved_errno = 0;
  if (written != smp_icon_png_len)
    saved_errno = ferror(fp) ? errno : EIO;
  if (fflush(fp) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(fp) != 0 && saved_errno == 0)
    saved_errno = errno;

  if (saved_errno != 0) {
    errno = saved_errno;
    log_debug("  [NOTIFY] icon write failed: %s (%s)", NOTIFY_ICON_FILE,
              strerror(errno));
    (void)unlink(NOTIFY_ICON_FILE);
    return false;
  }

  log_debug("  [NOTIFY] icon created: %s", NOTIFY_ICON_FILE);
  return true;
}

void sm_notifications_init(void) {
  if (g_notifications_initialized)
    return;

  g_notifications_initialized = ensure_notification_icon_present();
}

static void append_json_escaped(char *dst, size_t dst_size, const char *src) {
  size_t used = strlen(dst);
  if (used >= dst_size)
    return;

  for (; *src != '\0' && used + 1 < dst_size; ++src) {
    const char *escape = NULL;
    char single[2] = {0};

    switch (*src) {
    case '\\':
      escape = "\\\\";
      break;
    case '"':
      escape = "\\\"";
      break;
    case '\n':
      escape = "\\n";
      break;
    case '\r':
      escape = "\\r";
      break;
    case '\t':
      escape = "\\t";
      break;
    default:
      single[0] = *src;
      escape = single;
      break;
    }

    size_t escape_len = strlen(escape);
    if (used + escape_len >= dst_size)
      break;
    memcpy(dst + used, escape, escape_len);
    used += escape_len;
    dst[used] = '\0';
  }
}

static bool build_notification_timestamp(time_t timestamp, char out[32]) {
  struct tm tm_utc;
  if (gmtime_r(&timestamp, &tm_utc) == NULL)
    return false;
  return strftime(out, 32, "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc) != 0;
}

static bool build_notification_metadata(char *created_at,
                                        char *notification_id,
                                        size_t notification_id_size) {
  time_t now;

  now = time(NULL);
  if (!build_notification_timestamp(now, created_at))
    return false;

  snprintf(notification_id, notification_id_size, "%u",
           (unsigned)((uint32_t)now ^ (uint32_t)getpid()));
  return true;
}

static bool send_rich_notification(const char *message) {
  char escaped_message[4096];
  char escaped_version[128];
  char payload[8192];
  char created_at[32];
  char notification_id[32];

  if (!message || message[0] == '\0')
    return false;

  sm_notifications_init();
  if (!g_notifications_initialized)
    return false;

  escaped_message[0] = '\0';
  escaped_version[0] = '\0';
  append_json_escaped(escaped_message, sizeof(escaped_message), message);
  append_json_escaped(escaped_version, sizeof(escaped_version),
                      SHADOWMOUNT_VERSION);
  if (!build_notification_metadata(created_at, notification_id,
                                   sizeof(notification_id))) {
    return false;
  }

  int len = snprintf(
      payload, sizeof(payload),
      "{\n"
      "  \"rawData\": {\n"
      "    \"viewTemplateType\": \"InteractiveToastTemplateB\",\n"
      "    \"channelType\": \"ServiceFeedback\",\n"
      "    \"useCaseId\": \"IDC\",\n"
      "    \"toastOverwriteType\": \"No\",\n"
      "    \"isImmediate\": true,\n"
      "    \"priority\": 100,\n"
      "    \"viewData\": {\n"
      "      \"icon\": {\n"
      "        \"type\": \"Url\",\n"
      "        \"parameters\": {\n"
      "          \"url\": \"" NOTIFY_ICON_FILE "\"\n"
      "        }\n"
      "      },\n"
      "      \"message\": {\n"
      "        \"body\": \"%s\"\n"
      "      },\n"
      "      \"subMessage\": {\n"
      "        \"body\": \"ShadowMountPlus %s\"\n"
      "      },\n"
      "      \"actions\": [\n"
      "        {\n"
      "          \"actionName\": \"Go to Debug Settings\",\n"
      "          \"actionType\": \"DeepLink\",\n"
      "          \"defaultFocus\": true,\n"
      "          \"parameters\": {\n"
      "            \"actionUrl\": \"pssettings:play?function=debug_settings\"\n"
      "          }\n"
      "        }\n"
      "      ]\n"
      "    },\n"
      "    \"platformViews\": {\n"
      "      \"previewDisabled\": {\n"
      "        \"viewData\": {\n"
      "          \"icon\": {\n"
      "            \"type\": \"Predefined\",\n"
      "            \"parameters\": {\n"
      "              \"icon\": \"community\"\n"
      "            }\n"
      "          },\n"
      "          \"message\": {\n"
      "            \"body\": \"%s\"\n"
      "          }\n"
      "        }\n"
      "      }\n"
      "    }\n"
      "  },\n"
      "  \"createdDateTime\": \"%s\",\n"
      "  \"localNotificationId\": \"%s\"\n"
      "}",
      escaped_message, escaped_version, escaped_message, created_at,
      notification_id);
  if (len < 0 || (size_t)len >= sizeof(payload))
    return false;

  int rc = sceNotificationSend(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
                               payload);
  return rc == 0;
}

static bool send_game_installed_rich_notification(const char *title_id) {
  char escaped_title_id[128];
  char params[256];

  if (!title_id || title_id[0] == '\0')
    return false;

  escaped_title_id[0] = '\0';
  append_json_escaped(escaped_title_id, sizeof(escaped_title_id), title_id);
  int len = snprintf(params, sizeof(params),
                     "{\"npTitleId\":\"%s_00\"}", escaped_title_id);
  if (len < 0 || (size_t)len >= sizeof(params))
    return false;
  int rc = sceNotificationSendById(SCE_NOTIFICATION_LOCAL_USER_ID_SYSTEM, true,
                                   "NUC240", params);
  return rc == 0;
}

static void notify_system_plain_v(const char *fmt, va_list args) {
  char message[3075];
  vsnprintf(message, sizeof(message), fmt, args);
  notify_system_plain_message(message);
}

static void log_to_file(const char *fmt, va_list args) {
  mkdir(LOG_DIR, 0777);
  FILE *fp = fopen(LOG_FILE, "a");
  if (fp) {
    va_list args_file;
    time_t rawtime;
    struct tm *timeinfo;
    char buffer[80];
    time(&rawtime);
    timeinfo = localtime(&rawtime);
    strftime(buffer, sizeof(buffer), "%H:%M:%S", timeinfo);
    va_copy(args_file, args);
    fprintf(fp, "[%s] ", buffer);
    vfprintf(fp, fmt, args_file);
    fprintf(fp, "\n");
    va_end(args_file);
    fclose(fp);
  }
}

void log_debug(const char *fmt, ...) {
  if (runtime_config()->debug_enabled == false)
    return;

  va_list args;
  va_list args_copy;
  va_start(args, fmt);
  va_copy(args_copy, args);
  vprintf(fmt, args);
  printf("\n");
  log_to_file(fmt, args_copy);
  va_end(args_copy);
  va_end(args);
}

void notify_system_rich(bool allow_in_quiet_mode, const char *fmt, ...) {
  char message[3075];
  va_list args;

  if (runtime_config()->quiet_mode && !allow_in_quiet_mode)
    return;

  va_start(args, fmt);
  vsnprintf(message, sizeof(message), fmt, args);
  va_end(args);

  if (!send_rich_notification(message)) {
    notify_system_plain_message(message);
    return;
  }

  log_debug("NOTIFY: %s", message);
}

void notify_game_installed_rich(const char *title_id) {
  if (!send_game_installed_rich_notification(title_id)) {
    notify_system_info("Installed: %s",
                       (title_id && title_id[0] != '\0') ? title_id : "Unknown");
    return;
  }

  log_debug("NOTIFY: installed game %s", title_id ? title_id : "unknown");
}

void notify_system_info(const char *fmt, ...) {
  if (runtime_config()->quiet_mode)
    return;

  va_list args;
  va_start(args, fmt);
  notify_system_plain_v(fmt, args);
  va_end(args);
}

void sm_error_clear(void) {
  memset(&g_last_error, 0, sizeof(g_last_error));
}

void sm_error_set(const char *subsystem, int code, const char *path,
                  const char *fmt, ...) {
  memset(&g_last_error, 0, sizeof(g_last_error));
  g_last_error.valid = true;
  g_last_error.code = code;
  if (subsystem && subsystem[0] != '\0')
    (void)strlcpy(g_last_error.subsystem, subsystem,
                  sizeof(g_last_error.subsystem));
  if (path && path[0] != '\0')
    (void)strlcpy(g_last_error.path, path, sizeof(g_last_error.path));
  if (fmt && fmt[0] != '\0') {
    va_list args;
    va_start(args, fmt);
    vsnprintf(g_last_error.message, sizeof(g_last_error.message), fmt, args);
    va_end(args);
  }
}

const sm_error_t *sm_last_error(void) {
  return &g_last_error;
}

bool sm_error_notified(void) {
  return g_last_error.notified;
}

void sm_error_mark_notified(void) {
  g_last_error.notified = true;
}

void notify_system(const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  notify_system_plain_v(fmt, args);
  va_end(args);
}

void notify_image_mount_failed(const char *path, int mount_err) {
  if (g_last_error.valid && g_last_error.message[0] != '\0') {
    notify_system("Image mount failed: 0x%08X (%s)\n%s\n%s",
                  (uint32_t)mount_err, strerror(mount_err),
                  g_last_error.message,
                  (g_last_error.path[0] != '\0') ? g_last_error.path : path);
    g_last_error.notified = true;
    return;
  }

  notify_system("Image mount failed: 0x%08X (%s)\n%s", (uint32_t)mount_err,
                strerror(mount_err), path);
}
