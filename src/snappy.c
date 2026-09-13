// snappy.c - minimal, spec-compliant snappy compressor/decompressor.
// Produces streams accepted by the reference snappy decoder and decodes
// streams produced by the reference encoder.
#include "kvdb.h"

#include <assert.h>
#include <string.h>

#define SNAPPY_MAX_HASH_TABLE (1 << 14)

static char* snappy_put_varint32(char* dst, uint32_t v) {
  return ldb_encode_varint32(dst, v);
}

static uint32_t snappy_hash(uint32_t bytes, int shift) {
  return (bytes * 0x1e35a7bdu) >> shift;
}

static uint32_t snappy_load32(const char* p) {
  uint8_t b[4];
  memcpy(b, p, 4);
  return (uint32_t)b[0] | ((uint32_t)b[1] << 8) | ((uint32_t)b[2] << 16) |
         ((uint32_t)b[3] << 24);
}

static char* emit_literal(char* dst, const char* src, size_t len) {
  assert(len > 0);
  size_t n = len - 1;
  if (n < 60) {
    *dst++ = (char)(n << 2);
  } else if (n < (1 << 8)) {
    *dst++ = (char)(60 << 2);
    *dst++ = (char)n;
  } else if (n < (1 << 16)) {
    *dst++ = (char)(61 << 2);
    *dst++ = (char)(n & 0xff);
    *dst++ = (char)(n >> 8);
  } else if (n < (1 << 24)) {
    *dst++ = (char)(62 << 2);
    *dst++ = (char)(n & 0xff);
    *dst++ = (char)((n >> 8) & 0xff);
    *dst++ = (char)((n >> 16) & 0xff);
  } else {
    *dst++ = (char)(63 << 2);
    *dst++ = (char)(n & 0xff);
    *dst++ = (char)((n >> 8) & 0xff);
    *dst++ = (char)((n >> 16) & 0xff);
    *dst++ = (char)((n >> 24) & 0xff);
  }
  memcpy(dst, src, len);
  return dst + len;
}

static char* emit_copy(char* dst, size_t offset, size_t len) {
  // A single copy instruction encodes length-1 in 6 bits, so len <= 64.
  // Longer matches are emitted as successive copies with the same offset
  // (overlapping copies are well-defined in snappy).
  while (len > 0) {
    size_t n;
    if (len >= 4 && len <= 11 && offset < 2048) {
      // copy with 1-byte offset
      unsigned char tag = (unsigned char)(1 | ((len - 4) << 2) |
                                          ((offset >> 8) << 5));
      *dst++ = (char)tag;
      *dst++ = (char)(offset & 0xff);
      return dst;
    } else if (len <= 64) {
      n = len;
    } else {
      n = 64;
    }
    if (offset < 65536) {
      unsigned char tag = (unsigned char)(2 | ((n - 1) << 2));
      *dst++ = (char)tag;
      *dst++ = (char)(offset & 0xff);
      *dst++ = (char)((offset >> 8) & 0xff);
    } else {
      unsigned char tag = (unsigned char)(3 | ((n - 1) << 2));
      *dst++ = (char)tag;
      *dst++ = (char)(offset & 0xff);
      *dst++ = (char)((offset >> 8) & 0xff);
      *dst++ = (char)((offset >> 16) & 0xff);
      *dst++ = (char)((offset >> 24) & 0xff);
    }
    len -= n;
  }
  return dst;
}

int ldb_snappy_compress(const char* input, size_t input_len, ldb_buffer* out) {
  // Upper bound: 32 + 5*len
  size_t max_out = 32 + 5 * input_len;
  char* buf = (char*)malloc(max_out ? max_out : 1);
  if (!buf) return 0;
  char* dst = buf;
  dst = snappy_put_varint32(dst, (uint32_t)input_len);

  if (input_len < 4) {
    if (input_len > 0) dst = emit_literal(dst, input, input_len);
  } else {
    int shift = 32 - 8;
    while ((size_t)(1u << (32 - shift)) < input_len && shift > 4) shift--;
    size_t table_size = (size_t)1 << (32 - shift);
    if (table_size > SNAPPY_MAX_HASH_TABLE) {
      table_size = SNAPPY_MAX_HASH_TABLE;
      shift = 32 - 14;
    }
    uint32_t* table = (uint32_t*)malloc(sizeof(uint32_t) * table_size);
    if (!table) {
      free(buf);
      return 0;
    }
    memset(table, 0, sizeof(uint32_t) * table_size);

    size_t pos = 0;
    size_t next_emit = 0;
    while (pos + 4 <= input_len) {
      uint32_t cur = snappy_load32(input + pos);
      uint32_t h = snappy_hash(cur, shift);
      uint32_t cand = table[h];  // stored positions are offset by +1; 0=empty
      table[h] = (uint32_t)(pos + 1);
      if (cand != 0) {
        size_t cpos = (size_t)(cand - 1);
        if (pos - cpos <= 65535 && snappy_load32(input + cpos) == cur) {
          // Extend the match forward.
          size_t len = 4;
          while (pos + len < input_len &&
                 input[cpos + len] == input[pos + len]) {
            len++;
          }
          // Extend the match backward into the pending literal region.
          size_t new_pos = pos;
          size_t new_cand = cpos;
          while (new_pos > next_emit && new_cand > 0 &&
                 input[new_cand - 1] == input[new_pos - 1]) {
            new_pos--;
            new_cand--;
            len++;
          }
          if (new_pos > next_emit) {
            dst = emit_literal(dst, input + next_emit, new_pos - next_emit);
          }
          dst = emit_copy(dst, new_pos - new_cand, len);
          // Insert hash entries across the match for future lookups.
          for (size_t i = 1; i < len; i++) {
            if (new_pos + i + 4 <= input_len) {
              uint32_t hh =
                  snappy_hash(snappy_load32(input + new_pos + i), shift);
              table[hh] = (uint32_t)(new_pos + i + 1);
            }
          }
          pos = new_pos + len;
          next_emit = pos;
          continue;
        }
      }
      pos++;
    }
    if (next_emit < input_len) {
      dst = emit_literal(dst, input + next_emit, input_len - next_emit);
    }
    free(table);
  }

  ldb_buffer_clear(out);
  ldb_buffer_append(out, buf, (size_t)(dst - buf));
  free(buf);
  return 1;
}

int ldb_snappy_get_uncompressed_length(const char* input, size_t input_len,
                                       size_t* result) {
  uint32_t length;
  const char* p = input;
  if (!ldb_get_varint32(&p, input + input_len, &length)) return 0;
  *result = length;
  return 1;
}

int ldb_snappy_uncompress(const char* input, size_t input_len, char* output,
                          size_t output_len) {
  const char* p = input;
  const char* limit = input + input_len;
  uint32_t total;
  if (!ldb_get_varint32(&p, limit, &total)) return 0;
  if (total > output_len) return 0;
  char* out = output;
  char* out_limit = output + total;
  while (p < limit) {
    unsigned char tag = (unsigned char)*p++;
    if ((tag & 3) == 0) {
      // Literal
      size_t len = (size_t)(tag >> 2) + 1;
      if (len > 60) {
        size_t extra = len - 60;
        if ((size_t)(limit - p) < extra) return 0;
        size_t l = 0;
        for (size_t i = 0; i < extra; i++) {
          l |= (size_t)(unsigned char)p[i] << (8 * i);
        }
        p += extra;
        len = l + 1;
      }
      if ((size_t)(limit - p) < len || (size_t)(out_limit - out) < len) {
        return 0;
      }
      memcpy(out, p, len);
      p += len;
      out += len;
    } else {
      size_t len, offset;
      if ((tag & 3) == 1) {
        len = 4 + ((tag >> 2) & 0x7);
        if (limit - p < 1) return 0;
        offset = ((size_t)(tag >> 5) << 8) | (size_t)(unsigned char)p[0];
        p += 1;
      } else if ((tag & 3) == 2) {
        len = (size_t)(tag >> 2) + 1;
        if (limit - p < 2) return 0;
        offset = (size_t)(unsigned char)p[0] |
                 ((size_t)(unsigned char)p[1] << 8);
        p += 2;
      } else {
        len = (size_t)(tag >> 2) + 1;
        if (limit - p < 4) return 0;
        offset = (size_t)(unsigned char)p[0] |
                 ((size_t)(unsigned char)p[1] << 8) |
                 ((size_t)(unsigned char)p[2] << 16) |
                 ((size_t)(unsigned char)p[3] << 24);
        p += 4;
      }
      if (offset == 0 || offset > (size_t)(out - output) ||
          (size_t)(out_limit - out) < len) {
        return 0;
      }
      // Byte-by-byte copy handles overlapping copies correctly.
      const char* src = out - offset;
      for (size_t i = 0; i < len; i++) {
        *out++ = *src++;
      }
    }
  }
  if (out != out_limit) return 0;
  return 1;
}
