#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_fpkg_decoder.h"

#define MAX_INPUT_STORED (2u * 0x40000u)
#define MAX_OUTPUT_SIZE 0x40000u

static uint32_t get_le32(const uint8_t *bytes) {
  return (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 |
         (uint32_t)bytes[2] << 16 | (uint32_t)bytes[3] << 24;
}

static int read_exact(FILE *stream, void *buffer, size_t length) {
  return fread(buffer, 1u, length, stream) == length ? 0 : -1;
}

int main(void) {
  sm_fpkg_decoder_t *decoder = sm_fpkg_decoder_create();
  if (!decoder)
    return 2;

  for (;;) {
    uint8_t header[13];
    int first = fgetc(stdin);
    if (first == EOF) {
      if (ferror(stdin)) {
        sm_fpkg_decoder_free(decoder);
        return 2;
      }
      break;
    }
    header[0] = (uint8_t)first;
    if (read_exact(stdin, header + 1u, sizeof(header) - 1u) != 0) {
      sm_fpkg_decoder_free(decoder);
      return 2;
    }

    uint32_t stored_size = get_le32(header);
    uint32_t output_size = get_le32(header + 4u);
    uint32_t even_length = get_le32(header + 8u);
    uint8_t mode = header[12];
    if (stored_size > MAX_INPUT_STORED || output_size == 0 ||
        output_size > MAX_OUTPUT_SIZE || even_length > stored_size) {
      sm_fpkg_decoder_free(decoder);
      return 2;
    }

    uint8_t *stored = stored_size ? malloc(stored_size) : NULL;
    uint8_t *output = malloc(output_size);
    if ((stored_size && !stored) || !output) {
      free(stored);
      free(output);
      sm_fpkg_decoder_free(decoder);
      return 2;
    }
    if (stored_size && read_exact(stdin, stored, stored_size) != 0) {
      free(stored);
      free(output);
      sm_fpkg_decoder_free(decoder);
      return 2;
    }

    memset(output, 0xa5, output_size);
    sm_fpkg_status_t status = sm_fpkg_decode_block(
        decoder, stored, stored_size, even_length, mode, output, output_size);
    uint8_t accepted = status == SM_FPKG_OK;
    if (fwrite(&accepted, 1u, 1u, stdout) != 1u ||
        (accepted && fwrite(output, 1u, output_size, stdout) != output_size)) {
      free(stored);
      free(output);
      sm_fpkg_decoder_free(decoder);
      return 2;
    }
    free(stored);
    free(output);
  }

  sm_fpkg_decoder_free(decoder);
  return fflush(stdout) == 0 ? 0 : 2;
}
