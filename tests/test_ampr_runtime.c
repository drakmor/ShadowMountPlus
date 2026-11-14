#include "sm_ampr_runtime.h"
#include "sm_paths.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "check failed at %s:%d: %s (errno=%d)\n", __FILE__,    \
              __LINE__, #condition, errno);                                    \
      return 1;                                                                \
    }                                                                          \
  } while (0)

static int write_file(const char *path, const void *bytes, size_t size) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (fd < 0)
    return -1;
  const unsigned char *cursor = bytes;
  size_t remaining = size;
  while (remaining > 0) {
    ssize_t written = write(fd, cursor, remaining);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      int saved_errno = written < 0 ? errno : EIO;
      (void)close(fd);
      errno = saved_errno;
      return -1;
    }
    cursor += (size_t)written;
    remaining -= (size_t)written;
  }
  return close(fd);
}

int main(void) {
  char temp_root[] = "/tmp/sm-ampr-runtime-XXXXXX";
  CHECK(mkdtemp(temp_root) != NULL);

  char ng_dir[MAX_PATH];
  char ng_file[MAX_PATH];
  char fallback_root[MAX_PATH];
  char fallback_dir[MAX_PATH];
  char fallback_file[MAX_PATH];
  char top_level_file[MAX_PATH];
  CHECK(snprintf(ng_dir, sizeof(ng_dir), "%s/%s", temp_root,
                 AMPR_NG_EMULATOR_DIR) > 0);
  CHECK(mkdir(ng_dir, 0700) == 0);
  CHECK(snprintf(ng_file, sizeof(ng_file), "%s/%s", ng_dir,
                 AMPR_EMULATOR_FILE_NAME) > 0);
  CHECK(snprintf(fallback_root, sizeof(fallback_root), "%s/managed-runtime",
                 temp_root) > 0);
  CHECK(snprintf(fallback_dir, sizeof(fallback_dir), "%s/%s", fallback_root,
                 AMPR_NG_EMULATOR_DIR) > 0);
  CHECK(snprintf(fallback_file, sizeof(fallback_file), "%s/%s", fallback_dir,
                 AMPR_EMULATOR_FILE_NAME) > 0);
  CHECK(snprintf(top_level_file, sizeof(top_level_file), "%s/%s", temp_root,
                 AMPR_EMULATOR_FILE_NAME) > 0);

  unsigned char *binary = calloc(1, 64u * 1024u + 64u);
  CHECK(binary != NULL);
  static const char marker[] = "ampr-emu-ng 0.4.0";
  size_t split_marker_at = 64u * 1024u - 4u;
  memcpy(binary + split_marker_at, marker, sizeof(marker) - 1u);
  CHECK(write_file(ng_file, binary, 64u * 1024u + 64u) == 0);
  CHECK(sm_file_is_ampr_ng(ng_file) == 1);

  char resolved[MAX_PATH];
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == 0);
  CHECK(strcmp(resolved, ng_file) == 0);

  CHECK(write_file(top_level_file, "upstream AMPR", 13u) == 0);
  CHECK(sm_file_is_ampr_ng(top_level_file) == 0);
  CHECK(unlink(ng_file) == 0);
  CHECK(write_file(ng_file, "upstream AMPR", 13u) == 0);
  errno = 0;
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == -1);
  CHECK(errno == ENOEXEC);

  CHECK(unlink(ng_file) == 0);
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == 0);
  CHECK(strcmp(resolved, fallback_file) == 0);
  CHECK(sm_file_is_ampr_ng(fallback_file) == 1);
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == 0);
  CHECK(strcmp(resolved, fallback_file) == 0);

  CHECK(unlink(fallback_file) == 0);
  CHECK(write_file(fallback_file, "upstream AMPR", 13u) == 0);
  errno = 0;
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == -1);
  CHECK(errno == ENOEXEC);
  CHECK(sm_file_is_ampr_ng(fallback_file) == 0);
  CHECK(unlink(fallback_file) == 0);
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == 0);
  CHECK(sm_file_is_ampr_ng(fallback_file) == 1);

  CHECK(unlink(top_level_file) == 0);
  CHECK(unlink(fallback_file) == 0);
  CHECK(rmdir(fallback_dir) == 0);
  CHECK(rmdir(fallback_root) == 0);
  CHECK(symlink(ng_file, fallback_root) == 0);
  errno = 0;
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == -1);
  CHECK(errno == ENOTDIR);
  CHECK(unlink(fallback_root) == 0);
  CHECK(mkdir(fallback_root, 0700) == 0);
  CHECK(mkdir(fallback_dir, 0700) == 0);
  CHECK(symlink(ng_file, fallback_file) == 0);
  errno = 0;
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == -1);
  CHECK(errno == ELOOP);
  CHECK(unlink(fallback_file) == 0);
  CHECK(rmdir(fallback_dir) == 0);
  CHECK(rmdir(fallback_root) == 0);
  CHECK(symlink(temp_root, fallback_root) == 0);
  errno = 0;
  CHECK(sm_ampr_resolve_ng_runtime(temp_root, fallback_root, resolved) == -1);
  CHECK(errno == ENOTDIR);
  CHECK(unlink(fallback_root) == 0);
  CHECK(rmdir(ng_dir) == 0);
  CHECK(rmdir(temp_root) == 0);
  free(binary);
  puts("AMPR runtime path and identity checks passed");
  return 0;
}
