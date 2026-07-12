#ifndef SM_SCANNER_H
#define SM_SCANNER_H

#include <stdbool.h>

// Initialize scanner service resources such as wake pipe and file watches.
bool sm_scanner_init(void);
// Wake a blocked scanner wait. Safe to call from signal context.
void sm_scanner_wake(void);
// Return true after scanner-owned USB directory descriptors are closed.
bool sm_scanner_usb_watches_suspended(void);
// Run the initial full synchronization cycle.
bool sm_scanner_run_startup_sync(void);
// Run the steady-state scanner wait/scan loop until shutdown is requested.
void sm_scanner_run_loop(void);
// Release scanner service resources.
void sm_scanner_shutdown(void);

#endif
