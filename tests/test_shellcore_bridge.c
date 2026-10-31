#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/time.h>

#include "sm_shellcore_protocol.h"

extern const uint8_t sm_shellcore_bridge_blob_start[];
extern const uint8_t sm_shellcore_bridge_base_end[];
extern const uint8_t sm_shellcore_bridge_exchange[];
extern const uint8_t sm_shellcore_bridge_sandbox_hook[];
extern const uint8_t sm_shellcore_bridge_sandbox_original[];
extern const uint8_t sm_shellcore_bridge_socket[];
extern const uint8_t sm_shellcore_bridge_setsockopt[];
extern const uint8_t sm_shellcore_bridge_connect[];
extern const uint8_t sm_shellcore_bridge_read[];
extern const uint8_t sm_shellcore_bridge_write[];
extern const uint8_t sm_shellcore_bridge_close[];

static uint8_t written[SM_SHELLCORE_REQUEST_SIZE];
static size_t written_size;
static size_t read_size;
static int closes;
static int mode;
static uint16_t expected_op = SM_SHELLCORE_REQUEST_LAUNCH_VALUE;
static int test_spawn;
static int spawn_called;
static int spawn_result;
static const uint64_t session_id = UINT64_C(0x1234567890abcdef);

static int fake_socket(int domain, int type, int protocol) {
  assert(domain == 1 && type == 1 && protocol == 0);
  if (test_spawn) {
    expected_op = closes ? SM_SHELLCORE_REQUEST_PROCESS_SPAWNED_VALUE
                         : SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE;
    written_size = read_size = 0;
  }
  return 7;
}
static int fake_setsockopt(int fd, int level, int option,
                           const void *value, unsigned length) {
  assert(fd == 7 && level == 0xffff && option >= 0x1005 &&
         option <= 0x1006 && value && length == 16);
  const struct timeval *timeout = value;
  assert(timeout->tv_sec ==
         (expected_op == SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE ||
          expected_op == SM_SHELLCORE_REQUEST_PROCESS_SPAWNED_VALUE
              ? 0 : expected_op == SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE
                        ? 3 : 15));
  assert(timeout->tv_usec == (expected_op == SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE ||
                             expected_op == SM_SHELLCORE_REQUEST_PROCESS_SPAWNED_VALUE
                                 ? SM_SHELLCORE_BRIDGE_VIDEO_TIMEOUT_USEC : 0));
  return 0;
}
static int fake_connect(int fd, const void *address, unsigned length) {
  assert(fd == 7 && address && length == 31);
  return 0;
}
static long fake_write(int fd, const void *bytes, size_t size) {
  assert(fd == 7 && bytes && size && written_size < sizeof(written));
  size_t count = size < 7 ? size : 7;
  assert(count <= sizeof(written) - written_size);
  memcpy(written + written_size, bytes, count);
  written_size += count;
  return (long)count;
}
static long fake_read(int fd, void *bytes, size_t size) {
  assert(fd == 7 && bytes && size);
  if (mode == 3)
    return -1;
  sm_shellcore_response_t response;
  if (read_size < sizeof(response)) {
    response = sm_shellcore_response(session_id,
        SM_SHELLCORE_REQUEST_HELLO_VALUE, 0, 0);
  } else {
    assert(written_size == sizeof(written));
    sm_shellcore_request_t request;
    memcpy(&request, written, sizeof(request));
    assert(sm_shellcore_request_valid(&request, session_id));
    assert(request.operation == expected_op);
    if (expected_op == SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE)
      assert(request.title_id[0] == '\0' && request.pid == 42 &&
             request.module_handle == 5);
    else if (expected_op == SM_SHELLCORE_REQUEST_PROCESS_SPAWNED_VALUE)
      assert(request.title_id[0] == '\0' && request.pid == 42 &&
             request.app_id == 0 && request.module_handle == 0);
    else if (expected_op == SM_SHELLCORE_REQUEST_SANDBOX_READY_VALUE)
      assert(request.title_id[0] == '\0' && request.pid == 0 &&
             request.app_id == 9);
    else
      assert(strcmp(request.title_id, "PPSA12345") == 0);
    response = sm_shellcore_response(session_id, request.operation,
                                    request.request_id, EBUSY);
    if (mode == 1)
      response.request_id++;
    if (mode == 2)
      response.session_id++;
  }
  size_t offset = read_size % sizeof(response);
  size_t count = sizeof(response) - offset;
  if (count > size)
    count = size;
  if (count > 5)
    count = 5;
  memcpy(bytes, (const uint8_t *)&response + offset, count);
  read_size += count;
  return (long)count;
}
static int fake_close(int fd) {
  assert(fd == 7);
  closes++;
  return 0;
}

static void patch_pointer(uint8_t *copy, const uint8_t *slot,
                          uintptr_t pointer) {
  size_t offset = (size_t)(slot - sm_shellcore_bridge_blob_start);
  memcpy(copy + offset, &pointer, sizeof(pointer));
}

static int fake_spawn(uint32_t app_id, int32_t *out_pid, uint64_t a3,
                      uint64_t a4, uint64_t a5, uint64_t a6, uint64_t a7,
                      uint64_t a8, uint64_t a9, uint64_t a10) {
  assert(app_id == 9 && out_pid && a3 == 3 && a4 == 4 && a5 == 5 &&
         a6 == 6 && a7 == 7 && a8 == 8 && a9 == 9 && a10 == 10);
  *out_pid = 42;
  spawn_called++;
  return spawn_result;
}

int main(void) {
  size_t size = (size_t)(sm_shellcore_bridge_base_end -
                         sm_shellcore_bridge_blob_start);
  uint8_t *copy = mmap(NULL, 0x4000, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  assert(copy != MAP_FAILED && size <= 0x4000);
  memcpy(copy, sm_shellcore_bridge_blob_start, size);
  patch_pointer(copy, sm_shellcore_bridge_socket, (uintptr_t)fake_socket);
  patch_pointer(copy, sm_shellcore_bridge_setsockopt,
                (uintptr_t)fake_setsockopt);
  patch_pointer(copy, sm_shellcore_bridge_connect, (uintptr_t)fake_connect);
  patch_pointer(copy, sm_shellcore_bridge_read, (uintptr_t)fake_read);
  patch_pointer(copy, sm_shellcore_bridge_write, (uintptr_t)fake_write);
  patch_pointer(copy, sm_shellcore_bridge_close, (uintptr_t)fake_close);
  patch_pointer(copy, sm_shellcore_bridge_sandbox_original,
                (uintptr_t)fake_spawn);
  assert(mprotect(copy, 0x4000, PROT_READ | PROT_EXEC) == 0);
  int (*exchange)(uint16_t, const char *, uint32_t, int32_t, uint32_t) =
      (void *)(copy + (sm_shellcore_bridge_exchange -
                       sm_shellcore_bridge_blob_start));
  for (mode = 0; mode < 4; ++mode) {
    memset(written, 0, sizeof(written));
    written_size = read_size = 0;
    closes = 0;
    int result = exchange(SM_SHELLCORE_REQUEST_LAUNCH_VALUE, "PPSA12345",
                          0, 0, 0);
    assert(result == (mode == 0 ? EBUSY : -1));
    assert(closes == 1);
  }
  mode = 0;
  expected_op = SM_SHELLCORE_REQUEST_VIDEO_READY_VALUE;
  written_size = read_size = 0;
  closes = 0;
  assert(exchange(expected_op, NULL, 0, 42, 5) == EBUSY);
  assert(closes == 1);
  test_spawn = 1;
  closes = 0;
  int32_t spawned_pid = 0;
  int (*spawn)(uint32_t, int32_t *, uint64_t, uint64_t, uint64_t, uint64_t,
               uint64_t, uint64_t, uint64_t, uint64_t) =
      (void *)(copy + (sm_shellcore_bridge_sandbox_hook -
                       sm_shellcore_bridge_blob_start));
  assert(spawn(9, &spawned_pid, 3, 4, 5, 6, 7, 8, 9, 10) == 0);
  assert(spawned_pid == 42 && spawn_called == 1 && closes == 2);
  spawn_result = -EIO;
  closes = 0;
  assert(spawn(9, &spawned_pid, 3, 4, 5, 6, 7, 8, 9, 10) == -EIO);
  assert(spawn_called == 2 && closes == 1);
  assert(munmap(copy, 0x4000) == 0);
  return 0;
}
