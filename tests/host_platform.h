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
#ifdef __APPLE__
#include <sys/mount.h>
#endif
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
#ifdef strlcpy
#undef strlcpy
#endif
#define strlcpy test_strlcpy
#define IOVEC_ENTRY(x)                                                        \
  {(void *)(x),                                                               \
   ((const char *)(x) == NULL ? 0u : (size_t)(strlen((const char *)(x)) + 1u))}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))
#ifndef MNT_BYFSID
#define MNT_BYFSID 0x08000000
#endif
int sceAppInstUtilAppUnInstall(const char *title_id);
int sceAppInstUtilAppInstallAll(void *reserved);
int sceKernelUsleep(unsigned int microseconds);
uint32_t sm_firmware_major_version(void);
int nmount(struct iovec *iov, unsigned int iov_count, int flags);
#include "sm_smb.h"
#endif
