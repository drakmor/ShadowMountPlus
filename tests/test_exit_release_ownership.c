// Host regression test: compile the production ShellCore service with console
// I/O stubbed so the exit-release ownership rules run on Linux.
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char g_sandbox_root[64];
static uint64_t g_now_us;
static unsigned g_unmount_calls;
static pthread_barrier_t g_claimed;
static pthread_barrier_t g_finished;

static DIR *test_opendir(const char *path);
int sceKernelUsleep(unsigned int microseconds);
#define opendir test_opendir
#include "../src/sm_shellcore_service.c"
#undef opendir

static const char TEST_TITLE[] = "PPSA00000";

static DIR *test_opendir(const char *path) {
  assert(strcmp(path, "/mnt/sandbox") == 0);
  return opendir(g_sandbox_root);
}

uint64_t monotonic_time_us(void) { return g_now_us; }

void sm_scanner_wake(void) {}

void runtime_mount_state_lock(void) {}

void runtime_mount_state_unlock(void) {}

void log_debug(const char *format, ...) { (void)format; }

int sceKernelUsleep(unsigned int microseconds) {
  (void)microseconds;
  return 0;
}

bool unmount_title_runtime_layers(const char *title_id) {
  assert(strcmp(title_id, TEST_TITLE) == 0);
  ++g_unmount_calls;
  return true;
}

bool unmount_title_runtime_layers_quiet(const char *title_id) {
  return unmount_title_runtime_layers(title_id);
}

bool read_mount_image_chain(const char *title_id,
                            char paths[MAX_IMAGE_CHAIN_DEPTH][MAX_PATH],
                            size_t *count_out) {
  (void)title_id;
  (void)paths;
  *count_out = 0;
  return false;
}

bool release_runtime_image_mount(const char *path) {
  (void)path;
  abort();
}

static void wait_at(pthread_barrier_t *barrier) {
  int result = pthread_barrier_wait(barrier);
  assert(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD);
}

static void reset_owner(void) {
  bool claimed = false;
  clear_mount_owner(&g_service.prepared);
  clear_mount_owner(&g_service.outgoing);
  g_service.prepare_in_progress = false;
  g_service.launch_pending = false;
  g_service.pending_launch_title_id[0] = '\0';
  g_service.releasing_title_id[0] = '\0';
  g_unmount_calls = 0;
  assert(claim_prepared_title(TEST_TITLE, true, &claimed) == 0 && claimed);
  finish_prepared_title(TEST_TITLE, false);
  sm_shellcore_service_bind_prepared_app(TEST_TITLE, 0x100, 100);
}

static void *prepare_relaunch(void *unused) {
  (void)unused;
  bool claimed = true;
  assert(claim_prepared_title(TEST_TITLE, true, &claimed) == 0);
  assert(!claimed);
  wait_at(&g_claimed);
  wait_at(&g_finished);
  finish_prepared_title(TEST_TITLE, false);
  return NULL;
}

static bool run_case(const char *name, uint64_t now_us, bool relaunch,
                     bool bind_replacement, bool expect_unmount) {
  reset_owner();
  g_now_us = now_us;
  pthread_t thread;
  if (relaunch) {
    assert(pthread_create(&thread, NULL, prepare_relaunch, NULL) == 0);
    wait_at(&g_claimed);
  }
  char exited_title[MAX_TITLE_ID];
  assert(sm_shellcore_service_note_game_exit(100, 1000000, exited_title));
  if (relaunch) {
    wait_at(&g_finished);
    assert(pthread_join(thread, NULL) == 0);
    assert(g_service.launch_pending && !g_service.prepare_in_progress);
  }
  if (bind_replacement)
    sm_shellcore_service_bind_prepared_app(TEST_TITLE, 0x100, 200);

  int status = sm_shellcore_service_release_exited_titles();
  bool owner_retained = mount_owner_matches(&g_service.prepared, TEST_TITLE);
  bool unmounted = g_unmount_calls != 0;
  bool passed = unmounted == expect_unmount && owner_retained == !expect_unmount;
  printf("%s: status=%d unmounted=%d owner_retained=%d %s\n", name, status,
         unmounted, owner_retained, passed ? "PASS" : "FAIL");
  return passed;
}

int main(void) {
  assert(snprintf(g_sandbox_root, sizeof(g_sandbox_root),
                  "/tmp/shadowmount-exit-test-XXXXXX") > 0);
  assert(mkdtemp(g_sandbox_root));
  char sandbox[MAX_PATH];
  assert(snprintf(sandbox, sizeof(sandbox), "%s/%s_000", g_sandbox_root,
                  TEST_TITLE) > 0);
  assert(mkdir(sandbox, 0700) == 0);
  assert(pthread_barrier_init(&g_claimed, NULL, 2) == 0);
  assert(pthread_barrier_init(&g_finished, NULL, 2) == 0);

  bool passed = true;
  passed &= run_case("relaunch claimed before deadline", 10900000, true, false,
                     false);
  passed &= run_case("no relaunch after deadline", 11000000, false, false, true);
  passed &= run_case("relaunch already bound", 11000000, true, true, false);
  passed &= run_case("relaunch claimed, not yet bound", 11000000, true, false,
                     false);

  assert(pthread_barrier_destroy(&g_claimed) == 0);
  assert(pthread_barrier_destroy(&g_finished) == 0);
  assert(rmdir(sandbox) == 0);
  assert(rmdir(g_sandbox_root) == 0);
  return passed ? 0 : 1;
}
