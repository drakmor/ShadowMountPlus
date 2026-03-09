#include "sm_platform.h"

#include "sm_time.h"

uint64_t monotonic_time_us(void) {
  struct timespec ts;
  memset(&ts, 0, sizeof(ts));
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
    return 0;

  return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}
