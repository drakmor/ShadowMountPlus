/*
 * Native debug-FPKG eligibility reader for ShadowMountPlus.
 *
 * Format behavior follows the GPL-3.0-or-later Kraken Mount reader and its
 * ps5-dump-forge format references. This probe validates the FIH/CNT envelope,
 * plaintext outer-PFS marker, required outer metadata entries and their bounded
 * block maps, and the Kraken layout descriptor. It does not parse the inner
 * tree validation and native Kraken block decoding are performed through the
 * opaque reader API declared in sm_fpkg.h.
 */
#include "sm_fpkg.h"
#include "sm_fpkg_internal.h"

#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>

#define SM_FPKG_FIH_SIZE 0x100u
#define SM_FPKG_CNT_HEADER_SIZE 0x1000u
#define SM_FPKG_OUTER_BLOCK_SIZE 0x10000u
#define SM_FPKG_SUPERBLOCK_SIZE 0x5A0u
#define SM_FPKG_PLAINTEXT_MARKER "PPRPLAIN-NOAUTH!"
#define SM_FPKG_SUPERBLOCK_MAGIC 20130315u
#define SM_FPKG_DINODE_SIZE 0x2C8u
#define SM_FPKG_DINODE_DIRECT_OFFSET 0x64u
#define SM_FPKG_DINODE_DIRECT_COUNT 12u
#define SM_FPKG_DINODE_INDIRECT_COUNT 5u
#define SM_FPKG_SIG_SIZE 36u
#define SM_FPKG_MAX_FILE_BLOCKS (4u * 1024u * 1024u)
#define SM_FPKG_MAX_LAYOUT_SIZE (384u * 1024u * 1024u)
#define SM_FPKG_CNT_ENTRY_SIZE 0x20u
#define SM_FPKG_MAX_CNT_ENTRIES 4096u

struct sm_fpkg {
  sm_pkg_read_fn read;
  void *read_ctx;
  int *owned_fd_ctx;
  uint64_t size;
  sm_pkg_info_t metadata;
  uint32_t *image_map;
  uint64_t image_map_count;
  uint64_t image_size;
  uint64_t pfs_offset;
  uint64_t outer_block_count;
  sm_fpkg_layout_t layout;
  sm_fpkg_image_reader_t *image;
  sm_fpkg_tree_t tree;
};

static int read_fd_at(void *ctx, uint64_t offset, void *buffer, size_t size) {
  int fd = *(int *)ctx;
  size_t done = 0;
  while (done < size) {
    ssize_t count = pread(fd, (uint8_t *)buffer + done, size - done,
                          (off_t)(offset + done));
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return count == 0 ? EIO : errno;
    done += (size_t)count;
  }
  return 0;
}

static uint32_t read_be32(const uint8_t *bytes) {
  return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) |
         ((uint32_t)bytes[2] << 8) | (uint32_t)bytes[3];
}

static uint32_t read_le32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
         ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static uint64_t read_le64(const uint8_t *bytes) {
  uint64_t value = 0;
  for (int i = 7; i >= 0; --i)
    value = (value << 8) | bytes[i];
  return value;
}

static bool range_fits(uint64_t offset, uint64_t length, uint64_t total) {
  return offset <= total && length <= total - offset;
}

static bool pkg_extension(const char *name) {
  if (!name)
    return false;
  const char *dot = strrchr(name, '.');
  return dot && strcasecmp(dot, ".pkg") == 0;
}

static sm_fpkg_status_t validate_cnt_table(sm_pkg_read_fn read, void *ctx,
                                           uint64_t pkg_size,
                                           uint64_t cnt_offset) {
  uint8_t header[SM_FPKG_CNT_HEADER_SIZE];
  if (read(ctx, cnt_offset, header, sizeof(header)) != 0)
    return SM_FPKG_IO;
  if (read_be32(header) != 0x7F434E54u)
    return SM_FPKG_CORRUPT;
  uint32_t count = read_be32(header + 0x10u);
  uint64_t table_relative = read_be32(header + 0x18u);
  if (count > SM_FPKG_MAX_CNT_ENTRIES)
    return SM_FPKG_CORRUPT;
  uint64_t table_size = (uint64_t)count * SM_FPKG_CNT_ENTRY_SIZE;
  if (table_relative > UINT64_MAX - cnt_offset)
    return SM_FPKG_CORRUPT;
  uint64_t table_offset = cnt_offset + table_relative;
  if (!range_fits(table_offset, table_size, pkg_size))
    return SM_FPKG_CORRUPT;
  if (table_size == 0)
    return SM_FPKG_OK;
  uint8_t *entries = malloc((size_t)table_size);
  if (!entries)
    return SM_FPKG_NOMEM;
  if (read(ctx, table_offset, entries, (size_t)table_size) != 0) {
    free(entries);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t *entry = entries + (size_t)i * SM_FPKG_CNT_ENTRY_SIZE;
    uint64_t relative = read_be32(entry + 16u);
    uint64_t payload_size = read_be32(entry + 20u);
    if (relative > UINT64_MAX - cnt_offset ||
        !range_fits(cnt_offset + relative, payload_size, pkg_size)) {
      status = SM_FPKG_CORRUPT;
      break;
    }
  }
  free(entries);
  return status;
}

static sm_fpkg_status_t read_outer_block(sm_pkg_read_fn read, void *ctx,
                                         uint64_t pfs_offset,
                                         uint64_t block_count, uint32_t block,
                                         uint8_t *buffer) {
  if ((uint64_t)block >= block_count)
    return SM_FPKG_CORRUPT;
  uint64_t offset = pfs_offset + (uint64_t)block * SM_FPKG_OUTER_BLOCK_SIZE;
  return read(ctx, offset, buffer, SM_FPKG_OUTER_BLOCK_SIZE) == 0
             ? SM_FPKG_OK
             : SM_FPKG_IO;
}

static bool dinode_block(const uint8_t *dinode, unsigned slot,
                         uint32_t *block_out) {
  if (slot >= SM_FPKG_DINODE_DIRECT_COUNT)
    return false;
  size_t at = SM_FPKG_DINODE_DIRECT_OFFSET + (size_t)slot * 36u + 32u;
  *block_out = read_le32(dinode + at);
  return true;
}

static int compare_u32(const void *left, const void *right) {
  uint32_t a = *(const uint32_t *)left;
  uint32_t b = *(const uint32_t *)right;
  return (a > b) - (a < b);
}

static sm_fpkg_status_t collect_indirect_blocks(
    sm_pkg_read_fn read, void *ctx, uint64_t pfs_offset, uint64_t block_count,
    uint32_t table_block, unsigned level, uint64_t needed, uint32_t *mapping,
    uint64_t *mapped, uint32_t *references, uint64_t *reference_count,
    uint64_t reference_capacity, uint8_t *tables) {
  if (level >= SM_FPKG_DINODE_INDIRECT_COUNT ||
      (uint64_t)table_block >= block_count ||
      *reference_count >= reference_capacity)
    return SM_FPKG_CORRUPT;
  references[(*reference_count)++] = table_block;
  uint8_t *table = tables + (size_t)level * SM_FPKG_OUTER_BLOCK_SIZE;
  sm_fpkg_status_t status = read_outer_block(
      read, ctx, pfs_offset, block_count, table_block, table);
  if (status != SM_FPKG_OK)
    return status;

  uint64_t slots = SM_FPKG_OUTER_BLOCK_SIZE / SM_FPKG_SIG_SIZE;
  for (uint64_t i = 0; i < slots && *mapped < needed; ++i) {
    uint32_t child = read_le32(table + (size_t)i * SM_FPKG_SIG_SIZE + 32u);
    if (child == 0 || (uint64_t)child >= block_count ||
        *reference_count >= reference_capacity)
      return SM_FPKG_CORRUPT;
    if (level == 0) {
      references[(*reference_count)++] = child;
      mapping[(*mapped)++] = child;
    } else {
      status = collect_indirect_blocks(
          read, ctx, pfs_offset, block_count, child, level - 1, needed,
          mapping, mapped, references, reference_count, reference_capacity,
          tables);
      if (status != SM_FPKG_OK)
        return status;
    }
  }
  return SM_FPKG_OK;
}

/* Validate complete data and indirect-table mappings without retaining them. */
static sm_fpkg_status_t build_file_map(
    sm_pkg_read_fn read, void *ctx, uint64_t pfs_offset, uint64_t block_count,
    const uint8_t *dinode, const uint32_t *reserved_blocks,
    size_t reserved_count, uint32_t **mapping_out, uint64_t *mapped_out,
    uint32_t **references_out, uint64_t *reference_count_out) {
  if (!mapping_out || !mapped_out || !references_out ||
      !reference_count_out)
    return SM_FPKG_CORRUPT;
  *mapping_out = NULL;
  *mapped_out = 0;
  *references_out = NULL;
  *reference_count_out = 0;
  uint64_t file_size = read_le64(dinode + 8u);
  uint64_t needed = file_size / SM_FPKG_OUTER_BLOCK_SIZE +
                    (file_size % SM_FPKG_OUTER_BLOCK_SIZE != 0);
  if (needed == 0 || needed > block_count ||
      needed > SM_FPKG_MAX_FILE_BLOCKS)
    return SM_FPKG_CORRUPT;

  uint32_t *mapping = malloc((size_t)needed * sizeof(*mapping));
  uint64_t reference_capacity = 2u * needed;
  uint32_t *references =
      malloc((size_t)reference_capacity * sizeof(*references));
  uint8_t *tables = malloc(SM_FPKG_DINODE_INDIRECT_COUNT *
                           SM_FPKG_OUTER_BLOCK_SIZE);
  if (!mapping || !references || !tables) {
    free(mapping);
    free(references);
    free(tables);
    return SM_FPKG_NOMEM;
  }

  uint64_t mapped = 0, reference_count = 0;
  for (unsigned i = 0; i < SM_FPKG_DINODE_DIRECT_COUNT && mapped < needed;
       ++i) {
    uint32_t block;
    (void)dinode_block(dinode, i, &block);
    if ((uint64_t)block >= block_count) {
      free(mapping);
      free(references);
      free(tables);
      return SM_FPKG_CORRUPT;
    }
    mapping[mapped++] = block;
    references[reference_count++] = block;
  }

  sm_fpkg_status_t status = SM_FPKG_OK;
  for (unsigned level = 0;
       level < SM_FPKG_DINODE_INDIRECT_COUNT && mapped < needed; ++level) {
    size_t at = SM_FPKG_DINODE_DIRECT_OFFSET +
                SM_FPKG_DINODE_DIRECT_COUNT * SM_FPKG_SIG_SIZE +
                (size_t)level * SM_FPKG_SIG_SIZE + 32u;
    uint32_t table_block = read_le32(dinode + at);
    if (table_block == 0)
      break;
    status = collect_indirect_blocks(
        read, ctx, pfs_offset, block_count, table_block, level, needed,
        mapping, &mapped, references, &reference_count, reference_capacity,
        tables);
    if (status != SM_FPKG_OK)
      break;
  }
  if (status == SM_FPKG_OK && mapped != needed)
    status = SM_FPKG_CORRUPT;
  if (status == SM_FPKG_OK) {
    qsort(references, (size_t)reference_count, sizeof(*references),
          compare_u32);
    for (uint64_t i = 1; i < reference_count; ++i) {
      if (references[i - 1] == references[i]) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
  }
  for (uint64_t i = 0; status == SM_FPKG_OK && i < reference_count; ++i) {
    for (size_t j = 0; j < reserved_count; ++j) {
      if (references[i] == reserved_blocks[j]) {
        status = SM_FPKG_CORRUPT;
        break;
      }
    }
  }
  free(tables);
  if (status == SM_FPKG_OK) {
    *mapping_out = mapping;
    *mapped_out = mapped;
    *references_out = references;
    *reference_count_out = reference_count;
  } else {
    free(mapping);
    free(references);
  }
  return status;
}

static bool reference_sets_overlap(const uint32_t *a, uint64_t a_count,
                                  const uint32_t *b, uint64_t b_count) {
  uint64_t i = 0, j = 0;
  while (i < a_count && j < b_count) {
    if (a[i] == b[j])
      return true;
    if (a[i] < b[j])
      ++i;
    else
      ++j;
  }
  return false;
}

static const char *cnt_fallback_path(uint32_t id) {
  switch (id) {
    case 0x1200u: return "sce_sys/icon0.png";
    case 0x1280u: return "sce_sys/icon0.dds";
    case 0x2000u: return "sce_sys/param.json";
    default: return NULL;
  }
}

static sm_fpkg_tree_entry_t *find_tree_path(sm_fpkg_tree_t *tree,
                                             const char *path) {
  for (size_t i = 0; i < tree->count; ++i) {
    if (strcmp(tree->entries[i].path, path) == 0)
      return &tree->entries[i];
  }
  return NULL;
}

static sm_fpkg_status_t append_tree_path(sm_fpkg_tree_t *tree,
                                          const char *path, uint64_t size,
                                          uint64_t package_offset,
                                          bool is_dir, bool from_package) {
  if (tree->count >= 1000000u ||
      tree->count == SIZE_MAX / sizeof(*tree->entries))
    return SM_FPKG_UNSUPPORTED;
  void *grown = realloc(tree->entries,
                        (tree->count + 1u) * sizeof(*tree->entries));
  if (!grown)
    return SM_FPKG_NOMEM;
  tree->entries = grown;
  char *copy = strdup(path);
  if (!copy)
    return SM_FPKG_NOMEM;
  tree->entries[tree->count++] = (sm_fpkg_tree_entry_t){
      .path = copy,
      .size = size,
      .package_offset = package_offset,
      .is_dir = is_dir,
      .from_package = from_package,
  };
  return SM_FPKG_OK;
}

static sm_fpkg_status_t append_cnt_fallbacks(sm_pkg_read_fn read, void *ctx,
                                              uint64_t package_size,
                                              uint64_t cnt_offset,
                                              sm_fpkg_tree_t *tree) {
  uint8_t header[SM_FPKG_CNT_HEADER_SIZE];
  if (read(ctx, cnt_offset, header, sizeof(header)) != 0)
    return SM_FPKG_IO;
  uint32_t count = read_be32(header + 0x10u);
  uint64_t table_offset = cnt_offset + read_be32(header + 0x18u);
  size_t table_size = (size_t)count * SM_FPKG_CNT_ENTRY_SIZE;
  if (count == 0)
    return SM_FPKG_OK;
  uint8_t *entries = malloc(table_size);
  if (!entries)
    return SM_FPKG_NOMEM;
  if (read(ctx, table_offset, entries, table_size) != 0) {
    free(entries);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t status = SM_FPKG_OK;
  for (uint32_t i = 0; i < count && status == SM_FPKG_OK; ++i) {
    const uint8_t *row = entries + (size_t)i * SM_FPKG_CNT_ENTRY_SIZE;
    const char *path = cnt_fallback_path(read_be32(row));
    uint32_t flags = read_be32(row + 8u);
    uint64_t offset = cnt_offset + read_be32(row + 16u);
    uint64_t size = read_be32(row + 20u);
    if (!path || (flags & 0x80000000u) != 0 || size == 0)
      continue;
    if (!range_fits(offset, size, package_size)) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    if (find_tree_path(tree, path))
      continue;
    sm_fpkg_tree_entry_t *directory = find_tree_path(tree, "sce_sys");
    if (!directory) {
      status = append_tree_path(tree, "sce_sys", 0, 0, true, false);
      if (status != SM_FPKG_OK)
        break;
    } else if (!directory->is_dir) {
      status = SM_FPKG_CORRUPT;
      break;
    }
    status = append_tree_path(tree, path, size, offset, false, true);
  }
  free(entries);
  return status;
}

static sm_fpkg_status_t read_outer_file(
    sm_pkg_read_fn read, void *ctx, uint64_t pfs_offset, uint64_t block_count,
    const uint32_t *mapping, uint64_t mapped, uint64_t file_size,
    uint8_t *output, uint8_t *scratch) {
  if (!mapping || !output || !scratch)
    return SM_FPKG_CORRUPT;
  uint64_t remaining = file_size;
  size_t at = 0;
  for (uint64_t i = 0; remaining != 0 && i < mapped; ++i) {
    sm_fpkg_status_t status = read_outer_block(
        read, ctx, pfs_offset, block_count, mapping[i], scratch);
    if (status != SM_FPKG_OK)
      return status;
    size_t take = remaining > SM_FPKG_OUTER_BLOCK_SIZE
                      ? SM_FPKG_OUTER_BLOCK_SIZE
                      : (size_t)remaining;
    memcpy(output + at, scratch, take);
    at += take;
    remaining -= take;
  }
  return remaining == 0 ? SM_FPKG_OK : SM_FPKG_CORRUPT;
}

static sm_fpkg_status_t image_mount_read(void *ctx, uint64_t offset,
                                         void *output, size_t size) {
  return sm_fpkg_image_read(ctx, offset, output, size);
}

static sm_fpkg_status_t outer_child(
    sm_pkg_read_fn read, void *ctx, uint64_t pfs_offset, uint64_t block_count,
    const uint8_t *dinodes, uint64_t dinode_count, uint32_t directory_inode,
    const char *wanted, uint32_t *inode_out, uint32_t *data_block_out,
    uint8_t *block) {
  if ((uint64_t)directory_inode >= dinode_count)
    return SM_FPKG_CORRUPT;
  const uint8_t *directory = dinodes + (size_t)directory_inode * SM_FPKG_DINODE_SIZE;
  uint64_t directory_size = read_le64(directory + 8);
  uint32_t data_block;
  if (directory_size > SM_FPKG_OUTER_BLOCK_SIZE ||
      !dinode_block(directory, 0, &data_block))
    return SM_FPKG_CORRUPT;
  sm_fpkg_status_t status = read_outer_block(
      read, ctx, pfs_offset, block_count, data_block, block);
  if (status != SM_FPKG_OK)
    return status;
  if (data_block_out)
    *data_block_out = data_block;

  size_t limit = (size_t)directory_size;
  size_t at = 0;
  size_t wanted_length = strlen(wanted);
  while (at < limit) {
    if (limit - at < 16u)
      return SM_FPKG_CORRUPT;
    uint32_t inode = read_le32(block + at);
    uint32_t name_length = read_le32(block + at + 8u);
    uint32_t entry_size = read_le32(block + at + 12u);
    if (entry_size == 0)
      return SM_FPKG_CORRUPT;
    if (name_length > limit - at - 16u ||
        entry_size < 16u + (uint64_t)name_length ||
        entry_size > limit - at || (uint64_t)inode >= dinode_count)
      return SM_FPKG_CORRUPT;
    if (name_length == wanted_length &&
        memcmp(block + at + 16u, wanted, wanted_length) == 0) {
      *inode_out = inode;
      return SM_FPKG_OK;
    }
    at += entry_size;
  }
  return SM_FPKG_CORRUPT;
}

/*
 * Validate the first outer PFS inode table, required metadata entries and
 * complete direct/indirect mappings for those files, then retain the validated
 * inner tree and reader state when an opaque handle is requested.
 */
static sm_fpkg_status_t validate_outer_metadata(
    sm_pkg_read_fn read, void *ctx, uint64_t pfs_offset, uint64_t block_count,
    uint32_t superblock_index, uint64_t inner_meta_base,
    uint64_t package_size, uint64_t cnt_offset,
    const uint8_t *superblock, sm_fpkg_t *fpkg) {
  uint64_t inode_count = read_le64(superblock + 0x30u);
  uint32_t table_block = read_le32(superblock + 0xD8u);
  uint64_t inode_capacity = SM_FPKG_OUTER_BLOCK_SIZE / SM_FPKG_DINODE_SIZE;
  uint32_t uroot_inode, image_inode, layout_inode;
  if (inode_count < 4u || inode_count > inode_capacity ||
      (uint64_t)table_block >= block_count || table_block == superblock_index)
    return SM_FPKG_CORRUPT;
  uint8_t *workspace = malloc(2u * SM_FPKG_OUTER_BLOCK_SIZE);
  if (!workspace)
    return SM_FPKG_NOMEM;
  uint8_t *inode_table = workspace;
  uint8_t *io_block = workspace + SM_FPKG_OUTER_BLOCK_SIZE;
  uint8_t *descriptor = NULL;
  uint32_t *image_map = NULL, *layout_map = NULL;
  uint32_t *image_refs = NULL, *layout_refs = NULL;
  uint64_t image_mapped = 0, layout_mapped = 0;
  uint64_t image_ref_count = 0, layout_ref_count = 0;
  sm_fpkg_layout_t parsed_layout = {0};
  sm_fpkg_image_reader_t *image = NULL;
  sm_fpkg_tree_t tree = {0};
  sm_fpkg_status_t status = read_outer_block(
      read, ctx, pfs_offset, block_count, table_block, inode_table);
  if (status != SM_FPKG_OK)
    goto done;
  uint32_t root_dir_block, uroot_dir_block;
  status = outer_child(read, ctx, pfs_offset, block_count, inode_table,
                       inode_count, 0, "uroot", &uroot_inode,
                       &root_dir_block, io_block);
  if (status != SM_FPKG_OK)
    goto done;
  status = outer_child(read, ctx, pfs_offset, block_count, inode_table,
                       inode_count, uroot_inode, "pfs_image.dat", &image_inode,
                       &uroot_dir_block, io_block);
  if (status != SM_FPKG_OK)
    goto done;
  status = outer_child(read, ctx, pfs_offset, block_count, inode_table,
                       inode_count, uroot_inode, "naps_pkg_layout.dat",
                       &layout_inode, NULL, io_block);
  if (status != SM_FPKG_OK || image_inode == layout_inode ||
      root_dir_block == uroot_dir_block ||
      root_dir_block == superblock_index || uroot_dir_block == superblock_index ||
      root_dir_block == table_block || uroot_dir_block == table_block)
  {
    if (status == SM_FPKG_OK)
      status = SM_FPKG_CORRUPT;
    goto done;
  }
  const uint8_t *files[] = {
      inode_table + (size_t)image_inode * SM_FPKG_DINODE_SIZE,
      inode_table + (size_t)layout_inode * SM_FPKG_DINODE_SIZE};
  uint32_t reserved_blocks[] = {superblock_index, table_block, root_dir_block,
                                uroot_dir_block};
  uint64_t image_size = read_le64(files[0] + 8u);
  uint64_t layout_size = read_le64(files[1] + 8u);
  if (layout_size == 0 || layout_size > SM_FPKG_MAX_LAYOUT_SIZE ||
      layout_size > SIZE_MAX) {
    status = SM_FPKG_UNSUPPORTED;
    goto done;
  }
  status = build_file_map(read, ctx, pfs_offset, block_count, files[0],
                          reserved_blocks, 4, &image_map, &image_mapped,
                          &image_refs, &image_ref_count);
  if (status != SM_FPKG_OK)
    goto done;
  status = build_file_map(read, ctx, pfs_offset, block_count, files[1],
                          reserved_blocks, 4, &layout_map, &layout_mapped,
                          &layout_refs, &layout_ref_count);
  if (status != SM_FPKG_OK)
    goto done;
  if (reference_sets_overlap(image_refs, image_ref_count, layout_refs,
                             layout_ref_count)) {
    status = SM_FPKG_CORRUPT;
    goto done;
  }
  descriptor = malloc((size_t)layout_size);
  if (!descriptor) {
    status = SM_FPKG_NOMEM;
    goto done;
  }
  status = read_outer_file(read, ctx, pfs_offset, block_count, layout_map,
                           layout_mapped, layout_size, descriptor, io_block);
  if (status != SM_FPKG_OK)
    goto done;
  status = sm_fpkg_layout_parse(descriptor, (size_t)layout_size, image_size,
                                &parsed_layout);
  if (status == SM_FPKG_OK) {
    image = sm_fpkg_image_reader_create(
        read, ctx, pfs_offset, block_count, image_map, image_mapped,
        image_size, &parsed_layout);
    if (!image) {
      status = SM_FPKG_NOMEM;
    } else {
      status = sm_fpkg_tree_open(
          image_mount_read, image,
          parsed_layout.mount_size, inner_meta_base, &tree);
      if (status == SM_FPKG_OK)
        status = append_cnt_fallbacks(read, ctx, package_size, cnt_offset,
                                      &tree);
    }
  }
  if (status == SM_FPKG_OK && fpkg) {
    sm_fpkg_image_reader_free(image);
    image = NULL;
    fpkg->image_map = image_map;
    fpkg->image_map_count = image_mapped;
    fpkg->image_size = image_size;
    fpkg->pfs_offset = pfs_offset;
    fpkg->outer_block_count = block_count;
    fpkg->layout = parsed_layout;
    fpkg->tree = tree;
    image_map = NULL;
    memset(&parsed_layout, 0, sizeof(parsed_layout));
    memset(&tree, 0, sizeof(tree));
    fpkg->image = sm_fpkg_image_reader_create(
        read, ctx, pfs_offset, block_count, fpkg->image_map,
        fpkg->image_map_count, image_size, &fpkg->layout);
    if (!fpkg->image)
      status = SM_FPKG_NOMEM;
  }
done:
  sm_fpkg_tree_free(&tree);
  sm_fpkg_image_reader_free(image);
  sm_fpkg_layout_free(&parsed_layout);
  free(descriptor);
  free(image_map);
  free(layout_map);
  free(image_refs);
  free(layout_refs);
  free(workspace);
  return status;
}

sm_fpkg_status_t sm_fpkg_open_stream(const char *name, uint64_t size,
                                     sm_pkg_read_fn read, void *ctx,
                                     const sm_pkg_info_t *metadata,
                                     sm_fpkg_t **fpkg_out) {
  if (!fpkg_out)
    return SM_FPKG_CORRUPT;
  *fpkg_out = NULL;
  if (!name || !read || !metadata || size < SM_FPKG_FIH_SIZE ||
      !pkg_extension(name))
    return SM_FPKG_CORRUPT;
  if (metadata->title_id[0] == '\0' ||
      metadata->pkg_kind != PKG_KIND_BASE ||
      !metadata->pkg_classification_confident)
    return SM_FPKG_UNSUPPORTED;

  sm_fpkg_t *fpkg = calloc(1, sizeof(*fpkg));
  if (!fpkg)
    return SM_FPKG_NOMEM;
  fpkg->read = read;
  fpkg->read_ctx = ctx;
  fpkg->size = size;
  fpkg->metadata = *metadata;

  uint8_t fih[SM_FPKG_FIH_SIZE];
  int result = read(ctx, 0, fih, sizeof(fih));
  if (result != 0) {
    sm_fpkg_close(fpkg);
    return SM_FPKG_IO;
  }
  sm_fpkg_status_t early = SM_FPKG_OK;
  if (read_be32(fih) == 0x7F434E54u) early = SM_FPKG_NOT_PS5;
  else if (read_be32(fih) == 0x7F4C4948u) early = SM_FPKG_UNSUPPORTED;
  else if (read_be32(fih) != 0x7F464948u) early = SM_FPKG_NOT_PS5;
  else if (fih[5] != 0 || fih[6] != 3 || fih[7] != 0)
    early = SM_FPKG_UNSUPPORTED;
  else if (read_le32(fih + 0x60) != SM_FPKG_OUTER_BLOCK_SIZE)
    early = SM_FPKG_CORRUPT;
  if (early != SM_FPKG_OK) {
    sm_fpkg_close(fpkg);
    return early;
  }

  uint64_t pfs_offset = read_le64(fih + 0x10);
  uint64_t pfs_size = read_le64(fih + 0x18);
  uint64_t superblock_offset = read_le64(fih + 0x20);
  uint64_t cnt_offset = read_le64(fih + 0x58);
  if (pfs_offset < SM_FPKG_FIH_SIZE ||
      (pfs_offset % SM_FPKG_OUTER_BLOCK_SIZE) != 0 || pfs_size == 0 ||
      (pfs_size % SM_FPKG_OUTER_BLOCK_SIZE) != 0 ||
      !range_fits(pfs_offset, pfs_size, size) ||
      !range_fits(cnt_offset, SM_FPKG_CNT_HEADER_SIZE, size))
    { sm_fpkg_close(fpkg); return SM_FPKG_CORRUPT; }

  if (superblock_offset < pfs_offset ||
      ((superblock_offset - pfs_offset) % SM_FPKG_OUTER_BLOCK_SIZE) != 0 ||
      !range_fits(superblock_offset, SM_FPKG_SUPERBLOCK_SIZE,
                  pfs_offset + pfs_size))
    { sm_fpkg_close(fpkg); return SM_FPKG_CORRUPT; }

  sm_fpkg_status_t cnt_status =
      validate_cnt_table(read, ctx, size, cnt_offset);
  if (cnt_status != SM_FPKG_OK) {
    sm_fpkg_close(fpkg);
    return cnt_status;
  }

  uint8_t superblock[SM_FPKG_SUPERBLOCK_SIZE];
  result = read(ctx, superblock_offset, superblock, sizeof(superblock));
  if (result != 0) {
    sm_fpkg_close(fpkg);
    return SM_FPKG_IO;
  }
  if (read_le64(superblock) != 2 ||
      read_le64(superblock + 8) != SM_FPKG_SUPERBLOCK_MAGIC) {
    sm_fpkg_close(fpkg);
    return SM_FPKG_CORRUPT;
  }
  if (memcmp(superblock + 0x370, SM_FPKG_PLAINTEXT_MARKER, 16) != 0) {
    sm_fpkg_close(fpkg);
    return SM_FPKG_UNSUPPORTED;
  }

  uint64_t block_count = pfs_size / SM_FPKG_OUTER_BLOCK_SIZE;
  uint64_t superblock_index = (superblock_offset - pfs_offset) /
                              SM_FPKG_OUTER_BLOCK_SIZE;
  if (block_count > UINT32_MAX || superblock_index > UINT32_MAX) {
    sm_fpkg_close(fpkg);
    return SM_FPKG_CORRUPT;
  }
  sm_fpkg_status_t outer_status = validate_outer_metadata(
      read, ctx, pfs_offset, block_count, (uint32_t)superblock_index,
      (uint64_t)read_le32(fih + 0x50u) * SM_FPKG_OUTER_BLOCK_SIZE,
      size, cnt_offset, superblock, fpkg);
  if (outer_status != SM_FPKG_OK) {
    sm_fpkg_close(fpkg);
    return outer_status;
  }

  *fpkg_out = fpkg;
  return SM_FPKG_OK;
}

sm_fpkg_status_t sm_fpkg_open_fd(const char *name, int fd,
                                 const sm_pkg_info_t *metadata,
                                 sm_fpkg_t **fpkg_out) {
  if (!fpkg_out)
    return SM_FPKG_CORRUPT;
  *fpkg_out = NULL;
  if (!name || fd < 0 || !metadata)
    return SM_FPKG_CORRUPT;
  int flags = fcntl(fd, F_GETFL);
  if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY)
    return SM_FPKG_UNSUPPORTED;
  struct stat st;
  if (fstat(fd, &st) != 0)
    return SM_FPKG_IO;
  if (!S_ISREG(st.st_mode) || st.st_size <= 0 ||
      !sm_pkg_source_matches(metadata, &st))
    return SM_FPKG_UNSUPPORTED;
  int *fd_ctx = malloc(sizeof(*fd_ctx));
  if (!fd_ctx)
    return SM_FPKG_NOMEM;
  *fd_ctx = fd;
  sm_fpkg_status_t status = sm_fpkg_open_stream(
      name, (uint64_t)st.st_size, read_fd_at, fd_ctx, metadata, fpkg_out);
  if (status == SM_FPKG_OK)
    (*fpkg_out)->owned_fd_ctx = fd_ctx;
  else
    free(fd_ctx);
  return status;
}

int sm_fpkg_source_fd(const sm_fpkg_t *fpkg) {
  return fpkg && fpkg->owned_fd_ctx ? *fpkg->owned_fd_ctx : -1;
}

void sm_fpkg_close(sm_fpkg_t *fpkg) {
  if (!fpkg) return;
  sm_fpkg_tree_free(&fpkg->tree);
  sm_fpkg_image_reader_free(fpkg->image);
  sm_fpkg_layout_free(&fpkg->layout);
  free(fpkg->image_map);
  free(fpkg->owned_fd_ctx);
  free(fpkg);
}

uint64_t sm_fpkg_package_size(const sm_fpkg_t *fpkg) {
  return fpkg ? fpkg->size : 0;
}

size_t sm_fpkg_file_count(const sm_fpkg_t *fpkg) {
  return fpkg ? fpkg->tree.count : 0;
}

bool sm_fpkg_file_at(const sm_fpkg_t *fpkg, size_t index,
                     sm_fpkg_file_t *file_out) {
  if (!fpkg || !file_out || index >= fpkg->tree.count) return false;
  const sm_fpkg_tree_entry_t *entry = &fpkg->tree.entries[index];
  file_out->path = entry->path;
  file_out->size = entry->size;
  file_out->is_directory = entry->is_dir;
  return true;
}

static size_t find_layout_block(const sm_fpkg_layout_t *layout,
                                uint64_t logical_offset) {
  size_t low = 0, high = layout->block_count;
  while (low < high) {
    size_t middle = low + (high - low) / 2u;
    if (layout->blocks[middle].logical_offset < logical_offset)
      low = middle + 1u;
    else
      high = middle;
  }
  return low;
}

static sm_fpkg_status_t stored_range_to_package(const sm_fpkg_t *fpkg,
                                                uint64_t stored_offset,
                                                uint32_t stored_size,
                                                uint64_t *package_offset_out) {
  if (!package_offset_out || stored_size == 0 ||
      !range_fits(stored_offset, stored_size, fpkg->image_size))
    return SM_FPKG_CORRUPT;
  uint64_t done = 0, first = 0;
  while (done < stored_size) {
    uint64_t at = stored_offset + done;
    uint64_t map_index = at / SM_FPKG_OUTER_BLOCK_SIZE;
    uint64_t within = at % SM_FPKG_OUTER_BLOCK_SIZE;
    if (map_index >= fpkg->image_map_count)
      return SM_FPKG_CORRUPT;
    uint32_t outer_block = fpkg->image_map[map_index];
    if ((uint64_t)outer_block >= fpkg->outer_block_count)
      return SM_FPKG_CORRUPT;
    uint64_t absolute = fpkg->pfs_offset +
                        (uint64_t)outer_block * SM_FPKG_OUTER_BLOCK_SIZE +
                        within;
    uint64_t amount = SM_FPKG_OUTER_BLOCK_SIZE - within;
    if (amount > stored_size - done)
      amount = stored_size - done;
    if (done == 0)
      first = absolute;
    else if (first > UINT64_MAX - done || first + done != absolute)
      return SM_FPKG_UNSUPPORTED;
    done += amount;
  }
  if (!range_fits(first, stored_size, fpkg->size))
    return SM_FPKG_CORRUPT;
  *package_offset_out = first;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t collect_file_blocks(const sm_fpkg_t *fpkg,
                                            size_t index,
                                            sm_fpkg_block_t *blocks,
                                            size_t block_capacity,
                                            size_t *count_out) {
  if (!fpkg || !count_out || index >= fpkg->tree.count)
    return SM_FPKG_CORRUPT;
  *count_out = 0;
  const sm_fpkg_tree_entry_t *entry = &fpkg->tree.entries[index];
  if (entry->is_dir)
    return SM_FPKG_CORRUPT;
  if (entry->size == 0)
    return SM_FPKG_OK;

  if (entry->from_package) {
    uint64_t count = entry->size / 0x40000u +
                     (entry->size % 0x40000u != 0);
    if (count > SIZE_MAX)
      return SM_FPKG_UNSUPPORTED;
    *count_out = (size_t)count;
    if (!blocks)
      return block_capacity == 0 ? SM_FPKG_OK : SM_FPKG_CORRUPT;
    if (block_capacity < *count_out)
      return SM_FPKG_UNSUPPORTED;
    uint64_t done = 0;
    for (size_t i = 0; i < *count_out; ++i) {
      uint64_t remaining = entry->size - done;
      uint32_t raw_size = remaining > 0x40000u ? 0x40000u :
                                                    (uint32_t)remaining;
      if (entry->package_offset > UINT64_MAX - done ||
          !range_fits(entry->package_offset + done, raw_size, fpkg->size))
        return SM_FPKG_CORRUPT;
      blocks[i] = (sm_fpkg_block_t){
          .package_offset = entry->package_offset + done,
          .stored_size = raw_size,
          .raw_size = raw_size,
          .even_size = raw_size < 0x20000u ? raw_size : 0x20000u,
          .kraken_encoded = false,
      };
      done += raw_size;
    }
    return SM_FPKG_OK;
  }

  size_t layout_index = find_layout_block(&fpkg->layout,
                                          entry->logical_offset);
  if (layout_index >= fpkg->layout.block_count ||
      fpkg->layout.blocks[layout_index].logical_offset !=
          entry->logical_offset)
    return SM_FPKG_UNSUPPORTED;
  size_t count = 0;
  uint64_t produced = 0;
  size_t current = layout_index;
  while (produced < entry->size) {
    if (current >= fpkg->layout.block_count ||
        count >= fpkg->layout.block_count)
      return SM_FPKG_CORRUPT;
    const sm_fpkg_layout_block_t *block = &fpkg->layout.blocks[current];
    if (block->logical_offset != entry->logical_offset + produced ||
        block->logical_length == 0)
      return SM_FPKG_CORRUPT;
    uint64_t wanted = entry->size - produced;
    if (wanted > block->logical_length)
      wanted = block->logical_length;
    uint64_t package_offset;
    sm_fpkg_status_t status = stored_range_to_package(
        fpkg, block->stored_offset, block->stored_length, &package_offset);
    if (status != SM_FPKG_OK)
      return status;
    if (blocks) {
      if (count >= block_capacity)
        return SM_FPKG_UNSUPPORTED;
      blocks[count] = (sm_fpkg_block_t){
          .package_offset = package_offset,
          .stored_size = block->stored_length,
          .raw_size = block->logical_length,
          .even_size = block->even_length,
          .mode = block->mode,
          .kraken_encoded = !block->raw,
      };
    }
    ++count;
    produced += wanted;
    ++current;
  }
  *count_out = count;
  return blocks || block_capacity == 0 ? SM_FPKG_OK : SM_FPKG_CORRUPT;
}

sm_fpkg_status_t sm_fpkg_file_block_count(const sm_fpkg_t *fpkg,
                                           size_t index,
                                           size_t *count_out) {
  return collect_file_blocks(fpkg, index, NULL, 0, count_out);
}

sm_fpkg_status_t sm_fpkg_file_blocks(const sm_fpkg_t *fpkg, size_t index,
                                      sm_fpkg_block_t *blocks,
                                      size_t block_capacity,
                                      size_t *count_out) {
  if (!count_out || (!blocks && block_capacity != 0))
    return SM_FPKG_CORRUPT;
  size_t needed = 0;
  sm_fpkg_status_t status =
      collect_file_blocks(fpkg, index, NULL, 0, &needed);
  if (status != SM_FPKG_OK)
    return status;
  *count_out = needed;
  if (needed == 0)
    return SM_FPKG_OK;
  if (!blocks || block_capacity < needed)
    return SM_FPKG_UNSUPPORTED;
  return collect_file_blocks(fpkg, index, blocks, block_capacity, count_out);
}

sm_fpkg_status_t sm_fpkg_extract_file(sm_fpkg_t *fpkg, size_t index,
                                      const sm_fpkg_extract_sink_t *sink) {
  if (!fpkg || !sink || !sink->begin || !sink->write || !sink->finish ||
      index >= fpkg->tree.count)
    return SM_FPKG_CORRUPT;
  const sm_fpkg_tree_entry_t *entry = &fpkg->tree.entries[index];
  if (entry->is_dir)
    return SM_FPKG_UNSUPPORTED;
  uint8_t *buffer = malloc(64u * 1024u);
  if (!buffer)
    return SM_FPKG_NOMEM;
  sm_fpkg_file_t file = {
      .path = entry->path,
      .size = entry->size,
      .is_directory = false,
  };
  void *file_ctx = NULL;
  sm_fpkg_status_t status = sink->begin(sink->ctx, &file, &file_ctx);
  if (status != SM_FPKG_OK) {
    free(buffer);
    return status;
  }
  uint64_t offset = 0;
  while (offset < entry->size) {
    size_t amount = entry->size - offset > 64u * 1024u
                        ? 64u * 1024u
                        : (size_t)(entry->size - offset);
    status = sm_fpkg_read_file(fpkg, index, offset, buffer, amount);
    if (status != SM_FPKG_OK)
      break;
    status = sink->write(file_ctx, offset, buffer, amount);
    if (status != SM_FPKG_OK)
      break;
    offset += amount;
  }
  sm_fpkg_status_t finish_status = sink->finish(file_ctx,
                                                 status == SM_FPKG_OK);
  free(buffer);
  return status == SM_FPKG_OK ? finish_status : status;
}

sm_fpkg_status_t sm_fpkg_read_file(sm_fpkg_t *fpkg, size_t index,
                                   uint64_t offset, void *output,
                                   size_t size) {
  if (!fpkg || index >= fpkg->tree.count || (size && !output))
    return SM_FPKG_CORRUPT;
  const sm_fpkg_tree_entry_t *entry = &fpkg->tree.entries[index];
  if (entry->is_dir || !range_fits(offset, size, entry->size))
    return SM_FPKG_CORRUPT;
  if (!size) return SM_FPKG_OK;
  if (entry->from_package) {
    if (entry->package_offset > UINT64_MAX - offset ||
        !range_fits(entry->package_offset + offset, size, fpkg->size))
      return SM_FPKG_CORRUPT;
    return fpkg->read(fpkg->read_ctx, entry->package_offset + offset, output,
                      size) == 0
               ? SM_FPKG_OK
               : SM_FPKG_IO;
  }
  if (entry->logical_offset > UINT64_MAX - offset)
    return SM_FPKG_CORRUPT;
  return sm_fpkg_image_read(fpkg->image, entry->logical_offset + offset,
                            output, size);
}

sm_fpkg_status_t sm_fpkg_probe_stream(const char *name, uint64_t size,
                                      sm_pkg_read_fn read, void *ctx,
                                      const sm_pkg_info_t *metadata) {
  sm_fpkg_t *fpkg = NULL;
  sm_fpkg_status_t status = sm_fpkg_open_stream(name, size, read, ctx,
                                                metadata, &fpkg);
  sm_fpkg_close(fpkg);
  return status;
}
