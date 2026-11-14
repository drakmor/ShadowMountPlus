/* Bounded validator for the inner PFS inode table and directory tree. */
#include "sm_fpkg_internal.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* The superblock's PFS block size positions the inode table. The table itself
 * is paged in 64 KiB units, with 390 0xa8-byte inode records per page. */
#define TREE_TABLE_PAGE 0x10000u
#define TREE_INODE_SIZE 0xa8u
#define TREE_INODES_PER_BLOCK 390u
#define TREE_FIRST_DIR_INODE 4u
#define TREE_MAX_INODES 4000000u
#define TREE_MAX_ENTRIES 1000000u
#define TREE_MAX_PATH_BYTES (256u * 1024u * 1024u)
#define TREE_MAX_COMPONENT 255u
#define TREE_MAX_PATH 4096u
#define TREE_MAX_DEPTH 1024u
#define TREE_MAX_DIR_BYTES (64u * 1024u * 1024u)
#define TREE_SUPERBLOCK_MAGIC 20130315u

typedef struct {
  uint16_t mode;
  uint64_t size;
  uint64_t logical;
} tree_inode_t;

typedef struct {
  uint32_t inode;
  char *path;
  size_t depth;
} pending_dir_t;

typedef struct {
  pending_dir_t *items;
  size_t count;
  size_t capacity;
} pending_stack_t;

static uint16_t read_le16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t read_le32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t read_le64(const uint8_t *p) {
  uint64_t value = 0;
  for (unsigned i = 0; i < 8; ++i)
    value |= (uint64_t)p[i] << (i * 8u);
  return value;
}

static bool range_fits(uint64_t offset, uint64_t length, uint64_t total) {
  return offset <= total && length <= total - offset;
}

static bool add_u64(uint64_t left, uint64_t right, uint64_t *sum) {
  if (right > UINT64_MAX - left) return false;
  *sum = left + right;
  return true;
}

static sm_fpkg_status_t read_inode(sm_fpkg_mount_read_fn read, void *ctx,
                                   uint64_t mount_size, uint64_t table_at,
                                   uint64_t inode_count, uint32_t inode_number,
                                   tree_inode_t *inode) {
  if ((uint64_t)inode_number >= inode_count) return SM_FPKG_CORRUPT;
  uint64_t number = inode_number;
  uint64_t block_index = number / TREE_INODES_PER_BLOCK;
  uint64_t within = (number % TREE_INODES_PER_BLOCK) * TREE_INODE_SIZE;
  uint64_t inode_offset = block_index * TREE_TABLE_PAGE + within;
  uint64_t offset;
  if (!add_u64(table_at, inode_offset, &offset) ||
      !range_fits(offset, TREE_INODE_SIZE, mount_size))
    return SM_FPKG_CORRUPT;
  uint8_t raw[TREE_INODE_SIZE];
  sm_fpkg_status_t status = read(ctx, offset, raw, sizeof(raw));
  if (status != SM_FPKG_OK) return status;
  inode->mode = read_le16(raw);
  inode->size = read_le64(raw + 8u);
  inode->logical = read_le64(raw + 0x60u);
  return SM_FPKG_OK;
}

static bool valid_utf8_component(const uint8_t *text, size_t length) {
  if (length == 0 || length > TREE_MAX_COMPONENT) return false;
  if ((length == 1 && text[0] == '.') ||
      (length == 2 && text[0] == '.' && text[1] == '.'))
    return false;
  for (size_t i = 0; i < length;) {
    uint8_t first = text[i];
    if (first == '/' || first == '\\' || first == 0) return false;
    if (first < 0x80) {
      ++i;
      continue;
    }
    unsigned count;
    uint32_t codepoint;
    if (first >= 0xc2 && first <= 0xdf) {
      count = 2;
      codepoint = first & 0x1fu;
    } else if (first >= 0xe0 && first <= 0xef) {
      count = 3;
      codepoint = first & 0x0fu;
    } else if (first >= 0xf0 && first <= 0xf4) {
      count = 4;
      codepoint = first & 0x07u;
    } else {
      return false;
    }
    if (count > length - i) return false;
    for (unsigned j = 1; j < count; ++j) {
      uint8_t next = text[i + j];
      if ((next & 0xc0u) != 0x80u) return false;
      codepoint = (codepoint << 6) | (next & 0x3fu);
    }
    if ((count == 3 && codepoint < 0x800u) ||
        (count == 4 && codepoint < 0x10000u) ||
        (codepoint >= 0xd800u && codepoint <= 0xdfffu) ||
        codepoint > 0x10ffffu)
      return false;
    i += count;
  }
  return true;
}

static sm_fpkg_status_t push_dir(pending_stack_t *stack, uint32_t inode,
                                 char *path, size_t depth) {
  if (stack->count == stack->capacity) {
    size_t next = stack->capacity == 0 ? 32u : stack->capacity * 2u;
    if (next > TREE_MAX_ENTRIES || next <= stack->capacity ||
        next > SIZE_MAX / sizeof(*stack->items))
      return SM_FPKG_UNSUPPORTED;
    void *grown = realloc(stack->items, next * sizeof(*stack->items));
    if (!grown) return SM_FPKG_NOMEM;
    stack->items = grown;
    stack->capacity = next;
  }
  stack->items[stack->count++] =
      (pending_dir_t){.inode = inode, .path = path, .depth = depth};
  return SM_FPKG_OK;
}

static sm_fpkg_status_t append_entry(sm_fpkg_tree_t *tree,
                                     size_t *capacity, const char *path,
                                     uint64_t size, uint64_t logical,
                                     bool is_dir) {
  if (tree->count >= TREE_MAX_ENTRIES)
    return SM_FPKG_UNSUPPORTED;
  if (tree->count == *capacity) {
    size_t next = *capacity == 0 ? 64u : *capacity * 2u;
    if (next > TREE_MAX_ENTRIES || next <= *capacity ||
        next > SIZE_MAX / sizeof(*tree->entries))
      return SM_FPKG_UNSUPPORTED;
    void *grown = realloc(tree->entries, next * sizeof(*tree->entries));
    if (!grown) return SM_FPKG_NOMEM;
    tree->entries = grown;
    *capacity = next;
  }
  char *copy = strdup(path);
  if (!copy) return SM_FPKG_NOMEM;
  tree->entries[tree->count++] = (sm_fpkg_tree_entry_t){
      .path = copy,
      .size = size,
      .logical_offset = logical,
      .is_dir = is_dir,
  };
  return SM_FPKG_OK;
}

void sm_fpkg_tree_free(sm_fpkg_tree_t *tree) {
  if (!tree) return;
  for (size_t i = 0; i < tree->count; ++i)
    free(tree->entries[i].path);
  free(tree->entries);
  memset(tree, 0, sizeof(*tree));
}

sm_fpkg_status_t sm_fpkg_tree_open(sm_fpkg_mount_read_fn read, void *read_ctx,
                                   uint64_t mount_size, uint64_t meta_base,
                                   sm_fpkg_tree_t *tree_out) {
  if (!tree_out) return SM_FPKG_CORRUPT;
  memset(tree_out, 0, sizeof(*tree_out));
  if (!read || !range_fits(meta_base, 0x40u, mount_size))
    return SM_FPKG_CORRUPT;
  uint8_t superblock[0x40];
  sm_fpkg_status_t status = read(read_ctx, meta_base, superblock,
                                 sizeof(superblock));
  if (status != SM_FPKG_OK) return status;
  uint64_t pfs_block_size = read_le32(superblock + 0x20u);
  if (read_le64(superblock) != 2 ||
      read_le64(superblock + 8u) != TREE_SUPERBLOCK_MAGIC ||
      pfs_block_size < TREE_TABLE_PAGE)
    return SM_FPKG_CORRUPT;
  uint64_t inode_count = read_le64(superblock + 0x30u);
  if (inode_count <= TREE_FIRST_DIR_INODE || inode_count > TREE_MAX_INODES)
    return SM_FPKG_CORRUPT;
  uint64_t table_blocks =
      inode_count / TREE_INODES_PER_BLOCK +
      (inode_count % TREE_INODES_PER_BLOCK != 0);
  uint64_t table_length, table_at;
  if (table_blocks == 0 ||
      table_blocks > UINT64_MAX / TREE_TABLE_PAGE ||
      !add_u64(meta_base, pfs_block_size, &table_at))
    return SM_FPKG_CORRUPT;
  table_length = table_blocks * TREE_TABLE_PAGE;
  if (!range_fits(table_at, table_length, mount_size))
    return SM_FPKG_CORRUPT;
  uint64_t table_end = table_at + table_length;

  uint8_t *visited = calloc((size_t)inode_count, 1u);
  uint8_t *dir_buffer = NULL;
  pending_stack_t pending = {0};
  size_t entry_capacity = 0;
  if (!visited) return SM_FPKG_NOMEM;
  char *root_path = calloc(1, 1u);
  if (!root_path) {
    free(visited);
    return SM_FPKG_NOMEM;
  }
  status = push_dir(&pending, TREE_FIRST_DIR_INODE, root_path, 0);
  if (status != SM_FPKG_OK) {
    free(root_path);
    goto done;
  }

  uint64_t directory_budget = mount_size - meta_base;
  size_t path_bytes = 0;
  while (pending.count != 0) {
    pending_dir_t current = pending.items[--pending.count];
    if (visited[current.inode]) {
      free(current.path);
      status = SM_FPKG_CORRUPT;
      goto done;
    }
    visited[current.inode] = 1;
    tree_inode_t directory;
    status = read_inode(read, read_ctx, mount_size, table_at, inode_count,
                        current.inode, &directory);
    if (status != SM_FPKG_OK) {
      free(current.path);
      goto done;
    }
    if ((directory.mode & 0xf000u) != 0x4000u ||
        directory.size > TREE_MAX_DIR_BYTES ||
        !range_fits(directory.logical, directory.size, mount_size) ||
        (directory.size != 0 && directory.logical < table_end) ||
        directory.size > directory_budget) {
      free(current.path);
      status = SM_FPKG_CORRUPT;
      goto done;
    }
    directory_budget -= directory.size;
    if (directory.size > 0) {
      uint8_t *grown = realloc(dir_buffer, (size_t)directory.size);
      if (!grown) {
        free(current.path);
        status = SM_FPKG_NOMEM;
        goto done;
      }
      dir_buffer = grown;
      status = read(read_ctx, directory.logical, dir_buffer,
                    (size_t)directory.size);
      if (status != SM_FPKG_OK) {
        free(current.path);
        goto done;
      }
    }
    size_t directory_size = (size_t)directory.size;
    size_t at = 0;
    while (at < directory_size) {
      if (directory_size - at < 16u) {
        status = SM_FPKG_CORRUPT;
        free(current.path);
        goto done;
      }
      uint32_t inode_number = read_le32(dir_buffer + at);
      uint32_t kind = read_le32(dir_buffer + at + 4u);
      size_t name_length = read_le32(dir_buffer + at + 8u);
      size_t entry_size = read_le32(dir_buffer + at + 12u);
      if (entry_size == 0) break;
      if (name_length > directory_size - at - 16u ||
          entry_size < 16u + name_length ||
          entry_size > directory_size - at ||
          (uint64_t)inode_number >= inode_count) {
        status = SM_FPKG_CORRUPT;
        free(current.path);
        goto done;
      }
      const uint8_t *name = dir_buffer + at + 16u;
      at += entry_size;
      if (kind == 4u || kind == 5u) continue;
      if (!valid_utf8_component(name, name_length)) {
        status = SM_FPKG_CORRUPT;
        free(current.path);
        goto done;
      }
      size_t prefix_length = strlen(current.path);
      size_t extra = prefix_length == 0 ? name_length : name_length + 1u;
      if (extra > TREE_MAX_PATH || prefix_length > TREE_MAX_PATH - extra ||
          tree_out->count >= TREE_MAX_ENTRIES) {
        status = SM_FPKG_UNSUPPORTED;
        free(current.path);
        goto done;
      }
      size_t path_length = prefix_length + extra;
      if (path_length > TREE_MAX_PATH_BYTES - path_bytes) {
        free(current.path);
        status = SM_FPKG_UNSUPPORTED;
        goto done;
      }
      char *path = malloc(path_length + 1u);
      if (!path) {
        status = SM_FPKG_NOMEM;
        free(current.path);
        goto done;
      }
      if (prefix_length != 0) {
        memcpy(path, current.path, prefix_length);
        path[prefix_length] = '/';
      }
      memcpy(path + prefix_length + (prefix_length != 0), name, name_length);
      path[path_length] = '\0';
      path_bytes += path_length;
      if (kind == 3u) {
        if (current.depth >= TREE_MAX_DEPTH) {
          free(path);
          free(current.path);
          status = SM_FPKG_UNSUPPORTED;
          goto done;
        }
        status = append_entry(tree_out, &entry_capacity, path, 0, 0, true);
        if (status != SM_FPKG_OK) {
          free(path);
          free(current.path);
          goto done;
        }
        status = push_dir(&pending, inode_number, path, current.depth + 1u);
        if (status != SM_FPKG_OK) {
          free(path);
          free(current.path);
          goto done;
        }
      } else if (kind == 2u) {
        tree_inode_t file;
        status = read_inode(read, read_ctx, mount_size, table_at,
                            inode_count, inode_number, &file);
        uint64_t file_end;
        if (status == SM_FPKG_OK &&
            ((file.mode & 0xf000u) != 0x8000u ||
             !add_u64(file.logical, file.size, &file_end) ||
             file_end > meta_base))
          status = SM_FPKG_CORRUPT;
        if (status == SM_FPKG_OK)
          status = append_entry(tree_out, &entry_capacity, path, file.size,
                                file.logical, false);
        free(path);
        if (status != SM_FPKG_OK) {
          free(current.path);
          goto done;
        }
      } else {
        free(path);
        free(current.path);
        status = SM_FPKG_CORRUPT;
        goto done;
      }
    }
    free(current.path);
  }
  status = SM_FPKG_OK;

done:
  for (size_t i = 0; i < pending.count; ++i)
    free(pending.items[i].path);
  free(pending.items);
  free(dir_buffer);
  free(visited);
  if (status != SM_FPKG_OK)
    sm_fpkg_tree_free(tree_out);
  return status;
}

sm_fpkg_status_t sm_fpkg_tree_validate(sm_fpkg_mount_read_fn read,
                                       void *read_ctx, uint64_t mount_size,
                                       uint64_t meta_base) {
  sm_fpkg_tree_t tree = {0};
  sm_fpkg_status_t status =
      sm_fpkg_tree_open(read, read_ctx, mount_size, meta_base, &tree);
  sm_fpkg_tree_free(&tree);
  return status;
}
