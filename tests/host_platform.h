#ifndef SM_TEST_HOST_PLATFORM_H
#define SM_TEST_HOST_PLATFORM_H
#define _GNU_SOURCE
#define SM_PLATFORM_H
#include <assert.h>
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

static inline size_t test_strlcpy(char *dst, const char *src, size_t size) {
  size_t length = strlen(src);
  if (size > 0) {
    size_t copied = length < size - 1u ? length : size - 1u;
    memcpy(dst, src, copied);
    dst[copied] = '\0';
  }
  return length;
}
#define strlcpy test_strlcpy
#include "sm_smb.h"
#endif
