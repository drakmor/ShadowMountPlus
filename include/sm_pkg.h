#ifndef SM_PKG_H
#define SM_PKG_H
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <sys/stat.h>
#include "sm_limits.h"
#define SM_PKG_QUEUE_MAX 32u
#define SM_PKG_ICON_MAX (4u * 1024u * 1024u)
typedef enum { PKG_KIND_UNKNOWN, PKG_KIND_BASE, PKG_KIND_PATCH, PKG_KIND_DLC } pkg_kind_t;
typedef struct {
  char path[MAX_PATH], title_id[MAX_TITLE_ID], title_name[MAX_TITLE_NAME];
  char content_id[MAX_CONTENT_ID], category[16], app_ver[MAX_GAME_VERSION];
  uint64_t source_size, source_device, source_inode, source_ctime;
  uint64_t icon_offset;
  uint32_t icon_size;
  uint32_t sdk_version; // Packed param.json/param.sfo SDK version; zero when unavailable.
  time_t source_mtime;
  uint32_t source_mtime_ns, source_ctime_ns;
  uint32_t outer_content_type, outer_content_flags, content_type, content_flags;
  pkg_kind_t pkg_kind;
  bool pkg_classification_confident;
} sm_pkg_info_t;
typedef int (*sm_pkg_read_fn)(void *ctx, uint64_t offset, void *buffer, size_t size);
bool sm_pkg_parse_stream(const char *name, uint64_t size, sm_pkg_read_fn read,
                          void *ctx, sm_pkg_info_t *candidate);
bool sm_pkg_is_installed_content_path(const char *path);
bool sm_pkg_parse_file(const char *path, const struct stat *st, sm_pkg_info_t *candidate);
// Parse metadata from a caller-owned read-only regular-file descriptor. The
// descriptor remains open, allowing the caller to retain the same source
// snapshot for subsequent FPKG validation and reads.
bool sm_pkg_parse_fd(const char *path, int fd, sm_pkg_info_t *candidate);
// Match a local source snapshot, including replacement and in-place changes.
bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st);
int sm_pkg_read_icon(const char *path, unsigned char **data, size_t *size);
// Read version/SDK from a game directory's sce_sys/param.json or param.sfo.
bool sm_pkg_read_directory_metadata(const char *path, sm_pkg_info_t *info);
int sm_pkg_read_directory_icon(const char *path, unsigned char **data, size_t *size);
// Read the process-parameter SDK from a plain ELF or uncompressed SELF eboot.
// Returns zero for missing, malformed, encrypted or compressed executables.
uint32_t sm_pkg_read_executable_sdk(const char *path);
const char *sm_pkg_kind_name(pkg_kind_t kind);
bool sm_pkg_kind_is_installable(pkg_kind_t kind);
// A full media-app remaster can install a base or update an existing base.
bool sm_pkg_is_media_remaster(const sm_pkg_info_t *info);
#endif
