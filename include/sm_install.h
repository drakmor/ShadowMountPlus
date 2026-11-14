#ifndef SM_INSTALL_H
#define SM_INSTALL_H

#include <stdbool.h>

typedef struct scan_candidate scan_candidate_t;

// Register one already-staged title directory. Returns false when no usable
// TitleDir registration path is available; the API result is returned intact.
bool sm_install_register_title_dir(const char *title_id,
                                   const char *install_dir,
                                   int *result_out);

// Release a superseded native FPKG cache only after registration succeeds.
// previous_generation_name is a validated cache generation basename.
bool sm_install_release_replaced_fpkg_generation(
    const char *title_id, const char *previous_generation_name,
    const char *current_path);

// Install or remount the collected scan candidates.
void process_scan_candidates(const scan_candidate_t *candidates,
                             int candidate_count);

#endif
