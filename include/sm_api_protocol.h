#ifndef SM_API_PROTOCOL_H
#define SM_API_PROTOCOL_H

#define SM_API_DEFAULT_BIND_ADDRESS "127.0.0.1"
#define SM_API_DEFAULT_PORT 10101u
#define SM_API_VERSION 1u
#define SM_API_MAX_HTTP_HEADER_SIZE 8192u
#define SM_API_MAX_JSON_BODY_SIZE 4096u

#define SM_API_ROUTE_VERSION "/api/v1/version"
#define SM_API_ROUTE_IMAGES "/api/v1/images"
#define SM_API_ROUTE_GAMES "/api/v1/games"
#define SM_API_ROUTE_MOUNT "/api/v1/games/mount"
#define SM_API_ROUTE_UNMOUNT "/api/v1/games/unmount"

#endif
