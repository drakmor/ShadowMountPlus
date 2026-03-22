#ifndef SM_SCAN_TREE_H
#define SM_SCAN_TREE_H

#include <stdbool.h>

typedef enum {
  SM_SCAN_TREE_DIR_DESCEND = 0,
  SM_SCAN_TREE_DIR_SKIP_DESCEND,
  SM_SCAN_TREE_DIR_ABORT,
} sm_scan_tree_dir_visit_t;

typedef sm_scan_tree_dir_visit_t (*sm_scan_tree_dir_fn)(const char *dir_path,
                                                        unsigned int depth_from_root,
                                                        void *ctx);
typedef bool (*sm_scan_tree_image_fn)(const char *image_path,
                                      const char *image_name,
                                      unsigned int depth_from_root,
                                      void *ctx);

typedef struct {
  sm_scan_tree_dir_fn on_directory;
  sm_scan_tree_image_fn on_image_file;
} sm_scan_tree_callbacks_t;

// Walk a managed scan subtree using the shared scan-depth and path filtering rules.
bool sm_scan_tree_walk(const char *scan_root, const char *dir_path,
                       unsigned int depth_from_root,
                       unsigned int remaining_depth,
                       const sm_scan_tree_callbacks_t *callbacks, void *ctx);

#endif
