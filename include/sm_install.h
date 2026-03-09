#ifndef SM_INSTALL_H
#define SM_INSTALL_H

typedef struct scan_candidate scan_candidate_t;

// Install or remount the collected scan candidates.
void process_scan_candidates(const scan_candidate_t *candidates,
                             int candidate_count);

#endif
