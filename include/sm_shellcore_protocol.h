#ifndef SM_SHELLCORE_PROTOCOL_H
#define SM_SHELLCORE_PROTOCOL_H

#include <stdint.h>

#include "sm_limits.h"
#include "sm_shellcore_protocol_defs.h"

typedef enum {
  SM_SHELLCORE_REQUEST_LAUNCH = SM_SHELLCORE_REQUEST_LAUNCH_VALUE,
  SM_SHELLCORE_REQUEST_LAUNCH_FAILED =
      SM_SHELLCORE_REQUEST_LAUNCH_FAILED_VALUE
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
