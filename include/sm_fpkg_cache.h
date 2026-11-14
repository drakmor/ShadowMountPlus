#ifndef SM_FPKG_CACHE_H
#define SM_FPKG_CACHE_H

#include <stdbool.h>
#include <stdint.h>

#include "sm_fpkg_index.h"
#include "sm_paths.h"

#define SM_FPKG_CACHE_DEFAULT_ROOT FPKG_CACHE_PATH
#define SM_FPKG_CACHE_DEFAULT_EXTRACT_LIMIT UINT64_MAX

typedef bool (*sm_fpkg_cache_cancel_fn)(void *ctx);

typedef struct {
  uint64_t max_extract_bytes;
  sm_fpkg_cache_cancel_fn is_cancelled;
  void *cancel_ctx;
  const char *backport_path;
} sm_fpkg_cache_options_t;

typedef struct {
  char active_generation[MAX_PATH];
  size_t staging_generations;
  size_t orphaned_generations;
  size_t quarantined_generations;
} sm_fpkg_cache_recovery_report_t;

typedef struct {
  char generation_path[MAX_PATH];
  char source_path[MAX_PATH];
  sm_pkg_info_t source_metadata;
  uint64_t backport_fingerprint;
  bool source_matches_snapshot;
} sm_fpkg_cache_generation_info_t;

typedef bool (*sm_fpkg_cache_cleanup_begin_fn)(const char *title_id,
                                                const char *generation_path,
                                                void *ctx);
typedef void (*sm_fpkg_cache_cleanup_end_fn)(void *ctx);

/* Persistent per-title upper directory for game-created app0 files. It is
 * outside all generations and is never removed by generation cleanup. */
sm_fpkg_status_t sm_fpkg_cache_prepare_writable(
    const char *cache_root, const char *title_id,
    const char *generation_path,
    char writable_path_out[MAX_PATH]);

/*
 * Build one immutable cache generation for a validated package. The source fd
 * must be the same read-only descriptor used by the FPKG reader. Publication
 * is one rename inside the cache filesystem. Existing generations are never
 * modified or removed; the caller updates registration only after this returns
 * SM_FPKG_OK and the source snapshot still matches.
 */
sm_fpkg_status_t sm_fpkg_cache_build_generation(
    const char *cache_root, const char *source_path, int source_fd,
    const sm_pkg_info_t *metadata, sm_fpkg_t *fpkg,
    sm_fpkg_index_extract_fn should_extract_fn, void *extract_ctx,
    const sm_fpkg_cache_options_t *options,
    char generation_path_out[MAX_PATH]);

/* Inspect the per-title active pointer and incomplete generations. Recovery is
 * read-only: invalid pointers and orphaned stages are reported, never deleted.
 */
sm_fpkg_status_t sm_fpkg_cache_recover_title(
    const char *cache_root, const char *title_id,
    sm_fpkg_cache_recovery_report_t *report_out);

/* Return the verified active generation when it was built from this exact
 * source snapshot. A cache miss is SM_FPKG_UNSUPPORTED and never mutates data.
 */
sm_fpkg_status_t sm_fpkg_cache_find_current_generation(
    const char *cache_root, const sm_pkg_info_t *source_metadata,
    char generation_path_out[MAX_PATH]);
sm_fpkg_status_t sm_fpkg_cache_find_current_generation_with_backport(
    const char *cache_root, const sm_pkg_info_t *source_metadata,
    const char *backport_path, char generation_path_out[MAX_PATH]);

/* Read the source identity captured in one published generation. The source
 * match flag is false when the recorded file was replaced or is unavailable;
 * callers can still use the recorded parent path to release an owned mount.
 */
sm_fpkg_status_t sm_fpkg_cache_read_generation_info(
    const char *cache_root, const char *title_id,
    const char *generation_path,
    sm_fpkg_cache_generation_info_t *info_out);

/* After the recorded image chain is reattached, compare the package snapshot
 * using mount-stable size and timestamps. Device and inode may change across
 * an md AUTOUNIT remount; sm_pkg_source_matches remains strict for other uses.
 */
bool sm_fpkg_cache_source_matches_remounted_image(
    const sm_fpkg_cache_generation_info_t *info);
bool sm_fpkg_cache_backport_matches(
    const sm_fpkg_cache_generation_info_t *info, const char *backport_path);

/* Remove one inactive immutable generation only after its ownership manifest
 * and complete tree have been validated. The required begin/end callbacks must
 * serialize with SMP lifecycle/mount state; begin returns true only when the
 * generation is not in use and holds that guard until end is called. The guard
 * is released after the generation is atomically quarantined. Unknown files,
 * symlinks, and malformed manifests refuse cleanup.
 */
sm_fpkg_status_t sm_fpkg_cache_remove_generation(
    const char *cache_root, const char *title_id,
    const char *generation_name,
    sm_fpkg_cache_cleanup_begin_fn begin_cleanup,
    sm_fpkg_cache_cleanup_end_fn end_cleanup, void *ctx);

#endif
