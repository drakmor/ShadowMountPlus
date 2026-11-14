/* Stable, no-follow snapshot of files used to build an FPKG backport. */
#include "sm_platform.h"
#include "sm_fpkg_backport.h"
#include "sm_limits.h"
#include <strings.h>

#define BACKPORT_FILE_LIMIT 100000u

typedef struct {
  sm_fpkg_index_file_t file;
  struct stat st;
} backport_entry_t;

struct sm_fpkg_backport {
  char *root;
  int root_fd;
  backport_entry_t *entries;
  sm_fpkg_index_file_t *files;
  size_t count, capacity;
  uint64_t fingerprint;
};

static unsigned char fold_ascii(unsigned char c) {
  return c >= 'A' && c <= 'Z' ? (unsigned char)(c + ('a' - 'A')) : c;
}

static int compare_path(const char *a, const char *b) {
  while (*a && *b) {
    unsigned char x = fold_ascii((unsigned char)*a++);
    unsigned char y = fold_ascii((unsigned char)*b++);
    if (x != y)
      return x < y ? -1 : 1;
  }
  return ((unsigned char)*a > (unsigned char)*b) -
         ((unsigned char)*a < (unsigned char)*b);
}

static int compare_entries(const void *a, const void *b) {
  const backport_entry_t *x = a, *y = b;
  return compare_path(x->file.path, y->file.path);
}

static bool same_stat(const struct stat *a, const struct stat *b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
         a->st_mode == b->st_mode && a->st_size == b->st_size &&
         a->st_mtim.tv_sec == b->st_mtim.tv_sec &&
         a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
         a->st_ctim.tv_sec == b->st_ctim.tv_sec &&
         a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

static void hash_bytes(uint64_t *hash, const void *data, size_t size) {
  const unsigned char *p = data;
  for (size_t i = 0; i < size; ++i) {
    *hash ^= p[i];
    *hash *= UINT64_C(1099511628211);
  }
}

static sm_fpkg_status_t append_entry(sm_fpkg_backport_t *snapshot,
                                      const char *path, const struct stat *st) {
  if (snapshot->count >= BACKPORT_FILE_LIMIT || st->st_size < 0)
    return SM_FPKG_UNSUPPORTED;
  if (snapshot->count == snapshot->capacity) {
    size_t capacity = snapshot->capacity ? snapshot->capacity * 2u : 32u;
    if (capacity > BACKPORT_FILE_LIMIT)
      capacity = BACKPORT_FILE_LIMIT;
    backport_entry_t *entries = realloc(snapshot->entries,
                                       capacity * sizeof(*entries));
    if (!entries)
      return SM_FPKG_NOMEM;
    snapshot->entries = entries;
    snapshot->capacity = capacity;
  }
  char *copy = strdup(path);
  if (!copy)
    return SM_FPKG_NOMEM;
  snapshot->entries[snapshot->count++] = (backport_entry_t){
      .file = {.path = copy, .size = (uint64_t)st->st_size}, .st = *st};
  return SM_FPKG_OK;
}

static sm_fpkg_status_t walk_dir(sm_fpkg_backport_t *snapshot, int dir_fd,
                                 const char *prefix, unsigned depth) {
  if (depth > MAX_PATH / 2u)
    return SM_FPKG_UNSUPPORTED;
  struct stat before, after;
  if (fstat(dir_fd, &before) != 0 || !S_ISDIR(before.st_mode))
    return SM_FPKG_IO;
  int iterator_fd = openat(dir_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (iterator_fd < 0)
    return SM_FPKG_IO;
  DIR *dir = fdopendir(iterator_fd);
  if (!dir) {
    close(iterator_fd);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      if (errno)
        status = SM_FPKG_IO;
      break;
    }
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    if (!prefix[0] && (!strcasecmp(entry->d_name, "fakelib") ||
                       !strcasecmp(entry->d_name, "fakelib2")))
      continue;
    if (strchr(entry->d_name, '\n') || strchr(entry->d_name, '\r') ||
        strchr(entry->d_name, '\\')) {
      status = SM_FPKG_UNSUPPORTED;
      break;
    }
    char path[MAX_PATH];
    int length = snprintf(path, sizeof(path), "%s%s%s", prefix,
                          prefix[0] ? "/" : "", entry->d_name);
    if (length < 0 || (size_t)length >= sizeof(path)) {
      status = SM_FPKG_UNSUPPORTED;
      break;
    }
    struct stat st;
    if (fstatat(dir_fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0) {
      status = SM_FPKG_IO;
      break;
    }
    if (S_ISDIR(st.st_mode)) {
      int child = openat(dir_fd, entry->d_name,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat opened;
      if (child < 0 || fstat(child, &opened) != 0 || !same_stat(&st, &opened)) {
        if (child >= 0) close(child);
        status = SM_FPKG_IO;
        break;
      }
      status = walk_dir(snapshot, child, path, depth + 1u);
      close(child);
    } else if (S_ISREG(st.st_mode)) {
      status = append_entry(snapshot, path, &st);
    } else {
      status = SM_FPKG_UNSUPPORTED;
    }
    if (status != SM_FPKG_OK)
      break;
  }
  if (closedir(dir) != 0 && status == SM_FPKG_OK)
    status = SM_FPKG_IO;
  if (status == SM_FPKG_OK &&
      (fstat(dir_fd, &after) != 0 || !same_stat(&before, &after)))
    status = SM_FPKG_IO;
  return status;
}

void sm_fpkg_backport_close(sm_fpkg_backport_t *snapshot) {
  if (!snapshot) return;
  if (snapshot->root_fd >= 0) close(snapshot->root_fd);
  for (size_t i = 0; i < snapshot->count; ++i)
    free((char *)snapshot->entries[i].file.path);
  free(snapshot->entries);
  free(snapshot->files);
  free(snapshot->root);
  free(snapshot);
}

sm_fpkg_status_t sm_fpkg_backport_open(const char *path,
                                        sm_fpkg_backport_t **out) {
  if (!out) return SM_FPKG_CORRUPT;
  *out = NULL;
  sm_fpkg_backport_t *snapshot = calloc(1, sizeof(*snapshot));
  if (!snapshot) return SM_FPKG_NOMEM;
  snapshot->root_fd = -1;
  if (!path || !path[0]) {
    *out = snapshot;
    return SM_FPKG_OK;
  }
  char canonical[MAX_PATH];
  if (!realpath(path, canonical)) {
    sm_fpkg_backport_close(snapshot);
    return SM_FPKG_IO;
  }
  snapshot->root = strdup(canonical);
  snapshot->root_fd = open(canonical, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (!snapshot->root || snapshot->root_fd < 0) {
    sm_fpkg_backport_close(snapshot);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t status = walk_dir(snapshot, snapshot->root_fd, "", 0);
  if (status == SM_FPKG_OK && snapshot->count > 1) {
    qsort(snapshot->entries, snapshot->count, sizeof(*snapshot->entries),
          compare_entries);
    for (size_t i = 1; i < snapshot->count; ++i) {
      if (compare_path(snapshot->entries[i - 1].file.path,
                       snapshot->entries[i].file.path) == 0) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
  }
  if (status == SM_FPKG_OK && snapshot->count) {
    snapshot->files = calloc(snapshot->count, sizeof(*snapshot->files));
    if (!snapshot->files) status = SM_FPKG_NOMEM;
  }
  if (status == SM_FPKG_OK) {
    uint64_t hash = UINT64_C(14695981039346656037);
    hash_bytes(&hash, canonical, strlen(canonical) + 1u);
    for (size_t i = 0; i < snapshot->count; ++i) {
      backport_entry_t *e = &snapshot->entries[i];
      snapshot->files[i] = e->file;
      hash_bytes(&hash, e->file.path, strlen(e->file.path) + 1u);
      const uint64_t values[] = {(uint64_t)e->st.st_dev, (uint64_t)e->st.st_ino,
        (uint64_t)e->st.st_size, (uint64_t)e->st.st_mtim.tv_sec,
        (uint64_t)e->st.st_mtim.tv_nsec, (uint64_t)e->st.st_ctim.tv_sec,
        (uint64_t)e->st.st_ctim.tv_nsec};
      hash_bytes(&hash, values, sizeof(values));
    }
    snapshot->fingerprint = hash ? hash : 1u;
  }
  if (status != SM_FPKG_OK) {
    sm_fpkg_backport_close(snapshot);
    return status;
  }
  *out = snapshot;
  return SM_FPKG_OK;
}

size_t sm_fpkg_backport_count(const sm_fpkg_backport_t *s) { return s ? s->count : 0; }
const sm_fpkg_index_file_t *sm_fpkg_backport_files(const sm_fpkg_backport_t *s) {
  return s ? s->files : NULL;
}
uint64_t sm_fpkg_backport_fingerprint(const sm_fpkg_backport_t *s) {
  return s ? s->fingerprint : 0;
}
size_t sm_fpkg_backport_find(const sm_fpkg_backport_t *s, const char *path) {
  if (!s || !path) return SIZE_MAX;
  size_t left = 0, right = s->count;
  while (left < right) {
    size_t mid = left + (right - left) / 2u;
    int order = compare_path(s->entries[mid].file.path, path);
    if (order < 0) left = mid + 1u;
    else right = mid;
  }
  return left < s->count && !compare_path(s->entries[left].file.path, path)
      ? left : SIZE_MAX;
}

int sm_fpkg_backport_open_file(const sm_fpkg_backport_t *s, size_t index) {
  if (!s || s->root_fd < 0 || index >= s->count) { errno = EINVAL; return -1; }
  char path[MAX_PATH];
  if (strlcpy(path, s->entries[index].file.path, sizeof(path)) >= sizeof(path)) {
    errno = ENAMETOOLONG; return -1;
  }
  int parent = dup(s->root_fd);
  if (parent < 0) return -1;
  char *component = path;
  for (;;) {
    char *slash = strchr(component, '/');
    if (!slash) break;
    *slash = '\0';
    int next = openat(parent, component, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(parent);
    if (next < 0) return -1;
    parent = next;
    component = slash + 1;
  }
  int fd = openat(parent, component, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  close(parent);
  if (fd < 0) return -1;
  struct stat st;
  if (fstat(fd, &st) != 0 || !same_stat(&st, &s->entries[index].st)) {
    close(fd); errno = ESTALE; return -1;
  }
  return fd;
}

bool sm_fpkg_backport_unchanged(const sm_fpkg_backport_t *snapshot) {
  if (!snapshot) return false;
  sm_fpkg_backport_t *again = NULL;
  if (sm_fpkg_backport_open(snapshot->root, &again) != SM_FPKG_OK)
    return false;
  bool same = snapshot->fingerprint == again->fingerprint &&
              snapshot->count == again->count;
  sm_fpkg_backport_close(again);
  return same;
}
