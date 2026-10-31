/* Exercise the production privilege boundary, including post-syscall failures. */
#define _GNU_SOURCE
#include <assert.h>
#include <pthread.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <sched.h>
#include "sm_mdbg.h"

#define SCE_AUTHID_DEBUGGER UINT64_C(0x4800000000010003)
#define ORIGINAL_AUTHID UINT64_C(0x4800000000000006)
static uint64_t authid;
static uint8_t caps[16];
static unsigned auth_writes, cap_writes, calls, logs;
static bool fail_setup, fail_restore, fail_call;
static pthread_mutex_t credentials_mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned memory_calls;
void sm_mdbg_credentials_lock(void) { pthread_mutex_lock(&credentials_mutex); }
void sm_mdbg_credentials_unlock(void) { pthread_mutex_unlock(&credentials_mutex); }
static int mdbg_copyout(pid_t pid, intptr_t address, void *buffer, size_t size) {
  assert(pid == 456 && address == 0x10000 && size == 1);
  assert(authid == ORIGINAL_AUTHID);
  uint64_t saved = authid;
  authid = UINT64_C(0x4800000000000099);
  sched_yield();
  ++memory_calls;
  memset(buffer, 0x5a, size);
  authid = saved;
  return 0;
}
static int kernel_proc_copyin(pid_t pid, const void *buffer, intptr_t address, size_t size) {
  assert(*(const unsigned char *)buffer == 0x5a);
  unsigned char scratch;
  return mdbg_copyout(pid, address, &scratch, size);
}
static uint64_t kernel_get_ucred_authid(pid_t pid) { assert(pid == getpid()); return authid; }
static int kernel_get_ucred_caps(pid_t pid, uint8_t *out) {
  assert(pid == getpid()); memcpy(out, caps, sizeof(caps)); return 0;
}
static int kernel_set_ucred_authid(pid_t pid, uint64_t value) {
  assert(pid == getpid()); auth_writes++;
  errno = EIO; // restoration must not overwrite a ptrace failure's errno
  if ((fail_setup && value == SCE_AUTHID_DEBUGGER) ||
      (fail_restore && value == ORIGINAL_AUTHID)) return -1;
  authid = value; return 0;
}
static int kernel_set_ucred_caps(pid_t pid, const uint8_t *value) {
  assert(pid == getpid()); cap_writes++;
  memcpy(caps, value, sizeof(caps)); errno = EIO; return 0;
}
static int ptrace(int request, pid_t pid, caddr_t address, int data) {
  assert(request == 123 && pid == 456 && address == NULL && data == 0);
  assert(authid == SCE_AUTHID_DEBUGGER);
  for (size_t i = 0; i < sizeof(caps); ++i) assert(caps[i] == 255);
  calls++;
  if (fail_call) { errno = EBUSY; return -1; }
  return 0;
}
static void log_debug(const char *format, ...) { assert(format); logs++; }
#include "remote_ptrace.inc"

static void reset(void) {
  authid = ORIGINAL_AUTHID;
  memset(caps, 255, sizeof(caps));
  auth_writes = cap_writes = calls = logs = memory_calls = 0;
  fail_setup = fail_restore = fail_call = false;
}
static void *ptrace_worker(void *unused) {
  (void)unused;
  for (unsigned i = 0; i < 1000; ++i)
    assert(privileged_ptrace(123, 456, NULL, 0) == 0);
  return NULL;
}
static void *mixed_worker(void *kind) {
  for (unsigned i = 0; i < 1000; ++i) {
    unsigned char value = 0x5a;
    if ((uintptr_t)kind < 2) assert(privileged_ptrace(123, 456, NULL, 0) == 0);
    else if ((uintptr_t)kind == 2)
      assert(sm_remote_process_read(456, 0x10000, &value, 1) && value == 0x5a);
    else assert(sm_remote_process_copyin(456, 0x10000, &value, 1));
  }
  return NULL;
}
int main(void) {
  pthread_t workers[4];
  unsigned char value = 0x5a;
  assert(!sm_remote_process_read(-1, 0x10000, &value, 1));
  assert(!sm_remote_process_copyin(0, 0x10000, &value, 1));
  reset();
  assert(privileged_ptrace(123, 456, NULL, 0) == 0);
  assert(authid == ORIGINAL_AUTHID && auth_writes == 2 && !cap_writes && calls == 1);
  reset();
  authid = SCE_AUTHID_DEBUGGER;
  assert(privileged_ptrace(123, 456, NULL, 0) == 0);
  assert(!auth_writes && !cap_writes && calls == 1);
  reset();
  memset(caps, 0, sizeof(caps));
  assert(privileged_ptrace(123, 456, NULL, 0) == 0);
  assert(cap_writes == 2 && auth_writes == 2 && calls == 1);
  for (size_t i = 0; i < sizeof(caps); ++i) assert(caps[i] == 0);
  reset();
  for (uintptr_t i = 0; i < 4; ++i)
    assert(!pthread_create(&workers[i], NULL, mixed_worker, (void *)i));
  for (size_t i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
  assert(calls == 2000 && memory_calls == 2000 && authid == ORIGINAL_AUTHID);
  reset();
  fail_setup = true;
  assert(privileged_ptrace(123, 456, NULL, 0) == -1 && !calls);
  assert(authid == ORIGINAL_AUTHID && !cap_writes);
  reset();
  fail_restore = true;
  assert(privileged_ptrace(123, 456, NULL, 0) == 0);
  assert(calls == 1 && logs == 1); // successful ptrace must remain successful
  reset();
  fail_call = true;
  assert(privileged_ptrace(123, 456, NULL, 0) == -1 && errno == EBUSY);
  assert(authid == ORIGINAL_AUTHID && calls == 1 && !logs);
  reset();
  memset(caps, 0, sizeof(caps));
  for (size_t i = 0; i < 4; ++i) assert(!pthread_create(&workers[i], NULL, ptrace_worker, NULL));
  for (size_t i = 0; i < 4; ++i) assert(!pthread_join(workers[i], NULL));
  assert(calls == 4000 && auth_writes == 8000 && cap_writes == 8000 && authid == ORIGINAL_AUTHID);
  for (size_t i = 0; i < sizeof(caps); ++i) assert(caps[i] == 0);
  puts("ShellCore remote: mixed ptrace/memory credentials, actual operation result, errno, restoration failures and skipped redundant credential writes passed");
  return 0;
}
