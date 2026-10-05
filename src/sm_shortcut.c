#include "sm_platform.h"

#include <json-c/json.h>
#include <pthread.h>
#include <zlib.h>

#include "sm_filesystem.h"
#include "sm_gameinfo.h"
#include "sm_l10n.h"
#include "sm_log.h"
#include "sm_ps5sx2.h"
#include "sm_shortcut.h"
#include "sm_ucred.h"

// ShellCore's own authority: the launch services refuse other callers.
#define SCE_AUTHID_SHELLCORE 0x4801000000000013ull

#define SHORTCUT_FILE_MAX 4096
#define SHORTCUT_MAX_ARGS 8
#define SHORTCUT_ARG_MAX 1024
#define BIG_APP_EXIT_WAIT_US (8u * 1000u * 1000u)
#define POLL_US (250u * 1000u)

extern unsigned char assets_shortcut_launcher_bin[];
extern unsigned int assets_shortcut_launcher_bin_len;
extern unsigned char assets_shortcut_libc_prx_gz[];
extern unsigned int assets_shortcut_libc_prx_gz_len;

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceGetAppTitleId(int app_id, char *title_id);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv,
                              app_launch_ctx_t *ctx);

typedef struct {
  char title_id[MAX_TITLE_ID];
  char args[SHORTCUT_MAX_ARGS][SHORTCUT_ARG_MAX];
  size_t arg_count;
  bool ps5sx2;
  sm_ps5sx2_launch_t ps5sx2_launch;
} launch_job_t;

static pthread_mutex_t g_launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_launch_running = false;

static bool write_file_atomic(const char *path, const void *data, size_t len,
                              mode_t mode) {
  char tmp[MAX_PATH];
  if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
    return false;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, mode);
  if (fd < 0)
    return false;
  // Not left to the umask: the loader needs the execute bits.
  bool ok = fchmod(fd, mode) == 0 && write(fd, data, len) == (ssize_t)len;
  ok = close(fd) == 0 && ok;
  if (ok && rename(tmp, path) == 0)
    return true;
  unlink(tmp);
  return false;
}

// The app loader refuses an eboot or a module without the execute bits
// ("mount flag / attribute error"), so both are checked.
static bool is_in_place(const char *path, size_t size) {
  struct stat st;
  return stat(path, &st) == 0 && S_ISREG(st.st_mode) &&
         (size_t)st.st_size == size && (st.st_mode & 0111) == 0111;
}

// The gzip trailer ends with the uncompressed size, modulo 2^32.
static size_t libc_shim_size(void) {
  const unsigned char *end =
      assets_shortcut_libc_prx_gz + assets_shortcut_libc_prx_gz_len;
  return (size_t)end[-4] | (size_t)end[-3] << 8 | (size_t)end[-2] << 16 |
         (size_t)end[-1] << 24;
}

static bool write_libc_shim(const char *path) {
  size_t size = libc_shim_size();
  unsigned char *data = malloc(size);
  if (!data)
    return false;
  z_stream zs;
  memset(&zs, 0, sizeof(zs));
  bool ok = inflateInit2(&zs, 16 + MAX_WBITS) == Z_OK;
  if (ok) {
    zs.next_in = assets_shortcut_libc_prx_gz;
    zs.avail_in = assets_shortcut_libc_prx_gz_len;
    zs.next_out = data;
    zs.avail_out = (uInt)size;
    ok = inflate(&zs, Z_FINISH) == Z_STREAM_END && zs.total_out == size;
    inflateEnd(&zs);
  }
  ok = ok && write_file_atomic(path, data, size, 0755);
  free(data);
  return ok;
}

void sm_shortcut_ensure_launcher(const char *folder) {
  char path[MAX_PATH];
  struct stat st;
  if ((size_t)snprintf(path, sizeof(path), "%s/" SM_SHORTCUT_FILE, folder) >=
          sizeof(path) ||
      stat(path, &st) != 0)
    return;

  // The launcher, and the app loader's libc.prx shim it needs in sce_module/.
  bool ok = true;
  (void)snprintf(path, sizeof(path), "%s/eboot.bin", folder);
  if (!is_in_place(path, assets_shortcut_launcher_bin_len)) {
    ok = write_file_atomic(path, assets_shortcut_launcher_bin,
                           assets_shortcut_launcher_bin_len, 0755);
    log_debug("  [SHORTCUT] launcher %s: %s", ok ? "placed" : "not placed",
              path);
  }
  (void)snprintf(path, sizeof(path), "%s/sce_module", folder);
  mkdir(path, 0755);
  (void)snprintf(path, sizeof(path), "%s/sce_module/libc.prx", folder);
  if (ok && !is_in_place(path, libc_shim_size())) {
    ok = write_libc_shim(path);
    log_debug("  [SHORTCUT] libc.prx shim %s: %s",
              ok ? "placed" : "not placed", path);
  }
}

static bool read_shortcut_file(const char *path, char *buf, size_t size) {
  int fd = open(path, O_RDONLY);
  if (fd < 0)
    return false;
  ssize_t len = read(fd, buf, size);
  close(fd);
  if (len <= 0 || (size_t)len >= size)
    return false;
  buf[len] = '\0';
  return true;
}

// Parses smp-launch.json into job. Returns false for anything malformed.
static bool parse_shortcut(const char *text, launch_job_t *job,
                           const char **adapter_out) {
  struct json_tokener *tokener = json_tokener_new_ex(8);
  if (!tokener)
    return false;
  json_tokener_set_flags(tokener,
                         JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *root =
      json_tokener_parse_ex(tokener, text, (int)strlen(text));
  bool ok = json_tokener_get_error(tokener) == json_tokener_success &&
            json_object_is_type(root, json_type_object);
  json_tokener_free(tokener);

  struct json_object *value = NULL;
  ok = ok && json_object_object_get_ex(root, "title_id", &value) &&
       json_object_is_type(value, json_type_string) &&
       strlcpy(job->title_id, json_object_get_string(value),
               sizeof(job->title_id)) < sizeof(job->title_id) &&
       is_supported_game_title_id(job->title_id);

  if (ok && json_object_object_get_ex(root, "args", &value)) {
    ok = json_object_is_type(value, json_type_array) &&
         json_object_array_length(value) <= SHORTCUT_MAX_ARGS;
    for (size_t i = 0; ok && i < json_object_array_length(value); i++) {
      struct json_object *arg = json_object_array_get_idx(value, i);
      ok = json_object_is_type(arg, json_type_string) &&
           strlcpy(job->args[i], json_object_get_string(arg),
                   SHORTCUT_ARG_MAX) < SHORTCUT_ARG_MAX;
      job->arg_count = i + 1;
    }
  }

  *adapter_out = NULL;
  if (ok && json_object_object_get_ex(root, "adapter", &value)) {
    ok = json_object_is_type(value, json_type_string);
    if (ok && strcmp(json_object_get_string(value), "ps5sx2") == 0)
      *adapter_out = "ps5sx2";
    else
      ok = false;
  }
  json_object_put(root);
  return ok;
}

static int running_big_app(void *arg) {
  (void)arg;
  return sceSystemServiceGetAppIdOfRunningBigApp();
}

static int kill_app(void *arg) {
  return sceSystemServiceKillApp(*(int *)arg, -1, 0, 0);
}

static int app_title_id(void *arg) {
  char *title_id = arg;
  int app_id = sceSystemServiceGetAppIdOfRunningBigApp();
  if (app_id <= 0)
    return -1;
  return sceSystemServiceGetAppTitleId(app_id, title_id);
}

// Each launch service call runs with ShellCore's authid on its own, so the
// elevation does not last through the wait for the app to exit.
static bool close_big_app(void) {
  int app_id =
      sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, running_big_app, NULL);
  if (app_id <= 0)
    return true;
  int rc = sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, kill_app, &app_id);
  log_debug("  [SHORTCUT] closing the running app 0x%x: 0x%08x", app_id,
            (unsigned)rc);
  for (unsigned waited = 0; waited < BIG_APP_EXIT_WAIT_US;
       waited += POLL_US) {
    if (sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, running_big_app, NULL) <=
        0)
      return true;
    sceKernelUsleep(POLL_US);
  }
  return false;
}

static int launch_target(void *arg) {
  launch_job_t *job = arg;
  app_launch_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.structsize = sizeof(ctx);
  (void)sceUserServiceGetForegroundUser(&ctx.user_id);
  char *argv[SHORTCUT_MAX_ARGS + 1];
  for (size_t i = 0; i < job->arg_count; i++)
    argv[i] = job->args[i];
  argv[job->arg_count] = NULL;
  return sceSystemServiceLaunchApp(job->title_id, argv, &ctx);
}

static void *launch_thread_main(void *arg) {
  launch_job_t *job = arg;
  bool launched = false;

  if (job->ps5sx2 && !sm_ps5sx2_prepare(&job->ps5sx2_launch))
    goto done;
  if (!close_big_app()) {
    log_debug("  [SHORTCUT] the running app did not close");
    notify_system_l10n(SM_L10N_SHORTCUT_CLOSE_RUNNING_APP);
    goto done;
  }
  // The launch answers the new app's id, or a negative SCE error.
  int rc = sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, launch_target, job);
  log_debug("  [SHORTCUT] launch %s: 0x%08x", job->title_id, (unsigned)rc);
  launched = rc >= 0;
  if (!launched)
    notify_system_l10n(SM_L10N_SHORTCUT_LAUNCH_FAILED, job->title_id,
                       (unsigned)rc);

done:
  if (job->ps5sx2)
    sm_ps5sx2_finish(&job->ps5sx2_launch, launched);
  pthread_mutex_lock(&g_launch_mutex);
  g_launch_running = false;
  pthread_mutex_unlock(&g_launch_mutex);
  free(job);
  return NULL;
}

static int check_shortcut(launch_job_t *job, const char **reason_out) {
  char shortcut_id[MAX_TITLE_ID] = {0};
  if (sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, app_title_id,
                           shortcut_id) != 0 ||
      shortcut_id[0] == '\0') {
    *reason_out = "no_running_shortcut";
    return ENOENT;
  }

  char source[MAX_PATH];
  char path[MAX_PATH];
  char text[SHORTCUT_FILE_MAX];
  if (!read_mount_link(shortcut_id, source, sizeof(source)) ||
      (size_t)snprintf(path, sizeof(path), "%s/" SM_SHORTCUT_FILE, source) >=
          sizeof(path) ||
      !read_shortcut_file(path, text, sizeof(text))) {
    *reason_out = "no_running_shortcut";
    return ENOENT;
  }

  const char *adapter = NULL;
  if (!parse_shortcut(text, job, &adapter)) {
    *reason_out = "invalid_shortcut";
    return EINVAL;
  }
  char app_dir[MAX_PATH];
  struct stat st;
  (void)snprintf(app_dir, sizeof(app_dir), "/user/app/%s", job->title_id);
  if (stat(app_dir, &st) != 0) {
    *reason_out = "target_not_installed";
    return ENOENT;
  }
  if (adapter) {
    const char *args[SHORTCUT_MAX_ARGS];
    for (size_t i = 0; i < job->arg_count; i++)
      args[i] = job->args[i];
    job->ps5sx2 = true;
    int status = sm_ps5sx2_check(job->title_id, args, job->arg_count,
                                 &job->ps5sx2_launch, reason_out);
    if (status != 0)
      return status;
  }
  log_debug("  [SHORTCUT] %s starts %s%s", shortcut_id, job->title_id,
            adapter ? " (ps5sx2 adapter)" : "");
  return 0;
}

int sm_shortcut_request_launch(const char **reason_out) {
  *reason_out = NULL;
  launch_job_t *job = calloc(1, sizeof(*job));
  if (!job)
    return ENOMEM;
  int status = check_shortcut(job, reason_out);
  if (status != 0) {
    free(job);
    return status;
  }

  pthread_mutex_lock(&g_launch_mutex);
  if (g_launch_running) {
    pthread_mutex_unlock(&g_launch_mutex);
    free(job);
    *reason_out = "launch_in_progress";
    return EBUSY;
  }
  g_launch_running = true;
  pthread_mutex_unlock(&g_launch_mutex);

  pthread_t thread;
  if (pthread_create(&thread, NULL, launch_thread_main, job) != 0) {
    pthread_mutex_lock(&g_launch_mutex);
    g_launch_running = false;
    pthread_mutex_unlock(&g_launch_mutex);
    free(job);
    return EAGAIN;
  }
  pthread_detach(thread);
  return 0;
}
