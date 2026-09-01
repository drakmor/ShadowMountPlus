#include "sm_platform.h"

#include <dirent.h>

#include "sm_limits.h"
#include "sm_runtime.h"
#include "sm_storage.h"

#define STORAGE_COPY_BUFFER_SIZE (256u * 1024u)

static bool valid_storage_path(const char *path) {
  if (!path || path[0] != '/' || path[1] == '\0')
    return false;
  for (const char *component = path + 1; *component != '\0';) {
    const char *end = strchr(component, '/');
    size_t length = end ? (size_t)(end - component) : strlen(component);
    if ((length == 1 && component[0] == '.') ||
        (length == 2 && component[0] == '.' && component[1] == '.')) {
      return false;
    }
    if (!end)
      break;
    component = end + 1;
  }
  return true;
}

static bool storage_operation_cancelled(sm_storage_cancel_fn cancel,
                                        void *ctx) {
  return should_stop_requested() || runtime_sleep_mode_active() ||
         (cancel && cancel(ctx));
}

static int join_storage_path(const char *parent, const char *name,
                             char out[MAX_PATH]) {
  int written = snprintf(out, MAX_PATH, "%s/%s", parent, name);
  if (written < 0 || (size_t)written >= MAX_PATH) {
    errno = ENAMETOOLONG;
    return -1;
  }
  return 0;
}

static int measure_storage_path(const char *path, uint64_t *size,
                                uint64_t *file_count,
                                sm_storage_cancel_fn cancel, void *ctx,
                                bool allow_non_regular) {
  if (storage_operation_cancelled(cancel, ctx)) {
    errno = ECANCELED;
    return -1;
  }

  struct stat st;
  if (lstat(path, &st) != 0)
    return -1;
  if (!S_ISDIR(st.st_mode)) {
    if (!S_ISREG(st.st_mode) && !allow_non_regular) {
      errno = EINVAL;
      return -1;
    }
    if (S_ISREG(st.st_mode)) {
      uint64_t file_size = st.st_size > 0 ? (uint64_t)st.st_size : 0;
      if (UINT64_MAX - *size < file_size) {
        errno = EOVERFLOW;
        return -1;
      }
      *size += file_size;
    }
    if (*file_count == UINT64_MAX) {
      errno = EOVERFLOW;
      return -1;
    }
    (*file_count)++;
    return 0;
  }
  DIR *dir = opendir(path);
  if (!dir)
    return -1;
  int result = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno != 0)
        result = -1;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char child[MAX_PATH];
    if (join_storage_path(path, entry->d_name, child) != 0 ||
        measure_storage_path(child, size, file_count, cancel, ctx,
                             allow_non_regular) != 0) {
      result = -1;
      break;
    }
  }
  int saved_errno = result != 0 ? errno : 0;
  if (closedir(dir) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0)
    errno = saved_errno;
  return result;
}

int sm_storage_measure_path(const char *path, uint64_t *size_out) {
  uint64_t file_count = 0;
  return sm_storage_measure_path_progress(path, size_out, &file_count, NULL,
                                          NULL);
}

int sm_storage_measure_path_progress(const char *path, uint64_t *size_out,
                                     uint64_t *file_count_out,
                                     sm_storage_cancel_fn cancel, void *ctx) {
  if (!path || !size_out || !file_count_out) {
    errno = EINVAL;
    return -1;
  }
  *size_out = 0;
  *file_count_out = 0;
  return measure_storage_path(path, size_out, file_count_out, cancel, ctx,
                              false);
}

int sm_storage_measure_delete_path_progress(
    const char *path, uint64_t *size_out, uint64_t *file_count_out,
    sm_storage_cancel_fn cancel, void *ctx) {
  if (!path || !size_out || !file_count_out) {
    errno = EINVAL;
    return -1;
  }
  *size_out = 0;
  *file_count_out = 0;
  return measure_storage_path(path, size_out, file_count_out, cancel, ctx,
                              true);
}

static int copy_regular_file(const char *source, const char *destination,
                             mode_t mode, void *buffer,
                             sm_storage_progress_fn progress,
                             sm_storage_cancel_fn cancel, void *ctx) {
  int source_fd = open(source, O_RDONLY);
  if (source_fd < 0)
    return -1;
  int destination_fd =
      open(destination, O_WRONLY | O_CREAT | O_EXCL, mode & 0777);
  if (destination_fd < 0) {
    int saved_errno = errno;
    (void)close(source_fd);
    errno = saved_errno;
    return -1;
  }

  int result = 0;
  while (result == 0) {
    if (storage_operation_cancelled(cancel, ctx)) {
      errno = ECANCELED;
      result = -1;
      break;
    }
    ssize_t read_size = read(source_fd, buffer, STORAGE_COPY_BUFFER_SIZE);
    if (read_size < 0) {
      if (errno == EINTR)
        continue;
      result = -1;
      break;
    }
    if (read_size == 0)
      break;
    size_t written = 0;
    while (written < (size_t)read_size) {
      ssize_t write_size =
          write(destination_fd, (const unsigned char *)buffer + written,
                (size_t)read_size - written);
      if (write_size < 0) {
        if (errno == EINTR)
          continue;
        result = -1;
        break;
      }
      if (write_size == 0) {
        errno = EIO;
        result = -1;
        break;
      }
      written += (size_t)write_size;
      if (progress)
        progress((uint64_t)write_size, 0, ctx);
    }
  }
  int saved_errno = result != 0 ? errno : 0;
  if (result == 0 && fsync(destination_fd) != 0) {
    result = -1;
    saved_errno = errno;
  }
  if (close(destination_fd) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (close(source_fd) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0) {
    if (unlink(destination) != 0 && errno != ENOENT)
      saved_errno = errno;
  } else if (progress) {
    progress(0, 1, ctx);
  }
  if (result != 0)
    errno = saved_errno;
  return result;
}

static int delete_storage_path(const char *path,
                               sm_storage_progress_fn progress, void *ctx);

static int copy_storage_path(const char *source, const char *destination,
                             void *buffer,
                             sm_storage_progress_fn progress,
                             sm_storage_cancel_fn cancel, void *ctx) {
  if (storage_operation_cancelled(cancel, ctx)) {
    errno = ECANCELED;
    return -1;
  }

  struct stat st;
  if (lstat(source, &st) != 0)
    return -1;
  if (S_ISREG(st.st_mode))
    return copy_regular_file(source, destination, st.st_mode, buffer, progress,
                             cancel, ctx);
  if (!S_ISDIR(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  if (mkdir(destination, st.st_mode & 0777) != 0)
    return -1;

  DIR *dir = opendir(source);
  if (!dir) {
    int saved_errno = errno;
    (void)rmdir(destination);
    errno = saved_errno;
    return -1;
  }
  int result = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno != 0)
        result = -1;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char source_child[MAX_PATH];
    char destination_child[MAX_PATH];
    if (join_storage_path(source, entry->d_name, source_child) != 0 ||
        join_storage_path(destination, entry->d_name, destination_child) != 0 ||
        copy_storage_path(source_child, destination_child, buffer, progress,
                          cancel, ctx) != 0) {
      result = -1;
      break;
    }
  }
  int saved_errno = result != 0 ? errno : 0;
  if (closedir(dir) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0) {
    if (delete_storage_path(destination, NULL, NULL) != 0 && errno != ENOENT)
      saved_errno = errno;
    errno = saved_errno;
  }
  return result;
}

int sm_storage_copy_path(const char *source, const char *destination) {
  return sm_storage_copy_path_progress(source, destination, NULL, NULL, NULL);
}

int sm_storage_copy_path_progress(const char *source, const char *destination,
                                  sm_storage_progress_fn progress,
                                  sm_storage_cancel_fn cancel, void *ctx) {
  if (!valid_storage_path(source) || !valid_storage_path(destination) ||
      strcmp(source, destination) == 0) {
    errno = EINVAL;
    return -1;
  }
  struct stat st;
  if (lstat(destination, &st) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT)
    return -1;
  void *buffer = malloc(STORAGE_COPY_BUFFER_SIZE);
  if (!buffer) {
    errno = ENOMEM;
    return -1;
  }
  int result = copy_storage_path(source, destination, buffer, progress, cancel,
                                 ctx);
  int saved_errno = result != 0 ? errno : 0;
  free(buffer);
  if (result != 0)
    errno = saved_errno;
  return result;
}

static int delete_storage_path(const char *path,
                               sm_storage_progress_fn progress, void *ctx) {
  struct stat st;
  if (lstat(path, &st) != 0)
    return -1;
  if (!S_ISDIR(st.st_mode)) {
    if (unlink(path) != 0)
      return -1;
    if (progress) {
      uint64_t size = S_ISREG(st.st_mode) && st.st_size > 0
                          ? (uint64_t)st.st_size
                          : 0;
      progress(size, 1, ctx);
    }
    return 0;
  }

  DIR *dir = opendir(path);
  if (!dir)
    return -1;
  int result = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno != 0)
        result = -1;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    char child[MAX_PATH];
    if (join_storage_path(path, entry->d_name, child) != 0 ||
        delete_storage_path(child, progress, ctx) != 0) {
      result = -1;
      break;
    }
  }
  int saved_errno = result != 0 ? errno : 0;
  if (closedir(dir) != 0 && result == 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result == 0 && rmdir(path) != 0) {
    result = -1;
    saved_errno = errno;
  }
  if (result != 0)
    errno = saved_errno;
  return result;
}

int sm_storage_delete_path(const char *path) {
  return sm_storage_delete_path_progress(path, NULL, NULL);
}

int sm_storage_delete_path_progress(const char *path,
                                    sm_storage_progress_fn progress,
                                    void *ctx) {
  if (!valid_storage_path(path)) {
    errno = EINVAL;
    return -1;
  }
  if (storage_operation_cancelled(NULL, NULL)) {
    errno = ECANCELED;
    return -1;
  }
  // Once recursive deletion starts it must finish. Cancelling in the middle
  // would intentionally leave the only source tree partially removed.
  return delete_storage_path(path, progress, ctx);
}

int sm_storage_move_path(const char *source, const char *destination) {
  return sm_storage_move_path_progress(source, destination, NULL, NULL, NULL,
                                       NULL, NULL);
}

int sm_storage_move_path_progress(const char *source, const char *destination,
                                  sm_storage_progress_fn progress,
                                  sm_storage_cancel_fn cancel,
                                  sm_storage_finalize_fn begin_finalize,
                                  void *ctx,
                                  bool *renamed_out) {
  if (!valid_storage_path(source) || !valid_storage_path(destination) ||
      strcmp(source, destination) == 0) {
    errno = EINVAL;
    return -1;
  }
  if (renamed_out)
    *renamed_out = false;
  if (storage_operation_cancelled(cancel, ctx)) {
    errno = ECANCELED;
    return -1;
  }
  struct stat st;
  if (lstat(source, &st) != 0)
    return -1;
  if (!S_ISREG(st.st_mode) && !S_ISDIR(st.st_mode)) {
    errno = EINVAL;
    return -1;
  }
  if (lstat(destination, &st) == 0) {
    errno = EEXIST;
    return -1;
  }
  if (errno != ENOENT)
    return -1;
  if (rename(source, destination) == 0) {
    if (renamed_out)
      *renamed_out = true;
    return 0;
  }
  if (errno != EXDEV)
    return -1;
  if (sm_storage_copy_path_progress(source, destination, progress, cancel,
                                    ctx) != 0)
    return -1;
  if (storage_operation_cancelled(cancel, ctx) ||
      (begin_finalize && !begin_finalize(ctx)) ||
      storage_operation_cancelled(NULL, NULL)) {
    int saved_errno = ECANCELED;
    if (delete_storage_path(destination, NULL, NULL) != 0 && errno != ENOENT)
      saved_errno = errno;
    errno = saved_errno;
    return -1;
  }
  if (sm_storage_delete_path(source) == 0)
    return 0;
  // Keep the complete destination copy if source cleanup failed. Removing it
  // could leave only a partially deleted source and lose recoverable data.
  return -1;
}
