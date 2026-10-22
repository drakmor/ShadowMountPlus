#ifndef SM_PKG_INSTALL_H
#define SM_PKG_INSTALL_H

#include <stdbool.h>
#include <stdint.h>

#include "sm_types.h"

bool sm_pkg_install_queue_candidate(const scan_candidate_t *candidate);
uint64_t sm_pkg_install_next_wake_us(uint64_t now_us);
void sm_pkg_install_service(void);
bool sm_pkg_install_has_pending_work(void);
void sm_pkg_install_forget_path(const char *path);
void sm_pkg_install_shutdown(void);

#endif
