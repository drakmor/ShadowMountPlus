#ifndef SM_TIME_H
#define SM_TIME_H

#include <stdint.h>

// Return a monotonic timestamp in microseconds, or 0 on failure.
uint64_t monotonic_time_us(void);

#endif
