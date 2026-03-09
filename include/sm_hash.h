#ifndef SM_HASH_H
#define SM_HASH_H

#include <stdint.h>

// Return a stable 32-bit FNV-1a hash for a NUL-terminated string.
static inline uint32_t sm_fnv1a32(const char *s) {
  uint32_t h = 2166136261u;
  while (s && *s) {
    h ^= (uint8_t)(*s++);
    h *= 16777619u;
  }
  return h;
}

#endif
