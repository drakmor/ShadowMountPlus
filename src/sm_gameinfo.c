#include "sm_platform.h"
#include "sm_gameinfo.h"
#include "sm_limits.h"
#include "sm_path_state.h"

// --- Game Metadata Parsing (param.json) ---
static int extract_json_string(const char *json, const char *key, char *out,
                               size_t out_size) {
  if (!json || !key || !out || out_size == 0)
    return -1;

  char search[64];
  int written = snprintf(search, sizeof(search), "\"%s\"", key);
  if (written < 0 || (size_t)written >= sizeof(search))
    return -1;
  const char *p = strstr(json, search);
  if (!p)
    return -1;
  p = strchr(p + strlen(search), ':');
  if (!p)
    return -2;
  while (*++p && isspace(*p)) {
    /* skip */
  }
  if (*p != '"')
    return -3;
  p++;

  size_t i = 0;
  while (i < out_size - 1 && p[i] && p[i] != '"') {
    out[i] = p[i];
    i++;
  }
  out[i] = '\0';
  if (p[i] != '"') {
    out[0] = '\0';
    return -4;
  }
  return 0;
}

bool is_supported_game_title_id(const char *title_id) {
  if (!title_id || strlen(title_id) != 9u)
    return false;
  if (strncmp(title_id, "PPSA", 4u) != 0 &&
      strncmp(title_id, "CUSA", 4u) != 0) {
    return false;
  }
  for (size_t i = 4u; i < 9u; ++i) {
    if (!isdigit((unsigned char)title_id[i]))
      return false;
  }
  return true;
}

bool get_game_info(const char *base_path, const struct stat *param_st,
                   char *out_id, char *out_name) {
  out_id[0] = '\0';
  out_name[0] = '\0';
  if (!S_ISREG(param_st->st_mode))
    return false;

  bool cached_valid = false;
  if (load_cached_game_info(base_path, param_st, out_id, out_name, &cached_valid))
    return cached_valid;

  if (param_st->st_size <= 0 || param_st->st_size > MAX_PARAM_JSON_SIZE) {
    store_cached_game_info(base_path, param_st, false, "", "");
    return false;
  }

  char path[MAX_PATH];
  int written = snprintf(path, sizeof(path), "%s/sce_sys/param.json",
                         base_path);
  if (written < 0 || (size_t)written >= sizeof(path)) {
    store_cached_game_info(base_path, param_st, false, "", "");
    return false;
  }
  FILE *f = fopen(path, "rb");
  if (!f) {
    store_cached_game_info(base_path, param_st, false, "", "");
    return false;
  }

  size_t len = (size_t)param_st->st_size;
  char *buf = (char *)malloc(len + 1);
  if (!buf) {
    fclose(f);
    return false;
  }
  bool read_ok = (fread(buf, 1, len, f) == len);
  fclose(f);
  if (!read_ok) {
    free(buf);
    return false;
  }
  buf[len] = '\0';

  bool valid = false;
  int res = extract_json_string(buf, "titleId", out_id, MAX_TITLE_ID);
  if (res != 0)
    res = extract_json_string(buf, "title_id", out_id, MAX_TITLE_ID);
  if (res == 0 && is_supported_game_title_id(out_id)) {
    const char *en_ptr = strstr(buf, "\"en-US\"");
    const char *search_start = en_ptr ? en_ptr : buf;
    if (extract_json_string(search_start, "titleName", out_name,
                            MAX_TITLE_NAME) != 0)
      extract_json_string(buf, "titleName", out_name, MAX_TITLE_NAME);
    if (out_name[0] == '\0')
      (void)strlcpy(out_name, out_id, MAX_TITLE_NAME);
    valid = true;
  } else {
    out_id[0] = '\0';
    out_name[0] = '\0';
  }
  free(buf);

  store_cached_game_info(base_path, param_st, valid, out_id, out_name);
  return valid;
}

bool directory_has_param_json(const char *dir_path, struct stat *param_st_out) {
  int dir_fd = open(dir_path, O_RDONLY | O_DIRECTORY);
  if (dir_fd < 0)
    return false;

  struct stat st;
  if (fstatat(dir_fd, "sce_sys", &st, 0) == 0 && S_ISDIR(st.st_mode) &&
      fstatat(dir_fd, "sce_sys/param.json", &st, 0) == 0 &&
      S_ISREG(st.st_mode)) {
    if (param_st_out)
      *param_st_out = st;
    close(dir_fd);
    return true;
  }

  close(dir_fd);
  return false;
}
