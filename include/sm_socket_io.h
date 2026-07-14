#ifndef SM_SOCKET_IO_H
#define SM_SOCKET_IO_H

#include <stdbool.h>
#include <stddef.h>

bool sm_socket_read_full(int fd, void *buffer, size_t size);
bool sm_socket_write_full(int fd, const void *buffer, size_t size);

#endif
