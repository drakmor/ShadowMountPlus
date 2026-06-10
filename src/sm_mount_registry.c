#include "sm_platform.h"
#include "sm_mount_registry.h"
#include "sm_log.h"

#include <pthread.h>
#include <stdlib.h>

typedef struct sm_mount_record {
  char mount_point[MAX_PATH];
  char source_path[MAX_PATH];
  sm_mount_kind_t kind;
  struct sm_mount_record *prev;
  struct sm_mount_record *next;
} sm_mount_record_t;

static sm_mount_record_t *g_head = NULL;
static sm_mount_record_t *g_tail = NULL;
static pthread_mutex_t g_registry_mutex = PTHREAD_MUTEX_INITIALIZER;

static const char *mount_kind_name(sm_mount_kind_t kind) {
  switch (kind) {
  case SM_MOUNT_KIND_NULLFS_TITLE:    return "nullfs/title";
  case SM_MOUNT_KIND_NULLFS_BACKPORT: return "nullfs/backport";
  case SM_MOUNT_KIND_NULLFS_FAKELIB:  return "nullfs/fakelib";
  case SM_MOUNT_KIND_IMAGE:           return "image";
  default:                            return "unknown";
  }
}

void sm_mount_registry_push(const char *mount_point, const char *source_path,
                            sm_mount_kind_t kind) {
  if (!mount_point || mount_point[0] == '\0')
    return;

  sm_mount_record_t *rec = calloc(1, sizeof(*rec));
  if (!rec) {
    log_debug("  [REGISTRY] alloc failed for %s", mount_point);
    return;
  }

  (void)strlcpy(rec->mount_point, mount_point, sizeof(rec->mount_point));
  if (source_path && source_path[0] != '\0')
    (void)strlcpy(rec->source_path, source_path, sizeof(rec->source_path));
  rec->kind = kind;

  pthread_mutex_lock(&g_registry_mutex);
  rec->prev = g_tail;
  rec->next = NULL;
  if (g_tail)
    g_tail->next = rec;
  else
    g_head = rec;
  g_tail = rec;
  pthread_mutex_unlock(&g_registry_mutex);

  log_debug("  [REGISTRY] push [%s] %s <- %s",
            mount_kind_name(kind), mount_point,
            (source_path && source_path[0]) ? source_path : "(none)");
}

void sm_mount_registry_remove(const char *mount_point) {
  if (!mount_point || mount_point[0] == '\0')
    return;

  pthread_mutex_lock(&g_registry_mutex);
  sm_mount_record_t *rec = g_tail;
  while (rec) {
    if (strcmp(rec->mount_point, mount_point) == 0) {
      if (rec->prev)
        rec->prev->next = rec->next;
      else
        g_head = rec->next;
      if (rec->next)
        rec->next->prev = rec->prev;
      else
        g_tail = rec->prev;
      log_debug("  [REGISTRY] remove [%s] %s",
                mount_kind_name(rec->kind), rec->mount_point);
      free(rec);
      break;
    }
    rec = rec->prev;
  }
  pthread_mutex_unlock(&g_registry_mutex);
}

void sm_mount_registry_shutdown(void) {
  log_debug("  [REGISTRY] shutdown begin");
  pthread_mutex_lock(&g_registry_mutex);

  sm_mount_record_t *rec = g_tail;
  while (rec) {
    sm_mount_record_t *prev = rec->prev;

    log_debug("  [REGISTRY] unmounting [%s] %s",
              mount_kind_name(rec->kind), rec->mount_point);

    if (unmount(rec->mount_point, 0) != 0) {
      int err = errno;
      if (err != ENOENT && err != EINVAL) {
        log_debug("  [REGISTRY] force unmount %s: %s",
                  rec->mount_point, strerror(err));
        if (unmount(rec->mount_point, MNT_FORCE) != 0 &&
            errno != ENOENT && errno != EINVAL) {
          log_debug("  [REGISTRY] force unmount failed %s: %s",
                    rec->mount_point, strerror(errno));
        }
      }
    }

    free(rec);
    rec = prev;
  }

  g_head = NULL;
  g_tail = NULL;
  pthread_mutex_unlock(&g_registry_mutex);
  log_debug("  [REGISTRY] shutdown done");
}
