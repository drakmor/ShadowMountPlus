#ifndef SM_SCAN_H
#define SM_SCAN_H

typedef struct scan_candidate scan_candidate_t;

// Unmount and clean up mounts whose backing sources disappeared.
void cleanup_lost_sources_before_scan(void);
// Scan configured roots and collect install candidates.
int collect_scan_candidates(scan_candidate_t *candidates, int max_candidates,
                            int *total_found_out);
// Mount stable per-root backport overlays for already mounted titles.
void mount_backport_overlays(void);

#endif
