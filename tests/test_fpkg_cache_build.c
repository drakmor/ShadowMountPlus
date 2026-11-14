/* Synthetic end-to-end reader/cache publication fixture; no game content. */
#include "host_platform.h"

#define SM_TEST_FPKG_HEADER_EXTERNAL_SOURCE_MATCHES
#define main synthetic_fpkg_reader_checks_main
#include "test_fpkg_header.c"
#undef main

#include "sm_fpkg_cache.h"
#include "sm_time.h"

static uint64_t fake_clock = 1000;

uint64_t monotonic_time_us(void) { return ++fake_clock; }

bool sm_pkg_source_matches(const sm_pkg_info_t *info, const struct stat *st) {
  if (!info || !st || !S_ISREG(st->st_mode))
    return false;
#ifdef __APPLE__
  time_t mtime = st->st_mtimespec.tv_sec;
  uint32_t mtime_ns = (uint32_t)st->st_mtimespec.tv_nsec;
  uint64_t ctime = (uint64_t)st->st_ctimespec.tv_sec;
  uint32_t ctime_ns = (uint32_t)st->st_ctimespec.tv_nsec;
#else
  time_t mtime = st->st_mtim.tv_sec;
  uint32_t mtime_ns = (uint32_t)st->st_mtim.tv_nsec;
  uint64_t ctime = (uint64_t)st->st_ctim.tv_sec;
  uint32_t ctime_ns = (uint32_t)st->st_ctim.tv_nsec;
#endif
  return (uint64_t)st->st_size == info->source_size &&
         (uint64_t)st->st_dev == info->source_device &&
         (uint64_t)st->st_ino == info->source_inode &&
         ctime == info->source_ctime && ctime_ns == info->source_ctime_ns &&
         mtime == info->source_mtime && mtime_ns == info->source_mtime_ns;
}

typedef struct {
  unsigned calls;
  unsigned cancel_at;
} cancel_context_t;

typedef struct {
  unsigned calls;
  const char *path;
} mutate_context_t;

static bool cancel_on_call(void *opaque) {
  cancel_context_t *cancel = opaque;
  return ++cancel->calls >= cancel->cancel_at;
}

static bool mutate_backport_on_call(void *opaque) {
  mutate_context_t *mutation = opaque;
  if (++mutation->calls == 2u) {
    int fd = open(mutation->path, O_WRONLY | O_TRUNC | O_NOFOLLOW);
    assert(fd >= 0);
    assert(write(fd, "changed during build", 20) == 20);
    assert(close(fd) == 0);
  }
  return false;
}

static void write_all(int fd, const void *data, size_t size) {
  const uint8_t *bytes = data;
  size_t written = 0;
  while (written < size) {
    ssize_t count = write(fd, bytes + written, size - written);
    if (count < 0 && errno == EINTR)
      continue;
    assert(count > 0);
    written += (size_t)count;
  }
}

static void make_cache_fixture(memory_pkg_t *pkg) {
  valid_envelope(pkg);

  uint8_t value[8];
  put_le64(value, 9);
  put_inner_image(pkg, 0x10000u + 0x30u, value, sizeof(value));

  put_le64(value, 72);
  put_inner_image(pkg, 0x50000u + 4u * INNER_INODE_SIZE + 8u,
                  value, sizeof(value));

  uint8_t root_entries[72] = {0};
  make_dirent(root_entries, 5, "eboot.bin");
  put_le32(root_entries + 4, 2);
  make_dirent(root_entries + 25, 6, "sce_sys");
  put_le32(root_entries + 25 + 4, 3);
  make_dirent(root_entries + 48, 8, "data.bin");
  put_le32(root_entries + 48 + 4, 2);
  put_inner_image(pkg, 0xA0000u, root_entries, sizeof(root_entries));

  uint8_t metadata_inode[INNER_INODE_SIZE] = {0};
  put_le16(metadata_inode, 0x4000u);
  put_le64(metadata_inode + 8, 26);
  put_le64(metadata_inode + 0x60, 0xB0000u);
  put_inner_image(pkg, 0x50000u + 6u * INNER_INODE_SIZE,
                  metadata_inode, sizeof(metadata_inode));
  uint8_t metadata_entry[26] = {0};
  make_dirent(metadata_entry, 7, "param.json");
  put_le32(metadata_entry + 4, 2);
  put_inner_image(pkg, 0xB0000u, metadata_entry, sizeof(metadata_entry));

  uint8_t file_inode[INNER_INODE_SIZE] = {0};
  put_le16(file_inode, 0x8000u);
  put_le64(file_inode + 8, 2);
  put_le64(file_inode + 0x60, 0x2000u);
  put_inner_image(pkg, 0x50000u + 7u * INNER_INODE_SIZE,
                  file_inode, sizeof(file_inode));
  put_inner_image(pkg, 0x2000u, (const uint8_t *)"{}", 2u);

  memset(file_inode, 0, sizeof(file_inode));
  put_le16(file_inode, 0x8000u);
  put_le64(file_inode + 8, 0);
  put_le64(file_inode + 0x60, 0);
  put_inner_image(pkg, 0x50000u + 8u * INNER_INODE_SIZE,
                  file_inode, sizeof(file_inode));
}

static void remove_tree(const char *path) {
  DIR *directory = opendir(path);
  if (!directory) {
    assert(unlink(path) == 0);
    return;
  }
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
      continue;
    char child[MAX_PATH];
    int length = snprintf(child, sizeof(child), "%s/%s", path,
                          entry->d_name);
    assert(length > 0 && (size_t)length < sizeof(child));
    struct stat st;
    assert(lstat(child, &st) == 0);
    if (S_ISDIR(st.st_mode))
      remove_tree(child);
    else
      assert(unlink(child) == 0);
  }
  assert(closedir(directory) == 0);
  assert(rmdir(path) == 0);
}

static void set_source_identity(sm_pkg_info_t *info, const char *source_path,
                                const struct stat *st) {
  char canonical[MAX_PATH];
  assert(realpath(source_path, canonical) != NULL);
  (void)strlcpy(info->path, canonical, sizeof(info->path));
  info->source_size = (uint64_t)st->st_size;
  info->source_device = (uint64_t)st->st_dev;
  info->source_inode = (uint64_t)st->st_ino;
#ifdef __APPLE__
  info->source_mtime = st->st_mtimespec.tv_sec;
  info->source_mtime_ns = (uint32_t)st->st_mtimespec.tv_nsec;
  info->source_ctime = (uint64_t)st->st_ctimespec.tv_sec;
  info->source_ctime_ns = (uint32_t)st->st_ctimespec.tv_nsec;
#else
  info->source_mtime = st->st_mtim.tv_sec;
  info->source_mtime_ns = (uint32_t)st->st_mtim.tv_nsec;
  info->source_ctime = (uint64_t)st->st_ctim.tv_sec;
  info->source_ctime_ns = (uint32_t)st->st_ctim.tv_nsec;
#endif
}

int main(void) {
  char root_template[] = "/private/tmp/sm-fpkg-cache-build.XXXXXX";
  if (access("/private/tmp", W_OK) != 0)
    (void)strlcpy(root_template, "/tmp/sm-fpkg-cache-build.XXXXXX",
                  sizeof(root_template));
  char *root = mkdtemp(root_template);
  assert(root != NULL && chmod(root, 0700) == 0);

  char source_path[MAX_PATH], cache_root[MAX_PATH];
  assert(snprintf(source_path, sizeof(source_path), "%s/source.pkg", root) > 0);
  assert(snprintf(cache_root, sizeof(cache_root), "%s/cache", root) > 0);
  assert(mkdir(cache_root, 0700) == 0);

  memory_pkg_t pkg;
  make_cache_fixture(&pkg);
  sm_pkg_info_t stream_info = metadata();
  sm_fpkg_t *stream_fp = NULL;
  sm_fpkg_status_t stream_status = sm_fpkg_open_stream(
      "game.pkg", sizeof(pkg.bytes), memory_read, &pkg, &stream_info,
      &stream_fp);
  if (stream_status != SM_FPKG_OK)
    fprintf(stderr, "synthetic cache FPKG stream open returned %d\n",
            (int)stream_status);
  assert(stream_status == SM_FPKG_OK && sm_fpkg_file_count(stream_fp) == 4u);
  sm_fpkg_close(stream_fp);
  int write_fd = open(source_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                      0600);
  assert(write_fd >= 0);
  write_all(write_fd, pkg.bytes, sizeof(pkg.bytes));
  assert(fsync(write_fd) == 0 && close(write_fd) == 0);

  int source_fd = open(source_path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(source_fd >= 0);
  struct stat source_stat;
  assert(fstat(source_fd, &source_stat) == 0);
  sm_pkg_info_t info = metadata();
  set_source_identity(&info, source_path, &source_stat);

  sm_fpkg_t *fpkg = NULL;
  sm_fpkg_status_t open_status =
      sm_fpkg_open_fd(source_path, source_fd, &info, &fpkg);
  if (open_status != SM_FPKG_OK)
    fprintf(stderr, "synthetic cache FPKG fd open returned %d\n",
            (int)open_status);
  assert(open_status == SM_FPKG_OK);
  assert(fpkg && sm_fpkg_file_count(fpkg) == 4u);

  sm_fpkg_cache_options_t small_limit = {.max_extract_bytes = 1u};
  char limited_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &small_limit, limited_generation) == SM_FPKG_UNSUPPORTED);
  assert(limited_generation[0] == '\0');

  cancel_context_t cancel = {.cancel_at = 6u};
  sm_fpkg_cache_options_t cancelled_build = {
      .max_extract_bytes = SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT,
      .is_cancelled = cancel_on_call,
      .cancel_ctx = &cancel,
  };
  char generation_path[MAX_PATH];
  sm_fpkg_status_t cancelled_status = sm_fpkg_cache_build_generation(
      cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
      &cancelled_build, generation_path);
  if (cancelled_status != SM_FPKG_CANCELLED)
    fprintf(stderr, "synthetic cache cancellation returned %d\n",
            (int)cancelled_status);
  assert(cancelled_status == SM_FPKG_CANCELLED);
  assert(cancel.calls == cancel.cancel_at);
  assert(generation_path[0] == '\0');

  sm_fpkg_cache_recovery_report_t recovery = {0};
  assert(sm_fpkg_cache_recover_title(cache_root, info.title_id, &recovery) ==
         SM_FPKG_OK);
  assert(recovery.active_generation[0] == '\0' &&
         recovery.staging_generations == 1u);

  sm_fpkg_cache_options_t options = {
      .max_extract_bytes = SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT,
  };
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &options, generation_path) == SM_FPKG_OK);
  assert(generation_path[0] != '\0');

  sm_fpkg_cache_generation_info_t generation_info;
  assert(sm_fpkg_cache_read_generation_info(
             cache_root, info.title_id, generation_path,
             &generation_info) == SM_FPKG_OK);
  assert(generation_info.source_matches_snapshot &&
         strcmp(generation_info.source_path, source_path) == 0);
  char current_generation[MAX_PATH];
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &info, current_generation) == SM_FPKG_OK);
  assert(strcmp(current_generation, generation_path) == 0);

  char path[MAX_PATH];
  assert(snprintf(path, sizeof(path), "%s/eboot.bin", generation_path) > 0);
  int extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  struct stat extracted_stat;
  assert(fstat(extracted_fd, &extracted_stat) == 0 &&
         (extracted_stat.st_mode & 0777) == 0755);
  char extracted[13];
  assert(read(extracted_fd, extracted, sizeof(extracted)) ==
         (ssize_t)sizeof(extracted));
  assert(close(extracted_fd) == 0);
  assert(memcmp(extracted, "hello native!", sizeof(extracted)) == 0);
  assert(chmod(path, 0644) == 0);
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &info, current_generation) == SM_FPKG_UNSUPPORTED);
  assert(chmod(path, 0755) == 0);

  assert(snprintf(path, sizeof(path), "%s/sce_sys/param.json",
                  generation_path) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  char package_metadata[2];
  assert(read(extracted_fd, package_metadata, sizeof(package_metadata)) ==
         (ssize_t)sizeof(package_metadata));
  assert(close(extracted_fd) == 0);
  assert(memcmp(package_metadata, "{}", sizeof(package_metadata)) == 0);

  assert(snprintf(path, sizeof(path), "%s/ampr_emu.index", generation_path) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  uint8_t index_header[64];
  assert(read(extracted_fd, index_header, sizeof(index_header)) ==
         (ssize_t)sizeof(index_header));
  assert(close(extracted_fd) == 0);
  assert(memcmp(index_header, "AMPRIDX3", 8) == 0);

  assert(snprintf(path, sizeof(path), "%s/ampr_assets.index", generation_path) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  uint8_t manifest_header[128];
  assert(read(extracted_fd, manifest_header, sizeof(manifest_header)) ==
         (ssize_t)sizeof(manifest_header));
  assert(close(extracted_fd) == 0);
  assert(memcmp(manifest_header, "AMPRPAK4", 8) == 0 &&
         get_le64(manifest_header + 40) == 3u &&
         get_le64(manifest_header + 48) == 0u);

  assert(snprintf(path, sizeof(path), "%s/kraken.image", generation_path) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  char image_name[sizeof("kraken/source.pkg\n") - 1u];
  assert(read(extracted_fd, image_name, sizeof(image_name)) ==
         (ssize_t)sizeof(image_name));
  assert(close(extracted_fd) == 0);
  assert(memcmp(image_name, "kraken/source.pkg\n", sizeof(image_name)) == 0);

  static const char wrong_image[] = "kraken/xxxxxx.pkg\n";
  static const char correct_image[] = "kraken/source.pkg\n";
  assert(sizeof(wrong_image) == sizeof(correct_image));
  assert(snprintf(path, sizeof(path), "%s/kraken.image", generation_path) > 0);
  extracted_fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  write_all(extracted_fd, wrong_image, sizeof(wrong_image) - 1u);
  assert(fsync(extracted_fd) == 0 && close(extracted_fd) == 0);
  sm_fpkg_cache_generation_info_t invalid_generation = {0};
  assert(sm_fpkg_cache_read_generation_info(
             cache_root, info.title_id, generation_path,
             &invalid_generation) == SM_FPKG_CORRUPT);
  extracted_fd = open(path, O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  write_all(extracted_fd, correct_image, sizeof(correct_image) - 1u);
  assert(fsync(extracted_fd) == 0 && close(extracted_fd) == 0);
  assert(sm_fpkg_cache_read_generation_info(
             cache_root, info.title_id, generation_path,
             &invalid_generation) == SM_FPKG_OK);
  assert(invalid_generation.source_matches_snapshot);

  char second_source_path[MAX_PATH];
  assert(snprintf(second_source_path, sizeof(second_source_path),
                  "%s/source-v2.pkg", root) > 0);
  int second_write_fd = open(second_source_path,
                             O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  assert(second_write_fd >= 0);
  write_all(second_write_fd, pkg.bytes, sizeof(pkg.bytes));
  assert(fsync(second_write_fd) == 0 && close(second_write_fd) == 0);
  int second_source_fd = open(second_source_path,
                              O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(second_source_fd >= 0);
  struct stat second_source_stat;
  assert(fstat(second_source_fd, &second_source_stat) == 0);
  sm_pkg_info_t second_info = metadata();
  set_source_identity(&second_info, second_source_path, &second_source_stat);
  sm_fpkg_t *second_fpkg = NULL;
  assert(sm_fpkg_open_fd(second_source_path, second_source_fd, &second_info,
                         &second_fpkg) == SM_FPKG_OK);

  cancel_context_t late_cancel = {.cancel_at = 12u};
  sm_fpkg_cache_options_t cancelled_rebuild = {
      .max_extract_bytes = SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT,
      .is_cancelled = cancel_on_call,
      .cancel_ctx = &late_cancel,
  };
  char rejected_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, second_source_path, second_source_fd, &second_info,
             second_fpkg, NULL, NULL, &cancelled_rebuild,
             rejected_generation) == SM_FPKG_CANCELLED);
  assert(late_cancel.calls == late_cancel.cancel_at &&
         rejected_generation[0] == '\0');
  recovery = (sm_fpkg_cache_recovery_report_t){0};
  assert(sm_fpkg_cache_recover_title(cache_root, second_info.title_id,
                                     &recovery) == SM_FPKG_OK);
  assert(strcmp(recovery.active_generation, generation_path) == 0 &&
         recovery.staging_generations == 2u);
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &info, current_generation) == SM_FPKG_OK);
  assert(strcmp(current_generation, generation_path) == 0);
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &second_info, current_generation) ==
         SM_FPKG_UNSUPPORTED);
  sm_fpkg_close(second_fpkg);
  assert(close(second_source_fd) == 0);

  char repeated_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &options, repeated_generation) == SM_FPKG_OK);
  assert(strcmp(repeated_generation, generation_path) == 0);
  assert(snprintf(path, sizeof(path), "%s/eboot.bin", generation_path) > 0);
  assert(chmod(path, 0644) == 0);
  char upgraded_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &options, upgraded_generation) == SM_FPKG_OK);
  assert(strcmp(upgraded_generation, generation_path) != 0);
  assert(snprintf(path, sizeof(path), "%s/eboot.bin", upgraded_generation) > 0);
  assert(stat(path, &extracted_stat) == 0 &&
         (extracted_stat.st_mode & 0777) == 0755);
  char backport_path[MAX_PATH], backport_file[MAX_PATH], extra_file[MAX_PATH];
  assert(snprintf(backport_path, sizeof(backport_path), "%s/backport", root) > 0);
  assert(mkdir(backport_path, 0700) == 0);
  assert(snprintf(backport_file, sizeof(backport_file), "%s/DATA.BIN", backport_path) > 0);
  assert(snprintf(extra_file, sizeof(extra_file), "%s/extra.bin", backport_path) > 0);
  int backport_fd = open(backport_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(backport_fd >= 0);
  write_all(backport_fd, "patched", 7);
  assert(close(backport_fd) == 0);
  backport_fd = open(extra_file, O_WRONLY | O_CREAT | O_EXCL, 0600);
  assert(backport_fd >= 0);
  write_all(backport_fd, "new", 3);
  assert(close(backport_fd) == 0);
  options.backport_path = backport_path;
  mutate_context_t mutation = {.path = backport_file};
  sm_fpkg_cache_options_t racing = options;
  racing.is_cancelled = mutate_backport_on_call;
  racing.cancel_ctx = &mutation;
  char interrupted_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &racing, interrupted_generation) != SM_FPKG_OK);
  assert(!interrupted_generation[0]);
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &info, current_generation) == SM_FPKG_OK);
  assert(!strcmp(current_generation, upgraded_generation));
  backport_fd = open(backport_file, O_WRONLY | O_TRUNC | O_NOFOLLOW);
  assert(backport_fd >= 0);
  write_all(backport_fd, "patched", 7);
  assert(close(backport_fd) == 0);
  char backport_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &options, backport_generation) == SM_FPKG_OK);
  assert(strcmp(backport_generation, upgraded_generation));
  assert(sm_fpkg_cache_find_current_generation_with_backport(
             cache_root, &info, backport_path, current_generation) == SM_FPKG_OK);
  assert(!strcmp(current_generation, backport_generation));
  assert(sm_fpkg_cache_read_generation_info(
             cache_root, info.title_id, backport_generation,
             &generation_info) == SM_FPKG_OK);
  assert(sm_fpkg_cache_backport_matches(&generation_info, backport_path));
  assert(snprintf(path, sizeof(path), "%s/data.bin", backport_generation) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  char patched[7];
  assert(read(extracted_fd, patched, sizeof(patched)) == (ssize_t)sizeof(patched));
  assert(!memcmp(patched, "patched", sizeof(patched)) && close(extracted_fd) == 0);
  assert(snprintf(path, sizeof(path), "%s/ampr_assets.index", backport_generation) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  assert(read(extracted_fd, manifest_header, sizeof(manifest_header)) ==
         (ssize_t)sizeof(manifest_header));
  assert(close(extracted_fd) == 0 && get_le64(manifest_header + 40) == 4u &&
         get_le64(manifest_header + 48) == 0u);
  backport_fd = open(backport_file, O_WRONLY | O_TRUNC | O_NOFOLLOW);
  assert(backport_fd >= 0);
  write_all(backport_fd, "changed-backport", 16);
  assert(close(backport_fd) == 0);
  assert(!sm_fpkg_cache_backport_matches(&generation_info, backport_path));
  assert(sm_fpkg_cache_find_current_generation_with_backport(
             cache_root, &info, backport_path, current_generation) ==
         SM_FPKG_UNSUPPORTED);
  assert(sm_fpkg_cache_find_current_generation(
             cache_root, &info, current_generation) == SM_FPKG_UNSUPPORTED);
  char changed_generation[MAX_PATH];
  assert(sm_fpkg_cache_build_generation(
             cache_root, source_path, source_fd, &info, fpkg, NULL, NULL,
             &options, changed_generation) == SM_FPKG_OK);
  assert(strcmp(changed_generation, backport_generation));
  assert(snprintf(path, sizeof(path), "%s/data.bin", changed_generation) > 0);
  extracted_fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  assert(extracted_fd >= 0);
  char changed[16];
  assert(read(extracted_fd, changed, sizeof(changed)) == (ssize_t)sizeof(changed));
  assert(!memcmp(changed, "changed-backport", sizeof(changed)) &&
         close(extracted_fd) == 0);
  sm_fpkg_close(fpkg);
  assert(close(source_fd) == 0);
  remove_tree(root);
  puts("synthetic FPKG cache build, partial extraction recovery, failed replacement preservation, and generation reuse passed");
  return 0;
}
