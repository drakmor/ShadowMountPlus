/* Synthetic Kraken block vectors; no game data or package fixture. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sm_fpkg_decoder.h"

static void put_be32(uint8_t *p, uint32_t value) {
  p[0] = (uint8_t)(value >> 24);
  p[1] = (uint8_t)(value >> 16);
  p[2] = (uint8_t)(value >> 8);
  p[3] = (uint8_t)value;
}

static void put_raw_array_header(uint8_t *out, size_t size) {
  assert(size <= 0x3ffffu);
  out[0] = (uint8_t)(size >> 16);
  out[1] = (uint8_t)(size >> 8);
  out[2] = (uint8_t)size;
}

static void put_bits_be(uint8_t *out, size_t *bit_at, uint32_t value,
                        unsigned count) {
  for (unsigned i = count; i > 0; --i) {
    if ((value >> (i - 1u)) & 1u)
      out[*bit_at / 8u] |= (uint8_t)(1u << (7u - (unsigned)(*bit_at % 8u)));
    ++*bit_at;
  }
}

static void test_raw_block(sm_fpkg_decoder_t *decoder) {
  uint8_t *stored = malloc(0x40000u);
  uint8_t *output = malloc(0x40000u);
  assert(stored && output);
  for (size_t i = 0; i < 0x40000u; ++i)
    stored[i] = (uint8_t)((i * 29u + i / 113u) & 0xffu);
  assert(sm_fpkg_decode_block(decoder, stored, 0x40000u, 0x20000u, 0,
                              output, 0x40000u) == SM_FPKG_OK);
  assert(memcmp(stored, output, 0x40000u) == 0);
  free(stored);
  free(output);
}

static void test_lz_all_literals(sm_fpkg_decoder_t *decoder, bool delta) {
  const size_t logical = 0x20000u;
  const size_t literal_count = logical - 8u;
  const size_t stored_size = 8u + 1u + 3u + literal_count + 3u + 3u + 3u;
  uint8_t *stored = calloc(stored_size, 1u);
  uint8_t *output = calloc(logical, 1u);
  uint8_t *expected = malloc(logical);
  assert(stored && output && expected);
  for (size_t i = 0; i < logical; ++i)
    expected[i] = (uint8_t)((i * 31u + (i / 29u) * 7u) & 0xffu);

  memcpy(stored, expected, 8u);
  size_t at = 8u;
  stored[at++] = 0x80; /* excess framing, no length escapes */
  put_raw_array_header(stored + at, literal_count);
  at += 3u;
  for (size_t i = 8u; i < logical; ++i) {
    stored[at++] = delta ? (uint8_t)(expected[i] - expected[i - 8u])
                         : expected[i];
  }
  put_raw_array_header(stored + at, 0);
  at += 3u; /* no commands */
  put_raw_array_header(stored + at, 0);
  at += 3u; /* no offsets */
  put_raw_array_header(stored + at, 0);
  at += 3u; /* no lengths */
  assert(at == stored_size);
  sm_fpkg_status_t status = sm_fpkg_decode_block(
      decoder, stored, stored_size, (uint32_t)stored_size,
      (uint8_t)(2u | (delta ? 1u : 0u)), output, logical);
  if (status != SM_FPKG_OK)
    fprintf(stderr, "synthetic LZ decode status %d (delta %d)\n", status,
            delta);
  assert(status == SM_FPKG_OK);
  assert(memcmp(output, expected, logical) == 0);
  free(expected);
  free(output);
  free(stored);
}

static void test_lz_match(sm_fpkg_decoder_t *decoder) {
  uint8_t stored[8u + 1u + 3u + 18u + 3u + 1u + 3u + 1u + 3u + 1u] = {0};
  uint8_t output[32] = {0};
  uint8_t expected[32];
  for (size_t i = 0; i < 8; ++i) expected[i] = (uint8_t)(0x30u + i);
  memcpy(expected + 8, expected, 6u); /* one six-byte match at distance 8 */
  for (size_t i = 14; i < sizeof(expected); ++i)
    expected[i] = (uint8_t)(i * 9u + 1u);

  size_t at = 0;
  memcpy(stored + at, expected, 8u); at += 8u;
  stored[at++] = 0x80;
  put_raw_array_header(stored + at, 18u); at += 3u;
  memcpy(stored + at, expected + 14u, 18u); at += 18u;
  put_raw_array_header(stored + at, 1u); at += 3u;
  stored[at++] = 0xd0; /* new offset, six-byte match, no leading literals */
  put_raw_array_header(stored + at, 1u); at += 3u;
  stored[at++] = 0; /* traditional code for distance 8 */
  put_raw_array_header(stored + at, 0); at += 3u;
  stored[at++] = 0; /* the four zero offset bits */
  assert(at == sizeof(stored));
  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored),
                              sizeof(stored), 2u, output, sizeof(output)) ==
         SM_FPKG_OK);
  assert(memcmp(output, expected, sizeof(output)) == 0);
  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored) - 1u,
                              sizeof(stored) - 1u, 2u, output,
                              sizeof(output)) == SM_FPKG_CORRUPT);
  stored[8] = 0; /* invalid excess framing */
  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored),
                              sizeof(stored), 2u, output, sizeof(output)) ==
         SM_FPKG_CORRUPT);
}

static void test_escaped_match_length(sm_fpkg_decoder_t *decoder) {
  enum { LOGICAL = 8 + 272 };
  uint8_t stored[24] = {0};
  uint8_t output[LOGICAL] = {0};
  uint8_t expected[LOGICAL];
  for (size_t i = 0; i < 8u; ++i)
    expected[i] = (uint8_t)(0x60u + i);
  for (size_t i = 8u; i < LOGICAL; ++i)
    expected[i] = expected[i - 8u];

  size_t at = 0;
  memcpy(stored + at, expected, 8u); at += 8u;
  stored[at++] = 0x81; /* One byte at the end holds the escaped length. */
  put_raw_array_header(stored + at, 0u); at += 3u;
  put_raw_array_header(stored + at, 1u); at += 3u;
  stored[at++] = 0x3c; /* No literals, recent distance 8, escaped match. */
  put_raw_array_header(stored + at, 0u); at += 3u;
  put_raw_array_header(stored + at, 1u); at += 3u;
  stored[at++] = 0xff; /* Escape marker: 255-byte base plus zero. */
  stored[at++] = 0x80; /* MSB length code 64, yielding zero excess. */
  assert(at == sizeof(stored));

  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored),
                              sizeof(stored), 2u, output, LOGICAL) ==
         SM_FPKG_OK);
  assert(memcmp(output, expected, LOGICAL) == 0);
}

static void test_single_symbol_huffman_array(sm_fpkg_decoder_t *decoder) {
  uint8_t stored[8] = {0};
  uint8_t output[32] = {0};
  stored[0] = 0x20; /* type 2, decoded size 32 */
  put_be32(stored + 1, (31u << 18) | 3u);
  stored[5] = 0x00; /* old table form, count = 1 */
  stored[6] = 0x6a;
  stored[7] = 0xc0;
  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored),
                              sizeof(stored), 0, output, sizeof(output)) ==
         SM_FPKG_OK);
  for (size_t i = 0; i < sizeof(output); ++i) assert(output[i] == 0xab);
}

static void test_two_symbol_huffman_array(sm_fpkg_decoder_t *decoder) {
  uint8_t stored[17] = {0};
  uint8_t output[32] = {0};
  uint8_t body[12] = {0};
  size_t bit_at = 0;
  put_bits_be(body, &bit_at, 0, 1); /* old table form */
  put_bits_be(body, &bit_at, 0, 1); /* full table */
  put_bits_be(body, &bit_at, 2, 8); /* two symbols */
  put_bits_be(body, &bit_at, 0, 3); /* code lengths are one bit */
  put_bits_be(body, &bit_at, 'A', 8);
  put_bits_be(body, &bit_at, 'B', 8);
  assert(bit_at == 29u);
  const uint8_t table_prefix[] = {0x00, 0x82, 0x0a, 0x10};
  assert(memcmp(body, table_prefix, sizeof(table_prefix)) == 0);
  body[4] = 2; /* stream A byte count */
  body[6] = 0x00; body[7] = 0x00; /* A: eleven zero code bits */
  body[8] = 0x00; body[9] = 0x00; /* C: ten zero code bits */
  body[10] = 0x07; body[11] = 0xff; /* reversed B: eleven one bits */
  stored[0] = 0x20;
  put_be32(stored + 1, (31u << 18) | sizeof(body));
  memcpy(stored + 5, body, sizeof(body));
  assert(sm_fpkg_decode_block(decoder, stored, sizeof(stored),
                              sizeof(stored), 0, output, sizeof(output)) ==
         SM_FPKG_OK);
  for (size_t i = 0; i < sizeof(output); ++i)
    assert(output[i] == (i % 3u == 1u ? 'B' : 'A'));
}

static uint32_t next_corpus_value(uint32_t *state) {
  uint32_t value = *state;
  value ^= value << 13;
  value ^= value >> 17;
  value ^= value << 5;
  *state = value;
  return value;
}

static void test_malformed_input_corpus(sm_fpkg_decoder_t *decoder) {
  uint32_t random = 0x6d2b79f5u;
  uint8_t stored[128];
  uint8_t output[512];
  for (unsigned iteration = 0; iteration < 16384u; ++iteration) {
    size_t stored_size = 1u + next_corpus_value(&random) % sizeof(stored);
    for (size_t i = 0; i < stored_size; ++i)
      stored[i] = (uint8_t)next_corpus_value(&random);

    switch (iteration & 3u) {
      case 0:
        stored[0] &= 0x7fu; /* long raw-array header */
        break;
      case 1:
        stored[0] = (uint8_t)(0x80u | (stored[0] & 0x0fu));
        break;
      case 2:
        stored[0] = (uint8_t)(0x20u | (stored[0] & 0x1fu));
        break;
      default:
        stored[0] = (uint8_t)(0xa0u | (stored[0] & 0x0fu));
        break;
    }

    size_t output_size = 1u + next_corpus_value(&random) % sizeof(output);
    uint32_t even_length = (uint32_t)stored_size;
    if ((iteration & 7u) == 7u)
      even_length = 1u + next_corpus_value(&random) % stored_size;
    uint8_t mode = (uint8_t)(next_corpus_value(&random) & 0x1fu);
    memset(output, 0xa5, sizeof(output));
    sm_fpkg_status_t status = sm_fpkg_decode_block(
        decoder, stored, stored_size, even_length, mode, output, output_size);
    assert(status >= SM_FPKG_OK && status <= SM_FPKG_CANCELLED);
  }
}

int main(void) {
  sm_fpkg_decoder_t *decoder = sm_fpkg_decoder_create();
  assert(decoder);
  test_raw_block(decoder);
  test_lz_all_literals(decoder, false);
  test_lz_all_literals(decoder, true);
  test_lz_match(decoder);
  test_escaped_match_length(decoder);
  test_single_symbol_huffman_array(decoder);
  test_two_symbol_huffman_array(decoder);
  test_malformed_input_corpus(decoder);
  uint8_t output[16];
  memset(output, 0xa5, sizeof(output));
  assert(sm_fpkg_decode_block(decoder, NULL, 0, 0, 0, output,
                              sizeof(output)) == SM_FPKG_OK);
  for (size_t i = 0; i < sizeof(output); ++i)
    assert(output[i] == 0);
  const uint8_t raw[] = {0x31, 0x42, 0x53, 0x64};
  assert(sm_fpkg_decode_block(decoder, raw, sizeof(raw), sizeof(raw), 0x80,
                              output, sizeof(raw)) == SM_FPKG_OK);
  assert(memcmp(output, raw, sizeof(raw)) == 0);
  sm_fpkg_decoder_free(decoder);
  puts("synthetic Kraken decoder vectors and 16,384 malformed-input cases passed");
  return 0;
}
