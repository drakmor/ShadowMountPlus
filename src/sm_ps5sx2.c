#include "sm_platform.h"

#include <pthread.h>

#include "sm_log.h"
#include "sm_ps5sx2.h"
#include "sm_ucred.h"

#define PS5SX2_TITLE_ID "PPSA99203"
#define PS5SX2_APP_DIR "/user/app/" PS5SX2_TITLE_ID
#define PS5SX2_TOP_DIR "/data/PCSX2"
#define PS5SX2_GAMES_DIR PS5SX2_TOP_DIR "/games"
#define PS5SX2_LASTGAME PS5SX2_TOP_DIR "/lastgame.txt"
#define PS5SX2_FLAGS_DIR PS5SX2_TOP_DIR "/flags"
#define PS5SX2_NOMENU PS5SX2_FLAGS_DIR "/nomenu"
// Present while a nomenu flag we created is still in place, so it can be
// removed after a crash or a restart of this payload.
#define PS5SX2_NOMENU_MARKER "/data/shadowmount/ps5sx2_nomenu.pending"

// ShellCore's own authority: the launch services refuse other callers
// (SCE_LNC_UTIL_ERROR_NOT_ALLOWED).
#define SCE_AUTHID_SHELLCORE 0x4801000000000013ull

#define BIG_APP_EXIT_WAIT_US (8u * 1000u * 1000u)
#define POLL_US (250u * 1000u)
// PS5SX2 reads its flags after its jailbreak, which can wait for a cover
// download first; allow for a slow start before giving the flag up.
#define PICKUP_TIMEOUT_US (180u * 1000u * 1000u)

typedef struct app_launch_ctx {
  uint32_t structsize;
  uint32_t user_id;
  uint32_t app_opt;
  uint64_t crash_report;
  uint32_t check_flag;
} app_launch_ctx_t;

int sceUserServiceGetForegroundUser(uint32_t *user_id);
int sceSystemServiceGetAppIdOfRunningBigApp(void);
int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump);
int sceSystemServiceLaunchApp(const char *title_id, char **argv,
                              app_launch_ctx_t *ctx);

typedef struct {
  char image[PATH_MAX];
  char file[NAME_MAX + 1];
} launch_job_t;

static pthread_mutex_t g_launch_mutex = PTHREAD_MUTEX_INITIALIZER;
static bool g_launch_running = false;

static bool has_disc_image_extension(const char *name) {
  static const char *const extensions[] = {".iso", ".chd", ".cso", ".zso"};
  const char *dot = strrchr(name, '.');
  if (!dot)
    return false;
  for (size_t i = 0; i < sizeof(extensions) / sizeof(extensions[0]); i++) {
    if (strcasecmp(dot, extensions[i]) == 0)
      return true;
  }
  return false;
}

// PS5SX2 looks the remembered game up by file name in games/ and then in the
// top folder, so only images there can be started this way.
static bool split_image_path(const char *path, char *file_out,
                             size_t file_size) {
  const char *slash = strrchr(path, '/');
  if (!slash || slash[1] == '\0' || strstr(path, "/../") ||
      strstr(path, "/./"))
    return false;
  size_t dir_len = (size_t)(slash - path);
  bool in_games = dir_len == strlen(PS5SX2_GAMES_DIR) &&
                  strncmp(path, PS5SX2_GAMES_DIR, dir_len) == 0;
  bool in_top = dir_len == strlen(PS5SX2_TOP_DIR) &&
                strncmp(path, PS5SX2_TOP_DIR, dir_len) == 0;
  if (!in_games && !in_top)
    return false;
  if (!has_disc_image_extension(slash + 1))
    return false;
  return strlcpy(file_out, slash + 1, file_size) < file_size;
}

static bool write_text_file(const char *path, const char *text) {
  char tmp[PATH_MAX];
  if ((size_t)snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= sizeof(tmp))
    return false;
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
  if (fd < 0)
    return false;
  size_t len = strlen(text);
  bool ok = write(fd, text, len) == (ssize_t)len;
  ok = close(fd) == 0 && ok;
  if (ok && rename(tmp, path) == 0)
    return true;
  unlink(tmp);
  return false;
}

static bool get_mtime(const char *path, struct timespec *out) {
  struct stat st;
  if (stat(path, &st) != 0)
    return false;
  *out = st.st_mtim;
  return true;
}

static bool timespec_after(const struct timespec *a,
                           const struct timespec *b) {
  return a->tv_sec > b->tv_sec ||
         (a->tv_sec == b->tv_sec && a->tv_nsec > b->tv_nsec);
}

static void remove_our_nomenu_flag(const char *why) {
  if (access(PS5SX2_NOMENU_MARKER, F_OK) != 0)
    return;
  unlink(PS5SX2_NOMENU);
  unlink(PS5SX2_NOMENU_MARKER);
  log_debug("  [PS5SX2] nomenu flag removed (%s)", why);
}

static int running_big_app(void *arg) {
  (void)arg;
  return sceSystemServiceGetAppIdOfRunningBigApp();
}

static int kill_app(void *arg) {
  return sceSystemServiceKillApp(*(int *)arg, -1, 0, 0);
}

// Each launch service call runs with ShellCore's authid on its own, so the
// elevation does not last through the wait for the app to exit.
static bool close_big_app(void) {
  int app_id =
      sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, running_big_app, NULL);
  if (app_id <= 0)
    return true;
  int rc = sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, kill_app, &app_id);
  log_debug("  [PS5SX2] closing the running app 0x%x: 0x%08x", app_id,
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

static int launch_ps5sx2(void *arg) {
  launch_job_t *job = arg;
  app_launch_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.structsize = sizeof(ctx);
  (void)sceUserServiceGetForegroundUser(&ctx.user_id);
  char *argv[] = {"--boot", job->image, NULL};
  return sceSystemServiceLaunchApp(PS5SX2_TITLE_ID, argv, &ctx);
}

static void wait_for_pickup(const struct timespec *written) {
  for (unsigned waited = 0; waited < PICKUP_TIMEOUT_US; waited += POLL_US) {
    struct timespec now;
    // PS5SX2 writes lastgame.txt again once it has chosen the game, after
    // reading the nomenu flag.
    if (get_mtime(PS5SX2_LASTGAME, &now) && timespec_after(&now, written)) {
      remove_our_nomenu_flag("PS5SX2 picked the game up");
      return;
    }
    sceKernelUsleep(POLL_US);
  }
  remove_our_nomenu_flag("PS5SX2 did not pick the game up in time");
}

static void *launch_thread_main(void *arg) {
  launch_job_t *job = arg;
  bool flag_ours = false;
  struct timespec written = {0};

  char line[NAME_MAX + 2];
  (void)snprintf(line, sizeof(line), "%s\n", job->file);
  if (!write_text_file(PS5SX2_LASTGAME, line) ||
      !get_mtime(PS5SX2_LASTGAME, &written)) {
    log_debug("  [PS5SX2] cannot write %s: %s", PS5SX2_LASTGAME,
              strerror(errno));
    goto done;
  }
  if (access(PS5SX2_NOMENU, F_OK) != 0) {
    mkdir(PS5SX2_FLAGS_DIR, 0777);
    flag_ours = write_text_file(PS5SX2_NOMENU_MARKER, "") &&
                write_text_file(PS5SX2_NOMENU, "");
    if (!flag_ours) {
      log_debug("  [PS5SX2] cannot create %s: %s", PS5SX2_NOMENU,
                strerror(errno));
      unlink(PS5SX2_NOMENU_MARKER);
      goto done;
    }
  }

  if (!close_big_app()) {
    log_debug("  [PS5SX2] the running app did not close");
    notify_system_l10n(SM_L10N_PS5SX2_CLOSE_RUNNING_GAME);
    goto done;
  }
  // The launch answers the new app's id, or a negative SCE error.
  int rc = sm_ucred_with_authid(SCE_AUTHID_SHELLCORE, launch_ps5sx2, job);
  log_debug("  [PS5SX2] launch %s with %s: 0x%08x", PS5SX2_TITLE_ID,
            job->file, (unsigned)rc);
  if (rc < 0) {
    notify_system_l10n(SM_L10N_PS5SX2_LAUNCH_FAILED, (unsigned)rc);
    goto done;
  }
  if (flag_ours) {
    wait_for_pickup(&written);
    flag_ours = false;
  }

done:
  if (flag_ours)
    remove_our_nomenu_flag("launch failed");
  pthread_mutex_lock(&g_launch_mutex);
  g_launch_running = false;
  pthread_mutex_unlock(&g_launch_mutex);
  free(job);
  return NULL;
}

int sm_ps5sx2_request_launch(const char *image_path, const char **reason_out) {
  *reason_out = NULL;
  launch_job_t *job = calloc(1, sizeof(*job));
  if (!job)
    return ENOMEM;
  if (!image_path ||
      strlcpy(job->image, image_path, sizeof(job->image)) >=
          sizeof(job->image) ||
      !split_image_path(job->image, job->file, sizeof(job->file))) {
    free(job);
    *reason_out = "invalid_image_path";
    return EINVAL;
  }
  struct stat st;
  if (stat(job->image, &st) != 0 || !S_ISREG(st.st_mode)) {
    free(job);
    *reason_out = "image_not_found";
    return ENOENT;
  }
  if (stat(PS5SX2_APP_DIR, &st) != 0) {
    free(job);
    *reason_out = "ps5sx2_not_installed";
    return ENOENT;
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
  log_debug("  [PS5SX2] launch requested: %s", image_path);
  return 0;
}

void sm_ps5sx2_recover(void) {
  remove_our_nomenu_flag("left over from an interrupted launch");
}
