#ifndef SM_UCRED_H
#define SM_UCRED_H

#include <stdbool.h>
#include <stdint.h>

// The payload's authid and capabilities are process-wide. Every runtime change
// to them (a temporary elevation and its restore, or a lasting one) holds this
// lock, so two threads cannot interleave a save, change and restore and leave
// the process with the wrong identity. Calls that need the lasting authid (the
// kernel log reads) hold it too, so they never run during an elevation.
void sm_ucred_lock(void);
void sm_ucred_unlock(void);

// Runs fn with this process's authid set to authid, under the lock, and
// restores the previous authid. Returns false when the authid could not be
// changed or restored; fn's result goes to *result_out either way it ran.
bool sm_ucred_with_authid(uint64_t authid, int (*fn)(void *), void *arg,
                          int *result_out);

#endif
