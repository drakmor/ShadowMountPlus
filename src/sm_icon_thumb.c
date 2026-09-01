#include "sm_platform.h"

#include <png.h>

#include "sm_icon_thumb.h"
#include "sm_paths.h"

#define SM_ICON_SOURCE_MAX_SIZE (4u * 1024u * 1024u)
#define SM_ICON_SOURCE_MAX_DIMENSION 2048u
#define SM_ICON_TEMP_MAX_AGE_SECONDS 300

static unsigned int g_icon_temp_serial;

static bool ensure_icon_cache_directory(void) {
  if (mkdir(LOG_DIR, 0777) != 0 && errno != EEXIST)
    return false;
  return mkdir(ICON_CACHE_DIR, 0777) == 0 || errno == EEXIST;
}

static void resize_rgba(const unsigned char *source, uint32_t source_width,
                        uint32_t source_height, unsigned char *target) {
  const uint32_t target_size = SM_ICON_THUMB_SIZE;
  for (uint32_t y = 0; y < target_size; ++y) {
    uint64_t source_y = target_size > 1u
                            ? ((uint64_t)y * (source_height - 1u) << 16u) /
                                  (target_size - 1u)
                            : 0;
    uint32_t y0 = (uint32_t)(source_y >> 16u);
    uint32_t y1 = y0 + 1u < source_height ? y0 + 1u : y0;
    uint32_t wy = (uint32_t)(source_y & 0xffffu);
    for (uint32_t x = 0; x < target_size; ++x) {
      uint64_t source_x = target_size > 1u
                              ? ((uint64_t)x * (source_width - 1u) << 16u) /
                                    (target_size - 1u)
                              : 0;
      uint32_t x0 = (uint32_t)(source_x >> 16u);
      uint32_t x1 = x0 + 1u < source_width ? x0 + 1u : x0;
      uint32_t wx = (uint32_t)(source_x & 0xffffu);
      const unsigned char *p00 = source + ((size_t)y0 * source_width + x0) * 4u;
      const unsigned char *p10 = source + ((size_t)y0 * source_width + x1) * 4u;
      const unsigned char *p01 = source + ((size_t)y1 * source_width + x0) * 4u;
      const unsigned char *p11 = source + ((size_t)y1 * source_width + x1) * 4u;
      unsigned char *pixel = target + ((size_t)y * target_size + x) * 4u;
      for (size_t channel = 0; channel < 4u; ++channel) {
        uint32_t top = (p00[channel] * (65536u - wx) +
                        p10[channel] * wx + 32768u) >>
                       16u;
        uint32_t bottom = (p01[channel] * (65536u - wx) +
                           p11[channel] * wx + 32768u) >>
                          16u;
        pixel[channel] = (unsigned char)((top * (65536u - wy) +
                                          bottom * wy + 32768u) >>
                                         16u);
      }
    }
  }
}

static void cleanup_stale_temp_files(const char *title_id) {
  DIR *directory = opendir(ICON_CACHE_DIR);
  if (!directory)
    return;

  char prefix[MAX_TITLE_ID + 32u];
  int written = snprintf(prefix, sizeof(prefix), "%s-%u-", title_id,
                         SM_ICON_THUMB_SIZE);
  if (written < 0 || (size_t)written >= sizeof(prefix)) {
    (void)closedir(directory);
    return;
  }

  time_t now = time(NULL);
  struct dirent *entry;
  while ((entry = readdir(directory)) != NULL) {
    if (strncmp(entry->d_name, prefix, (size_t)written) != 0)
      continue;
    char path[MAX_PATH];
    int path_written = snprintf(path, sizeof(path), "%s/%s", ICON_CACHE_DIR,
                                entry->d_name);
    if (path_written < 0 || (size_t)path_written >= sizeof(path) ||
        !strstr(entry->d_name, ".tmp.")) {
      continue;
    }
    struct stat st;
    if (stat(path, &st) != 0 || now < st.st_mtime ||
        now - st.st_mtime < SM_ICON_TEMP_MAX_AGE_SECONDS) {
      continue;
    }
    (void)unlink(path);
  }
  (void)closedir(directory);
}

static bool create_thumbnail(const char *source_path, const char *cache_path) {
  png_image image;
  memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;
  if (!png_image_begin_read_from_file(&image, source_path))
    return false;
  if (image.width == 0 || image.height == 0 ||
      image.width > SM_ICON_SOURCE_MAX_DIMENSION ||
      image.height > SM_ICON_SOURCE_MAX_DIMENSION) {
    png_image_free(&image);
    return false;
  }

  image.format = PNG_FORMAT_RGBA;
  size_t source_size = PNG_IMAGE_SIZE(image);
  unsigned char *source = malloc(source_size);
  if (!source) {
    png_image_free(&image);
    return false;
  }
  if (!png_image_finish_read(&image, NULL, source, 0, NULL)) {
    free(source);
    png_image_free(&image);
    return false;
  }
  uint32_t source_width = image.width;
  uint32_t source_height = image.height;
  png_image_free(&image);

  size_t target_size = (size_t)SM_ICON_THUMB_SIZE * SM_ICON_THUMB_SIZE * 4u;
  unsigned char *target = malloc(target_size);
  if (!target) {
    free(source);
    return false;
  }
  resize_rgba(source, source_width, source_height, target);
  free(source);

  unsigned int serial = __sync_add_and_fetch(&g_icon_temp_serial, 1u);
  char temp_path[MAX_PATH];
  int written = snprintf(temp_path, sizeof(temp_path), "%s.tmp.%ld.%u",
                         cache_path, (long)getpid(), serial);
  if (written < 0 || (size_t)written >= sizeof(temp_path)) {
    free(target);
    return false;
  }

  png_image output;
  memset(&output, 0, sizeof(output));
  output.version = PNG_IMAGE_VERSION;
  output.width = SM_ICON_THUMB_SIZE;
  output.height = SM_ICON_THUMB_SIZE;
  output.format = PNG_FORMAT_RGBA;
  (void)unlink(temp_path);
  bool encoded = png_image_write_to_file(&output, temp_path, 0, target, 0,
                                         NULL) != 0;
  free(target);
  png_image_free(&output);
  if (!encoded) {
    (void)unlink(temp_path);
    return false;
  }

  int fd = open(temp_path, O_RDONLY);
  bool synced = fd >= 0 && fsync(fd) == 0;
  if (fd >= 0)
    (void)close(fd);
  if (!synced || rename(temp_path, cache_path) != 0) {
    (void)unlink(temp_path);
    return false;
  }
  return true;
}

bool sm_icon_thumbnail_path(const char *title_id, const char *source_path,
                            const struct stat *source_st,
                            char out[MAX_PATH]) {
  if (!title_id || title_id[0] == '\0' || strpbrk(title_id, "/\\") ||
      !source_path || !source_st || !S_ISREG(source_st->st_mode) ||
      source_st->st_size <= 0 ||
      (uint64_t)source_st->st_size > SM_ICON_SOURCE_MAX_SIZE ||
      !ensure_icon_cache_directory()) {
    return false;
  }

  int written = snprintf(out, MAX_PATH, "%s/%s-%u-%lld-%lld.png",
                         ICON_CACHE_DIR, title_id, SM_ICON_THUMB_SIZE,
                         (long long)source_st->st_mtime,
                         (long long)source_st->st_size);
  if (written < 0 || written >= MAX_PATH)
    return false;

  struct stat cache_st;
  if (stat(out, &cache_st) == 0 && S_ISREG(cache_st.st_mode) &&
      cache_st.st_size > 0) {
    return true;
  }
  if (!create_thumbnail(source_path, out))
    return false;
  cleanup_stale_temp_files(title_id);
  return true;
}
