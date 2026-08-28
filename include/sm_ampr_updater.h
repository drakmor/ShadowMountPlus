#ifndef SM_AMPR_UPDATER_H
#define SM_AMPR_UPDATER_H

#include <stdbool.h>

#include "sm_types.h"

// Start the dormant-by-default AMPR update worker.
bool sm_ampr_updater_start(void);
// Interrupt any active request and join the update worker.
void sm_ampr_updater_stop(void);
// Cancel network work on sleep entry and wake the dormant worker on resume.
void sm_ampr_updater_on_sleep_change(bool active);
// Wake the worker when its enabled state, URL, or destination changes.
void sm_ampr_updater_on_config_reload(const runtime_config_t *old_cfg,
                                      const runtime_config_t *new_cfg);

#endif
