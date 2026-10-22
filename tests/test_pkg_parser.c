#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sm_pkg.h"

static void put_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void put_le16(uint8_t *p, uint16_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)value;
  p[1] = (uint8_t)(value >> 8);
  p[2] = (uint8_t)(value >> 16);
  p[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *p, uint64_t value) {
  for (int i = 0; i < 8; i++) {
    p[i] = (uint8_t)value;
    value >>= 8;
  }
}

static size_t build_sfo(uint8_t *out, const char *title_id,
                        const char *category) {
  static const char keys[] = "TITLE\0TITLE_ID\0CATEGORY\0APP_VER\0";
  const char *names[] = {"TITLE", "TITLE_ID", "CATEGORY", "APP_VER"};
  const char *texts[] = {"Test Game", title_id, category, "01.02"};
  const uint32_t key_offset = 20u + 4u * 16u;
  const uint32_t data_offset = key_offset + (uint32_t)sizeof(keys);
  memset(out, 0, 512);
  memcpy(out, "\0PSF", 4);
  put_le32(out + 8, key_offset);
  put_le32(out + 12, data_offset);
  put_le32(out + 16, 4);
  size_t key_pos = 0;
  size_t data_pos = 0;
  for (size_t i = 0; i < 4; i++) {
    uint8_t *entry = out + 20u + i * 16u;
    put_le16(entry, (uint16_t)key_pos);
    put_le16(entry + 2, 0x0204);
    put_le32(entry + 4, (uint32_t)strlen(texts[i]) + 1u);
    put_le32(entry + 8, (uint32_t)strlen(texts[i]) + 1u);
    put_le32(entry + 12, (uint32_t)data_pos);
    key_pos += strlen(names[i]) + 1u;
    data_pos += strlen(texts[i]) + 1u;
  }
  memcpy(out + key_offset, keys, sizeof(keys));
  data_pos = 0;
  for (size_t i = 0; i < 4; i++) {
    size_t length = strlen(texts[i]) + 1u;
    memcpy(out + data_offset + data_pos, texts[i], length);
    data_pos += length;
  }
  return (size_t)data_offset + data_pos;
}

static size_t build_cnt_kind(uint8_t *out, uint32_t content_type,
                             uint32_t content_flags, const char *title_id,
                             const char *category) {
  uint8_t sfo[512];
  size_t sfo_size = build_sfo(sfo, title_id, category);
  const uint32_t table_offset = 0xA0;
  const uint32_t sfo_offset = table_offset + 0x20;
  memset(out, 0, 2048);
  put_be32(out, 0x7F434E54u);
  put_be32(out + 0x10, 1);
  put_be32(out + 0x18, table_offset);
  put_be32(out + 0x74, 0x20u);
  memcpy(out + 0x40, "UP0000-PPSA12345_00-TESTGAME0000000", 36);
  uint8_t *entry = out + table_offset;
  put_be32(entry, 0x1000);
  put_be32(entry + 0x10, sfo_offset);
  put_be32(entry + 0x14, (uint32_t)sfo_size);
  memcpy(out + sfo_offset, sfo, sfo_size);
  put_be32(out + 0x74, content_type);
  put_be32(out + 0x78, content_flags);
  return sfo_offset + sfo_size;
}

static size_t build_cnt(uint8_t *out, uint32_t base) {
  (void)base;
  return build_cnt_kind(out, 0x20u, 0u, "PPSA12345", "gd");
}

static void write_file(const char *path, const uint8_t *data, size_t size) {
  FILE *file = fopen(path, "wb");
  assert(file);
  assert(fwrite(data, 1, size, file) == size);
  assert(fclose(file) == 0);
}

int main(void) {
  char path[] = "/tmp/shadowmount-test-XXXXXX.pkg";
  int fd = mkstemps(path, 4);
  assert(fd >= 0);
  close(fd);

  uint8_t cnt[2048];
  size_t cnt_size = build_cnt(cnt, 0);
  write_file(path, cnt, cnt_size);
  struct stat st;
  assert(stat(path, &st) == 0);
  scan_candidate_t candidate;
  assert(sm_pkg_parse_file(path, &st, &candidate));
  assert(candidate.kind == SCAN_CANDIDATE_PKG);
  assert(strcmp(candidate.title_id, "PPSA12345") == 0);
  assert(strcmp(candidate.title_name, "Test Game") == 0);
  assert(strcmp(candidate.category, "gd") == 0);
  assert(candidate.pkg_kind == PKG_KIND_BASE);
  assert(candidate.pkg_classification_confident);
  assert(candidate.content_type == 0x20u);
  assert(strcmp(candidate.app_ver, "01.02") == 0);
  assert(strcmp(candidate.content_id, "UP0000-PPSA12345_00-TESTGAME0000000") == 0);

  uint8_t fih[4096];
  memset(fih, 0, sizeof(fih));
  memcpy(fih, cnt, 0xA0);
  put_be32(fih, 0x7F464948u);
  put_le16(fih + 6, 3);
  put_le64(fih + 0x58, 0x200);
  memcpy(fih + 0x200, cnt, cnt_size);
  write_file(path, fih, 0x200 + cnt_size);
  assert(stat(path, &st) == 0);
  assert(sm_pkg_parse_file(path, &st, &candidate));
  assert(strcmp(candidate.title_id, "PPSA12345") == 0);
  assert(candidate.pkg_kind == PKG_KIND_BASE);
  assert(candidate.outer_content_type == 0x20u);

  uint8_t patch[2048];
  size_t patch_size = build_cnt_kind(patch, 0x20u, 0x40100000u,
                                     "PPSA12345", "gp");
  write_file(path, patch, patch_size);
  assert(stat(path, &st) == 0);
  assert(sm_pkg_parse_file(path, &st, &candidate));
  assert(candidate.pkg_kind == PKG_KIND_PATCH);

  uint8_t dlc[2048];
  size_t dlc_size = build_cnt_kind(dlc, 0x21u, 0u, "PPSA12345", "ac");
  write_file(path, dlc, dlc_size);
  assert(stat(path, &st) == 0);
  assert(sm_pkg_parse_file(path, &st, &candidate));
  assert(candidate.pkg_kind == PKG_KIND_DLC);

  uint8_t system_app[2048];
  size_t system_app_size =
      build_cnt_kind(system_app, 0x26u, 0u, "PPSA12345", "gd");
  write_file(path, system_app, system_app_size);
  assert(stat(path, &st) == 0);
  assert(!sm_pkg_parse_file(path, &st, &candidate));

  uint8_t contradictory[2048];
  size_t contradictory_size =
      build_cnt_kind(contradictory, 0x20u, 0u, "PPSA12345", "gp");
  write_file(path, contradictory, contradictory_size);
  assert(stat(path, &st) == 0);
  assert(!sm_pkg_parse_file(path, &st, &candidate));

  contradictory_size =
      build_cnt_kind(contradictory, 0x20u, 0u, "FAKE12345", "gd");
  memcpy(contradictory + 0x40, "UP0000-FAKE12345_00-TESTGAME0000000", 36);
  write_file(path, contradictory, contradictory_size);
  assert(stat(path, &st) == 0);
  assert(!sm_pkg_parse_file(path, &st, &candidate));

  assert(sm_pkg_compare_versions("1.10", "1.9") > 0);
  assert(sm_pkg_compare_versions("1.02", "1.02") == 0);
  assert(sm_pkg_is_installed_content_path("/user/app/PPSA12345/app.pkg"));
  assert(sm_pkg_is_installed_content_path("/mnt/ext0/user/addcont/PPSA12345/x.pkg"));
  assert(!sm_pkg_is_installed_content_path("/user/data/game.pkg"));

  fih[0] = 0;
  write_file(path, fih, 0x200 + cnt_size);
  assert(stat(path, &st) == 0);
  assert(!sm_pkg_parse_file(path, &st, &candidate));

  unlink(path);
  puts("pkg parser tests passed");
  return 0;
}
