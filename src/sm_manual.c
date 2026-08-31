#include "sm_platform.h"

#include <pthread.h>

#include "sm_manual.h"

#include "sm_appdb.h"
#include "sm_install_queue.h"
#include "sm_limits.h"
#include "sm_log.h"
#include "sm_paths.h"
#include "sm_title_state.h"

#define MANUAL_STATUS_CAPACITY 1024

typedef enum {
  MANUAL_STATUS_UNKNOWN = 0,
  MANUAL_STATUS_INSTALLED,
  MANUAL_STATUS_DELETED,
} manual_status_t;

typedef struct {
  char path[MAX_PATH];
  char title_id[MAX_TITLE_ID];
  char title_name[MAX_TITLE_NAME];
  manual_status_t status;
  bool remove_from_list;
} manual_status_entry_t;

static manual_status_entry_t g_manual_status_entries[MANUAL_STATUS_CAPACITY];
static pthread_mutex_t g_manual_list_mutex = PTHREAD_MUTEX_INITIALIZER;

static void trim_manual_line(char *line) {
  if (!line)
    return;

  char *start = line;
  while (*start != '\0' && isspace((unsigned char)*start))
    start++;
  if (start != line)
    memmove(line, start, strlen(start) + 1u);

  size_t len = strlen(line);
  while (len > 0 && isspace((unsigned char)line[len - 1u])) {
    line[len - 1u] = '\0';
    len--;
  }
  while (len > 1 && line[len - 1u] == '/') {
    line[len - 1u] = '\0';
    len--;
  }
}

static void drain_manual_file_line(FILE *f) {
  int ch;
  while ((ch = fgetc(f)) != EOF && ch != '\n') {
  }
}

static void copy_manual_file_line_remainder(FILE *in, FILE *out,
                                            int *saved_errno) {
  int ch;
  while ((ch = fgetc(in)) != EOF) {
    if (saved_errno && *saved_errno == 0 && fputc(ch, out) == EOF)
      *saved_errno = errno;
    if (ch == '\n')
      break;
  }
}

bool sm_manual_normalize_path(const char *path, char out[MAX_PATH]) {
  if (!path || !out || strlcpy(out, path, MAX_PATH) >= MAX_PATH)
    return false;
  if (strchr(out, '\r') || strchr(out, '\n'))
    return false;
  trim_manual_line(out);
  return out[0] == '/' && out[1] != '\0';
}

static bool update_manual_list(const char *path, bool add,
                               bool *changed_out) {
  if (changed_out)
    *changed_out = false;

  char normalized_path[MAX_PATH];
  if (!sm_manual_normalize_path(path, normalized_path)) {
    errno = EINVAL;
    return false;
  }

  pthread_mutex_lock(&g_manual_list_mutex);
  mkdir(LOG_DIR, 0777);

  FILE *in = fopen(MANUAL_LIST_FILE, "r");
  if (!in && errno != ENOENT) {
    int saved_errno = errno;
    pthread_mutex_unlock(&g_manual_list_mutex);
    errno = saved_errno;
    return false;
  }

  bool needs_separator = false;
  if (in && fseek(in, 0, SEEK_END) == 0) {
    long size = ftell(in);
    if (size > 0 && fseek(in, -1, SEEK_END) == 0)
      needs_separator = fgetc(in) != '\n';
    rewind(in);
  }

  char tmp_path[MAX_PATH];
  int tmp_len = snprintf(tmp_path, sizeof(tmp_path), "%s.tmp",
                         MANUAL_LIST_FILE);
  if (tmp_len <= 0 || (size_t)tmp_len >= sizeof(tmp_path)) {
    if (in)
      fclose(in);
    pthread_mutex_unlock(&g_manual_list_mutex);
    errno = ENAMETOOLONG;
    return false;
  }

  FILE *out = fopen(tmp_path, "w");
  if (!out) {
    int saved_errno = errno;
    if (in)
      fclose(in);
    pthread_mutex_unlock(&g_manual_list_mutex);
    errno = saved_errno;
    return false;
  }

  bool found = false;
  bool changed = false;
  int saved_errno = 0;
  char line[MAX_PATH + 64];
  while (in && fgets(line, sizeof(line), in) != NULL) {
    bool truncated =
        strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1u;
    char normalized[MAX_PATH + 64];
    (void)strlcpy(normalized, line, sizeof(normalized));
    trim_manual_line(normalized);
    bool matches = !truncated && normalized[0] != '\0' &&
                   normalized[0] != '#' &&
                   strcmp(normalized, normalized_path) == 0;
    if (matches) {
      found = true;
      if (!add) {
        changed = true;
        continue;
      }
    }

    if (fputs(line, out) == EOF) {
      saved_errno = errno;
      break;
    }
    if (truncated)
      copy_manual_file_line_remainder(in, out, &saved_errno);
  }
  if (in && ferror(in) && saved_errno == 0)
    saved_errno = errno;

  if (saved_errno == 0 && add && !found) {
    if (needs_separator && fputc('\n', out) == EOF)
      saved_errno = errno;
    if (saved_errno == 0 &&
        fprintf(out, "%s\n", normalized_path) < 0) {
      saved_errno = errno;
    }
    if (saved_errno == 0)
      changed = true;
  }

  if (fflush(out) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(out) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (in)
    fclose(in);

  if (saved_errno != 0) {
    (void)unlink(tmp_path);
    pthread_mutex_unlock(&g_manual_list_mutex);
    errno = saved_errno;
    return false;
  }
  if (!changed) {
    (void)unlink(tmp_path);
    pthread_mutex_unlock(&g_manual_list_mutex);
    return true;
  }
  if (rename(tmp_path, MANUAL_LIST_FILE) != 0) {
    int rename_errno = errno;
    (void)unlink(tmp_path);
    pthread_mutex_unlock(&g_manual_list_mutex);
    errno = rename_errno;
    return false;
  }

  pthread_mutex_unlock(&g_manual_list_mutex);
  if (changed_out)
    *changed_out = true;
  log_debug("  [MANUAL] API %s source: %s", add ? "added" : "removed",
            normalized_path);
  return true;
}

bool sm_manual_add_path(const char *path, bool *changed_out) {
  return update_manual_list(path, true, changed_out);
}

bool sm_manual_remove_path(const char *path, bool *changed_out) {
  return update_manual_list(path, false, changed_out);
}

bool sm_manual_for_each_path(bool (*visit)(const char *path, void *ctx),
                             void *ctx) {
  if (!visit)
    return false;

  FILE *f = fopen(MANUAL_LIST_FILE, "r");
  if (!f) {
    if (errno != ENOENT)
      log_debug("  [MANUAL] open failed for %s: %s", MANUAL_LIST_FILE,
                strerror(errno));
    return false;
  }

  char line[MAX_PATH + 64];
  bool completed = true;
  while (fgets(line, sizeof(line), f) != NULL) {
    bool truncated =
        strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1u;
    trim_manual_line(line);
    if (line[0] == '\0' || line[0] == '#') {
      if (truncated)
        drain_manual_file_line(f);
      continue;
    }
    if (truncated || strlen(line) >= MAX_PATH) {
      log_debug("  [MANUAL] path too long, ignored: %.80s", line);
      if (truncated)
        drain_manual_file_line(f);
      continue;
    }
    if (!visit(line, ctx)) {
      completed = false;
      break;
    }
  }

  if (ferror(f)) {
    log_debug("  [MANUAL] read failed for %s: %s", MANUAL_LIST_FILE,
              strerror(errno));
    completed = false;
  }
  fclose(f);
  return completed;
}

static const char *manual_status_name(manual_status_t status) {
  switch (status) {
  case MANUAL_STATUS_INSTALLED:
    return "installed";
  case MANUAL_STATUS_DELETED:
    return "deleted";
  default:
    return "unknown";
  }
}

static manual_status_t parse_manual_status_name(const char *name) {
  if (!name)
    return MANUAL_STATUS_UNKNOWN;
  if (strcasecmp(name, "installed") == 0)
    return MANUAL_STATUS_INSTALLED;
  if (strcasecmp(name, "deleted") == 0)
    return MANUAL_STATUS_DELETED;
  return MANUAL_STATUS_UNKNOWN;
}

static bool parse_manual_status_line(char *line, manual_status_entry_t *entry) {
  char *status = strtok(line, "\t");
  char *title_id = strtok(NULL, "\t");
  char *path = strtok(NULL, "\t");
  char *title_name = strtok(NULL, "\n");
  if (!status || !title_id || !path)
    return false;

  trim_manual_line(status);
  trim_manual_line(title_id);
  trim_manual_line(path);
  if (title_name)
    trim_manual_line(title_name);

  manual_status_t parsed_status = parse_manual_status_name(status);
  if (parsed_status == MANUAL_STATUS_UNKNOWN || title_id[0] == '\0' ||
      path[0] == '\0') {
    return false;
  }

  memset(entry, 0, sizeof(*entry));
  entry->status = parsed_status;
  (void)strlcpy(entry->title_id, title_id, sizeof(entry->title_id));
  (void)strlcpy(entry->path, path, sizeof(entry->path));
  if (title_name && title_name[0] != '\0')
    (void)strlcpy(entry->title_name, title_name, sizeof(entry->title_name));
  return true;
}

static int load_manual_status(void) {
  FILE *f = fopen(MANUAL_STATUS_FILE, "r");
  if (!f) {
    if (errno != ENOENT)
      log_debug("  [MANUAL] status open failed for %s: %s",
                MANUAL_STATUS_FILE, strerror(errno));
    return 0;
  }

  int count = 0;
  char line[MAX_PATH + MAX_TITLE_NAME + MAX_TITLE_ID + 64];
  while (fgets(line, sizeof(line), f) != NULL) {
    if (count >= MANUAL_STATUS_CAPACITY) {
      log_debug("  [MANUAL] status limit reached (%d)",
                MANUAL_STATUS_CAPACITY);
      break;
    }
    manual_status_entry_t entry;
    if (!parse_manual_status_line(line, &entry))
      continue;
    g_manual_status_entries[count++] = entry;
  }

  if (ferror(f))
    log_debug("  [MANUAL] status read failed for %s: %s",
              MANUAL_STATUS_FILE, strerror(errno));
  fclose(f);
  return count;
}

static bool write_manual_status(int entry_count) {
  mkdir(LOG_DIR, 0777);

  char tmp_path[MAX_PATH];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MANUAL_STATUS_FILE);
  FILE *f = fopen(tmp_path, "w");
  if (!f) {
    log_debug("  [MANUAL] status write open failed for %s: %s", tmp_path,
              strerror(errno));
    return false;
  }

  int saved_errno = 0;
  for (int i = 0; i < entry_count; i++) {
    const manual_status_entry_t *entry = &g_manual_status_entries[i];
    if (entry->status == MANUAL_STATUS_UNKNOWN || entry->path[0] == '\0' ||
        entry->title_id[0] == '\0') {
      continue;
    }
    if (fprintf(f, "%s\t%s\t%s\t%s\n", manual_status_name(entry->status),
                entry->title_id, entry->path, entry->title_name) < 0) {
      saved_errno = errno;
      break;
    }
  }

  if (fflush(f) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(f) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (saved_errno != 0) {
    errno = saved_errno;
    log_debug("  [MANUAL] status write failed for %s: %s", tmp_path,
              strerror(errno));
    (void)unlink(tmp_path);
    return false;
  }

  if (rename(tmp_path, MANUAL_STATUS_FILE) != 0) {
    log_debug("  [MANUAL] status rename failed for %s: %s",
              MANUAL_STATUS_FILE, strerror(errno));
    (void)unlink(tmp_path);
    return false;
  }
  return true;
}

static int find_manual_status_entry(int entry_count, const char *path,
                                    const char *title_id) {
  for (int i = 0; i < entry_count; i++) {
    if (path && path[0] != '\0' &&
        strcmp(g_manual_status_entries[i].path, path) == 0) {
      return i;
    }
    if (title_id && title_id[0] != '\0' &&
        strcmp(g_manual_status_entries[i].title_id, title_id) == 0) {
      return i;
    }
  }
  return -1;
}

static bool rewrite_manual_list_without_deleted_entries(int entry_count) {
  FILE *in = fopen(MANUAL_LIST_FILE, "r");
  if (!in) {
    if (errno == ENOENT)
      return true;
    log_debug("  [MANUAL] list rewrite open failed for %s: %s",
              MANUAL_LIST_FILE, strerror(errno));
    return false;
  }

  char tmp_path[MAX_PATH];
  snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", MANUAL_LIST_FILE);
  FILE *out = fopen(tmp_path, "w");
  if (!out) {
    log_debug("  [MANUAL] list rewrite temp open failed for %s: %s", tmp_path,
              strerror(errno));
    fclose(in);
    return false;
  }

  bool changed = false;
  int saved_errno = 0;
  char line[MAX_PATH + 64];
  while (fgets(line, sizeof(line), in) != NULL) {
    bool truncated =
        strchr(line, '\n') == NULL && strlen(line) == sizeof(line) - 1u;
    char normalized[MAX_PATH + 64];
    (void)strlcpy(normalized, line, sizeof(normalized));
    trim_manual_line(normalized);
    if (!truncated && normalized[0] != '\0' && normalized[0] != '#') {
      bool remove_line = false;
      for (int i = 0; i < entry_count; i++) {
        if (g_manual_status_entries[i].remove_from_list &&
            strcmp(g_manual_status_entries[i].path, normalized) == 0) {
          remove_line = true;
          break;
        }
      }
      if (remove_line) {
        changed = true;
        continue;
      }
    }
    if (fputs(line, out) == EOF) {
      saved_errno = errno;
      break;
    }
    if (truncated)
      copy_manual_file_line_remainder(in, out, &saved_errno);
  }
  if (ferror(in) && saved_errno == 0)
    saved_errno = errno;
  if (fflush(out) != 0 && saved_errno == 0)
    saved_errno = errno;
  if (fclose(out) != 0 && saved_errno == 0)
    saved_errno = errno;
  fclose(in);

  if (saved_errno != 0) {
    errno = saved_errno;
    log_debug("  [MANUAL] list rewrite failed for %s: %s", MANUAL_LIST_FILE,
              strerror(errno));
    (void)unlink(tmp_path);
    return false;
  }

  if (!changed) {
    (void)unlink(tmp_path);
    return true;
  }

  if (rename(tmp_path, MANUAL_LIST_FILE) != 0) {
    log_debug("  [MANUAL] list rewrite rename failed for %s: %s",
              MANUAL_LIST_FILE, strerror(errno));
    (void)unlink(tmp_path);
    return false;
  }
  return true;
}

bool sm_manual_reconcile_deleted_titles(
    const struct AppDbTitleList *app_db_titles, bool app_db_titles_ready) {
  if (!app_db_titles_ready || !app_db_titles)
    return true;

  int entry_count = load_manual_status();
  if (entry_count <= 0)
    return true;

  bool changed = false;

  for (int i = 0; i < entry_count; i++) {
    manual_status_entry_t *entry = &g_manual_status_entries[i];
    if (entry->status != MANUAL_STATUS_INSTALLED ||
        entry->title_id[0] == '\0') {
      continue;
    }
    if (app_db_title_list_contains(app_db_titles, entry->title_id))
      continue;

    entry->status = MANUAL_STATUS_DELETED;
    entry->remove_from_list = true;
    sm_install_forget_pending_title(entry->title_id);
    clear_register_attempts(entry->title_id);
    clear_failed_mount_attempts(entry->title_id);
    changed = true;
    log_debug("  [MANUAL] title removed from app.db, disabling manual entry: "
              "%s (%s)",
              entry->title_name, entry->title_id);
  }

  if (!changed)
    return true;

  pthread_mutex_lock(&g_manual_list_mutex);
  bool list_rewritten =
      rewrite_manual_list_without_deleted_entries(entry_count);
  pthread_mutex_unlock(&g_manual_list_mutex);
  if (!list_rewritten)
    return false;
  return write_manual_status(entry_count);
}

void sm_manual_note_installed(const char *manual_source_path,
                              const char *title_id,
                              const char *title_name) {
  if (!manual_source_path || manual_source_path[0] == '\0' || !title_id ||
      title_id[0] == '\0') {
    return;
  }

  int entry_count = load_manual_status();
  int index =
      find_manual_status_entry(entry_count, manual_source_path, title_id);
  if (index < 0) {
    if (entry_count >= MANUAL_STATUS_CAPACITY) {
      log_debug("  [MANUAL] status full, cannot record %s (%s)",
                manual_source_path, title_id);
      return;
    }
    index = entry_count++;
    memset(&g_manual_status_entries[index], 0,
           sizeof(g_manual_status_entries[index]));
  }

  manual_status_entry_t *entry = &g_manual_status_entries[index];
  manual_status_t previous_status = entry->status;
  bool unchanged = previous_status == MANUAL_STATUS_INSTALLED &&
                   strcmp(entry->path, manual_source_path) == 0 &&
                   strcmp(entry->title_id, title_id) == 0 &&
                   (!title_name || title_name[0] == '\0' ||
                    strcmp(entry->title_name, title_name) == 0);
  if (unchanged)
    return;

  entry->status = MANUAL_STATUS_INSTALLED;
  (void)strlcpy(entry->path, manual_source_path, sizeof(entry->path));
  (void)strlcpy(entry->title_id, title_id, sizeof(entry->title_id));
  if (title_name && title_name[0] != '\0')
    (void)strlcpy(entry->title_name, title_name, sizeof(entry->title_name));

  if (!write_manual_status(entry_count))
    return;
  if (previous_status != MANUAL_STATUS_INSTALLED) {
    log_debug("  [MANUAL] installed status recorded: %s (%s) from %s",
              entry->title_name, entry->title_id, entry->path);
  }
}
