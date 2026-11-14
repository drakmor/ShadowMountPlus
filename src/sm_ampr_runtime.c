#include "sm_ampr_runtime.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sm_paths.h"

#define AMPR_IDENTITY_CHUNK (64u * 1024u)

extern unsigned char sm_ampr_ng_runtime[];
extern unsigned int sm_ampr_ng_runtime_len;

static bool contains_marker(const unsigned char *bytes, size_t size,
                            const char *marker, size_t marker_size) {
  if (size < marker_size)
    return false;
  for (size_t i = 0; i <= size - marker_size; ++i) {
    if (memcmp(bytes + i, marker, marker_size) == 0)
      return true;
  }
  return false;
}

static int descriptor_is_ampr_ng(int fd) {
  static const char marker[] = "ampr-emu-ng ";
  enum { MARKER_SIZE = sizeof(marker) - 1u };
  unsigned char buffer[AMPR_IDENTITY_CHUNK + MARKER_SIZE - 1u];
  size_t kept = 0;
  int result = 0;
  int saved_errno = 0;
  for (;;) {
    ssize_t received = read(fd, buffer + kept, AMPR_IDENTITY_CHUNK);
    if (received < 0 && errno == EINTR)
      continue;
    if (received < 0) {
      result = -1;
      saved_errno = errno;
      break;
    }
    if (received == 0)
      break;

    size_t length = kept + (size_t)received;
    if (contains_marker(buffer, length, marker, MARKER_SIZE)) {
      result = 1;
      break;
    }
    kept = length < MARKER_SIZE - 1u ? length : MARKER_SIZE - 1u;
    memmove(buffer, buffer + length - kept, kept);
  }

  if (result < 0)
    errno = saved_errno;
  return result;
}

int sm_file_is_ampr_ng(const char *path) {
  int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK);
  if (fd < 0)
    return -1;

  struct stat st;
  int result;
  int saved_errno = 0;
  if (fstat(fd, &st) != 0) {
    result = -1;
    saved_errno = errno;
  } else if (!S_ISREG(st.st_mode)) {
    result = -1;
    saved_errno = EINVAL;
  } else {
    result = descriptor_is_ampr_ng(fd);
    if (result < 0)
      saved_errno = errno;
  }

  if (close(fd) != 0 && result >= 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result < 0)
    errno = saved_errno;
  return result;
}

static bool format_runtime_path(const char *root, char path[MAX_PATH]) {
  int written = snprintf(path, MAX_PATH, "%s/%s/%s", root,
                         AMPR_NG_EMULATOR_DIR, AMPR_EMULATOR_FILE_NAME);
  if (written <= 0 || (size_t)written >= MAX_PATH) {
    path[0] = '\0';
    errno = ENAMETOOLONG;
    return false;
  }
  return true;
}

// Returns 1 for an identity-checked file, 0 when missing, and -1 on errors.
static int validate_runtime_file(const char *path) {
  int is_ng = sm_file_is_ampr_ng(path);
  if (is_ng < 0 && (errno == ENOENT || errno == ENOTDIR))
    return 0;
  if (is_ng < 0)
    return -1;
  if (is_ng == 0) {
    errno = ENOEXEC;
    return -1;
  }
  return 1;
}

static bool ensure_directory(const char *path) {
  if (!path || path[0] != '/') {
    errno = EINVAL;
    return false;
  }
  struct stat st;
  if (lstat(path, &st) == 0) {
    if (S_ISDIR(st.st_mode))
      return true;
    errno = ENOTDIR;
    return false;
  }
  if (errno != ENOENT)
    return false;

  size_t path_length = strlen(path);
  if (path_length >= MAX_PATH || path_length <= 1u ||
      path[path_length - 1u] == '/') {
    errno = EINVAL;
    return false;
  }
  char parent[MAX_PATH];
  memcpy(parent, path, path_length + 1u);
  char *separator = strrchr(parent, '/');
  if (!separator) {
    errno = EINVAL;
    return false;
  }
  if (separator == parent)
    parent[1] = '\0';
  else
    *separator = '\0';
  if (strcmp(parent, path) != 0 && !ensure_directory(parent))
    return false;

  if (mkdir(path, 0777) == 0)
    return true;
  if (errno != EEXIST || lstat(path, &st) != 0)
    return false;
  if (S_ISDIR(st.st_mode))
    return true;
  errno = ENOTDIR;
  return false;
}

static bool write_all(int fd, const unsigned char *bytes, size_t size) {
  size_t written_total = 0;
  while (written_total < size) {
    ssize_t written = write(fd, bytes + written_total,
                            size - written_total);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      if (written == 0)
        errno = EIO;
      return false;
    }
    written_total += (size_t)written;
  }
  return true;
}

static bool sync_directory(const char *path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  if (fd < 0)
    return false;
  bool ok = fsync(fd) == 0;
  int saved_errno = errno;
  if (close(fd) != 0 && ok) {
    ok = false;
    saved_errno = errno;
  }
  if (!ok)
    errno = saved_errno;
  return ok;
}

static int install_bundled_runtime(const char *fallback_root,
                                   char runtime_path[MAX_PATH]) {
  if (!fallback_root || !fallback_root[0]) {
    errno = EINVAL;
    return -1;
  }
  if (!ensure_directory(fallback_root))
    return -1;

  char runtime_directory[MAX_PATH];
  int written = snprintf(runtime_directory, sizeof(runtime_directory),
                         "%s/%s", fallback_root, AMPR_NG_EMULATOR_DIR);
  if (written <= 0 || (size_t)written >= sizeof(runtime_directory)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  if (!ensure_directory(runtime_directory) ||
      !format_runtime_path(fallback_root, runtime_path))
    return -1;

  int present = validate_runtime_file(runtime_path);
  if (present > 0)
    return 0;
  if (present < 0)
    return -1;

  static const char marker[] = "ampr-emu-ng ";
  if (sm_ampr_ng_runtime_len == 0 ||
      !contains_marker(sm_ampr_ng_runtime, sm_ampr_ng_runtime_len, marker,
                       sizeof(marker) - 1u)) {
    errno = ENOEXEC;
    return -1;
  }

  char temporary_path[MAX_PATH];
  written = snprintf(temporary_path, sizeof(temporary_path),
                     "%s/.%s.XXXXXX", runtime_directory,
                     AMPR_EMULATOR_FILE_NAME);
  if (written <= 0 || (size_t)written >= sizeof(temporary_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  int fd = mkstemp(temporary_path);
  if (fd < 0)
    return -1;

  bool staged = fchmod(fd, 0755) == 0 &&
                write_all(fd, sm_ampr_ng_runtime, sm_ampr_ng_runtime_len) &&
                fsync(fd) == 0;
  int saved_errno = errno;
  if (close(fd) != 0 && staged) {
    staged = false;
    saved_errno = errno;
  }
  if (!staged) {
    (void)unlink(temporary_path);
    errno = saved_errno;
    return -1;
  }

  if (link(temporary_path, runtime_path) != 0) {
    saved_errno = errno;
    (void)unlink(temporary_path);
    if (saved_errno != EEXIST) {
      errno = saved_errno;
      return -1;
    }
    present = validate_runtime_file(runtime_path);
    if (present <= 0)
      return -1;
    return 0;
  }
  if (unlink(temporary_path) != 0 || !sync_directory(runtime_directory))
    return -1;

  present = validate_runtime_file(runtime_path);
  if (present != 1)
    return -1;
  return 0;
}

int sm_ampr_resolve_ng_runtime(const char *emulators_path,
                               const char *fallback_root,
                               char runtime_path[MAX_PATH]) {
  if (!emulators_path || !emulators_path[0] || !fallback_root ||
      !runtime_path) {
    errno = EINVAL;
    return -1;
  }

  if (!format_runtime_path(emulators_path, runtime_path))
    return -1;
  int present = validate_runtime_file(runtime_path);
  if (present > 0)
    return 0;
  if (present < 0)
    return -1;

  return install_bundled_runtime(fallback_root, runtime_path);
}
