#ifndef SM_PKG_H
#define SM_PKG_H

#include <stdbool.h>
#include <sys/stat.h>

#include "sm_types.h"

bool sm_pkg_path_is_candidate(const char *path, const char *name);
bool sm_pkg_is_installed_content_path(const char *path);
bool sm_pkg_parse_file(const char *path, const struct stat *st,
                       scan_candidate_t *candidate);
const char *sm_pkg_kind_name(pkg_kind_t kind);
bool sm_pkg_kind_is_installable(pkg_kind_t kind);
int sm_pkg_compare_versions(const char *left, const char *right);

#endif
