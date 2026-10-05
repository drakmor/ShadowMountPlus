#include "sm_platform.h"

#include <pthread.h>

#include "sm_log.h"
#include "sm_ucred.h"

static pthread_mutex_t g_ucred_mutex = PTHREAD_MUTEX_INITIALIZER;

void sm_ucred_lock(void) { pthread_mutex_lock(&g_ucred_mutex); }

void sm_ucred_unlock(void) { pthread_mutex_unlock(&g_ucred_mutex); }

int sm_ucred_with_authid(uint64_t authid, int (*fn)(void *), void *arg) {
  pid_t self = getpid();
  sm_ucred_lock();
  uint64_t saved = kernel_get_ucred_authid(self);
  if (!saved || kernel_set_ucred_authid(self, authid) != 0) {
    sm_ucred_unlock();
    return -1;
  }
  int result = fn(arg);
  bool restored = kernel_set_ucred_authid(self, saved) == 0;
  sm_ucred_unlock();
  if (!restored) {
    log_debug("  [UCRED] could not restore authid 0x%016llx",
              (unsigned long long)saved);
    return -1;
  }
  return result;
}
