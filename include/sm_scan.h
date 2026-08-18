#ifndef SM_SCAN_H
#define SM_SCAN_H

#include <stdbool.h>

#include "sm_limits.h"

typedef struct scan_candidate scan_candidate_t;

// Unmount and clean up mounts whose backing sources disappeared.
void cleanup_lost_sources_before_scan(void);
// Unmount and clean up mounts whose backing sources disappeared under one root.
void cleanup_lost_sources_for_scan_root(const char *scan_root);
// Immediately unmount runtime mounts backed by USB storage for suspend.
void unmount_usb_sources_for_suspend(void);
// Scan configured roots and collect install candidates.
int collect_scan_candidates(scan_candidate_t *candidates, int max_candidates,
                            int *total_found_out,
                            bool *unstable_found_out);
// Scan a single configured root and collect install candidates.
int collect_scan_candidates_for_scan_root(const char *scan_root,
                                          scan_candidate_t *candidates,
                                          int max_candidates,
                                          int *total_found_out,
                                          bool *unstable_found_out);
// Resolve the source directory of a title backport without mounting it.
bool resolve_backport_path_for_title(const char *title_id,
                                     const char *owning_scan_root,
                                     char backport_path[MAX_PATH]);
// Ensure a stable backport overlay is mounted when one is present.
// Absence is harmless; an unusable or failed required overlay returns false.
bool mount_backport_overlay_for_title(const char *source_path,
                                      const char *title_id,
                                      const char *owning_scan_root,
                                      bool *unstable_found_out);
// Remove discovery title/image mounts while preserving persistent metadata.
bool release_scan_runtime_mounts(void);

#endif
