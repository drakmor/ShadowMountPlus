#include "host_platform.h"
#include "sm_paths.h"

struct statfs {
  char f_mntonname[64];
};

static bool storage_mounted = true, stat_failed, statfs_failed;
static const char *image_source;
static const char *scan_roots[] = SM_DEFAULT_SCAN_PATHS_INITIALIZER;

static int test_stat(const char *path, struct stat *st) {
  if (stat_failed) {
    errno = EIO;
    return -1;
  }
  memset(st, 0, sizeof(*st));
  st->st_dev = strcmp(path, "/mnt") != 0 && storage_mounted ? 2 : 1;
  return 0;
}

static inline int test_statfs(const char *path, struct statfs *fs) {
  assert(strcmp(path, "/mnt/ext1") == 0);
  if (statfs_failed) {
    errno = EIO;
    return -1;
  }
  strlcpy(fs->f_mntonname, storage_mounted ? "/mnt/ext1" : "/mnt",
          sizeof(fs->f_mntonname));
  return 0;
}

static bool read_mount_image_link(const char *title_id, char *out, size_t size) {
  assert(strcmp(title_id, "PPSA28416") == 0);
  if (!image_source)
    return false;
  strlcpy(out, image_source, size);
  return true;
}

static int get_scan_path_count(void) {
  return (int)(sizeof(scan_roots) / sizeof(scan_roots[0])) - 1;
}

static bool get_scan_path(int index, char out[MAX_PATH]) {
  strlcpy(out, scan_roots[index], MAX_PATH);
  return true;
}

int sm_smb_normalize(const char *path, char out[MAX_PATH]) {
  strlcpy(out, path, MAX_PATH);
  return 0;
}

#define stat(...) test_stat(__VA_ARGS__)
#define statfs(...) test_statfs(__VA_ARGS__)
#include "storage_path_helpers.inc"

int main(void) {
  assert(!is_usb_storage_path(NULL));
  assert(!is_usb_storage_path(""));
  const char *non_usb[] = {
      "/mnt/ext1", "/mnt/ext1/", "/mnt/ext1/homebrew/Game",
      "/mnt/ext1/homebrew/Game.ffpfsc", "/data/homebrew/Game",
      "/mnt/ext10/Game", "/mnt/ext0other/Game", "/mnt/usb00/Game",
      "/mnt/usb8/Game"};
  for (size_t i = 0; i < sizeof(non_usb) / sizeof(non_usb[0]); ++i) {
    assert(!is_usb_storage_path(non_usb[i]));
    assert(!usb_storage_root_mounted(non_usb[i]));
  }

  for (int slot = 0; slot <= 8; ++slot) {
    char root[sizeof("/mnt/usb0")], child[MAX_PATH];
    if (slot == 8)
      strlcpy(root, "/mnt/ext0", sizeof(root));
    else
      snprintf(root, sizeof(root), "/mnt/usb%d", slot);
    snprintf(child, sizeof(child), "%s/homebrew/Game", root);
    assert(is_usb_storage_path(root) && is_usb_storage_path(child));
    storage_mounted = true;
    assert(usb_storage_root_mounted(child));
    storage_mounted = false;
    assert(!usb_storage_root_mounted(child));
    storage_mounted = true;
    stat_failed = true;
    assert(!usb_storage_root_mounted(child));
    stat_failed = false;
    assert(title_is_usb_backed("PPSA28416", child, true));
    assert(!title_is_usb_backed("PPSA28416", child, false));
    image_source = child;
    assert(title_is_usb_backed("PPSA28416", "/mnt/shadowmnt/Game", true));
    image_source = NULL;
  }

  const char *m2_game = "/mnt/ext1/homebrew/STAR WARS Galactic Racer PPSA28416";
  assert(!title_is_usb_backed("PPSA28416", m2_game, true));
  image_source = "/mnt/ext1/homebrew/Game.ffpfsc";
  assert(!title_is_usb_backed("PPSA28416", "/mnt/shadowmnt/Game", true));
  assert(!title_is_usb_backed("PPSA28416", m2_game, false));
  image_source = "/mnt/usb0/homebrew/Game.ffpfsc";
  assert(title_is_usb_backed("PPSA28416", m2_game, true));
  image_source = NULL;

  assert(destination_is_managed("/mnt/ext1/homebrew"));
  assert(destination_is_managed(m2_game));
  assert(!destination_is_managed("/other/homebrew"));
  assert(destination_is_managed("/mnt/usb0/homebrew"));
  storage_mounted = false;
  assert(!destination_is_managed("/mnt/ext1/homebrew"));
  assert(!destination_is_managed("/mnt/usb0/homebrew"));
  assert(destination_is_managed("/data/homebrew"));
  storage_mounted = true;
  statfs_failed = true;
  assert(!destination_is_managed("/mnt/ext1/homebrew"));
  statfs_failed = false;
  assert(destination_is_managed("/mnt/ext1/homebrew"));
  puts("Storage paths: M.2 folders/images survive USB classification; USB and destination guards passed");
  return 0;
}
