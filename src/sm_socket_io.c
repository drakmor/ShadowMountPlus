#include "sm_platform.h"

#include <sys/socket.h>

#include "sm_socket_io.h"

bool sm_socket_read_full(int fd, void *buffer, size_t size) {
  uint8_t *bytes = (uint8_t *)buffer;
  while (size > 0) {
    ssize_t count = recv(fd, bytes, size, 0);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
}

bool sm_socket_write_full(int fd, const void *buffer, size_t size) {
  const uint8_t *bytes = (const uint8_t *)buffer;
  while (size > 0) {
    ssize_t count = send(fd, bytes, size, 0);
    if (count == 0)
      return false;
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return false;
    }
    bytes += (size_t)count;
    size -= (size_t)count;
  }
  return true;
}
