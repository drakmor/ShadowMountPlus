#ifndef SM_API_PROTOCOL_H
#define SM_API_PROTOCOL_H

#define SM_API_DEFAULT_BIND_ADDRESS "127.0.0.1"
#define SM_API_DEFAULT_PORT 10101u
#define SM_API_VERSION 1u
#define SM_API_MAX_HTTP_HEADER_SIZE 8192u
#define SM_API_MAX_JSON_BODY_SIZE 4096u

#define SM_API_ROUTE_INDEX "/"
#define SM_API_ROUTE_VERSION "/api/v1/version"
#define SM_API_ROUTE_STORAGE "/api/v1/storage"
#define SM_API_ROUTE_IMAGES "/api/v1/images"
#define SM_API_ROUTE_GAMES "/api/v1/games"
#define SM_API_ROUTE_GAME_INFO "/api/v1/games/info"
#define SM_API_ROUTE_GAME_ICON "/api/v1/games/icon"
#define SM_API_ROUTE_MOUNT "/api/v1/games/mount"
#define SM_API_ROUTE_UNMOUNT "/api/v1/games/unmount"
#define SM_API_ROUTE_UNINSTALL "/api/v1/games/uninstall"
#define SM_API_ROUTE_GAME_MOVE "/api/v1/games/move"
#define SM_API_ROUTE_GAME_COPY "/api/v1/games/copy"
#define SM_API_ROUTE_GAME_DELETE "/api/v1/games/delete"
#define SM_API_ROUTE_GAME_STORAGE_STATUS "/api/v1/games/storage/status"
#define SM_API_ROUTE_GAME_STORAGE_CANCEL "/api/v1/games/storage/cancel"
#define SM_API_ROUTE_MANUAL_LIST "/api/v1/manual/list"
#define SM_API_ROUTE_MANUAL_ADD "/api/v1/manual/add"
#define SM_API_ROUTE_MANUAL_REMOVE "/api/v1/manual/remove"
#define SM_API_ROUTE_SCAN "/api/v1/scan"

#endif
