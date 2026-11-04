// Host regression for the actual scanner configuration watch/deadline helpers.
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>

#define EVFILT_READ (-1)
#define EVFILT_VNODE (-4)
#define EV_ADD 1
#define EV_ENABLE 2
#define EV_CLEAR 4
#define NOTE_WRITE 1
#define NOTE_EXTEND 2
#define NOTE_ATTRIB 4
#define NOTE_DELETE 8
#define NOTE_RENAME 16
#define NOTE_REVOKE 32
struct kevent { uintptr_t ident; int filter; unsigned fflags; };
#define EV_SET(e, id, filter_, flags, fflags_, data, udata) \
  (*(e) = (struct kevent){(id), (filter_), (fflags_)})
static const char *const g_scanner_config_paths[] = {"config.ini", "profiles.ini"};
static int g_scanner_config_fds[2] = {-1, -1};
static int g_scanner_manual_fd = -1, g_scanner_wake_pipe[2] = {30, 31};
static bool g_scanner_config_reload_pending;
static uint64_t g_scanner_config_reload_ready_after_us, g_scanner_config_probe_due_us;
static uint64_t g_scanner_manual_scan_due_us, g_scanner_manual_probe_due_us;
static uint64_t g_scanner_usb_mount_probe_due_us;
typedef struct { bool dirty; uint64_t ready_after_us; } scanner_root_state_t;
static scanner_root_state_t g_scanner_root_states[2];
static bool missing[2], opened[2];
static int fail_registration = -1, open_calls[2], close_calls[2];
static void log_debug(const char *fmt, ...) { (void)fmt; }
static void drain_scanner_wake_pipe(void) {}
static int get_scan_path_count(void) { return 2; }
static uint64_t sm_install_next_wake_us(uint64_t now) { (void)now; return 0; }
static int mock_open(const char *path, int flags) {
  (void)flags;
  int i = !strcmp(path, g_scanner_config_paths[1]);
  ++open_calls[i];
  if (missing[i]) { errno = ENOENT; return -1; }
  assert(!opened[i]); opened[i] = true;
  return 20 + i;
}
static int mock_close(int fd) {
  if (fd >= 20 && fd <= 21) {
    int i = fd - 20;
    assert(opened[i]); opened[i] = false; ++close_calls[i];
  }
  return 0;
}
static int kevent(int kq, const struct kevent *changes, int nchanges,
    struct kevent *events, int count, const struct timespec *timeout) {
  (void)kq; (void)events; (void)count; (void)timeout;
  assert(nchanges == 1 && opened[changes->ident - 20]);
  if ((int)changes->ident == fail_registration) { errno = EIO; return -1; }
  return 0;
}
#define open mock_open
#define close mock_close
#include "config_watch_helpers.inc"

int main(void) {
  struct timespec timeout;
  reopen_config_file_watch(1, 100);
  assert(opened[0] && opened[1] && !g_scanner_config_probe_due_us);
  assert(compute_next_scan_deadline_us(100, 0, 0, true) == 0);
  assert(build_wait_timeout(&timeout, 100, 0) == NULL);
  for (int i = 0; i < 2; ++i) {
    struct kevent event = {20 + i, EVFILT_VNODE, NOTE_WRITE};
    assert(handle_scanner_control_event(1, &event, 200));
    assert(!config_reload_due(200) && config_reload_due(250200));
    // Config reload remains scheduled even while scan work is blocked by a game.
    assert(compute_next_scan_deadline_us(200, 0, 0, false) == 250200);
    g_scanner_config_reload_pending = false;
  }
  struct kevent renamed = {21, EVFILT_VNODE, NOTE_RENAME};
  assert(handle_scanner_control_event(1, &renamed, 300));
  assert(close_calls[1] == 1 && open_calls[1] == 2 && open_calls[0] == 1);
  g_scanner_config_reload_pending = false;
  // Missing profiles get the existing bounded probe, never a zero-timeout loop.
  missing[1] = true;
  assert(handle_scanner_control_event(1, &renamed, 400));
  g_scanner_config_reload_pending = false;
  uint64_t deadline = compute_next_scan_deadline_us(400, 0, 0, false);
  assert(deadline == 10000400 && !config_probe_due(deadline - 1) && config_probe_due(deadline));
  assert(build_wait_timeout(&timeout, 400, deadline) == &timeout && timeout.tv_sec == 10);
  g_scanner_root_states[0] = (scanner_root_state_t){true, 900};
  assert(compute_next_scan_deadline_us(400, 0, 0, true) == 900);
  assert(compute_next_scan_deadline_us(400, 0, 0, false) == deadline);
  reopen_config_file_watch(1, deadline);
  assert(open_calls[0] == 1 && g_scanner_config_probe_due_us == deadline + 10000000);
  missing[1] = false;
  reopen_config_file_watch(1, deadline + 10000000);
  assert(opened[1] && !g_scanner_config_probe_due_us);
  fail_registration = 21;
  register_config_file_watch(1, 500);
  assert(opened[0] && !opened[1] && g_scanner_config_probe_due_us == 10000500);
  fail_registration = -1;
  reopen_config_file_watch(1, 10000500);
  assert(opened[0] && opened[1] && !g_scanner_config_probe_due_us);
  // Resume reopens both files; shutdown closes both, including partial init.
  close_scanner_config_file();
  reopen_config_file_watch(1, 20000000);
  assert(opened[0] && opened[1]);
  close_scanner_config_file();
  close_scanner_config_file();
  assert(!opened[0] && !opened[1]);
  puts("config watches: two files, rename/delete, bounded probes, game blocking, shutdown passed");
  return 0;
}
