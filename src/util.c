// util.c - slices, buffers, status, coding, crc32c, hash, logging,
// comparator, and bloom filter policy (mirrors leveldb/util/*).
#include "kvdb.h"

#include <assert.h>
#include <time.h>

// =================================================================== buffer
void ldb_buffer_init(ldb_buffer* b) {
  b->data = NULL;
  b->size = 0;
  b->cap = 0;
}

void ldb_buffer_destroy(ldb_buffer* b) {
  free(b->data);
  b->data = NULL;
  b->size = 0;
  b->cap = 0;
}

void ldb_buffer_clear(ldb_buffer* b) { b->size = 0; }

void ldb_buffer_reserve(ldb_buffer* b, size_t n) {
  if (b->cap < n) {
    size_t newcap = b->cap ? b->cap : 16;
    while (newcap < n) newcap *= 2;
    char* p = (char*)realloc(b->data, newcap);
    assert(p != NULL);
    b->data = p;
    b->cap = newcap;
  }
}

void ldb_buffer_resize(ldb_buffer* b, size_t n) {
  if (n > b->size) {
    ldb_buffer_reserve(b, n);
    // Growth zero-fills, matching std::string::resize: the SSTable footer
    // padding must be deterministic bytes rather than recycled heap memory.
    memset(b->data + b->size, 0, n - b->size);
  } else {
    ldb_buffer_reserve(b, n);
  }
  b->size = n;
}

void ldb_buffer_append(ldb_buffer* b, const void* data, size_t n) {
  if (n == 0) return;
  ldb_buffer_reserve(b, b->size + n);
  memcpy(b->data + b->size, data, n);
  b->size += n;
}

void ldb_buffer_append_slice(ldb_buffer* b, const ldb_slice* s) {
  ldb_buffer_append(b, s->data, s->size);
}

void ldb_buffer_append_str(ldb_buffer* b, const char* s) {
  ldb_buffer_append(b, s, strlen(s));
}

void ldb_buffer_copy(ldb_buffer* b, const ldb_buffer* src) {
  if (b == src) return;
  ldb_buffer_resize(b, src->size);
  if (src->size) memcpy(b->data, src->data, src->size);
}

void ldb_buffer_swap(ldb_buffer* a, ldb_buffer* b) {
  ldb_buffer t = *a;
  *a = *b;
  *b = t;
}

void ldb_buffer_appendf(ldb_buffer* b, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  va_end(ap);
  if (n > 0) {
    size_t old = b->size;
    ldb_buffer_resize(b, old + (size_t)n + 1);
    vsnprintf(b->data + old, (size_t)n + 1, fmt, ap2);
    b->size = old + (size_t)n;
  }
  va_end(ap2);
}

// =================================================================== status
ldb_status ldb_status_ok(void) {
  ldb_status s;
  s.code = LDB_OK;
  s.msg = NULL;
  return s;
}

ldb_status ldb_status_new(int code, const char* msg1, const char* msg2) {
  ldb_status s;
  s.code = code;
  if (msg1 == NULL && msg2 == NULL) {
    s.msg = NULL;
  } else {
    size_t n1 = msg1 ? strlen(msg1) : 0;
    size_t n2 = (msg2 && *msg2) ? 2 + strlen(msg2) : 0;
    s.msg = (char*)malloc(n1 + n2 + 1);
    assert(s.msg);
    char* p = s.msg;
    if (msg1) {
      memcpy(p, msg1, n1);
      p += n1;
    }
    if (msg2 && *msg2) {
      memcpy(p, ": ", 2);
      p += 2;
      size_t l = strlen(msg2);
      memcpy(p, msg2, l);
      p += l;
    }
    *p = '\0';
  }
  return s;
}

ldb_status ldb_status_notfound(const char* m1, const char* m2) {
  return ldb_status_new(LDB_NOTFOUND, m1, m2);
}
ldb_status ldb_status_corruption(const char* m1, const char* m2) {
  return ldb_status_new(LDB_CORRUPTION, m1, m2);
}
ldb_status ldb_status_notsupported(const char* m1, const char* m2) {
  return ldb_status_new(LDB_NOTSUPPORTED, m1, m2);
}
ldb_status ldb_status_invalid_argument(const char* m1, const char* m2) {
  return ldb_status_new(LDB_INVALID_ARGUMENT, m1, m2);
}
ldb_status ldb_status_io_error(const char* m1, const char* m2) {
  return ldb_status_new(LDB_IO_ERROR, m1, m2);
}

ldb_status ldb_status_copy(ldb_status s) {
  ldb_status r;
  r.code = s.code;
  r.msg = s.msg ? strdup(s.msg) : NULL;
  return r;
}

void ldb_status_destroy(ldb_status* s) {
  free(s->msg);
  s->msg = NULL;
  s->code = LDB_OK;
}

char* ldb_status_to_string(ldb_status s) {
  const char* type = NULL;
  switch (s.code) {
    case LDB_OK:
      type = "OK";
      break;
    case LDB_NOTFOUND:
      type = "NotFound: ";
      break;
    case LDB_CORRUPTION:
      type = "Corruption: ";
      break;
    case LDB_NOTSUPPORTED:
      type = "Not implemented: ";
      break;
    case LDB_INVALID_ARGUMENT:
      type = "Invalid argument: ";
      break;
    case LDB_IO_ERROR:
      type = "IO error: ";
      break;
    default:
      snprintf(NULL, 0, "Unknown code(%d): ", s.code);
      {
        char buf[64];
        snprintf(buf, sizeof(buf), "Unknown code(%d): ", s.code);
        char* out = strdup(s.msg ? s.msg : "");
        size_t l1 = strlen(buf), l2 = strlen(out);
        char* r = (char*)malloc(l1 + l2 + 1);
        memcpy(r, buf, l1);
        memcpy(r + l1, out, l2 + 1);
        free(out);
        return r;
      }
  }
  const char* msg = (s.code == LDB_OK) ? "" : (s.msg ? s.msg : "");
  size_t l1 = strlen(type), l2 = strlen(msg);
  char* out = (char*)malloc(l1 + l2 + 1);
  memcpy(out, type, l1);
  memcpy(out + l1, msg, l2 + 1);
  return out;
}

void ldb_save_error(char** errptr, ldb_status s) {
  assert(errptr != NULL);
  if (ldb_ok(s)) {
    ldb_status_destroy(&s);
    return;
  }
  if (*errptr != NULL) {
    free(*errptr);
  }
  *errptr = ldb_status_to_string(s);
  ldb_status_destroy(&s);
}

// =================================================================== logging
void ldb_log(ldb_logger* info_log, const char* fmt, ...) {
  if (info_log == NULL) return;
  va_list ap;
  va_start(ap, fmt);
  info_log->logv(info_log, fmt, ap);
  va_end(ap);
}

void ldb_append_number_to(ldb_buffer* str, uint64_t num) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)num);
  ldb_buffer_append_str(str, buf);
}

char* ldb_number_to_string(uint64_t num) {
  char buf[32];
  snprintf(buf, sizeof(buf), "%llu", (unsigned long long)num);
  return strdup(buf);
}

void ldb_append_escaped_string_to(ldb_buffer* str, const ldb_slice* value) {
  char buf[10];
  for (size_t i = 0; i < value->size; i++) {
    unsigned char c = (unsigned char)value->data[i];
    if (c >= 0x20 && c <= 0x7e) {
      snprintf(buf, sizeof(buf), "%c", (char)c);
      ldb_buffer_append_str(str, buf);
    } else {
      snprintf(buf, sizeof(buf), "\\x%02x", (unsigned)c);
      ldb_buffer_append_str(str, buf);
    }
  }
}

char* ldb_escape_string(const ldb_slice* value) {
  ldb_buffer b;
  ldb_buffer_init(&b);
  ldb_append_escaped_string_to(&b, value);
  char* out = (char*)malloc(b.size + 1);
  if (b.data) {
    memcpy(out, b.data, b.size);
  }
  out[b.size] = '\0';
  ldb_buffer_destroy(&b);
  return out;
}

int ldb_consume_decimal_number(ldb_slice* in, uint64_t* val) {
  uint64_t v = 0;
  int digits = 0;
  const uint64_t kMax = ~(uint64_t)0;
  while (in->size > 0) {
    unsigned char c = (unsigned char)in->data[0];
    if (c >= '0' && c <= '9') {
      digits++;
      int delta = c - '0';
      if (v > kMax / 10 || (v == kMax / 10 && (uint64_t)delta > kMax % 10)) {
        return 0;
      }
      v = (v * 10) + (uint64_t)delta;
      in->data++;
      in->size--;
    } else {
      break;
    }
  }
  *val = v;
  return digits > 0;
}

// =================================================================== coding
char* ldb_encode_varint32(char* dst, uint32_t v) {
  unsigned char* ptr = (unsigned char*)dst;
  static const int B = 128;
  if (v < (1 << 7)) {
    *(ptr++) = (unsigned char)v;
  } else if (v < (1 << 14)) {
    *(ptr++) = (unsigned char)(v | B);
    *(ptr++) = (unsigned char)(v >> 7);
  } else if (v < (1 << 21)) {
    *(ptr++) = (unsigned char)(v | B);
    *(ptr++) = (unsigned char)((v >> 7) | B);
    *(ptr++) = (unsigned char)(v >> 14);
  } else if (v < (1 << 28)) {
    *(ptr++) = (unsigned char)(v | B);
    *(ptr++) = (unsigned char)((v >> 7) | B);
    *(ptr++) = (unsigned char)((v >> 14) | B);
    *(ptr++) = (unsigned char)(v >> 21);
  } else {
    *(ptr++) = (unsigned char)(v | B);
    *(ptr++) = (unsigned char)((v >> 7) | B);
    *(ptr++) = (unsigned char)((v >> 14) | B);
    *(ptr++) = (unsigned char)((v >> 21) | B);
    *(ptr++) = (unsigned char)(v >> 28);
  }
  return (char*)ptr;
}

char* ldb_encode_varint64(char* dst, uint64_t v) {
  static const int B = 128;
  unsigned char* ptr = (unsigned char*)dst;
  while (v >= B) {
    *(ptr++) = (unsigned char)(v | B);
    v >>= 7;
  }
  *(ptr++) = (unsigned char)v;
  return (char*)ptr;
}

void ldb_put_varint32(ldb_buffer* dst, uint32_t v) {
  char buf[5];
  char* p = ldb_encode_varint32(buf, v);
  ldb_buffer_append(dst, buf, (size_t)(p - buf));
}

void ldb_put_varint64(ldb_buffer* dst, uint64_t v) {
  char buf[10];
  char* p = ldb_encode_varint64(buf, v);
  ldb_buffer_append(dst, buf, (size_t)(p - buf));
}

void ldb_put_varint32_varint32(ldb_buffer* dst, uint32_t a, uint32_t b) {
  char buf[10];
  char* p = ldb_encode_varint32(buf, a);
  p = ldb_encode_varint32(p, b);
  ldb_buffer_append(dst, buf, (size_t)(p - buf));
}

void ldb_put_fixed32(ldb_buffer* dst, uint32_t v) {
  char buf[4];
  ldb_encode_fixed32(buf, v);
  ldb_buffer_append(dst, buf, 4);
}

void ldb_put_fixed64(ldb_buffer* dst, uint64_t v) {
  char buf[8];
  ldb_encode_fixed64(buf, v);
  ldb_buffer_append(dst, buf, 8);
}

void ldb_put_length_prefixed_slice(ldb_buffer* dst, const ldb_slice* s) {
  ldb_put_varint32(dst, (uint32_t)s->size);
  ldb_buffer_append_slice(dst, s);
}

void ldb_put_length_prefixed_slice2(ldb_buffer* dst, const ldb_slice* a,
                                    const ldb_slice* b) {
  ldb_put_varint32(dst, (uint32_t)(a->size + b->size));
  ldb_buffer_append_slice(dst, a);
  ldb_buffer_append_slice(dst, b);
}

int ldb_get_varint32(const char** pp, const char* limit, uint32_t* value) {
  const char* p = *pp;
  uint32_t result = 0;
  if (limit - p < 5) {
    // Fast path: single byte, common case
    if (p < limit && ((unsigned char)*p) < 128) {
      result = (unsigned char)*p;
      *pp = p + 1;
      *value = result;
      return 1;
    }
    // Slow path with bounds
    uint32_t b;
    uint32_t shift = 0;
    const char* q = p;
    do {
      if (q >= limit) return 0;
      b = (unsigned char)*q;
      q++;
      if (shift > 28) return 0;
      result |= (b & 127) << shift;
      shift += 7;
    } while (b >= 128);
    *pp = q;
    *value = result;
    return 1;
  } else {
    uint32_t b;
    uint32_t shift = 0;
    const char* q = p;
    do {
      b = (unsigned char)*q;
      q++;
      if (shift > 28) return 0;
      result |= (b & 127) << shift;
      shift += 7;
    } while (b >= 128);
    *pp = q;
    *value = result;
    return 1;
  }
}

int ldb_get_varint64(const char** pp, const char* limit, uint64_t* value) {
  uint64_t result = 0;
  uint32_t shift = 0;
  const char* p = *pp;
  while (shift <= 63) {
    if (p >= limit) return 0;
    uint64_t b = (unsigned char)*p;
    p++;
    if (shift == 63 && b > 1) return 0;
    result |= (b & 127) << shift;
    if (b < 128) {
      *pp = p;
      *value = result;
      return 1;
    }
    shift += 7;
  }
  return 0;
}

int ldb_get_length_prefixed_slice(const char** pp, const char* limit,
                                  ldb_slice* out) {
  uint32_t len;
  const char* p = *pp;
  if (!ldb_get_varint32(&p, limit, &len)) return 0;
  if ((size_t)(limit - p) < len) return 0;
  *out = ldb_slice_make(p, len);
  *pp = p + len;
  return 1;
}

// =================================================================== crc32c
static uint32_t crc32c_table[8 * 256];
static int crc32c_table_ready = 0;

static void crc32c_init(void) {
  if (crc32c_table_ready) return;
  for (int i = 0; i < 256; i++) {
    uint32_t c = (uint32_t)i;
    for (int k = 0; k < 8; k++) {
      c = c & 1 ? 0x82F63B78u ^ (c >> 1) : c >> 1;
    }
    crc32c_table[i] = c;
  }
  crc32c_table_ready = 1;
}

uint32_t ldb_crc32c_extend(uint32_t crc, const char* data, size_t n) {
  crc32c_init();
  // leveldb semantics: Extend(crc, data) XORs in and out so that
  // Extend(Value(a), b) == Value(ab) and Value(x, 0) == 0.
  uint32_t c = crc ^ 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c = crc32c_table[(c ^ (unsigned char)data[i]) & 0xFF] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

uint32_t ldb_crc32c_value(const char* data, size_t n) {
  return ldb_crc32c_extend(0, data, n);
}

// =================================================================== hash
uint32_t ldb_hash(const char* data, size_t n, uint32_t seed) {
  const uint32_t m = 0xc6a4a793u;
  const uint32_t r = 24;
  const char* limit = data + n;
  uint32_t h = (uint32_t)(seed ^ (uint32_t)(n * m));
  while (data + 4 <= limit) {
    uint32_t w = ldb_decode_fixed32(data);
    data += 4;
    h += w;
    h *= m;
    h ^= (h >> 16);
  }
  switch (limit - data) {
    case 3:
      h += (uint32_t)((unsigned char)data[2]) << 16;
      /* fallthrough */
    case 2:
      h += (uint32_t)((unsigned char)data[1]) << 8;
      /* fallthrough */
    case 1:
      h += (uint32_t)((unsigned char)data[0]);
      h *= m;
      h ^= (h >> r);
      break;
    default:
      break;
  }
  return h;
}

// =================================================================== comparator
static const char* bytewise_name(const ldb_comparator* c) {
  (void)c;
  return "leveldb.BytewiseComparator";
}

static int bytewise_compare(const ldb_comparator* c, const ldb_slice* a,
                            const ldb_slice* b) {
  (void)c;
  return ldb_slice_compare(a, b);
}

static void bytewise_find_shortest_separator(const ldb_comparator* c,
                                             ldb_buffer* start,
                                             const ldb_slice* limit) {
  (void)c;
  size_t min_length = start->size < limit->size ? start->size : limit->size;
  size_t diff_index = 0;
  while (diff_index < min_length &&
         start->data[diff_index] == limit->data[diff_index]) {
    diff_index++;
  }
  if (diff_index >= min_length) {
    // Do not shorten if one string is a prefix of the other
  } else {
    uint8_t diff_byte = (uint8_t)start->data[diff_index];
    if (diff_byte < (uint8_t)0xff &&
        diff_byte + 1 < (uint8_t)limit->data[diff_index]) {
      start->data[diff_index]++;
      start->size = diff_index + 1;
    }
  }
}

static void bytewise_find_short_successor(const ldb_comparator* c,
                                          ldb_buffer* key) {
  (void)c;
  for (size_t i = 0; i < key->size; i++) {
    const uint8_t byte = (uint8_t)key->data[i];
    if (byte != (uint8_t)0xff) {
      key->data[i] = (char)(byte + 1);
      key->size = i + 1;
      return;
    }
  }
}

static const ldb_comparator bytewise = {
    bytewise_name, bytewise_compare, bytewise_find_shortest_separator,
    bytewise_find_short_successor, NULL};

const ldb_comparator* ldb_bytewise_comparator(void) { return &bytewise; }

// =================================================================== internal key comparator
void ldb_ikc_init(ldb_ikc* ikc, const ldb_comparator* ucmp) {
  ikc->user_comparator = ucmp;
}

const char* ldb_ikc_name(void) { return "leveldb.InternalKeyComparator"; }

int ldb_ikc_compare(const ldb_ikc* ikc, const ldb_slice* akey,
                    const ldb_slice* bkey) {
  ldb_slice au = ldb_extract_user_key(akey);
  ldb_slice bu = ldb_extract_user_key(bkey);
  int r = ldb_cmp(ikc->user_comparator, &au, &bu);
  if (r == 0) {
    const uint64_t anum = ldb_decode_fixed64(akey->data + akey->size - 8);
    const uint64_t bnum = ldb_decode_fixed64(bkey->data + bkey->size - 8);
    if (anum > bnum) {
      r = -1;
    } else if (anum < bnum) {
      r = 1;
    }
  }
  return r;
}

void ldb_ikc_find_shortest_separator(const ldb_ikc* ikc, ldb_buffer* start,
                                     const ldb_slice* limit) {
  ldb_slice start_slice = ldb_buffer_slice(start);
  ldb_slice user_start = ldb_extract_user_key(&start_slice);
  ldb_slice user_limit = ldb_extract_user_key(limit);
  ldb_buffer tmp;
  ldb_buffer_init(&tmp);
  ldb_buffer_append_slice(&tmp, &user_start);
  ikc->user_comparator->find_shortest_separator(ikc->user_comparator, &tmp,
                                                &user_limit);
  ldb_slice tmp_slice = ldb_buffer_slice(&tmp);
  if (tmp.size < user_start.size &&
      ldb_cmp(ikc->user_comparator, &user_start, &tmp_slice) < 0) {
    // User key has become shorter physically, but larger logically.
    // Tack on the earliest possible number to the shortened user key.
    ldb_put_fixed64(&tmp, ldb_pack_sequence_and_type(
                              LDB_K_MAX_SEQUENCE_NUMBER,
                              LDB_VALUE_TYPE_FOR_SEEK));
    ldb_buffer_swap(start, &tmp);
  }
  ldb_buffer_destroy(&tmp);
}

void ldb_ikc_find_short_successor(const ldb_ikc* ikc, ldb_buffer* key) {
  ldb_slice key_slice = ldb_buffer_slice(key);
  ldb_slice user_key = ldb_extract_user_key(&key_slice);
  ldb_buffer tmp;
  ldb_buffer_init(&tmp);
  ldb_buffer_append_slice(&tmp, &user_key);
  ikc->user_comparator->find_short_successor(ikc->user_comparator, &tmp);
  ldb_slice tmp_slice = ldb_buffer_slice(&tmp);
  if (tmp.size < user_key.size &&
      ldb_cmp(ikc->user_comparator, &user_key, &tmp_slice) < 0) {
    ldb_put_fixed64(&tmp, ldb_pack_sequence_and_type(
                              LDB_K_MAX_SEQUENCE_NUMBER,
                              LDB_VALUE_TYPE_FOR_SEEK));
    ldb_buffer_swap(key, &tmp);
  }
  ldb_buffer_destroy(&tmp);
}

// =================================================================== dbformat
int ldb_parse_internal_key(const ldb_slice* ikey,
                           ldb_parsed_internal_key* out) {
  if (ikey->size < 8) return 0;
  uint64_t num = ldb_decode_fixed64(ikey->data + ikey->size - 8);
  unsigned char c = (unsigned char)(num & 0xff);
  out->type = (int)c;
  out->sequence = num >> 8;
  out->user_key = ldb_slice_make(ikey->data, ikey->size - 8);
  return c <= (unsigned char)LDB_TYPE_VALUE;
}

void ldb_append_internal_key(ldb_buffer* dst, const ldb_slice* user_key,
                             uint64_t seq, int type) {
  ldb_buffer_append_slice(dst, user_key);
  ldb_put_fixed64(dst, ldb_pack_sequence_and_type(seq, type));
}

void ldb_append_internal_key_parsed(ldb_buffer* dst,
                                    const ldb_parsed_internal_key* key) {
  ldb_append_internal_key(dst, &key->user_key, key->sequence, key->type);
}

void ldb_lookup_key_init(ldb_lookup_key* lk, const ldb_slice* user_key,
                         uint64_t seq) {
  ldb_buffer_init(&lk->kbuf);
  size_t usize = user_key->size;
  char buf[5];
  char* p = ldb_encode_varint32(buf, (uint32_t)(usize + 8));
  ldb_buffer_append(&lk->kbuf, buf, (size_t)(p - buf));
  ldb_buffer_append_slice(&lk->kbuf, user_key);
  ldb_put_fixed64(&lk->kbuf,
                  ldb_pack_sequence_and_type(seq, LDB_VALUE_TYPE_FOR_SEEK));
  size_t varint_len = lk->kbuf.size - usize - 8;
  lk->memtable_key = ldb_slice_make(lk->kbuf.data, lk->kbuf.size);
  lk->internal_key = ldb_slice_make(lk->kbuf.data + varint_len, usize + 8);
  lk->user_key = ldb_slice_make(lk->kbuf.data + varint_len, usize);
}

void ldb_lookup_key_destroy(ldb_lookup_key* lk) { ldb_buffer_destroy(&lk->kbuf); }

// =================================================================== options
void ldb_options_init(ldb_options* opt) {
  memset(opt, 0, sizeof(*opt));
  opt->comparator = ldb_bytewise_comparator();
  opt->env = NULL;
  opt->write_buffer_size = 4 * 1024 * 1024;
  opt->max_open_files = 1000;
  opt->block_size = 4 * 1024;
  opt->block_restart_interval = 16;
  opt->max_file_size = 2 * 1024 * 1024;
  opt->compression = LDB_SNAPPY_COMPRESSION;
}

void ldb_read_options_init(ldb_read_options* ro) {
  memset(ro, 0, sizeof(*ro));
  ro->fill_cache = 1;
}

void ldb_write_options_init(ldb_write_options* wo) { memset(wo, 0, sizeof(*wo)); }

// =================================================================== bloom filter policy
static uint32_t bloom_hash(const ldb_slice* key) {
  return ldb_hash(key->data, key->size, 0xbc9f1d34u);
}

static const char* bloom_name(const ldb_filterpolicy* p) {
  (void)p;
  return "leveldb.BuiltinBloomFilter2";
}

typedef struct bloom_impl {
  ldb_filterpolicy base;
  size_t bits_per_key;
  size_t k;
} bloom_impl;

static void bloom_create_filter(const ldb_filterpolicy* p,
                                const ldb_slice* keys, int n, ldb_buffer* dst) {
  const bloom_impl* b = (const bloom_impl*)p;
  size_t bits = (size_t)n * b->bits_per_key;
  if (bits < 64) bits = 64;
  size_t bytes = (bits + 7) / 8;
  bits = bytes * 8;
  const size_t init_size = dst->size;
  ldb_buffer_resize(dst, init_size + bytes);
  memset(dst->data + init_size, 0, bytes);
  char* array = dst->data + init_size;
  for (int i = 0; i < n; i++) {
    // Use double-hashing to generate a sequence of hash values.
    uint32_t h = bloom_hash(&keys[i]);
    const uint32_t delta = (h >> 17) | (h << 15);  // Rotate right 17 bits
    for (size_t j = 0; j < b->k; j++) {
      const uint32_t bitpos = h % (uint32_t)bits;
      array[bitpos / 8] |= (char)(1 << (bitpos % 8));
      h += delta;
    }
  }
  dst->data[dst->size] = (char)b->k;  // Remember # of probes
  dst->size++;
}

static int bloom_key_may_match(const ldb_filterpolicy* p, const ldb_slice* key,
                               const ldb_slice* bloom_filter) {
  (void)p;
  const size_t len = bloom_filter->size;
  if (len < 2) return 0;  // Filters are always at least 2 bytes (k + 1 byte)
  const char* array = bloom_filter->data;
  const size_t k = (size_t)(unsigned char)array[len - 1];
  if (k > 30) {
    // Reserved for potentially new encodings for short bloom filters.
    return 1;
  }
  uint32_t h = bloom_hash(key);
  const uint32_t delta = (h >> 17) | (h << 15);
  const uint32_t nbits = (uint32_t)(len - 1) * 8;
  for (size_t i = 0; i < k; i++) {
    const uint32_t bitpos = h % nbits;
    if ((array[bitpos / 8] & (1 << (bitpos % 8))) == 0) return 0;
    h += delta;
  }
  return 1;
}

static void bloom_destroy(ldb_filterpolicy* p) { free(p); }

const ldb_filterpolicy* ldb_new_bloom_filter_policy(int bits_per_key) {
  bloom_impl* b = (bloom_impl*)malloc(sizeof(bloom_impl));
  b->base.name = bloom_name;
  b->base.create_filter = bloom_create_filter;
  b->base.key_may_match = bloom_key_may_match;
  b->base.destroy = bloom_destroy;
  b->bits_per_key = (size_t)bits_per_key;
  b->k = (size_t)(bits_per_key * 0.69);  // 0.69 =~ ln(2)
  if (b->k < 1) b->k = 1;
  if (b->k > 30) b->k = 30;
  return &b->base;
}

// InternalFilterPolicy: extracts user keys before delegating.
static void ifp_create_filter(const ldb_filterpolicy* p, const ldb_slice* keys,
                              int n, ldb_buffer* dst) {
  const ldb_internal_filter_policy* ip = (const ldb_internal_filter_policy*)p;
  ldb_slice* mkeys = (ldb_slice*)malloc(sizeof(ldb_slice) * (size_t)(n > 0 ? n : 1));
  for (int i = 0; i < n; i++) {
    mkeys[i] = ldb_extract_user_key(&keys[i]);
  }
  ip->user_policy->create_filter(ip->user_policy, mkeys, n, dst);
  free(mkeys);
}

static int ifp_key_may_match(const ldb_filterpolicy* p, const ldb_slice* key,
                             const ldb_slice* filter) {
  const ldb_internal_filter_policy* ip = (const ldb_internal_filter_policy*)p;
  ldb_slice ukey = ldb_extract_user_key(key);
  return ip->user_policy->key_may_match(ip->user_policy, &ukey, filter);
}

static const char* ifp_name(const ldb_filterpolicy* p) {
  const ldb_internal_filter_policy* ip = (const ldb_internal_filter_policy*)p;
  return ip->user_policy->name(ip->user_policy);
}

static ldb_internal_filter_policy ifp_singleton;

const ldb_filterpolicy* ldb_get_internal_filter_policy(
    const ldb_filterpolicy* user_policy) {
  ifp_singleton.base.name = ifp_name;
  ifp_singleton.base.create_filter = ifp_create_filter;
  ifp_singleton.base.key_may_match = ifp_key_may_match;
  ifp_singleton.base.destroy = NULL;
  ifp_singleton.user_policy = user_policy;
  return &ifp_singleton.base;
}

// =================================================================== arena
#define LDB_ARENA_BLOCK_SIZE 4096

void ldb_arena_init(ldb_arena* a) {
  a->alloc_ptr = NULL;
  a->alloc_bytes_remaining = 0;
  a->blocks = NULL;
  a->blocks_count = 0;
  a->blocks_cap = 0;
  a->memory_usage = 0;
}

void ldb_arena_destroy(ldb_arena* a) {
  for (size_t i = 0; i < a->blocks_count; i++) {
    free(a->blocks[i]);
  }
  free(a->blocks);
  ldb_arena_init(a);
}

static void arena_add_block(ldb_arena* a, char* block) {
  if (a->blocks_count == a->blocks_cap) {
    a->blocks_cap = a->blocks_cap ? a->blocks_cap * 2 : 8;
    a->blocks = (char**)realloc(a->blocks, sizeof(char*) * a->blocks_cap);
    assert(a->blocks);
  }
  a->blocks[a->blocks_count++] = block;
}

static char* arena_allocate_fallback(ldb_arena* a, size_t bytes) {
  if (bytes > LDB_ARENA_BLOCK_SIZE / 4) {
    // Object is more than a quarter of a block.  Allocate it separately
    // to avoid wasting too much space in leftover bytes of the block.
    char* result = (char*)malloc(bytes);
    assert(result);
    arena_add_block(a, result);
    a->memory_usage += bytes;
    return result;
  }
  // We waste the remaining space in the current block.
  size_t size = LDB_ARENA_BLOCK_SIZE;
  char* block = (char*)malloc(size);
  assert(block);
  arena_add_block(a, block);
  a->alloc_ptr = block + bytes;
  a->alloc_bytes_remaining = size - bytes;
  a->memory_usage += size;
  return block;
}

char* ldb_arena_allocate(ldb_arena* a, size_t bytes) {
  assert(bytes > 0);
  if (a->alloc_bytes_remaining >= bytes) {
    char* result = a->alloc_ptr;
    a->alloc_ptr += bytes;
    a->alloc_bytes_remaining -= bytes;
    a->memory_usage += bytes;
    return result;
  }
  return arena_allocate_fallback(a, bytes);
}

size_t ldb_arena_memory_usage(const ldb_arena* a) { return a->memory_usage; }

// =================================================================== uint64 set
void uint64_set_init(uint64_set_t* s) {
  s->items = NULL;
  s->count = 0;
  s->cap = 0;
}

void uint64_set_destroy(uint64_set_t* s) {
  free(s->items);
  uint64_set_init(s);
}

static int uint64_set_lower_bound(const uint64_set_t* s, uint64_t v) {
  size_t lo = 0, hi = s->count;
  while (lo < hi) {
    size_t mid = (lo + hi) / 2;
    if (s->items[mid] < v) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  return (int)lo;
}

void uint64_set_insert(uint64_set_t* s, uint64_t v) {
  int pos = uint64_set_lower_bound(s, v);
  if ((size_t)pos < s->count && s->items[pos] == v) return;
  if (s->count == s->cap) {
    s->cap = s->cap ? s->cap * 2 : 8;
    s->items = (uint64_t*)realloc(s->items, sizeof(uint64_t) * s->cap);
    assert(s->items);
  }
  memmove(s->items + pos + 1, s->items + pos,
          (s->count - (size_t)pos) * sizeof(uint64_t));
  s->items[pos] = v;
  s->count++;
}

int uint64_set_contains(const uint64_set_t* s, uint64_t v) {
  int pos = uint64_set_lower_bound(s, v);
  return (size_t)pos < s->count && s->items[pos] == v;
}

void uint64_set_erase(uint64_set_t* s, uint64_t v) {
  int pos = uint64_set_lower_bound(s, v);
  if ((size_t)pos < s->count && s->items[pos] == v) {
    memmove(s->items + pos, s->items + pos + 1,
            (s->count - (size_t)pos - 1) * sizeof(uint64_t));
    s->count--;
  }
}

size_t uint64_set_size(const uint64_set_t* s) { return s->count; }

uint64_t uint64_set_first(const uint64_set_t* s) {
  assert(s->count > 0);
  return s->items[0];
}

void ldb_buffer_remove_prefix(ldb_buffer* b, size_t n) {
  assert(n <= b->size);
  if (n > 0) {
    memmove(b->data, b->data + n, b->size - n);
    b->size -= n;
  }
}

int ldb_get_length_prefixed_slice_unbounded(const char** pp, ldb_slice* out) {
  uint32_t len;
  const char* p = *pp;
  if (!ldb_get_varint32(&p, p + 5, &len)) return 0;
  *out = ldb_slice_make(p, len);
  *pp = p + len;
  return 1;
}
