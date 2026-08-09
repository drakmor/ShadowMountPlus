#ifndef SM_SHELLCORE_PROTOCOL_H
#define SM_SHELLCORE_PROTOCOL_H

#include <stdint.h>

#include "sm_limits.h"

#define SM_SHELLCORE_SOCKET_PATH "/system_tmp/shadowmount.sock"
#define SM_SHELLCORE_PROTOCOL_MAGIC 0x534d5343u
#define SM_SHELLCORE_PROTOCOL_VERSION 1u

typedef enum {
  SM_SHELLCORE_REQUEST_LAUNCH = 1,
  SM_SHELLCORE_REQUEST_LAUNCH_FAILED = 2
} sm_shellcore_request_op_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  char title_id[MAX_TITLE_ID];
} sm_shellcore_request_t;

typedef struct {
  int32_t status;
} sm_shellcore_response_t;

#endif
