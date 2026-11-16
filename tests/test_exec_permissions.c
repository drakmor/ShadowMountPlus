#include "host_platform.h"
#include "sm_exec_permissions.h"

#include "../src/sm_exec_permissions.c"

static char root[] = "/tmp/sm-exec-permissions-XXXXXX";

static void make_file(const char *path, mode_t mode) {
  int fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0600);
  assert(fd >= 0 && close(fd) == 0);
  assert(chmod(path, mode) == 0);
}

static mode_t mode_of(const char *path) {
  struct stat st;
  assert(lstat(path, &st) == 0);
  return st.st_mode & 07777;
}

static void test_file_mode_repair(mode_t before, mode_t expected_after,
                                  int expected_result) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/eboot.bin", root);
  make_file(path, before);
  assert(sm_add_exec_permission(path) == expected_result);
  assert(mode_of(path) == expected_after);
  assert(unlink(path) == 0);
}

static void test_missing_file_is_ignored(void) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/missing.prx", root);
  assert(sm_add_exec_permission(path) == 0);
}

static void test_directory_is_ignored(void) {
  char path[PATH_MAX];
  snprintf(path, sizeof(path), "%s/sce_module", root);
  const mode_t directory_mode = 0700;
  assert(mkdir(path, directory_mode) == 0);
  assert(sm_add_exec_permission(path) == 0);
  assert(mode_of(path) == directory_mode);
  assert(rmdir(path) == 0);
}

static void test_symlink_target_is_not_changed(void) {
  char target[PATH_MAX], link_path[PATH_MAX];
  snprintf(target, sizeof(target), "%s/outside.bin", root);
  snprintf(link_path, sizeof(link_path), "%s/libc.prx", root);
  const mode_t target_mode = 0644;
  make_file(target, target_mode);
  assert(symlink(target, link_path) == 0);
  assert(sm_add_exec_permission(link_path) == 0);
  assert(mode_of(target) == target_mode);
  assert(unlink(link_path) == 0 && unlink(target) == 0);
}

int main(void) {
  assert(mkdtemp(root));
  // Typical FTP/ZIP copy: read/write bits kept, all execute bits added.
  test_file_mode_repair(0644, 0755, 1);
  // Owner-only file: group/other read access is not widened.
  test_file_mode_repair(0600, 0711, 1);
  // Partially executable file only gains the missing bits.
  test_file_mode_repair(0744, 0755, 1);
  // Already executable file is left untouched.
  test_file_mode_repair(0755, 0755, 0);
  test_missing_file_is_ignored();
  test_directory_is_ignored();
  test_symlink_target_is_not_changed();
  assert(rmdir(root) == 0);
  puts("Exec permissions: missing execute bits added; other bits, directories and symlink targets unchanged");
  return 0;
}
