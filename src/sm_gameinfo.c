#include "sm_platform.h"
#include <json-c/json.h>
#include "sm_gameinfo.h"
#include "sm_limits.h"
#include "sm_path_state.h"

// --- Game Metadata Parsing (param.json) ---
static bool extract_json_string(struct json_object *object, const char *key,
                                 char *out, size_t out_size) {
  struct json_object *value = NULL;
  if (!json_object_is_type(object, json_type_object) ||
      !json_object_object_get_ex(object, key, &value) ||
      !json_object_is_type(value, json_type_string))
    return false;
  size_t length = (size_t)json_object_get_string_len(value);
  const char *text = json_object_get_string(value);
  if (length == 0 || length >= out_size || memchr(text, '\0', length))
    return false;
  memcpy(out, text, length + 1u);
  return true;
}

static void extract_title_name(struct json_object *root, char *out) {
  struct json_object *localized = NULL;
  struct json_object *language = NULL;
  if (json_object_object_get_ex(root, "localizedParameters", &localized) &&
      json_object_is_type(localized, json_type_object)) {
    if (json_object_object_get_ex(localized, "en-US", &language) &&
        extract_json_string(language, "titleName", out, MAX_TITLE_NAME))
      return;
    char default_language[32];
    if (extract_json_string(localized, "defaultLanguage", default_language,
                             sizeof(default_language)) &&
        json_object_object_get_ex(localized, default_language, &language) &&
        extract_json_string(language, "titleName", out, MAX_TITLE_NAME))
      return;
  }
  if (extract_json_string(root, "titleName", out, MAX_TITLE_NAME))
    return;
  if (json_object_is_type(localized, json_type_object)) {
    json_object_object_foreach(localized, key, value) {
      (void)key;
      if (extract_json_string(value, "titleName", out, MAX_TITLE_NAME))
        return;
    }
  }
}

bool is_supported_game_title_id(const char *title_id) {
  if (!title_id || strlen(title_id) != 9u)
    return false;
  if (strncmp(title_id, "PPSA", 4u) != 0 &&
      strncmp(title_id, "CUSA", 4u) != 0 &&
      strncmp(title_id, "FAKE", 4u) != 0) {
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
  if (!f)
    return false;

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

  struct json_tokener *tokener = json_tokener_new_ex(32);
  if (!tokener) {
    free(buf);
    return false;
  }
  json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
  struct json_object *root = json_tokener_parse_ex(tokener, buf, (int)len);
  size_t parsed = json_tokener_get_parse_end(tokener);
  while (parsed < len && isspace((unsigned char)buf[parsed]))
    parsed++;
  bool valid = json_tokener_get_error(tokener) == json_tokener_success &&
               parsed == len && json_object_is_type(root, json_type_object);
  json_tokener_free(tokener);
  if (valid) {
    valid = extract_json_string(root, "titleId", out_id, MAX_TITLE_ID) ||
            extract_json_string(root, "title_id", out_id, MAX_TITLE_ID);
    valid = valid && is_supported_game_title_id(out_id);
  }
  if (valid) {
    extract_title_name(root, out_name);
    if (out_name[0] == '\0')
      (void)strlcpy(out_name, out_id, MAX_TITLE_NAME);
  } else {
    out_id[0] = '\0';
    out_name[0] = '\0';
  }
  json_object_put(root);
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
