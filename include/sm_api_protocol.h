#ifndef SM_API_PROTOCOL_H
#define SM_API_PROTOCOL_H

#include <stdint.h>

#define SM_API_SOCKET_PATH "/system_tmp/shadowmount-api.sock"
#define SM_API_MAGIC 0x50414d53u /* "SMAP" in little endian */
#define SM_API_VERSION 1u
#define SM_API_PATH_SIZE 1024u
#define SM_API_TITLE_ID_SIZE 32u
#define SM_API_TITLE_NAME_SIZE 256u
#define SM_API_VERSION_STRING_SIZE 64u
#define SM_API_MAX_PAGE_ITEMS 16u

typedef enum {
  SM_API_GET_VERSION = 1,
  SM_API_LIST_IMAGES = 2,
  SM_API_LIST_GAMES = 3,
  SM_API_MOUNT_GAME = 4,
  SM_API_UNMOUNT_GAME = 5,
} sm_api_operation_t;

enum {
  SM_API_CAP_LIST_IMAGES = 1u << 0,
  SM_API_CAP_LIST_GAMES = 1u << 1,
  SM_API_CAP_MOUNT_GAME = 1u << 2,
  SM_API_CAP_UNMOUNT_GAME = 1u << 3,
};

enum {
  SM_API_IMAGE_COMPLETE = 1u << 0,
  SM_API_IMAGE_SOURCE_AVAILABLE = 1u << 1,
  SM_API_IMAGE_MAPPED = 1u << 2,
  SM_API_IMAGE_MOUNTED = 1u << 3,
};

enum {
  SM_API_GAME_INSTALLED = 1u << 0,
  SM_API_GAME_MANAGED = 1u << 1,
  SM_API_GAME_MOUNTED = 1u << 2,
  SM_API_GAME_IMAGE_BACKED = 1u << 3,
  SM_API_GAME_SOURCE_AVAILABLE = 1u << 4,
};

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint32_t size;
  uint32_t offset;
  uint32_t limit;
  uint32_t flags;
  char title_id[SM_API_TITLE_ID_SIZE];
  uint8_t reserved[8];
} sm_api_request_t;

typedef struct {
  uint32_t magic;
  uint16_t version;
  uint16_t operation;
  uint32_t size;
  int32_t status;
  uint32_t total_count;
  uint32_t item_count;
  uint32_t item_size;
  uint32_t flags;
} sm_api_response_t;

typedef struct {
  uint32_t api_version;
  uint32_t capabilities;
  char shadowmount_version[SM_API_VERSION_STRING_SIZE];
} sm_api_version_info_t;

typedef struct {
  char path[SM_API_PATH_SIZE];
  char mount_point[SM_API_PATH_SIZE];
  int64_t size;
  int64_t mtime_sec;
  int32_t mtime_nsec;
  int32_t unit_id;
  uint32_t backend;
  uint32_t flags;
} sm_api_image_info_t;

typedef struct {
  char path[SM_API_PATH_SIZE];
  char image_path[SM_API_PATH_SIZE];
  char title_id[SM_API_TITLE_ID_SIZE];
  char title_name[SM_API_TITLE_NAME_SIZE];
  uint32_t flags;
  uint32_t reserved;
} sm_api_game_info_t;

#endif
