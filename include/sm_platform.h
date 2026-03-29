#ifndef SM_PLATFORM_H
#define SM_PLATFORM_H

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mdioctl.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <ps5/kernel.h>

typedef struct {
  uint32_t app_id;
  uint64_t unknown1;
  char title_id[14];
  char unknown2[0x3c];
} app_info_t;

// --- SDK Imports ---
int sceAppInstUtilInitialize(void);
int sceAppInstUtilAppInstallAll(void);
int sceAppInstUtilAppInstallTitleDir(const char *title_id,
                                     const char *install_path, void *reserved);
                                     
int sceKernelGetAppInfo(pid_t pid, app_info_t *info);
int sceKernelUsleep(unsigned int microseconds);
int sceUserServiceInitialize(void *);
void sceUserServiceTerminate(void);

// Standard Notification
typedef struct notify_request {
  char unused[45];
  char message[3075];
} notify_request_t;
int sceKernelSendNotificationRequest(int, notify_request_t *, size_t, int);

#define IOVEC_ENTRY(x)                                                        \
  {(void *)(x),                                                               \
   ((const char *)(x) == NULL ? 0u : (size_t)(strlen((const char *)(x)) + 1u))}
#define IOVEC_SIZE(x) (sizeof(x) / sizeof(struct iovec))

#endif
