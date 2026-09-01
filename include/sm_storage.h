#ifndef SM_STORAGE_H
#define SM_STORAGE_H

#include <stdbool.h>
#include <stdint.h>

typedef bool (*sm_storage_cancel_fn)(void *ctx);
typedef void (*sm_storage_progress_fn)(uint64_t bytes_delta,
                                       uint64_t files_delta, void *ctx);
typedef bool (*sm_storage_finalize_fn)(void *ctx);

// Sum regular-file sizes under a file or directory without following links.
int sm_storage_measure_path(const char *path, uint64_t *size_out);
// Measure a source and count regular files, with optional cooperative cancel.
int sm_storage_measure_path_progress(const char *path, uint64_t *size_out,
                                     uint64_t *file_count_out,
                                     sm_storage_cancel_fn cancel, void *ctx);
// Measure every non-directory entry for deletion; only regular files add bytes.
int sm_storage_measure_delete_path_progress(
    const char *path, uint64_t *size_out, uint64_t *file_count_out,
    sm_storage_cancel_fn cancel, void *ctx);
// Copy a regular file or directory tree exactly. Destination must not exist.
int sm_storage_copy_path(const char *source, const char *destination);
// Copy while reporting written-byte and completed-file deltas.
int sm_storage_copy_path_progress(const char *source, const char *destination,
                                  sm_storage_progress_fn progress,
                                  sm_storage_cancel_fn cancel, void *ctx);
// Move a file or directory, falling back to copy+delete across filesystems.
int sm_storage_move_path(const char *source, const char *destination);
// Move while reporting progress for a cross-filesystem copy. begin_finalize
// atomically closes cancellation before source deletion; returning false keeps
// the source and removes the destination copy. renamed_out is true when an
// atomic same-filesystem rename completed without copying.
int sm_storage_move_path_progress(const char *source, const char *destination,
                                  sm_storage_progress_fn progress,
                                  sm_storage_cancel_fn cancel,
                                  sm_storage_finalize_fn begin_finalize,
                                  void *ctx,
                                  bool *renamed_out);
// Recursively remove a regular file or directory without following links.
int sm_storage_delete_path(const char *path);
// Delete while reporting removed-byte and removed-file deltas. Cancellation
// must be decided before calling because recursive deletion is irreversible.
int sm_storage_delete_path_progress(const char *path,
                                    sm_storage_progress_fn progress,
                                    void *ctx);

#endif
