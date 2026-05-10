#ifndef SM_SCAN_H
#define SM_SCAN_H

#include <stdbool.h>

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
// Mount stable backport overlays for already mounted titles.
void mount_backport_overlays(bool *unstable_found_out);

#endif
