#ifndef SM_GAMEINFO_H
#define SM_GAMEINFO_H

#include <stdbool.h>
#include <sys/stat.h>

// Read title ID and title name from a mounted game directory.
bool get_game_info(const char *base_path, const struct stat *param_st,
                   char *out_id, char *out_name);
// Accept canonical PS4/PS5 game title IDs used in filesystem paths.
bool is_supported_game_title_id(const char *title_id);
// Check whether a directory contains sce_sys/param.json and stat it.
bool directory_has_param_json(const char *dir_path, struct stat *param_st_out);

#endif
