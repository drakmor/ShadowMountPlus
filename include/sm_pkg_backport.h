#ifndef SM_PKG_BACKPORT_H
#define SM_PKG_BACKPORT_H

#include <stdbool.h>
#include <sys/types.h>

// Add child NSFS redirects immediately before the package process is spawned.
// Files use plain redirects; missing directory subtrees use overlays. Up to
// 256 redirect entries are tracked per sandbox. Missing backports and
// non-NSFS app0 paths are harmless skips.
bool sm_pkg_backport_on_sandbox_ready(const char *title_id);
// Remove an unbound redirect when ShellCore aborts the launch.
void sm_pkg_backport_on_launch_failed(const char *title_id);
// Bind an installed redirect to the process that consumed the sandbox.
// This deliberately does not create a late redirect or retry a failed ioctl.
void sm_pkg_backport_on_exec(pid_t pid, const char *title_id);
// Remove the redirect owned by an exited process.
void sm_pkg_backport_on_exit(pid_t pid);
// Best-effort cleanup of every redirect owned by this payload.
void sm_pkg_backport_shutdown(void);

#endif
