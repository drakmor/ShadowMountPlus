#include "sm_platform.h"
#include "sm_stability.h"
#include "sm_config_mount.h"
#include "sm_log.h"
#include "sm_types.h"

bool is_path_stable_now(const char *path, double *root_diff_out,
                        int *stat_errno_out) {
  struct stat st;
  time_t now = time(NULL);
  ensure_runtime_config_ready();
  if (stat_errno_out)
    *stat_errno_out = 0;

  if (stat(path, &st) != 0) {
    if (root_diff_out)
      *root_diff_out = -1.0;
    if (stat_errno_out)
      *stat_errno_out = errno;
    return false;
  }

  double root_diff = difftime(now, st.st_mtime);
  if (root_diff_out)
    *root_diff_out = root_diff;
  if (root_diff < 0.0)
    return true;
  return root_diff > (double)runtime_config()->stability_wait_seconds;
}

bool wait_for_stability_fast(const char *path, const char *name) {
  double diff = 0.0;
  int st_err = 0;
  if (is_path_stable_now(path, &diff, &st_err))
    return true;

  if (st_err != 0)
    log_debug("  [WAIT] %s stat failed for %s: %s", name, path,
              strerror(st_err));
  else
    log_debug("  [WAIT] %s modified %.0fs ago. Waiting...", name, diff);
  return false;
}
