/*
 * Bounded native Kraken block decoder.
 *
 * This is a C port of the decoder in quer3q/kraken-mount at
 * 4b420d579bd381796d773875e36d14c9acaabf39 (GPL-3.0-or-later), whose format
 * implementation is attributed to ps5-dump-forge at
 * 8a3c7d3e4bfeac3b4ddf8e9ff020e819ae8ff1cd. Keep this implementation bounded
 * to one 256 KiB logical block and caller-owned reusable state.
 */
#include "sm_fpkg_decoder.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define K_HALF 0x20000u
#define K_BLOCK 0x40000u
#define K_SEED 8u
#define K_LUT_BITS 11u
#define K_LUT_SIZE (1u << K_LUT_BITS)

struct sm_fpkg_decoder {
  uint16_t lut[K_LUT_SIZE];
};

typedef struct {
  const uint8_t *src;
  size_t start, end, next, pad;
  uint64_t acc;
  unsigned n;
  bool back;
} lsb_reader_t;

typedef struct {
  const uint8_t *src;
  size_t start, end;
  size_t consumed_bits;
  bool back;
} msb_reader_t;

typedef struct {
  uint8_t *bytes;
  size_t size;
} byte_array_t;

static uint32_t msb_get(msb_reader_t *reader, unsigned count) {
  uint32_t value = 0;
  for (unsigned i = 0; i < count; ++i) {
    size_t byte_index;
    unsigned bit_index;
    if (!reader->back) {
      size_t absolute = reader->start * 8u + reader->consumed_bits;
      byte_index = absolute / 8u;
      bit_index = 7u - (unsigned)(absolute % 8u);
    } else {
      size_t distance = reader->consumed_bits / 8u;
      byte_index = distance < reader->end - reader->start
                       ? reader->end - 1u - distance
                       : SIZE_MAX;
      bit_index = 7u - (unsigned)(reader->consumed_bits % 8u);
    }
    uint8_t bit = byte_index >= reader->start && byte_index < reader->end
                      ? (uint8_t)((reader->src[byte_index] >> bit_index) & 1u)
                      : 0;
    value = (value << 1) | bit;
    ++reader->consumed_bits;
  }
  return value;
}

static unsigned msb_zeros(const msb_reader_t *reader) {
  for (unsigned i = 0; i < 64; ++i) {
    size_t byte_index;
    unsigned bit_index;
    if (!reader->back) {
      size_t absolute = reader->start * 8u + reader->consumed_bits + i;
      byte_index = absolute / 8u;
      bit_index = 7u - (unsigned)(absolute % 8u);
    } else {
      size_t consumed = reader->consumed_bits + i;
      size_t distance = consumed / 8u;
      byte_index = distance < reader->end - reader->start
                       ? reader->end - 1u - distance
                       : SIZE_MAX;
      bit_index = 7u - (unsigned)(consumed % 8u);
    }
    uint8_t bit = byte_index >= reader->start && byte_index < reader->end
                      ? (uint8_t)((reader->src[byte_index] >> bit_index) & 1u)
                      : 0;
    if (bit) return i;
  }
  return 64;
}

static size_t msb_seam(const msb_reader_t *reader) {
  size_t used = (reader->consumed_bits + 7u) / 8u;
  if (used > reader->end - reader->start) return SIZE_MAX;
  return reader->back ? reader->end - used : reader->start + used;
}

static uint32_t reverse_bits32(uint32_t value) {
  value = ((value & 0x55555555u) << 1) | ((value >> 1) & 0x55555555u);
  value = ((value & 0x33333333u) << 2) | ((value >> 2) & 0x33333333u);
  value = ((value & 0x0f0f0f0fu) << 4) | ((value >> 4) & 0x0f0f0f0fu);
  value = ((value & 0x00ff00ffu) << 8) | ((value >> 8) & 0x00ff00ffu);
  return (value << 16) | (value >> 16);
}

static void lsb_init(lsb_reader_t *reader, const uint8_t *src, size_t start,
                     size_t end, bool back) {
  memset(reader, 0, sizeof(*reader));
  reader->src = src;
  reader->start = start;
  reader->end = end;
  reader->next = back ? end : start;
  reader->back = back;
}

static size_t lsb_left(const lsb_reader_t *reader) {
  return reader->back ? reader->next - reader->start
                      : reader->end - reader->next;
}

static void lsb_refill(lsb_reader_t *reader) {
  while (reader->n < K_LUT_BITS) {
    uint8_t byte = 0;
    if (lsb_left(reader) == 0) {
      ++reader->pad;
    } else if (reader->back) {
      byte = reader->src[--reader->next];
    } else {
      byte = reader->src[reader->next++];
    }
    reader->acc |= (uint64_t)byte << reader->n;
    reader->n += 8;
  }
}

static size_t lsb_bytes_used(const lsb_reader_t *reader) {
  size_t passed = reader->back ? reader->end - reader->next
                               : reader->next - reader->start;
  size_t bits = 8u * (passed + reader->pad) - reader->n;
  return (bits + 7u) / 8u;
}

static sm_fpkg_status_t huffman_decode(sm_fpkg_decoder_t *decoder,
                                       const uint8_t *src, size_t src_size,
                                       uint8_t *output, size_t output_size) {
  msb_reader_t bits = {.src = src, .start = 0, .end = src_size};
  if (msb_get(&bits, 1) != 0) return SM_FPKG_UNSUPPORTED;
  uint8_t symbols[256], lengths[256];
  size_t count = 0;
  if (msb_get(&bits, 1) == 1) {
    unsigned fb = msb_get(&bits, 2);
    unsigned limit = 20u >> fb;
    bool skip = msb_get(&bits, 1) == 1;
    unsigned sym = 0;
    int avg = 32;
    while (sym != 256) {
      if (skip) {
        skip = false;
      } else {
        unsigned zeros = msb_zeros(&bits);
        if (zeros > 7) return SM_FPKG_CORRUPT;
        uint32_t encoded = msb_get(&bits, 2u * (zeros + 1u));
        if (encoded == 0 || encoded - 1u > 256u - sym)
          return SM_FPKG_CORRUPT;
        sym += encoded - 1u;
        if (sym >= 256) break;
      }
      unsigned zeros = msb_zeros(&bits);
      if (zeros > 7) return SM_FPKG_CORRUPT;
      uint32_t run = msb_get(&bits, 2u * (zeros + 1u)) - 1u;
      if (run == 0 || run > 256u - sym || run > 256u - count)
        return SM_FPKG_CORRUPT;
      for (uint32_t i = 0; i < run; ++i) {
        zeros = msb_zeros(&bits);
        if (zeros > limit) return SM_FPKG_CORRUPT;
        (void)msb_get(&bits, zeros + 1u);
        uint32_t low = fb ? msb_get(&bits, fb) : 0;
        int v = (int)((zeros << fb) | low);
        int delta = (-(v & 1) ^ (v >> 1));
        int length = delta + ((avg + 2) >> 2);
        if (length < 1 || length > (int)K_LUT_BITS)
          return SM_FPKG_CORRUPT;
        avg = length + ((3 * avg + 2) >> 2);
        symbols[count] = (uint8_t)sym++;
        lengths[count++] = (uint8_t)length;
      }
    }
    if (sym != 256 || count < 2) return SM_FPKG_CORRUPT;
  } else {
    unsigned entries = msb_get(&bits, 8);
    if (entries == 0) return SM_FPKG_CORRUPT;
    if (entries == 1) {
      uint8_t symbol = (uint8_t)msb_get(&bits, 8);
      if (msb_seam(&bits) != src_size) return SM_FPKG_CORRUPT;
      memset(output, symbol, output_size);
      return SM_FPKG_OK;
    }
    unsigned cb = msb_get(&bits, 3);
    if (cb > 4 || entries > 256) return SM_FPKG_CORRUPT;
    for (unsigned i = 0; i < entries; ++i) {
      symbols[count] = (uint8_t)msb_get(&bits, 8);
      unsigned length = msb_get(&bits, cb) + 1u;
      if (length > K_LUT_BITS) return SM_FPKG_CORRUPT;
      lengths[count++] = (uint8_t)length;
    }
  }

  unsigned max_length = 0;
  for (size_t i = 0; i < count; ++i)
    if (lengths[i] > max_length) max_length = lengths[i];
  size_t starts[K_LUT_BITS + 2] = {0};
  for (size_t i = 0; i < count; ++i) ++starts[lengths[i] + 1u];
  for (unsigned i = 1; i < sizeof(starts) / sizeof(starts[0]); ++i)
    starts[i] += starts[i - 1];
  uint8_t ordered_symbols[256], ordered_lengths[256];
  for (size_t i = 0; i < count; ++i) {
    size_t at = starts[lengths[i]]++;
    ordered_symbols[at] = symbols[i];
    ordered_lengths[at] = lengths[i];
  }
  /* Restore bucket starts after the stable counting sort. */
  size_t slot = 0;
  for (size_t i = 0; i < count; ++i) {
    unsigned length = ordered_lengths[i];
    size_t step = (size_t)1u << (max_length - length);
    if (slot + step > ((size_t)1u << max_length))
      return SM_FPKG_CORRUPT;
    while (slot < ((size_t)1u << max_length)) {
      uint32_t reversed = reverse_bits32((uint32_t)slot);
      size_t base = reversed >> (32u - max_length);
      size_t repetitions = (size_t)1u << (max_length - length);
      for (size_t j = 0; j < repetitions; ++j) {
        size_t index = base + (j << length);
        decoder->lut[index] = (uint16_t)((ordered_symbols[i] << 8) | length);
      }
      slot += step;
      break;
    }
  }
  if (slot != ((size_t)1u << max_length)) return SM_FPKG_CORRUPT;
  if (max_length < K_LUT_BITS) {
    /* Expand a short complete table to the fixed 11-bit lookup width. */
    size_t scale = (size_t)1u << (K_LUT_BITS - max_length);
    size_t short_size = (size_t)1u << max_length;
    for (size_t i = 0; i < short_size; ++i) {
      uint16_t entry = decoder->lut[i];
      for (size_t j = 1; j < scale; ++j)
        decoder->lut[i + j * short_size] = entry;
    }
  }

  size_t start = msb_seam(&bits);
  if (start == SIZE_MAX || start > src_size || src_size - start < 3u)
    return SM_FPKG_CORRUPT;
  size_t split = (size_t)src[start] | ((size_t)src[start + 1u] << 8);
  size_t stream_start = start + 2u;
  if (split > src_size - stream_start) return SM_FPKG_CORRUPT;
  size_t split_at = stream_start + split;
  lsb_reader_t streams[3];
  lsb_init(&streams[0], src, stream_start, split_at, false);
  lsb_init(&streams[1], src, split_at, src_size, true);
  lsb_init(&streams[2], src, split_at, src_size, false);
  for (size_t at = 0; at < output_size; ++at) {
    lsb_reader_t *reader = &streams[at % 3u];
    lsb_refill(reader);
    uint16_t entry = decoder->lut[reader->acc & (K_LUT_SIZE - 1u)];
    unsigned length = entry & 0xffu;
    if (length == 0 || length > reader->n) return SM_FPKG_CORRUPT;
    output[at] = (uint8_t)(entry >> 8);
    reader->acc >>= length;
    reader->n -= length;
  }
  if (lsb_bytes_used(&streams[0]) != split ||
      lsb_bytes_used(&streams[1]) + lsb_bytes_used(&streams[2]) !=
          src_size - stream_start - split)
    return SM_FPKG_CORRUPT;
  return SM_FPKG_OK;
}

static sm_fpkg_status_t get_array(sm_fpkg_decoder_t *decoder,
                                  const uint8_t *src, size_t at, size_t end,
                                  size_t max_size, byte_array_t *array,
                                  size_t *used) {
  memset(array, 0, sizeof(*array));
  if (at > end || end - at < 2u) return SM_FPKG_CORRUPT;
  uint8_t b0 = src[at];
  unsigned type = (b0 >> 4) & 7u;
  size_t header, source_size, dest_size;
  if (type == 0) {
    if (b0 >= 0x80) {
      header = 2;
      source_size = (((size_t)b0 << 8) | src[at + 1u]) & 0xfffu;
    } else {
      if (end - at < 3u) return SM_FPKG_CORRUPT;
      header = 3;
      source_size = ((size_t)b0 << 16) | ((size_t)src[at + 1u] << 8) |
                    src[at + 2u];
      if (source_size & ~0x3ffffu) return SM_FPKG_CORRUPT;
    }
    if (source_size > max_size || header > end - at ||
        source_size > end - at - header)
      return SM_FPKG_CORRUPT;
    array->bytes = malloc(source_size ? source_size : 1u);
    if (!array->bytes) return SM_FPKG_NOMEM;
    if (source_size) memcpy(array->bytes, src + at + header, source_size);
    array->size = source_size;
    *used = header + source_size;
    return SM_FPKG_OK;
  }
  if (type != 2) return SM_FPKG_UNSUPPORTED;
  if (b0 >= 0x80) {
    if (end - at < 3u) return SM_FPKG_CORRUPT;
    uint32_t v = ((uint32_t)b0 << 16) | ((uint32_t)src[at + 1u] << 8) |
                 src[at + 2u];
    header = 3;
    source_size = v & 0x3ffu;
    dest_size = source_size + ((v >> 10) & 0x3ffu) + 1u;
  } else {
    if (end - at < 5u) return SM_FPKG_CORRUPT;
    uint32_t v = ((uint32_t)src[at + 1u] << 24) |
                 ((uint32_t)src[at + 2u] << 16) |
                 ((uint32_t)src[at + 3u] << 8) | src[at + 4u];
    header = 5;
    source_size = v & 0x3ffffu;
    dest_size = (((v >> 18) | ((uint32_t)b0 << 14)) & 0x3ffffu) + 1u;
  }
  if (source_size >= dest_size || dest_size > max_size ||
      header > end - at || source_size > end - at - header)
    return SM_FPKG_CORRUPT;
  array->bytes = malloc(dest_size ? dest_size : 1u);
  if (!array->bytes) return SM_FPKG_NOMEM;
  sm_fpkg_status_t status = huffman_decode(
      decoder, src + at + header, source_size, array->bytes, dest_size);
  if (status != SM_FPKG_OK) {
    free(array->bytes);
    memset(array, 0, sizeof(*array));
    return status;
  }
  array->size = dest_size;
  *used = header + source_size;
  return SM_FPKG_OK;
}

static uint32_t bit_get(const uint8_t *src, size_t start, size_t end,
                        size_t *consumed, bool back, unsigned count) {
  msb_reader_t reader = {.src = src, .start = start, .end = end,
                         .consumed_bits = *consumed, .back = back};
  uint32_t value = msb_get(&reader, count);
  *consumed = reader.consumed_bits;
  return value;
}

static unsigned bit_zeros(const uint8_t *src, size_t start, size_t end,
                          size_t consumed, bool back) {
  msb_reader_t reader = {.src = src, .start = start, .end = end,
                         .consumed_bits = consumed, .back = back};
  return msb_zeros(&reader);
}

static size_t bit_seam(size_t start, size_t end, size_t consumed, bool back) {
  size_t used = (consumed + 7u) / 8u;
  if (used > end - start) return SIZE_MAX;
  return back ? end - used : start + used;
}

static bool put_literals(uint8_t *out, size_t dst, size_t end,
                         const uint8_t *literals, size_t length, bool delta,
                         size_t distance) {
  if (length > end - dst || (delta && (distance < 8u || distance > dst)))
    return false;
  if (!delta) {
    memcpy(out + dst, literals, length);
  } else {
    for (size_t i = 0; i < length; ++i)
      out[dst + i] = (uint8_t)(literals[i] + out[dst + i - distance]);
  }
  return true;
}

static sm_fpkg_status_t decode_half(sm_fpkg_decoder_t *decoder,
                                    const uint8_t *src, size_t src_size,
                                    uint8_t *out, size_t base, size_t length,
                                    bool lz, bool delta) {
  size_t end = base + length;
  if (!lz) {
    if (src_size == length) {
      memcpy(out + base, src, length);
      return SM_FPKG_OK;
    }
    byte_array_t array = {0};
    size_t used = 0;
    sm_fpkg_status_t status = get_array(decoder, src, 0, src_size, length,
                                        &array, &used);
    if (status == SM_FPKG_OK && (used != src_size || array.size != length))
      status = SM_FPKG_CORRUPT;
    if (status == SM_FPKG_OK) memcpy(out + base, array.bytes, length);
    free(array.bytes);
    return status;
  }

  size_t p = 0, dst = base;
  if (base == 0) {
    if (src_size < K_SEED || length < K_SEED) return SM_FPKG_CORRUPT;
    memcpy(out, src, K_SEED);
    p = K_SEED;
    dst = K_SEED;
  }
  if (src_size < p + 13u) return SM_FPKG_CORRUPT;
  uint8_t flag = src[p++];
  if ((flag & 0xc0u) != 0x80u) return SM_FPKG_CORRUPT;
  size_t excess = flag & 0x3fu;
  if (excess > 0x1fu) {
    if (p >= src_size) return SM_FPKG_CORRUPT;
    excess += (size_t)src[p++] * 0x20u;
  }
  if (excess > src_size - p) return SM_FPKG_CORRUPT;
  size_t main_end = src_size - excess;
  byte_array_t literals = {0}, commands = {0}, offset_codes = {0}, lengths = {0};
  size_t used = 0;
  sm_fpkg_status_t status = get_array(decoder, src, p, main_end, length,
                                      &literals, &used);
  if (status != SM_FPKG_OK) goto done;
  p += used;
  status = get_array(decoder, src, p, main_end, length, &commands, &used);
  if (status != SM_FPKG_OK) goto done;
  p += used;
  if (p >= main_end) { status = SM_FPKG_CORRUPT; goto done; }
  bool scaled = src[p] == 0x80;
  if (scaled) ++p;
  status = get_array(decoder, src, p, main_end, commands.size,
                     &offset_codes, &used);
  if (status != SM_FPKG_OK) goto done;
  p += used;
  status = get_array(decoder, src, p, main_end, length / 4u, &lengths, &used);
  if (status != SM_FPKG_OK) goto done;
  p += used;

  size_t *offsets = calloc(offset_codes.size ? offset_codes.size : 1u,
                           sizeof(*offsets));
  if (!offsets) { status = SM_FPKG_NOMEM; goto done; }
  size_t consumed_a = 0, consumed_b = 0;
  for (size_t i = 0; i < offset_codes.size; ++i) {
    uint8_t code = offset_codes.bytes[i];
    size_t *consumed = (i & 1u) ? &consumed_b : &consumed_a;
    bool back = (i & 1u) != 0;
    uint32_t value;
    if (scaled) {
      unsigned nb = code >> 3;
      if (nb > 26) { free(offsets); status = SM_FPKG_CORRUPT; goto done; }
      value = ((8u + (code & 7u)) << nb) |
              bit_get(src, p, main_end, consumed, back, nb);
      if (value < 8u) { free(offsets); status = SM_FPKG_CORRUPT; goto done; }
      offsets[i] = value - 8u;
    } else {
      if (code >= 0xf0u) { free(offsets); status = SM_FPKG_CORRUPT; goto done; }
      unsigned nb = (code >> 4) + 4u;
      uint32_t hi = (1u << nb) |
                    bit_get(src, p, main_end, consumed, back, nb);
      uint64_t value64 = ((uint64_t)hi << 4) + (code & 0xfu);
      if (value64 < 248u || value64 - 248u > SIZE_MAX) {
        free(offsets); status = SM_FPKG_CORRUPT; goto done;
      }
      offsets[i] = (size_t)(value64 - 248u);
    }
  }
  if (consumed_a > (main_end - p) * 8u ||
      consumed_b > (main_end - p) * 8u ||
      bit_seam(p, main_end, consumed_a, false) !=
      bit_seam(p, main_end, consumed_b, true)) {
    free(offsets); status = SM_FPKG_CORRUPT; goto done;
  }

  size_t escape_count = 0;
  for (size_t i = 0; i < lengths.size; ++i)
    if (lengths.bytes[i] == 255) ++escape_count;
  size_t *escapes = calloc(escape_count ? escape_count : 1u,
                           sizeof(*escapes));
  if (!escapes) { free(offsets); status = SM_FPKG_NOMEM; goto done; }
  size_t escape_a = 0, escape_b = 0;
  for (size_t i = 0; i < escape_count; ++i) {
    size_t *consumed = (i & 1u) ? &escape_b : &escape_a;
    bool back = (i & 1u) != 0;
    unsigned zeros = bit_zeros(src, main_end, src_size, *consumed, back);
    if (zeros > 12) { free(escapes); free(offsets); status = SM_FPKG_CORRUPT; goto done; }
    (void)bit_get(src, main_end, src_size, consumed, back, zeros);
    uint32_t value = bit_get(src, main_end, src_size, consumed, back,
                             zeros + 7u);
    if (value < 64u) { free(escapes); free(offsets); status = SM_FPKG_CORRUPT; goto done; }
    escapes[i] = value - 64u;
  }
  if (escape_a > (src_size - main_end) * 8u ||
      escape_b > (src_size - main_end) * 8u ||
      bit_seam(main_end, src_size, escape_a, false) !=
      bit_seam(main_end, src_size, escape_b, true)) {
    free(escapes); free(offsets); status = SM_FPKG_CORRUPT; goto done;
  }

  size_t lit_at = 0, offset_at = 0, length_at = 0, escape_at = 0;
  size_t recent[3] = {8, 8, 8};
  for (size_t i = 0; i < commands.size; ++i) {
    uint8_t command = commands.bytes[i];
    size_t literal_count = command & 3u;
    if (literal_count == 3u) {
      if (length_at >= lengths.size) { status = SM_FPKG_CORRUPT; break; }
      uint8_t value = lengths.bytes[length_at++];
      size_t extra = value;
      if (value == 255u) {
        if (escape_at >= escape_count ||
            escapes[escape_at] > SIZE_MAX - extra) {
          status = SM_FPKG_CORRUPT; break;
        }
        /* The 255-byte base is already in `extra`; the bitstream supplies
         * only the additional length. */
        extra += escapes[escape_at++];
      }
      literal_count = 3u + extra;
    }
    size_t kind = command >> 6;
    size_t code = (command >> 2) & 0xfu;
    if (literal_count > literals.size - lit_at || literal_count > end - dst ||
        !put_literals(out, dst, end, literals.bytes + lit_at,
                      literal_count, delta, recent[0])) {
      status = SM_FPKG_CORRUPT; break;
    }
    dst += literal_count;
    lit_at += literal_count;
    size_t distance;
    if (kind == 3) {
      if (offset_at >= offset_codes.size) { status = SM_FPKG_CORRUPT; break; }
      distance = offsets[offset_at++];
      recent[2] = recent[1]; recent[1] = recent[0]; recent[0] = distance;
    } else {
      distance = recent[kind];
      for (size_t j = kind; j > 0; --j) recent[j] = recent[j - 1u];
      recent[0] = distance;
    }
    size_t match_length = code + 2u;
    if (code == 15u) {
      if (length_at >= lengths.size) { status = SM_FPKG_CORRUPT; break; }
      uint8_t value = lengths.bytes[length_at++];
      size_t extra = value;
      if (value == 255u) {
        if (escape_at >= escape_count ||
            escapes[escape_at] > SIZE_MAX - extra) {
          status = SM_FPKG_CORRUPT; break;
        }
        /* Match codes use a base of 17, plus this escaped length. */
        extra += escapes[escape_at++];
      }
      match_length = 14u + 3u + extra;
    }
    if (distance < 8u || distance > dst || match_length > end - dst) {
      status = SM_FPKG_CORRUPT; break;
    }
    for (size_t j = 0; j < match_length; ++j)
      out[dst + j] = out[dst + j - distance];
    dst += match_length;
  }
  if (status == SM_FPKG_OK) {
    size_t tail = end - dst;
    if (lit_at > literals.size || literals.size - lit_at != tail ||
        !put_literals(out, dst, end, literals.bytes + lit_at, tail, delta,
                      recent[0]) || offset_at != offset_codes.size ||
        length_at != lengths.size || escape_at != escape_count)
      status = SM_FPKG_CORRUPT;
  }
  free(escapes);
  free(offsets);
done:
  free(literals.bytes);
  free(commands.bytes);
  free(offset_codes.bytes);
  free(lengths.bytes);
  return status;
}

sm_fpkg_decoder_t *sm_fpkg_decoder_create(void) {
  return calloc(1, sizeof(sm_fpkg_decoder_t));
}

void sm_fpkg_decoder_free(sm_fpkg_decoder_t *decoder) { free(decoder); }

sm_fpkg_status_t sm_fpkg_decode_block(sm_fpkg_decoder_t *decoder,
                                      const uint8_t *stored,
                                      size_t stored_size,
                                      uint32_t even_length, uint8_t mode,
                                      uint8_t *output, size_t output_size) {
  if (!decoder || !output || output_size == 0 ||
      output_size > K_BLOCK || stored_size > 2u * K_BLOCK ||
      even_length > stored_size)
    return SM_FPKG_CORRUPT;
  if (stored_size == 0) {
    if (even_length != 0 || mode != 0)
      return SM_FPKG_CORRUPT;
    memset(output, 0, output_size);
    return SM_FPKG_OK;
  }
  if (!stored)
    return SM_FPKG_CORRUPT;
  size_t even_logical = output_size < K_HALF ? output_size : K_HALF;
  size_t odd_stored = stored_size - even_length;
  sm_fpkg_status_t status = decode_half(
      decoder, stored, even_length, output, 0, even_logical,
      (mode & 2u) != 0, (mode & 1u) != 0);
  if (status != SM_FPKG_OK) return status;
  if (output_size > K_HALF)
    return decode_half(decoder, stored + even_length, odd_stored, output,
                       K_HALF, output_size - K_HALF, (mode & 0x10u) != 0,
                       (mode & 8u) != 0);
  return odd_stored == 0 ? SM_FPKG_OK : SM_FPKG_CORRUPT;
}
