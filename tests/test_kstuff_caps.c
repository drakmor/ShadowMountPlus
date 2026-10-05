// Host regression test: the production capability prober, with the three
// kernel reads stubbed so a case can say what ShellCore's text looks like.
//
// The offset table can only be checked against a real console. What is checked
// here is the logic around it: which row a firmware picks, that a whole patch
// signature must match, and above all WHAT GETS CACHED.
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#include "sm_kstuff_caps.h"

// --- the three kernel reads the prober makes, and the log sink ---

static uint32_t test_fw = 0x04030000u;
static pid_t test_pid = 42;
static intptr_t test_base = 0x400000;
static bool test_copyout_fails;
#define TEST_SIG_MAX 6u
static uint8_t test_sysdir[TEST_SIG_MAX];
static uint8_t test_trophy[TEST_SIG_MAX];
static uint32_t test_sysdir_off, test_trophy_off;
static int test_copyouts;

uint32_t kernel_get_fw_version(void);
uint32_t kernel_get_fw_version(void) { return test_fw; }

pid_t find_pid_by_name(const char *name, bool exclude_self);
pid_t find_pid_by_name(const char *name, bool exclude_self) {
  (void)exclude_self;
  // The kernel reports "SceShellCore" with no ".elf"; the wrong name returns
  // "not running", which reads exactly like an honest "no patches".
  assert(strcmp(name, "SceShellCore") == 0);
  return test_pid;
}

intptr_t kernel_dynlib_mapbase_addr(pid_t pid, unsigned int handle);
intptr_t kernel_dynlib_mapbase_addr(pid_t pid, unsigned int handle) {
  assert(pid == test_pid);
  assert(handle == 0u);   // ShellCore's own module, not a dependency
  return test_base;
}

int kernel_proc_copyout(pid_t pid, intptr_t src, void *dst, size_t len);
int kernel_proc_copyout(pid_t pid, intptr_t src, void *dst, size_t len) {
  assert(pid == test_pid);
  if (test_copyout_fails)
    return -1;
  test_copyouts++;
  assert(len <= TEST_SIG_MAX);
  // Routed by call order, not by length: a patch the table describes in two
  // bytes would otherwise be served the wrong buffer. run_probe reads the
  // getSceSysDirPath site and then the trophy site, so an odd count is the
  // first of a pair. The offsets are recorded so a case can assert the row the
  // firmware selected.
  if ((test_copyouts % 2) == 1u) {
    test_sysdir_off = (uint32_t)(src - test_base);
    memcpy(dst, test_sysdir, len);
  } else {
    test_trophy_off = (uint32_t)(src - test_base);
    memcpy(dst, test_trophy, len);
  }
  return 0;
}

void log_debug(const char *fmt, ...);
void log_debug(const char *fmt, ...) { (void)fmt; }

#include "../src/sm_kstuff_caps.c"

// --- helpers ---

// Every knob is restored, not just the cache: a case that left an unknown
// firmware behind would decide the NEXT case for a reason it never set.
static void reset_probe_state(void) {
  g_caps_probed = false;
  g_caps_valid = false;
  g_caps = 0;
  test_fw = 0x04030000u;
  test_copyout_fails = false;
  test_pid = 42;
  test_base = 0x400000;
  test_copyouts = 0;
  memset(test_sysdir, 0, sizeof(test_sysdir));
  memset(test_trophy, 0, sizeof(test_trophy));
}

// The bytes the probe reads back, taken from the row the firmware under test
// selects, so a case states which patches are present and nothing about what
// they look like on that firmware.
static void set_patched(bool sysdir, bool trophy) {
  const shellcore_cap_row_t *row = find_cap_row();
  memset(test_sysdir, 0xcc, sizeof(test_sysdir));   // unrelated code
  memset(test_trophy, 0xcc, sizeof(test_trophy));
  if (row && sysdir)
    memcpy(test_sysdir, row->sysdir_sig, row->sysdir_len);
  if (row && trophy)
    memcpy(test_trophy, row->trophy_sig, row->trophy_len);
  else
    test_trophy[0] = 0x74u;                         // jz, unpatched
}

// --- cases ---

static void test_both_patches_present(void) {
  reset_probe_state();
  test_fw = 0x04030000u;
  set_patched(true, true);
  uint32_t caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
  // 4.03's row, asserted rather than assumed
  assert(test_sysdir_off == 0x43db4cu);
  assert(test_trophy_off == 0x8337a7u);
}

static void test_neither_patch_present(void) {
  reset_probe_state();
  set_patched(false, false);
  uint32_t caps = 0xdeadbeefu;
  // An unpatched ShellCore, read honestly: valid, no capabilities.
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == 0u);
}

static void test_zero_is_not_latched(void) {
  reset_probe_state();
  set_patched(false, false);
  uint32_t caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == 0u);

  // kstuff loading after this payload is ordering we do not control, and
  // latching the zero is what made a correct kstuff-lite refuse a launch.
  set_patched(true, true);
  caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
}

static void test_partial_is_not_latched(void) {
  reset_probe_state();
  // patch_shellcore writes the two entries in turn, so one bit is as
  // uncacheable as none.
  set_patched(true, false);
  uint32_t caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == SM_KSTUFF_CAP_SYSDIRPATH);

  set_patched(true, true);
  caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
}

static void test_complete_reading_is_cached(void) {
  reset_probe_state();
  set_patched(true, true);
  uint32_t caps = 0;
  assert(sm_kstuff_probe_caps(&caps));
  int after_first = test_copyouts;
  assert(after_first == 2);

  // Patched once at kstuff load, so a complete positive cannot go stale.
  assert(sm_kstuff_probe_caps(&caps));
  assert(test_copyouts == after_first);
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
}

static void test_failure_does_not_downgrade(void) {
  reset_probe_state();
  set_patched(true, false);
  uint32_t caps = 0;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == SM_KSTUFF_CAP_SYSDIRPATH);

  // A later failure is not evidence the patches went away.
  test_pid = 0;
  caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == SM_KSTUFF_CAP_SYSDIRPATH);
}

static void test_unknown_firmware(void) {
  reset_probe_state();
  test_fw = 0x13000000u;   // past the table: 12.70 is the last measured row
  set_patched(true, true);
  uint32_t caps = 0xdeadbeefu;
  assert(!sm_kstuff_probe_caps(&caps));
  assert(caps == 0xdeadbeefu);   // nothing written on an unmeasured answer
  assert(test_copyouts == 0);
}

static void test_shellcore_not_running(void) {
  reset_probe_state();
  test_pid = 0;
  uint32_t caps = 0xdeadbeefu;
  assert(!sm_kstuff_probe_caps(&caps));
  assert(caps == 0xdeadbeefu);
}

static void test_copyout_failure(void) {
  reset_probe_state();
  test_copyout_fails = true;
  uint32_t caps = 0xdeadbeefu;
  assert(!sm_kstuff_probe_caps(&caps));
  assert(caps == 0xdeadbeefu);
}

static void test_no_mapbase(void) {
  reset_probe_state();
  test_base = 0;
  uint32_t caps = 0xdeadbeefu;
  assert(!sm_kstuff_probe_caps(&caps));
  assert(test_copyouts == 0);
}

static void test_signature_must_match_whole(void) {
  reset_probe_state();
  // Right offset, wrong bytes -- a devkit lands these somewhere unrelated, and
  // a first-byte match would report a capability kstuff does not have.
  memset(test_sysdir, 0xcc, sizeof(test_sysdir));
  test_sysdir[0] = 0x66;   // the signature's first byte, and nothing else
  memset(test_trophy, 0x74u, sizeof(test_trophy));
  uint32_t caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == 0u);
}

static void test_signature_follows_firmware(void) {
  // getSceSysDirPath is NOP-ed in 6 bytes before 7.00 and 2 after, and the
  // table carries the bytes per firmware. Either is wrong on the other side,
  // so a neighbour's signature must not read as patched.
  static const uint8_t short_nop[2] = {0x66, 0x90};
  static const uint8_t wide_nop[6] = {0x66, 0x0f, 0x1f, 0x44, 0x00, 0x00};

  reset_probe_state();
  test_fw = 0x06000000u;
  set_patched(false, true);
  memcpy(test_sysdir, short_nop, sizeof(short_nop));
  uint32_t caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert((caps & SM_KSTUFF_CAP_SYSDIRPATH) == 0u);
  assert(caps & SM_KSTUFF_CAP_TROPHY);

  reset_probe_state();
  test_fw = 0x07000000u;
  set_patched(false, true);
  memcpy(test_sysdir, wide_nop, sizeof(wide_nop));
  caps = 0xdeadbeefu;
  assert(sm_kstuff_probe_caps(&caps));
  assert((caps & SM_KSTUFF_CAP_SYSDIRPATH) == 0u);

  // Each firmware's own signature, from its own row, does read as patched.
  reset_probe_state();
  test_fw = 0x06000000u;
  set_patched(true, true);
  caps = 0;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));

  reset_probe_state();
  test_fw = 0x07000000u;
  set_patched(true, true);
  caps = 0;
  assert(sm_kstuff_probe_caps(&caps));
  assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
}

static void test_every_row_is_reachable(void) {
  // A duplicated or mistyped firmware stamp makes a row dead, and the console
  // it was measured on then silently reports "no patch offsets". A table
  // regenerated against a newer kstuff-lite is checked here, not by eye.
  size_t rows = sizeof(g_cap_rows) / sizeof(g_cap_rows[0]);
  assert(rows > 0);
  for (size_t i = 0; i < rows; i++) {
    reset_probe_state();
    test_fw = (uint32_t)g_cap_rows[i].firmware << 16;
    set_patched(true, true);
    uint32_t caps = 0;
    assert(sm_kstuff_probe_caps(&caps));
    assert(caps == (SM_KSTUFF_CAP_SYSDIRPATH | SM_KSTUFF_CAP_TROPHY));
    assert(test_sysdir_off == g_cap_rows[i].sysdir_off);
    assert(test_trophy_off == g_cap_rows[i].trophy_off);
    // A zero length compares equal to anything, and one past the buffer reads
    // the row's signature out of bounds.
    assert(g_cap_rows[i].sysdir_len > 0u && g_cap_rows[i].sysdir_len <= CAP_SIG_MAX);
    assert(g_cap_rows[i].trophy_len > 0u && g_cap_rows[i].trophy_len <= CAP_SIG_MAX);
    assert(g_cap_rows[i].name != NULL);
    for (size_t j = 0; j < i; j++)
      assert(g_cap_rows[j].firmware != g_cap_rows[i].firmware);
  }
  printf("  %zu firmware rows, each reachable\n", rows);
}

static void test_a_firmware_revision_does_not_fall_through(void) {
  // Keyed on the top 16 bits, but an absent major.minor must miss rather than
  // borrow a neighbour's offsets.
  reset_probe_state();
  test_fw = 0x0403ffffu;
  set_patched(true, true);
  uint32_t caps = 0;
  assert(sm_kstuff_probe_caps(&caps));
  assert(test_sysdir_off == 0x43db4cu);

  reset_probe_state();
  test_fw = 0x04100000u;   // 4.10 has no row; 4.00/4.02/4.03 do
  set_patched(true, true);
  caps = 0xdeadbeefu;
  assert(!sm_kstuff_probe_caps(&caps));
}

int main(void) {
  test_both_patches_present();
  test_neither_patch_present();
  test_zero_is_not_latched();
  test_partial_is_not_latched();
  test_complete_reading_is_cached();
  test_failure_does_not_downgrade();
  test_unknown_firmware();
  test_shellcore_not_running();
  test_copyout_failure();
  test_no_mapbase();
  test_signature_must_match_whole();
  test_signature_follows_firmware();
  test_every_row_is_reachable();
  test_a_firmware_revision_does_not_fall_through();
  printf("test_kstuff_caps: all cases passed\n");
  return 0;
}
