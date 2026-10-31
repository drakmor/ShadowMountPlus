#ifndef SM_SHELLCORE_PROTOCOL_H
#define SM_SHELLCORE_PROTOCOL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "sm_limits.h"
#include "sm_shellcore_protocol_defs.h"

typedef enum {
  SM_SHELLCORE_REQUEST_LAUNCH = SM_SHELLCORE_REQUEST_LAUNCH_VALUE,
  SM_SHELLCORE_REQUEST_LAUNCH_FAILED =
      SM_SHELLCORE_REQUEST_LAUNCH_FAILED_VALUE,
  SM_SHELLCORE_REQUEST_SANDBOX_READY =
      SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE,
  SM_SHELLCORE_REQUEST_VIDEO_READY = SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE,
  SM_SHELLCORE_REQUEST_PROCESS_SPAWNED =
      SM_SHELLCORE_REQUEST_PROCESS_SPAWNED_VALUE
} sm_shellcore_request_op_t;

// Fixed wire layouts, shared by ShellCore and future process-loader clients.
typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint64_t session_id;
  uint64_t request_id;
  int32_t pid;
  uint32_t app_id;
  uint32_t module_handle;
  uint32_t size;
  char title_id[MAX_TITLE_ID];
} sm_shellcore_request_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint64_t session_id;
  uint64_t request_id;
  int32_t status;
  uint32_t size;
} sm_shellcore_response_t;

_Static_assert(sizeof(sm_shellcore_request_t) == SM_SHELLCORE_REQUEST_SIZE, "IPC request size");
_Static_assert(sizeof(sm_shellcore_response_t) == SM_SHELLCORE_RESPONSE_SIZE, "IPC response size");
_Static_assert(offsetof(sm_shellcore_request_t, title_id) == SM_SHELLCORE_REQUEST_TITLE_OFFSET, "IPC title offset");
_Static_assert(offsetof(sm_shellcore_request_t, app_id) == SM_SHELLCORE_REQUEST_APP_ID_OFFSET, "IPC app offset");

static inline sm_shellcore_response_t sm_shellcore_response(
    uint64_t session_id, uint16_t operation, uint64_t request_id, int32_t status) {
  return (sm_shellcore_response_t){SM_SHELLCORE_PROTOCOL_MAGIC,
      SM_SHELLCORE_PROTOCOL_VERSION, operation, session_id, request_id,
      status, SM_SHELLCORE_RESPONSE_SIZE};
}

static inline bool sm_shellcore_request_valid(const sm_shellcore_request_t *request,
                                               uint64_t session_id) {
  return request->magic == SM_SHELLCORE_PROTOCOL_MAGIC &&
      request->version == SM_SHELLCORE_PROTOCOL_VERSION &&
      request->size == SM_SHELLCORE_REQUEST_SIZE && session_id &&
      request->session_id == session_id && request->request_id && request->pid >= 0 &&
      request->title_id[MAX_TITLE_ID - 1] == '\0';
}

static inline bool sm_shellcore_response_matches(const sm_shellcore_response_t *response,
                                                  const sm_shellcore_request_t *request) {
  return response->magic == SM_SHELLCORE_PROTOCOL_MAGIC &&
      response->version == SM_SHELLCORE_PROTOCOL_VERSION &&
      response->size == SM_SHELLCORE_RESPONSE_SIZE && response->session_id &&
      response->session_id == request->session_id && response->request_id == request->request_id &&
      response->operation == request->operation && response->status >= 0;
}

#endif
