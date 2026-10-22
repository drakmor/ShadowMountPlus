#ifndef SM_PKG_SERVER_H
#define SM_PKG_SERVER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct sm_pkg_server sm_pkg_server_t;

int sm_pkg_server_start(sm_pkg_server_t **out, const char *pkg_path,
                        const char *attempt, uint16_t *port_out);
uint64_t sm_pkg_server_bytes_served(sm_pkg_server_t *server);
uint64_t sm_pkg_server_total(sm_pkg_server_t *server);
bool sm_pkg_server_complete(sm_pkg_server_t *server);
uint64_t sm_pkg_server_last_request_us(sm_pkg_server_t *server);
void sm_pkg_server_stop(sm_pkg_server_t *server);

#endif
