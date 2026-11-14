#ifndef SM_FPKG_BACKPORT_H
#define SM_FPKG_BACKPORT_H

#include "sm_fpkg_index.h"

typedef struct sm_fpkg_backport sm_fpkg_backport_t;

/* A null or empty path represents absence of a backport. Captures regular
 * files outside fakelib/fakelib2 without following links. */
sm_fpkg_status_t sm_fpkg_backport_open(const char *path,
                                        sm_fpkg_backport_t **out);
void sm_fpkg_backport_close(sm_fpkg_backport_t *snapshot);
size_t sm_fpkg_backport_count(const sm_fpkg_backport_t *snapshot);
const sm_fpkg_index_file_t *sm_fpkg_backport_files(
    const sm_fpkg_backport_t *snapshot);
size_t sm_fpkg_backport_find(const sm_fpkg_backport_t *snapshot,
                              const char *path);
uint64_t sm_fpkg_backport_fingerprint(const sm_fpkg_backport_t *snapshot);
/* Opens a file only if it still matches the captured snapshot. */
int sm_fpkg_backport_open_file(const sm_fpkg_backport_t *snapshot, size_t index);
bool sm_fpkg_backport_unchanged(const sm_fpkg_backport_t *snapshot);

#endif
