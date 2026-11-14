#ifndef SM_FPKG_DECODER_H
#define SM_FPKG_DECODER_H

#include <stddef.h>
#include <stdint.h>

#include "sm_fpkg.h"

typedef struct sm_fpkg_decoder sm_fpkg_decoder_t;

/* Decoder state is heap allocated because it owns the reusable Huffman table. */
sm_fpkg_decoder_t *sm_fpkg_decoder_create(void);
void sm_fpkg_decoder_free(sm_fpkg_decoder_t *decoder);

/* Decode one described, unshuffled Kraken block (at most 256 KiB). A zero
 * stored length with a zero descriptor produces a sparse zero-filled block,
 * matching the pinned Kraken decoder. */
sm_fpkg_status_t sm_fpkg_decode_block(sm_fpkg_decoder_t *decoder,
                                      const uint8_t *stored,
                                      size_t stored_size,
                                      uint32_t even_length, uint8_t mode,
                                      uint8_t *output, size_t output_size);

#endif
