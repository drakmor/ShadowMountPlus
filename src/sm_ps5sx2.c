#include "sm_platform.h"

#include "sm_log.h"
#include "sm_ps5sx2.h"

#define PS5SX2_TITLE_ID "PPSA99203"
#define PS5SX2_TOP_DIR "/data/PCSX2"
#define PS5SX2_GAMES_DIR PS5SX2_TOP_DIR "/games"
#define PS5SX2_LASTGAME PS5SX2_TOP_DIR "/lastgame.txt"
#define PS5SX2_FLAGS_DIR PS5SX2_TOP_DIR "/flags"
#define PS5SX2_NOMENU PS5SX2_FLAGS_DIR "/nomenu"
// Present while a nomenu flag we created is still in place, so it can be
// removed after a crash or a restart of this payload.
#define PS5SX2_NOMENU_MARKER "/data/shadowmount/ps5sx2_nomenu.pending"

#define POLL_US (250u * 1000u)
// PS5SX2 reads its flags after its jailbreak, which can wait for a cover
// download first; allow for a slow start before giving the flag up.
#define PICKUP_TIMEOUT_US (180u * 1000u * 1000u)

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
static bool image_file_name(const char *path, char *file_out,
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

int sm_ps5sx2_check(const char *target_title_id, const char *const *args,
                    size_t arg_count, sm_ps5sx2_launch_t *launch,
                    const char **reason_out) {
  memset(launch, 0, sizeof(*launch));
  if (strcmp(target_title_id, PS5SX2_TITLE_ID) != 0) {
    *reason_out = "invalid_shortcut";
    return EINVAL;
  }
  const char *image = NULL;
  for (size_t i = 0; i + 1 < arg_count; i++) {
    if (strcmp(args[i], "--boot") == 0)
      image = args[i + 1];
  }
  if (!image ||
      !image_file_name(image, launch->file, sizeof(launch->file))) {
    *reason_out = "invalid_image_path";
    return EINVAL;
  }
  struct stat st;
  if (stat(image, &st) != 0 || !S_ISREG(st.st_mode)) {
    *reason_out = "image_not_found";
    return ENOENT;
  }
  return 0;
}

bool sm_ps5sx2_prepare(sm_ps5sx2_launch_t *launch) {
  char line[NAME_MAX + 2];
  (void)snprintf(line, sizeof(line), "%s\n", launch->file);
  if (!write_text_file(PS5SX2_LASTGAME, line) ||
      !get_mtime(PS5SX2_LASTGAME, &launch->written)) {
    log_debug("  [PS5SX2] cannot write %s: %s", PS5SX2_LASTGAME,
              strerror(errno));
    return false;
  }
  if (access(PS5SX2_NOMENU, F_OK) == 0)
    return true;
  mkdir(PS5SX2_FLAGS_DIR, 0777);
  launch->flag_ours = write_text_file(PS5SX2_NOMENU_MARKER, "") &&
                      write_text_file(PS5SX2_NOMENU, "");
  if (!launch->flag_ours) {
    log_debug("  [PS5SX2] cannot create %s: %s", PS5SX2_NOMENU,
              strerror(errno));
    unlink(PS5SX2_NOMENU_MARKER);
    return false;
  }
  return true;
}

void sm_ps5sx2_finish(sm_ps5sx2_launch_t *launch, bool launched) {
  if (!launch->flag_ours)
    return;
  launch->flag_ours = false;
  if (!launched) {
    remove_our_nomenu_flag("launch failed");
    return;
  }
  for (unsigned waited = 0; waited < PICKUP_TIMEOUT_US; waited += POLL_US) {
    struct timespec now;
    // PS5SX2 writes lastgame.txt again once it has chosen the game, after
    // reading the nomenu flag.
    if (get_mtime(PS5SX2_LASTGAME, &now) &&
        timespec_after(&now, &launch->written)) {
      remove_our_nomenu_flag("PS5SX2 picked the game up");
      return;
    }
    sceKernelUsleep(POLL_US);
  }
  remove_our_nomenu_flag("PS5SX2 did not pick the game up in time");
}

void sm_ps5sx2_recover(void) {
  remove_our_nomenu_flag("left over from an interrupted launch");
}
