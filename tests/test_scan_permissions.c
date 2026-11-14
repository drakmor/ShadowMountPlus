#include "host_platform.h"
#include <strings.h>
#include "sm_scan_tree.h"
#include "sm_config_mount.h"
#include "sm_filesystem.h"
#include "sm_game_lifecycle.h"
#include "sm_image.h"
#include "sm_path_utils.h"
#include "sm_paths.h"
#include "sm_runtime.h"

static const char *blocked_path;
static int blocked_error;
static unsigned files_seen;
static bool fpkg_feature_enabled;
static unsigned packages_seen;

static DIR *test_opendir(const char *path) {
  if (blocked_path && strcmp(path, blocked_path) == 0) {
    errno = blocked_error;
    return NULL;
  }
  return opendir(path);
}

bool should_stop_requested(void) { return false; }
bool runtime_sleep_mode_active(void) { return false; }
bool sm_game_lifecycle_has_active_game(void) { return false; }
bool sm_config_fpkg_direct_mount_enabled(void) { return fpkg_feature_enabled; }
int get_scan_path_count(void) { return 0; }
bool get_scan_path(int index, char out[MAX_PATH]) {
  (void)index; (void)out; return false;
}
bool is_pfsc_image_mount_base_or_child(const char *path) {
  (void)path; return false;
}
bool is_under_image_mount_base(const char *path) {
  (void)path; return false;
}
bool path_matches_root_or_child(const char *path, const char *root) {
  (void)path; (void)root; return false;
}
bool is_supported_image_file_path(const char *path, const char *name) {
  (void)path; return strcmp(name, "game.exfat") == 0;
}

static bool visit_image(const char *path, const char *name, unsigned depth,
                        void *ctx) {
  (void)path; (void)name; (void)depth; (void)ctx;
  ++files_seen;
  return true;
}

static bool visit_package(const char *path, const char *name, unsigned depth,
                          void *ctx) {
  (void)depth;
  (void)ctx;
  assert(strstr(path, name) != NULL);
  assert(strcasecmp(strrchr(name, '.'), ".pkg") == 0);
  ++packages_seen;
  return true;
}

#define opendir test_opendir
#include "../src/sm_scan_tree.c"
#undef opendir

int main(void) {
  char root[] = "/tmp/sm-scan-permissions-XXXXXX";
  assert(mkdtemp(root));
  char blocked[MAX_PATH], game[MAX_PATH], package[MAX_PATH];
  snprintf(blocked, sizeof(blocked), "%s/blocked", root);
  snprintf(game, sizeof(game), "%s/game.exfat", root);
  snprintf(package, sizeof(package), "%s/synthetic.PKG", root);
  assert(mkdir(blocked, 0700) == 0);
  int fd = open(game, O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0 && close(fd) == 0);
  fd = open(package, O_CREAT | O_WRONLY, 0600);
  assert(fd >= 0 && close(fd) == 0);
  sm_scan_tree_callbacks_t callbacks = {
      .on_image_file = visit_image,
      .on_package_file = visit_package,
  };
  const int errors[] = {EACCES, EPERM};
  for (size_t i = 0; i < sizeof(errors) / sizeof(errors[0]); ++i) {
    blocked_error = errors[i];
    blocked_path = root;
    callbacks.skip_permission_errors = false;
    assert(!sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
    callbacks.skip_permission_errors = true;
    assert(sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
    blocked_path = blocked;
    files_seen = 0;
    packages_seen = 0;
    assert(sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
    assert(files_seen == 1);
    assert(packages_seen == 0); // An explicit off setting disables package visits.
    fpkg_feature_enabled = true;
    assert(sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
    assert(packages_seen == 1);
    fpkg_feature_enabled = false;
    callbacks.skip_permission_errors = false;
    assert(!sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
  }
  blocked_path = root;
  blocked_error = EIO;
  callbacks.skip_permission_errors = true;
  assert(!sm_scan_tree_walk(root, root, 0, 2, &callbacks, NULL));
  assert(unlink(package) == 0 && unlink(game) == 0 && rmdir(blocked) == 0 &&
         rmdir(root) == 0);
  puts("Scan permissions: watcher skips denied roots/subdirectories; content scans and I/O failures remain strict");
  return 0;
}
