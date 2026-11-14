#include "host_platform.h"
#include "sm_limits.h"
#include "launch_types.inc"

#define TITLE_A "PPSA03527"
#define TITLE_B "PPSA47989"
#define TITLE_C "PPSA99008"

static unsigned wakes, unmounts;
static bool fail_unmount, sleeping;

static void sm_scanner_wake(void) { ++wakes; }
static void log_debug(const char *format, ...) { (void)format; }
static bool sm_game_lifecycle_has_active_game(void) { return false; }
static bool runtime_sleep_mode_active(void) { return sleeping; }
static uint64_t monotonic_time_us(void) { return 100000000; }
static bool shellcore_service_stopping(void) { return false; }
static void runtime_mount_state_lock(void) {}
static void runtime_mount_state_unlock(void) {}
static int sceKernelUsleep(unsigned us) { (void)us; return 0; }
static bool is_supported_game_title_id(const char *title) {
  return title && strncmp(title, "PPSA", 4) == 0;
}
static bool read_mount_link(const char *title, char *path, size_t size) {
  (void)title; strlcpy(path, "/data/game", size); return true;
}
static bool is_under_image_mount_base(const char *path) {
  (void)path; return false;
}
static bool wait_for_resumed_runtime_source(const char *title, char *path) {
  (void)title; (void)path; return true;
}
static bool is_data_mounted(const char *title) { (void)title; return true; }
static void sm_fakelib_prepare_title_cache(const char *title, const char *path) {
  (void)title; (void)path;
}
static bool prepare_title_runtime(const char *title, char *path, const bool *ro) {
  (void)title; (void)path; (void)ro; return true;
}
static bool unmount_title_runtime_layers(const char *title) {
  (void)title; ++unmounts;
  if (fail_unmount) { errno = EIO; return false; }
  return true;
}
static bool unmount_title_runtime_layers_quiet(const char *title) {
  return unmount_title_runtime_layers(title);
}
static bool release_backing_image(const char *title) { (void)title; return true; }
static bool sm_shellcore_title_is_protected(const char *title) {
  const shellcore_mount_owner_t *owners[] = {&g_service.prepared, &g_service.outgoing};
  for (size_t i = 0; i < 2; ++i)
    if (!strcmp(owners[i]->title_id, title) && owners[i]->pid > 0 &&
        !owners[i]->game_exited)
      return true;
  return false;
}
static void sm_fakelib_game_on_launch_failed(const char *title) { (void)title; }
static void sm_pkg_backport_on_launch_failed(const char *title) { (void)title; }

#include "launch_helpers.inc"

// Exercise the actual NOTE_EXEC dispatch after a same-title relaunch with a
// different app_id. The service helpers above retain the exited old owner.
static _Atomic pid_t g_active_game_pid;
static char g_active_game_title_id[MAX_TITLE_ID];
static uint32_t g_active_game_app_id;
static struct { bool active; } g_pending_game_exit;
typedef enum { GAME_HANDOFF_NONE, GAME_HANDOFF_COMPLETE,
               GAME_HANDOFF_DEFERRED } game_handoff_result_t;
static unsigned ensure_calls, monitor_starts;
static bool watch_fails;
static bool register_game_exit_watch(int kq, pid_t pid) {
  (void)kq; (void)pid; return !watch_fails;
}
static game_handoff_result_t try_game_process_handoff(
    pid_t pid, const char *title, uint32_t app_id) {
  (void)pid; (void)title; (void)app_id; return GAME_HANDOFF_NONE;
}
static void finalize_pending_game_exit(void) {
  g_pending_game_exit.active = false;
}
static bool sm_shellcore_service_title_is_prepared(const char *title) {
  return find_mount_owner_locked(title) != NULL;
}
static bool sm_shellcore_ensure_title_runtime(const char *title) {
  ++ensure_calls;
  return mount_managed_title_runtime(title, true, true, NULL) == 0;
}
static void publish_active_game(pid_t pid, const char *title, uint32_t app_id) {
  atomic_store(&g_active_game_pid, pid);
  strlcpy(g_active_game_title_id, title, sizeof(g_active_game_title_id));
  g_active_game_app_id = app_id;
}
static void sm_mdbg_game_on_exec(pid_t pid, const char *title, uint64_t time) {
  (void)pid; (void)title; (void)time; ++monitor_starts;
}
static void sm_pkg_backport_on_exec(pid_t pid, const char *title) {
  (void)pid; (void)title;
}
static void sm_fakelib_game_on_exec(pid_t pid, const char *title, bool fresh) {
  (void)pid; (void)title; (void)fresh;
}
#include "dispatch_helpers.inc"

static void reset_state(void) {
  clear_mount_owner(&g_service.prepared);
  clear_mount_owner(&g_service.outgoing);
  g_service.prepare_in_progress = g_service.launch_pending = false;
  g_service.external_mutation_in_progress = false;
  g_service.pending_launch_title_id[0] = g_service.releasing_title_id[0] = '\0';
  fail_unmount = sleeping = false;
  wakes = unmounts = 0;
  ensure_calls = monitor_starts = 0;
  watch_fails = false;
  atomic_store(&g_active_game_pid, 0);
  g_active_game_title_id[0] = '\0';
  g_active_game_app_id = 0;
  g_pending_game_exit.active = false;
}

static void start_title(const char *title, uint32_t app_id, pid_t pid) {
  assert(handle_launch_request(title) == 0);
  assert(sm_shellcore_service_bind_prepared_app(title, app_id, pid));
}

static void abandon_switch(void) {
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  assert(handle_launch_request(TITLE_B) == 0);
  assert(handle_launch_failed_request(TITLE_B) == 0);
  assert(sm_shellcore_service_note_game_exit(173, 100000000, NULL));
  shellcore_mount_owner_t exited = g_service.outgoing;
  assert(release_title_runtime_expected(TITLE_A, false, &exited));
  assert(!g_service.outgoing.title_id[0]);
  assert(g_service.prepared.pid == 0 && g_service.launch_pending);
  unmounts = 0;
}

int main(void) {
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  assert(sm_shellcore_service_note_game_exit(173, 100000000, NULL));
  shellcore_mount_owner_t old_exit = g_service.prepared;
  assert(dispatch_game_launch(1, 200, 110000000, TITLE_A, 0x2018));
  assert(g_service.prepared.pid == 200 && g_service.prepared.app_id == 0x2018);
  assert(!g_service.prepared.game_exited && !g_service.launch_pending);
  assert(ensure_calls == 0 && monitor_starts == 1 && unmounts == 0);
  assert(!release_title_runtime_expected(TITLE_A, false, &old_exit));
  assert(errno == EBUSY && unmounts == 0); // Old cleanup cannot unmount new app.

  reset_state();
  assert(dispatch_game_launch(1, 200, 110000000, TITLE_A, 0x2018));
  assert(ensure_calls == 1 && monitor_starts == 1);
  assert(g_service.prepared.pid == 200);
  watch_fails = true;
  assert(!dispatch_game_launch(1, 201, 110000000, TITLE_A, 0x4018));
  assert(g_service.prepared.pid == 200 && monitor_starts == 1);

  abandon_switch();
  unsigned before = wakes;
  assert(handle_launch_request(TITLE_C) == 0);
  assert(unmounts == 1 && wakes > before);
  assert(mount_owner_matches(&g_service.prepared, TITLE_C));
  assert(sm_shellcore_service_bind_prepared_app(TITLE_C, 0x2018, 200));
  assert(!launch_preparation_pending_locked());

  abandon_switch();
  assert(handle_launch_request(TITLE_B) == 0 && unmounts == 0);
  abandon_switch();
  assert(mount_managed_title_runtime(TITLE_C, true, true, NULL) == EBUSY);
  assert(unmounts == 0); // NOTE_EXEC fallback cannot abandon another launch.

  abandon_switch();
  fail_unmount = true;
  assert(handle_launch_request(TITLE_C) == 0);
  assert(unmounts == 1 && mount_owner_matches(&g_service.prepared, TITLE_C));

  for (unsigned guard = 0; guard < 5; ++guard) {
    abandon_switch();
    if (guard == 0) g_service.prepare_in_progress = true;
    if (guard == 1) g_service.external_mutation_in_progress = true;
    if (guard == 2) strlcpy(g_service.releasing_title_id, TITLE_A, MAX_TITLE_ID);
    if (guard == 3) set_mount_owner(&g_service.outgoing, TITLE_A);
    if (guard == 4) sleeping = true;
    assert(handle_launch_request(TITLE_C) == EBUSY && unmounts == 0);
  }
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  assert(handle_launch_request(TITLE_C) == 0 && unmounts == 0);
  assert(g_service.outgoing.pid == 173);

  // A side app can exit while the original game stays alive in outgoing.
  reset_state();
  start_title(TITLE_A, 0x18, 173);
  start_title(TITLE_B, 0x1a, 189);
  assert(sm_shellcore_service_note_game_exit(189, 100000000, NULL));
  shellcore_mount_owner_t exited = g_service.prepared;
  assert(release_title_runtime_expected(TITLE_B, false, &exited));
  assert(!g_service.prepared.title_id[0] && g_service.outgoing.pid == 173);
  assert(handle_launch_request(TITLE_A) == 0);
  assert(g_service.prepared.pid == 173 && g_service.prepared.app_id == 0x18);
  assert(!g_service.outgoing.title_id[0]);
  assert(!launch_preparation_pending_locked());
  unsigned released = unmounts;
  assert(handle_launch_failed_request(TITLE_A) == 0);
  assert(unmounts == released && !launch_preparation_pending_locked());
  assert(sm_shellcore_service_note_game_exit(173, 110000000, NULL));
  exited = g_service.prepared;
  assert(release_title_runtime_expected(TITLE_A, false, &exited));
  assert(!g_service.prepared.title_id[0] && !g_service.outgoing.title_id[0]);
  assert(handle_launch_request(TITLE_C) == 0);

  reset_state();
  set_mount_owner(&g_service.outgoing, TITLE_A);
  g_service.outgoing.pid = 173;
  g_service.outgoing.game_exited = true;
  bool claimed = true;
  assert(claim_prepared_title(TITLE_A, true, &claimed) == EBUSY);
  assert(!claimed && !g_service.prepared.title_id[0]);
  assert(g_service.outgoing.pid == 173 && g_service.outgoing.game_exited);
  puts("Launch ownership: abandoned switch recovery, retry identity, fallback, live owner and mutation guards passed");
  return 0;
}
