#include "host_platform.h"
#include "sm_fpkg_backport.h"

static void write_file(const char *path, const char *value) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
  assert(fd >= 0);
  assert(write(fd, value, strlen(value)) == (ssize_t)strlen(value));
  assert(close(fd) == 0);
}

int main(void) {
  char root[] = "/tmp/sm-fpkg-backport.XXXXXX";
  assert(mkdtemp(root));
  char data[MAX_PATH], extra[MAX_PATH], lib[MAX_PATH], ignored[MAX_PATH];
  assert(snprintf(data, sizeof(data), "%s/data.bin", root) > 0);
  assert(snprintf(extra, sizeof(extra), "%s/extra.bin", root) > 0);
  assert(snprintf(lib, sizeof(lib), "%s/fakelib", root) > 0);
  assert(snprintf(ignored, sizeof(ignored), "%s/fakelib/ignore.sprx", root) > 0);
  assert(mkdir(lib, 0700) == 0);
  write_file(ignored, "library");
  write_file(data, "patched");
  sm_fpkg_backport_t *snapshot = NULL;
  assert(sm_fpkg_backport_open(root, &snapshot) == SM_FPKG_OK);
  assert(sm_fpkg_backport_count(snapshot) == 1);
  assert(!strcmp(sm_fpkg_backport_files(snapshot)[0].path, "data.bin"));
  assert(sm_fpkg_backport_files(snapshot)[0].size == 7);
  assert(sm_fpkg_backport_unchanged(snapshot));
  int fd = sm_fpkg_backport_open_file(snapshot, 0);
  assert(fd >= 0 && close(fd) == 0);
  write_file(data, "changed-length");
  assert(!sm_fpkg_backport_unchanged(snapshot));
  assert(sm_fpkg_backport_open_file(snapshot, 0) < 0);
  sm_fpkg_backport_close(snapshot);
  assert(symlink("/tmp", extra) == 0);
  assert(sm_fpkg_backport_open(root, &snapshot) == SM_FPKG_UNSUPPORTED);
  assert(unlink(extra) == 0);
  assert(unlink(data) == 0 && unlink(ignored) == 0 && rmdir(lib) == 0);
  assert(rmdir(root) == 0);
  puts("FPKG backport snapshots detect replacement and reject symlinks");
  return 0;
}
