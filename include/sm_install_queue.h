#ifndef SM_INSTALL_QUEUE_H
#define SM_INSTALL_QUEUE_H

#include <stdbool.h>
#include <stdint.h>

typedef struct scan_candidate scan_candidate_t;

// Return whether a title is already waiting for async batch installation.
bool is_title_install_pending(const char *title_id);
// Return the next pending-install wake deadline in monotonic microseconds.
uint64_t sm_install_next_wake_us(uint64_t now_us);
// Poll async batch installs for success/timeout completion only.
void sm_install_poll_pending(void);
// Poll async batch installs for success/timeout completion and auto-submit
// queued installs when possible.
void sm_install_service_pending(void);
// Queue a staged title for the next batch install request.
bool sm_install_queue_candidate(const scan_candidate_t *candidate,
                                bool has_src_snd0);
// Submit all queued titles when no batch install is currently pending.
bool sm_install_submit_queued(void);
// Record retry state for every queued title after a batch submit failure.
void sm_install_note_submit_failure(void);

#endif
