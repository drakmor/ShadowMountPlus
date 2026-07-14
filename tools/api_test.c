#include "sm_platform.h"

#include <sys/socket.h>
#include <sys/un.h>

#include "sm_api_protocol.h"

#define API_TEST_MAX_PAYLOAD (1024u * 1024u)

static bool socket_write_full(int fd, const void *buffer, size_t size) {
  const uint8_t *bytes = (const uint8_t *)buffer;
  while (size > 0) {
    ssize_t count = send(fd, bytes, size, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
}

static bool socket_read_full(int fd, void *buffer, size_t size) {
  uint8_t *bytes = (uint8_t *)buffer;
  while (size > 0) {
    ssize_t count = recv(fd, bytes, size, 0);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return false;
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
}

static void notify_result(const char *message) {
  notify_request_t request;
  memset(&request, 0, sizeof(request));
  (void)strlcpy(request.message, message, sizeof(request.message));
  (void)sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
}

static bool api_request(uint16_t operation, uint32_t offset, uint32_t limit,
                        const char *title_id, sm_api_response_t *response,
                        void **payload_out) {
  *payload_out = NULL;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0)
    return false;

  struct sockaddr_un address;
  memset(&address, 0, sizeof(address));
  address.sun_family = AF_UNIX;
  (void)strlcpy(address.sun_path, SM_API_SOCKET_PATH,
                sizeof(address.sun_path));
  if (connect(fd, (struct sockaddr *)&address, sizeof(address)) != 0) {
    close(fd);
    return false;
  }

  sm_api_request_t request;
  memset(&request, 0, sizeof(request));
  request.magic = SM_API_MAGIC;
  request.version = SM_API_VERSION;
  request.operation = operation;
  request.size = sizeof(request);
  request.offset = offset;
  request.limit = limit;
  if (title_id)
    (void)strlcpy(request.title_id, title_id, sizeof(request.title_id));

  bool ok = socket_write_full(fd, &request, sizeof(request)) &&
            socket_read_full(fd, response, sizeof(*response));
  if (!ok)
    goto done;
  if (response->magic != SM_API_MAGIC ||
      response->version != SM_API_VERSION ||
      response->operation != operation ||
      response->size < sizeof(*response)) {
    errno = EPROTO;
    ok = false;
    goto done;
  }

  uint32_t payload_size = response->size - sizeof(*response);
  if (payload_size > API_TEST_MAX_PAYLOAD ||
      (response->item_count != 0 &&
       (response->item_size == 0 ||
        response->item_count > payload_size / response->item_size)) ||
      (uint64_t)response->item_count * response->item_size != payload_size) {
    errno = EPROTO;
    ok = false;
    goto done;
  }
  if (payload_size > 0) {
    void *payload = malloc(payload_size);
    if (!payload || !socket_read_full(fd, payload, payload_size)) {
      free(payload);
      ok = false;
      goto done;
    }
    *payload_out = payload;
  }

done:
  close(fd);
  return ok;
}

static bool test_version(void) {
  sm_api_response_t response;
  void *payload = NULL;
  if (!api_request(SM_API_GET_VERSION, 0, 0, NULL, &response, &payload))
    return false;
  bool ok = response.status == 0 && response.item_count == 1 &&
            response.item_size == sizeof(sm_api_version_info_t);
  if (ok) {
    const sm_api_version_info_t *version =
        (const sm_api_version_info_t *)payload;
    printf("[API-TEST] ShadowMount=%s API=%u capabilities=0x%08X\n",
           version->shadowmount_version, version->api_version,
           version->capabilities);
  }
  free(payload);
  return ok;
}

static bool test_image_list(uint32_t *total_out) {
  uint32_t offset = 0;
  uint32_t total = 0;
  do {
    sm_api_response_t response;
    void *payload = NULL;
    if (!api_request(SM_API_LIST_IMAGES, offset, SM_API_MAX_PAGE_ITEMS, NULL,
                     &response, &payload)) {
      return false;
    }
    bool ok = response.status == 0 &&
              response.item_size == sizeof(sm_api_image_info_t) &&
              response.item_count <= SM_API_MAX_PAGE_ITEMS;
    if (!ok) {
      free(payload);
      return false;
    }
    total = response.total_count;
    if (response.item_count == 0 && offset < total) {
      free(payload);
      return false;
    }
    offset += response.item_count;
    free(payload);
  } while (offset < total);
  *total_out = total;
  printf("[API-TEST] images=%u\n", total);
  return true;
}

static bool test_game_list(uint32_t *total_out,
                           char mount_candidate[SM_API_TITLE_ID_SIZE]) {
  uint32_t offset = 0;
  uint32_t total = 0;
  mount_candidate[0] = '\0';
  do {
    sm_api_response_t response;
    void *payload = NULL;
    if (!api_request(SM_API_LIST_GAMES, offset, SM_API_MAX_PAGE_ITEMS, NULL,
                     &response, &payload)) {
      return false;
    }
    bool ok = response.status == 0 &&
              response.item_size == sizeof(sm_api_game_info_t) &&
              response.item_count <= SM_API_MAX_PAGE_ITEMS;
    if (!ok) {
      free(payload);
      return false;
    }
    const sm_api_game_info_t *games = (const sm_api_game_info_t *)payload;
    for (uint32_t i = 0; i < response.item_count; ++i) {
      uint32_t required = SM_API_GAME_MANAGED | SM_API_GAME_SOURCE_AVAILABLE;
      if (mount_candidate[0] == '\0' &&
          (games[i].flags & required) == required &&
          (games[i].flags & SM_API_GAME_MOUNTED) == 0) {
        (void)strlcpy(mount_candidate, games[i].title_id,
                      SM_API_TITLE_ID_SIZE);
      }
    }
    total = response.total_count;
    if (response.item_count == 0 && offset < total) {
      free(payload);
      return false;
    }
    offset += response.item_count;
    free(payload);
  } while (offset < total);
  *total_out = total;
  printf("[API-TEST] games=%u candidate=%s\n", total,
         mount_candidate[0] ? mount_candidate : "none");
  return true;
}

static bool test_mount_cycle(const char *title_id, bool *skipped_out) {
  *skipped_out = title_id[0] == '\0';
  if (*skipped_out)
    return true;

  sm_api_response_t response;
  void *payload = NULL;
  if (!api_request(SM_API_MOUNT_GAME, 0, 0, title_id, &response, &payload))
    return false;
  free(payload);
  if (response.status == EBUSY) {
    *skipped_out = true;
    printf("[API-TEST] mount cycle skipped: %s is busy\n", title_id);
    return true;
  }
  if (response.status != 0)
    return false;

  payload = NULL;
  if (!api_request(SM_API_UNMOUNT_GAME, 0, 0, title_id, &response, &payload))
    return false;
  free(payload);
  printf("[API-TEST] mount cycle: %s unmount_status=%d\n", title_id,
         response.status);
  return response.status == 0;
}

int main(void) {
  sceUserServiceInitialize(0);
  uint32_t image_count = 0;
  uint32_t game_count = 0;
  char candidate[SM_API_TITLE_ID_SIZE];
  bool cycle_skipped = false;

  bool ok = test_version() && test_image_list(&image_count) &&
            test_game_list(&game_count, candidate) &&
            test_mount_cycle(candidate, &cycle_skipped);
  char message[256];
  (void)snprintf(message, sizeof(message),
                 "ShadowMount API test: %s\nimages=%u games=%u mount=%s",
                 ok ? "PASS" : "FAIL", image_count, game_count,
                 cycle_skipped ? "SKIP" : (ok ? "PASS" : "FAIL"));
  notify_result(message);
  printf("[API-TEST] %s\n", message);
  sceUserServiceTerminate();
  return ok ? 0 : 1;
}
