#include "sm_platform.h"

#include <pthread.h>

#include "sm_gameinfo.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_pkg_backport.h"
#include "sm_scan.h"

#define NSFS_CONTROL_DEVICE "/dev/nsfsctl"
#define NSFS_IOCTL_CREATE_REDIRECT 0xC0406E00UL
#define NSFS_IOCTL_DELETE_REDIRECT 0xC0406E01UL
#define NSFS_REDIRECT_OVERLAY 1u
#define NSFS_REDIRECT_PLAIN 2u
#define NSFS_SCOPE_UNCONDITIONAL 0u
#define NSFS_BUDGET_BIGAPP 0
#define PKG_BACKPORT_MAX_SESSIONS 2u
#define PKG_BACKPORT_MAX_REDIRECTS 256u
#define PKG_BACKPORT_MAX_DIRECTORY_DEPTH 64u

typedef struct {
  const char *source;
  const char *destination;
  uint64_t aux0;
  uint32_t type;
  uint32_t scope;
  int32_t budget_id;
  uint32_t reserved0;
  uint8_t opaque[0x18];
} nsfs_redirect_desc_t;

typedef struct {
  uint64_t reserved0;
  const char *destination;
  uint8_t zero[0x30];
} nsfs_delete_redirect_t;

typedef struct {
  uint32_t destination_offset;
} pkg_backport_redirect_t;

typedef struct {
  dev_t device;
  fsid_t fsid;
} pkg_backport_namespace_id_t;

typedef struct {
  bool installed;
  bool ready;
  pid_t pid;
  char title_id[MAX_TITLE_ID];
  char root_source[MAX_PATH];
  char root_destination[MAX_PATH];
  pkg_backport_namespace_id_t namespace_id;
  size_t redirect_count;
  size_t destination_storage_size;
  size_t destination_storage_capacity;
  char *destination_storage;
  pkg_backport_redirect_t redirects[PKG_BACKPORT_MAX_REDIRECTS];
} pkg_backport_session_t;

_Static_assert(sizeof(nsfs_redirect_desc_t) == 0x40,
               "unexpected NSFS redirect descriptor size");
_Static_assert(sizeof(nsfs_delete_redirect_t) == 0x40,
               "unexpected NSFS delete descriptor size");
_Static_assert(PKG_BACKPORT_MAX_REDIRECTS * MAX_PATH <= UINT32_MAX,
               "redirect path storage offsets must fit in uint32_t");

static pthread_mutex_t g_pkg_backport_mutex = PTHREAD_MUTEX_INITIALIZER;
static pkg_backport_session_t
    g_pkg_backport_sessions[PKG_BACKPORT_MAX_SESSIONS];

static void reset_session_locked(pkg_backport_session_t *session) {
  free(session->destination_storage);
  memset(session, 0, sizeof(*session));
}

static const char *redirect_destination(
    const pkg_backport_session_t *session,
    const pkg_backport_redirect_t *redirect) {
  if (!session->destination_storage ||
      redirect->destination_offset == UINT32_MAX ||
      redirect->destination_offset >= session->destination_storage_size) {
    return NULL;
  }
  return session->destination_storage + redirect->destination_offset;
}

static bool reserve_destination_storage(pkg_backport_session_t *session,
                                        size_t needed) {
  if (needed <= session->destination_storage_capacity)
    return true;

  size_t capacity = session->destination_storage_capacity;
  if (capacity == 0)
    capacity = 4096u;
  while (capacity < needed) {
    if (capacity > SIZE_MAX / 2u) {
      errno = ENOMEM;
      return false;
    }
    capacity *= 2u;
  }

  char *storage = realloc(session->destination_storage, capacity);
  if (!storage) {
    errno = ENOMEM;
    return false;
  }
  session->destination_storage = storage;
  session->destination_storage_capacity = capacity;
  return true;
}

static bool resolve_pkg_app0_path(
    const char *title_id, char app0_path[MAX_PATH],
    pkg_backport_namespace_id_t *namespace_id) {
  app0_path[0] = '\0';
  memset(namespace_id, 0, sizeof(*namespace_id));
  DIR *dir = opendir("/mnt/sandbox");
  if (!dir)
    return false;

  size_t title_length = strlen(title_id);
  long best_index = -1;
  int scan_errno = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      scan_errno = errno;
      break;
    }
    size_t entry_length = strlen(entry->d_name);
    if (entry_length <= title_length + 1u ||
        strncmp(entry->d_name, title_id, title_length) != 0 ||
        entry->d_name[title_length] != '_') {
      continue;
    }
    const char *suffix = entry->d_name + title_length + 1u;
    if (!isdigit((unsigned char)suffix[0])) {
      continue;
    }
    errno = 0;
    char *end = NULL;
    long index = strtol(suffix, &end, 10);
    if (errno == ERANGE || !end || *end != '\0' || index < best_index)
      continue;

    char candidate[MAX_PATH];
    int written = snprintf(candidate, sizeof(candidate),
                           "/mnt/sandbox/%s/app0", entry->d_name);
    if (written <= 0 || (size_t)written >= sizeof(candidate))
      continue;
    struct stat st;
    struct statfs fs;
    if (stat(candidate, &st) != 0 || !S_ISDIR(st.st_mode) ||
        statfs(candidate, &fs) != 0 ||
        strcmp(fs.f_fstypename, "nsfs") != 0 ||
        strcmp(fs.f_mntonname, candidate) != 0) {
      continue;
    }
    best_index = index;
    (void)strlcpy(app0_path, candidate, MAX_PATH);
    namespace_id->device = st.st_dev;
    memcpy(&namespace_id->fsid, &fs.f_fsid, sizeof(namespace_id->fsid));
  }
  if (closedir(dir) != 0 && scan_errno == 0)
    scan_errno = errno;
  if (scan_errno != 0) {
    app0_path[0] = '\0';
    errno = scan_errno;
    return false;
  }
  if (best_index < 0) {
    app0_path[0] = '\0';
    return false;
  }
  return true;
}

static pkg_backport_session_t *
find_session_by_title_locked(const char *title_id) {
  for (size_t i = 0; i < PKG_BACKPORT_MAX_SESSIONS; ++i) {
    pkg_backport_session_t *session = &g_pkg_backport_sessions[i];
    if (session->installed && strcmp(session->title_id, title_id) == 0)
      return session;
  }
  return NULL;
}

static pkg_backport_session_t *find_free_session_locked(void) {
  for (size_t i = 0; i < PKG_BACKPORT_MAX_SESSIONS; ++i) {
    if (!g_pkg_backport_sessions[i].installed)
      return &g_pkg_backport_sessions[i];
  }
  return NULL;
}

static bool namespace_id_equal(const pkg_backport_namespace_id_t *left,
                               const pkg_backport_namespace_id_t *right) {
  return left->device == right->device &&
         memcmp(&left->fsid, &right->fsid, sizeof(left->fsid)) == 0;
}

static pkg_backport_session_t *prepare_session_locked(
    const char *title_id, const char *source, const char *destination,
    const pkg_backport_namespace_id_t *namespace_id, bool *already_ready) {
  *already_ready = false;

  pkg_backport_session_t *session = find_session_by_title_locked(title_id);
  if (session && !namespace_id_equal(&session->namespace_id, namespace_id)) {
    log_debug("  [BKP][NSFS] stale package namespace released: title=%s "
              "dst=%s",
              session->title_id, session->root_destination);
    reset_session_locked(session);
    session = NULL;
  }

  if (session) {
    if (session->ready && strcmp(session->root_source, source) == 0 &&
        strcmp(session->root_destination, destination) == 0) {
      *already_ready = true;
      return session;
    }
    errno = EBUSY;
    return NULL;
  }

  session = find_free_session_locked();
  if (!session) {
    errno = EBUSY;
    return NULL;
  }

  reset_session_locked(session);
  (void)strlcpy(session->title_id, title_id, sizeof(session->title_id));
  (void)strlcpy(session->root_source, source, sizeof(session->root_source));
  (void)strlcpy(session->root_destination, destination,
                sizeof(session->root_destination));
  session->namespace_id = *namespace_id;
  return session;
}

static bool delete_redirect(int fd, const char *destination,
                            int *delete_errno) {
  nsfs_delete_redirect_t request = {0};
  request.destination = destination;
  if (ioctl(fd, NSFS_IOCTL_DELETE_REDIRECT, &request) == 0) {
    *delete_errno = 0;
    return true;
  }
  *delete_errno = errno;
  return *delete_errno == ENOENT || *delete_errno == EINVAL;
}

static bool rollback_redirects_locked(int fd,
                                      pkg_backport_session_t *session) {
  bool cleaned = true;
  for (size_t i = session->redirect_count; i > 0; --i) {
    pkg_backport_redirect_t *redirect = &session->redirects[i - 1u];
    const char *destination = redirect_destination(session, redirect);
    if (!destination)
      continue;
    int delete_errno = 0;
    if (!delete_redirect(fd, destination, &delete_errno)) {
      cleaned = false;
      log_debug("  [BKP][NSFS] rollback failed: title=%s dst=%s "
                "error=%d (%s)",
                session->title_id, destination, delete_errno,
                strerror(delete_errno));
      continue;
    }
    redirect->destination_offset = UINT32_MAX;
  }
  if (cleaned)
    session->redirect_count = 0;
  return cleaned;
}

static bool create_redirect_locked(int fd, pkg_backport_session_t *session,
                                   const char *source,
                                   const char *destination, uint32_t type) {
  if (session->redirect_count >= PKG_BACKPORT_MAX_REDIRECTS) {
    errno = E2BIG;
    return false;
  }

  size_t destination_length = strlen(destination) + 1u;
  if (destination_length > SIZE_MAX - session->destination_storage_size) {
    errno = EOVERFLOW;
    return false;
  }
  size_t destination_offset = session->destination_storage_size;
  size_t needed = destination_offset + destination_length;
  if (destination_offset > UINT32_MAX ||
      !reserve_destination_storage(session, needed)) {
    if (destination_offset > UINT32_MAX)
      errno = EOVERFLOW;
    return false;
  }

  nsfs_redirect_desc_t request = {0};
  request.source = source;
  request.destination = destination;
  request.type = type;
  request.scope = NSFS_SCOPE_UNCONDITIONAL;
  request.budget_id = NSFS_BUDGET_BIGAPP;
  if (ioctl(fd, NSFS_IOCTL_CREATE_REDIRECT, &request) != 0)
    return false;

  pkg_backport_redirect_t *redirect =
      &session->redirects[session->redirect_count++];
  memcpy(session->destination_storage + destination_offset, destination,
         destination_length);
  session->destination_storage_size = needed;
  redirect->destination_offset = (uint32_t)destination_offset;
  return true;
}

static bool create_child_redirects_locked(int fd,
                                          pkg_backport_session_t *session,
                                          const char *source_directory,
                                          const char *destination_directory,
                                          bool source_root, unsigned depth) {
  if (depth >= PKG_BACKPORT_MAX_DIRECTORY_DEPTH) {
    errno = ELOOP;
    return false;
  }
  DIR *dir = opendir(source_directory);
  if (!dir)
    return false;

  int failure_errno = 0;
  for (;;) {
    errno = 0;
    struct dirent *entry = readdir(dir);
    if (!entry) {
      failure_errno = errno;
      break;
    }
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    if (source_root && (strcmp(entry->d_name, "fakelib") == 0 ||
                        strcmp(entry->d_name, "fakelib2") == 0)) {
      continue;
    }

    char child_source[MAX_PATH];
    char child_destination[MAX_PATH];
    int source_written =
        snprintf(child_source, sizeof(child_source), "%s/%s",
                 source_directory, entry->d_name);
    int destination_written =
        snprintf(child_destination, sizeof(child_destination), "%s/%s",
                 destination_directory, entry->d_name);
    if (source_written <= 0 ||
        (size_t)source_written >= sizeof(child_source) ||
        destination_written <= 0 ||
        (size_t)destination_written >= sizeof(child_destination)) {
      failure_errno = ENAMETOOLONG;
      break;
    }

    struct stat source_st;
    if (lstat(child_source, &source_st) != 0) {
      failure_errno = errno;
      break;
    }
    if (S_ISREG(source_st.st_mode)) {
      if (!create_redirect_locked(fd, session, child_source,
                                  child_destination, NSFS_REDIRECT_PLAIN)) {
        failure_errno = errno;
        break;
      }
      continue;
    }
    if (!S_ISDIR(source_st.st_mode))
      continue;

    struct stat destination_st;
    if (stat(child_destination, &destination_st) == 0) {
      if (!S_ISDIR(destination_st.st_mode)) {
        failure_errno = ENOTDIR;
        break;
      }
      if (!create_child_redirects_locked(fd, session, child_source,
                                         child_destination, false,
                                         depth + 1u)) {
        failure_errno = errno;
        break;
      }
      continue;
    }
    if (errno != ENOENT ||
        !create_redirect_locked(fd, session, child_source,
                                child_destination, NSFS_REDIRECT_OVERLAY)) {
      failure_errno = errno;
      break;
    }
  }

  if (closedir(dir) != 0 && failure_errno == 0)
    failure_errno = errno;
  if (failure_errno == 0)
    return true;
  errno = failure_errno;
  return false;
}

static bool session_namespace_released(const pkg_backport_session_t *session) {
  struct stat st;
  struct statfs fs;
  if (stat(session->root_destination, &st) != 0)
    return errno == ENOENT;
  if (!S_ISDIR(st.st_mode))
    return true;
  if (statfs(session->root_destination, &fs) != 0)
    return errno == ENOENT;
  if (strcmp(fs.f_fstypename, "nsfs") != 0 ||
      strcmp(fs.f_mntonname, session->root_destination) != 0) {
    return true;
  }

  pkg_backport_namespace_id_t current = {.device = st.st_dev};
  memcpy(&current.fsid, &fs.f_fsid, sizeof(current.fsid));
  return !namespace_id_equal(&session->namespace_id, &current);
}

static bool cleanup_session_locked(pkg_backport_session_t *session,
                                   const char *reason) {
  if (!session->installed)
    return true;

  size_t redirect_count = session->redirect_count;
  if (session_namespace_released(session)) {
    log_debug("  [BKP][NSFS] redirects released with namespace: title=%s "
              "count=%zu reason=%s",
              session->title_id, redirect_count, reason);
    reset_session_locked(session);
    return true;
  }

  int fd = open(NSFS_CONTROL_DEVICE, O_RDWR);
  if (fd < 0) {
    int open_errno = errno;
    log_debug("  [BKP][NSFS] redirect cleanup unavailable: title=%s "
              "reason=%s error=%d (%s)",
              session->title_id, reason, open_errno, strerror(open_errno));
    errno = open_errno;
    return false;
  }

  bool cleaned = true;
  int first_errno = 0;
  for (size_t i = session->redirect_count; i > 0; --i) {
    pkg_backport_redirect_t *redirect = &session->redirects[i - 1u];
    const char *destination = redirect_destination(session, redirect);
    if (!destination)
      continue;
    int delete_errno = 0;
    if (!delete_redirect(fd, destination, &delete_errno)) {
      if (first_errno == 0)
        first_errno = delete_errno;
      cleaned = false;
      log_debug("  [BKP][NSFS] redirect cleanup failed: title=%s dst=%s "
                "reason=%s error=%d (%s)",
                session->title_id, destination, reason, delete_errno,
                strerror(delete_errno));
      continue;
    }
    redirect->destination_offset = UINT32_MAX;
  }
  (void)close(fd);
  if (!cleaned) {
    errno = first_errno;
    return false;
  }

  log_debug("  [BKP][NSFS] redirects removed: title=%s count=%zu reason=%s",
            session->title_id, redirect_count, reason);
  reset_session_locked(session);
  return true;
}

bool sm_pkg_backport_on_sandbox_ready(const char *title_id) {
  if (!title_id || !is_supported_game_title_id(title_id))
    return true;

  char source[MAX_PATH];
  if (!resolve_backport_path_for_title(title_id, NULL, source))
    return true;

  char app0_path[MAX_PATH];
  pkg_backport_namespace_id_t namespace_id;
  if (!resolve_pkg_app0_path(title_id, app0_path, &namespace_id)) {
    log_debug("  [BKP][NSFS] package app0 not ready or not NSFS: %s",
              title_id);
    return true;
  }

  pthread_mutex_lock(&g_pkg_backport_mutex);
  bool already_ready = false;
  pkg_backport_session_t *session = prepare_session_locked(
      title_id, source, app0_path, &namespace_id, &already_ready);
  if (!session) {
    pthread_mutex_unlock(&g_pkg_backport_mutex);
    return false;
  }
  if (already_ready) {
    pthread_mutex_unlock(&g_pkg_backport_mutex);
    return true;
  }

  int fd = open(NSFS_CONTROL_DEVICE, O_RDWR);
  bool redirects_created =
      fd >= 0 && create_child_redirects_locked(fd, session, source, app0_path,
                                               true, 0u);
  int redirect_errno = redirects_created ? 0 : errno;

  if (redirect_errno != 0) {
    if (fd < 0 || rollback_redirects_locked(fd, session)) {
      reset_session_locked(session);
    } else {
      session->installed = true;
    }
    if (fd >= 0)
      (void)close(fd);
    pthread_mutex_unlock(&g_pkg_backport_mutex);
    log_debug("  [BKP][NSFS] file redirect failed: title=%s src=%s dst=%s "
              "error=%d (%s)",
              title_id, source, app0_path, redirect_errno,
              strerror(redirect_errno));
    notify_system_l10n(SM_L10N_BACKPORT_OVERLAY_FAILED, title_id,
                       source, (uint32_t)redirect_errno);
    errno = redirect_errno;
    return false;
  }
  (void)close(fd);

  if (session->redirect_count == 0) {
    reset_session_locked(session);
    pthread_mutex_unlock(&g_pkg_backport_mutex);
    return true;
  }

  size_t redirect_count = session->redirect_count;
  session->installed = true;
  session->ready = true;
  pthread_mutex_unlock(&g_pkg_backport_mutex);

  log_debug("  [BKP][NSFS] package file fallback active: title=%s src=%s "
            "dst=%s redirects=%zu",
            title_id, source, app0_path, redirect_count);
  return true;
}

void sm_pkg_backport_on_launch_failed(const char *title_id) {
  if (!title_id)
    return;
  pthread_mutex_lock(&g_pkg_backport_mutex);
  pkg_backport_session_t *session = find_session_by_title_locked(title_id);
  if (session && session->pid <= 0)
    (void)cleanup_session_locked(session, "launch failed");
  pthread_mutex_unlock(&g_pkg_backport_mutex);
}

void sm_pkg_backport_on_exec(pid_t pid, const char *title_id) {
  if (pid <= 0 || !title_id || !is_supported_game_title_id(title_id))
    return;

  pthread_mutex_lock(&g_pkg_backport_mutex);
  pkg_backport_session_t *session = find_session_by_title_locked(title_id);
  if (session)
    session->pid = pid;
  pthread_mutex_unlock(&g_pkg_backport_mutex);
}

void sm_pkg_backport_on_exit(pid_t pid) {
  if (pid <= 0)
    return;
  pthread_mutex_lock(&g_pkg_backport_mutex);
  for (size_t i = 0; i < PKG_BACKPORT_MAX_SESSIONS; ++i) {
    pkg_backport_session_t *session = &g_pkg_backport_sessions[i];
    if (session->installed && session->pid == pid)
      (void)cleanup_session_locked(session, "game exit");
  }
  pthread_mutex_unlock(&g_pkg_backport_mutex);
}

void sm_pkg_backport_shutdown(void) {
  pthread_mutex_lock(&g_pkg_backport_mutex);
  for (size_t i = 0; i < PKG_BACKPORT_MAX_SESSIONS; ++i)
    (void)cleanup_session_locked(&g_pkg_backport_sessions[i], "shutdown");
  pthread_mutex_unlock(&g_pkg_backport_mutex);
}
