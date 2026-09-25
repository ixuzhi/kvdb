// kvdb.h - internal declarations for the kvdb LevelDB-compatible engine.
// The module layout mirrors leveldb: util/ (slices, coding, crc, hash, bloom,
// arena, comparator, cache, env), db/ (dbformat, filename, log, memtable,
// skiplist, write_batch, version_set, db_impl), table/ (block, table, filter,
// merger, two_level_iterator), and the C bindings (c_api.c).
#ifndef KVDB_KVDB_H_
#define KVDB_KVDB_H_

#if !defined(_WIN32) && !defined(_GNU_SOURCE)
// Must precede the system headers below: -std=c11's strict mode hides
// POSIX declarations (strdup, pread, clock_gettime) otherwise. port.h
// defines it too for translation units that include port.h directly.
#define _GNU_SOURCE
#endif

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>

#include "port.h"

#ifdef __cplusplus
extern "C" {
#endif

// ------------------------------------------------------------------ config
#define LDB_K_NUM_LEVELS 7
#define LDB_K_L0_COMPACTION_TRIGGER 4
#define LDB_K_L0_SLOWDOWN_WRITES_TRIGGER 8
#define LDB_K_L0_STOP_WRITES_TRIGGER 12
#define LDB_K_MAX_MEM_COMPACT_LEVEL 2
#define LDB_K_MAX_SEQUENCE_NUMBER ((uint64_t)((0x1ull << 56) - 1))

// ------------------------------------------------------------------ slice
typedef struct ldb_slice {
  const char* data;
  size_t size;
} ldb_slice;

static inline ldb_slice ldb_slice_make(const char* d, size_t n) {
  ldb_slice s;
  s.data = d;
  s.size = n;
  return s;
}
static inline ldb_slice ldb_slice_str(const char* s) {
  return ldb_slice_make(s, strlen(s));
}
static inline int ldb_slice_compare(const ldb_slice* a, const ldb_slice* b) {
  size_t min = a->size < b->size ? a->size : b->size;
  int r = min ? memcmp(a->data, b->data, min) : 0;
  if (r == 0) {
    if (a->size < b->size)
      r = -1;
    else if (a->size > b->size)
      r = 1;
  }
  return r;
}
static inline int ldb_slice_equals(const ldb_slice* a, const ldb_slice* b) {
  return a->size == b->size &&
         (a->size == 0 || memcmp(a->data, b->data, a->size) == 0);
}
static inline int ldb_slice_empty(const ldb_slice* s) { return s->size == 0; }
static inline int ldb_slice_starts_with(const ldb_slice* s, const ldb_slice* p) {
  return s->size >= p->size && memcmp(s->data, p->data, p->size) == 0;
}

// ------------------------------------------------------------------ buffer
typedef struct ldb_buffer {
  char* data;
  size_t size;
  size_t cap;
} ldb_buffer;

void ldb_buffer_init(ldb_buffer* b);
void ldb_buffer_destroy(ldb_buffer* b);
void ldb_buffer_clear(ldb_buffer* b);
static inline int ldb_buffer_empty(const ldb_buffer* b) { return b->size == 0; }
// Re-views a buffer's contents as a slice (does not copy).
static inline ldb_slice ldb_bslice(const ldb_buffer* b) {
  return ldb_slice_make(b->data, b->size);
}
void ldb_buffer_remove_prefix(ldb_buffer* b, size_t n);
void ldb_buffer_reserve(ldb_buffer* b, size_t n);
void ldb_buffer_resize(ldb_buffer* b, size_t n);
void ldb_buffer_append(ldb_buffer* b, const void* data, size_t n);
void ldb_buffer_append_slice(ldb_buffer* b, const ldb_slice* s);
void ldb_buffer_append_str(ldb_buffer* b, const char* s);
void ldb_buffer_copy(ldb_buffer* b, const ldb_buffer* src);
void ldb_buffer_swap(ldb_buffer* a, ldb_buffer* b);
static inline ldb_slice ldb_buffer_slice(const ldb_buffer* b) {
  return ldb_slice_make(b->data, b->size);
}

// ------------------------------------------------------------------ status
enum {
  LDB_OK = 0,
  LDB_NOTFOUND = 1,
  LDB_CORRUPTION = 2,
  LDB_NOTSUPPORTED = 3,
  LDB_INVALID_ARGUMENT = 4,
  LDB_IO_ERROR = 5,
};

typedef struct ldb_status {
  int code;
  char* msg;  // malloc'ed or NULL; owned.
} ldb_status;

static inline int ldb_ok(ldb_status s) { return s.code == LDB_OK; }
ldb_status ldb_status_ok(void);
ldb_status ldb_status_new(int code, const char* msg1, const char* msg2);
ldb_status ldb_status_notfound(const char* msg1, const char* msg2);
ldb_status ldb_status_corruption(const char* msg1, const char* msg2);
ldb_status ldb_status_notsupported(const char* msg1, const char* msg2);
ldb_status ldb_status_invalid_argument(const char* msg1, const char* msg2);
ldb_status ldb_status_io_error(const char* msg1, const char* msg2);
ldb_status ldb_status_copy(ldb_status s);
void ldb_status_destroy(ldb_status* s);
// Assigns v into *s, destroying the old value first.
static inline void ldb_status_set(ldb_status* s, ldb_status v) {
  ldb_status_destroy(s);
  *s = v;
}
// Drops a status the caller deliberately ignores, freeing its message. The C++
// reference can discard a Status by simply not naming it (RAII); a value type
// cannot, so every "ignore errors" call site has to say so out loud.
static inline void ldb_status_release(ldb_status s) { ldb_status_destroy(&s); }
// Returns malloc'ed string; caller must free. Formats like leveldb:
// "Corruption: msg", "NotFound: msg", "OK", "IO error: msg", ...
char* ldb_status_to_string(ldb_status s);
// Sets *errptr per the leveldb C API protocol. errptr may be NULL.
void ldb_save_error(char** errptr, ldb_status s);
static inline ldb_status ldb_status_from_errno(int e, const char* context) {
  char buf[128];
  snprintf(buf, sizeof(buf), "%s (%s)", context ? context : "error",
           strerror(e));
  return ldb_status_io_error(buf, NULL);
}

// ------------------------------------------------------------------ logging
typedef struct ldb_logger {
  void (*logv)(struct ldb_logger*, const char* fmt, va_list ap);
  // Releases whatever the env acquired in new_logger (a FILE*, a handle).
  // NULL means the logger owns nothing and is freed by the caller.
  void (*destroy)(struct ldb_logger*);
} ldb_logger;

void ldb_log(ldb_logger* info_log, const char* fmt, ...);
void ldb_append_number_to(ldb_buffer* str, uint64_t num);
char* ldb_number_to_string(uint64_t num);
void ldb_append_escaped_string_to(ldb_buffer* str, const ldb_slice* value);
char* ldb_escape_string(const ldb_slice* value);
int ldb_consume_decimal_number(ldb_slice* in, uint64_t* val);
// Appends a printf-formatted text; used for stats strings.
void ldb_buffer_appendf(ldb_buffer* b, const char* fmt, ...);

// ------------------------------------------------------------------ coding
void ldb_put_fixed32(ldb_buffer* dst, uint32_t v);
void ldb_put_fixed64(ldb_buffer* dst, uint64_t v);
void ldb_put_varint32(ldb_buffer* dst, uint32_t v);
void ldb_put_varint64(ldb_buffer* dst, uint64_t v);
void ldb_put_varint32_varint32(ldb_buffer* dst, uint32_t a, uint32_t b);
void ldb_put_length_prefixed_slice(ldb_buffer* dst, const ldb_slice* s);
void ldb_put_length_prefixed_slice2(ldb_buffer* dst, const ldb_slice* a,
                                    const ldb_slice* b);
char* ldb_encode_varint32(char* dst, uint32_t v);
char* ldb_encode_varint64(char* dst, uint64_t v);
static inline void ldb_encode_fixed32(char* dst, uint32_t v) {
  uint8_t b[4];
  b[0] = (uint8_t)(v & 0xff);
  b[1] = (uint8_t)((v >> 8) & 0xff);
  b[2] = (uint8_t)((v >> 16) & 0xff);
  b[3] = (uint8_t)((v >> 24) & 0xff);
  memcpy(dst, b, 4);
}
static inline void ldb_encode_fixed64(char* dst, uint64_t v) {
  uint8_t b[8];
  b[0] = (uint8_t)(v & 0xff);
  b[1] = (uint8_t)((v >> 8) & 0xff);
  b[2] = (uint8_t)((v >> 16) & 0xff);
  b[3] = (uint8_t)((v >> 24) & 0xff);
  b[4] = (uint8_t)((v >> 32) & 0xff);
  b[5] = (uint8_t)((v >> 40) & 0xff);
  b[6] = (uint8_t)((v >> 48) & 0xff);
  b[7] = (uint8_t)((v >> 56) & 0xff);
  memcpy(dst, b, 8);
}
static inline uint32_t ldb_decode_fixed32(const char* src) {
  const uint8_t* p = (const uint8_t*)src;
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
         ((uint32_t)p[3] << 24);
}
static inline uint64_t ldb_decode_fixed64(const char* src) {
  const uint8_t* p = (const uint8_t*)src;
  uint32_t lo = ldb_decode_fixed32((const char*)p);
  uint32_t hi = ldb_decode_fixed32((const char*)(p + 4));
  return ((uint64_t)hi << 32) | lo;
}
// Returns 1 on success; *value set; advances *pp.
int ldb_get_varint32(const char** pp, const char* limit, uint32_t* value);
int ldb_get_varint64(const char** pp, const char* limit, uint64_t* value);
// Length-prefixed slice readers. On success advances *pp.
int ldb_get_length_prefixed_slice(const char** pp, const char* limit,
                                  ldb_slice* out);
// Decodes a length-prefixed slice without an end bound (used for skiplist
// entries whose total length is unknown; mirrors leveldb GetLengthPrefixedSlice).
int ldb_get_length_prefixed_slice_unbounded(const char** pp, ldb_slice* out);

// ------------------------------------------------------------------ crc32c
uint32_t ldb_crc32c_value(const char* data, size_t n);
uint32_t ldb_crc32c_extend(uint32_t crc, const char* data, size_t n);
static inline uint32_t ldb_crc32c_mask(uint32_t crc) {
  const uint32_t kMaskDelta = 0xa282ead8u;
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}
static inline uint32_t ldb_crc32c_unmask(uint32_t masked) {
  const uint32_t kMaskDelta = 0xa282ead8u;
  uint32_t rot = masked - kMaskDelta;
  return ((rot >> 17) | (rot << 15));
}

// ------------------------------------------------------------------ hash
uint32_t ldb_hash(const char* data, size_t n, uint32_t seed);

// ------------------------------------------------------------------ random
typedef struct ldb_random {
  uint32_t seed;
} ldb_random;
static inline void ldb_random_init(ldb_random* r, uint32_t s) { r->seed = s & 0x7fffffffu; }
static inline uint32_t ldb_random_next(ldb_random* r) {
  const uint32_t M = 2147483647L;  // 2^31-1
  const uint64_t A = 16807;
  uint64_t product = r->seed * A;
  r->seed = (uint32_t)((product >> 31) + (product & M));
  if (r->seed > M) r->seed -= M;
  return r->seed;
}
static inline uint32_t ldb_random_uniform(ldb_random* r, int n) {
  return ldb_random_next(r) % (uint32_t)n;
}
static inline int ldb_random_one_in(ldb_random* r, int n) {
  return ldb_random_uniform(r, n) == 0;
}
static inline uint32_t ldb_random_skewed(ldb_random* r, int max_log) {
  return ldb_random_uniform(r, (int)(1 << ldb_random_uniform(r, max_log + 1)));
}

// ------------------------------------------------------------------ comparator
typedef struct ldb_comparator {
  const char* (*name)(const struct ldb_comparator*);
  int (*compare)(const struct ldb_comparator*, const ldb_slice* a, const ldb_slice* b);
  // Mutates *start (ldb_buffer) so that it is placed between start and limit.
  void (*find_shortest_separator)(const struct ldb_comparator*, ldb_buffer* start,
                                  const ldb_slice* limit);
  void (*find_short_successor)(const struct ldb_comparator*, ldb_buffer* key);
  void* impl;  // implementation-specific state (may be NULL)
} ldb_comparator;

const ldb_comparator* ldb_bytewise_comparator(void);
static inline int ldb_cmp(const ldb_comparator* c, const ldb_slice* a,
                          const ldb_slice* b) {
  return c->compare(c, a, b);
}

// Internal key comparator (wraps a user comparator).
typedef struct ldb_ikc {
  const ldb_comparator* user_comparator;
} ldb_ikc;

void ldb_ikc_init(ldb_ikc* ikc, const ldb_comparator* ucmp);
const char* ldb_ikc_name(void);
int ldb_ikc_compare(const ldb_ikc* ikc, const ldb_slice* akey,
                    const ldb_slice* bkey);
void ldb_ikc_find_shortest_separator(const ldb_ikc* ikc, ldb_buffer* start,
                                     const ldb_slice* limit);
void ldb_ikc_find_short_successor(const ldb_ikc* ikc, ldb_buffer* key);
// Wraps an internal key comparator in the generic ldb_comparator interface
// (adapter->impl points at *ikc, which must outlive the adapter).
void ldb_internal_comparator_adapter(const ldb_ikc* ikc,
                                     ldb_comparator* adapter);

// ------------------------------------------------------------------ dbformat
enum {
  LDB_TYPE_DELETION = 0,
  LDB_TYPE_VALUE = 1,
};
#define LDB_VALUE_TYPE_FOR_SEEK LDB_TYPE_VALUE
#define LDB_VALUE_TYPE_FOR_SEEK_FOR_PREV LDB_TYPE_DELETION

typedef struct ldb_parsed_internal_key {
  ldb_slice user_key;
  uint64_t sequence;
  int type;
} ldb_parsed_internal_key;

static inline uint64_t ldb_pack_sequence_and_type(uint64_t seq, int t) {
  return (seq << 8) | (uint64_t)t;
}
static inline void ldb_unpack_sequence_and_type(uint64_t tag, uint64_t* seq,
                                                int* type) {
  *seq = tag >> 8;
  *type = (int)(tag & 0xff);
}
static inline ldb_slice ldb_extract_user_key(const ldb_slice* ikey) {
  // Matches leveldb's dbformat.h: the caller guarantees an internal key, and
  // without this the subtraction below silently wraps to ~2^64.
  assert(ikey->size >= 8);
  return ldb_slice_make(ikey->data, ikey->size - 8);
}
int ldb_parse_internal_key(const ldb_slice* ikey,
                           ldb_parsed_internal_key* out);
void ldb_append_internal_key(ldb_buffer* dst, const ldb_slice* user_key,
                             uint64_t seq, int type);
void ldb_append_internal_key_parsed(ldb_buffer* dst,
                                    const ldb_parsed_internal_key* key);

// LookupKey: internal key used for seeks. Owns its buffer.
typedef struct ldb_lookup_key {
  ldb_buffer kbuf;  // varint32(len+8) | user_key | fixed64(seq|type)
  ldb_slice memtable_key;
  ldb_slice user_key;
  ldb_slice internal_key;
} ldb_lookup_key;

void ldb_lookup_key_init(ldb_lookup_key* lk, const ldb_slice* user_key,
                         uint64_t seq);
void ldb_lookup_key_destroy(ldb_lookup_key* lk);

// ------------------------------------------------------------------ env
typedef struct ldb_seq_file ldb_seq_file;
typedef struct ldb_rand_file ldb_rand_file;
typedef struct ldb_writable_file ldb_writable_file;
typedef struct ldb_file_lock ldb_file_lock;
typedef struct ldb_env ldb_env;
typedef struct ldb_strings {
  char** items;
  size_t count;
  size_t cap;
} ldb_strings;

void ldb_strings_init(ldb_strings* v);
void ldb_strings_destroy(ldb_strings* v);
void ldb_strings_push(ldb_strings* v, char* s);  // takes ownership

typedef struct ldb_seq_file_methods {
  void (*destroy)(ldb_seq_file*);
  // Reads up to n bytes into scratch; sets *result (points into scratch).
  ldb_status (*read)(ldb_seq_file*, size_t n, ldb_slice* result, char* scratch);
  // Skips n bytes (used by log reader).
  ldb_status (*skip)(ldb_seq_file*, uint64_t n);
} ldb_seq_file_methods;

typedef struct ldb_rand_file_methods {
  void (*destroy)(ldb_rand_file*);
  ldb_status (*read)(ldb_rand_file*, uint64_t offset, size_t n,
                     ldb_slice* result, char* scratch);
} ldb_rand_file_methods;

typedef struct ldb_writable_file_methods {
  void (*destroy)(ldb_writable_file*);
  ldb_status (*append)(ldb_writable_file*, const ldb_slice* data);
  ldb_status (*close)(ldb_writable_file*);
  ldb_status (*flush)(ldb_writable_file*);
  ldb_status (*sync)(ldb_writable_file*);
} ldb_writable_file_methods;

struct ldb_seq_file {
  const ldb_seq_file_methods* m;
};

struct ldb_rand_file {
  const ldb_rand_file_methods* m;
};

struct ldb_writable_file {
  const ldb_writable_file_methods* m;
};

struct ldb_file_lock {
  void* impl;
};

typedef struct ldb_env_vtbl {
  ldb_status (*new_sequential_file)(ldb_env*, const char* fname,
                                    ldb_seq_file** out);
  ldb_status (*new_random_access_file)(ldb_env*, const char* fname,
                                       ldb_rand_file** out);
  ldb_status (*new_writable_file)(ldb_env*, const char* fname,
                                  ldb_writable_file** out);
  ldb_status (*new_appendable_file)(ldb_env*, const char* fname,
                                    ldb_writable_file** out);
  int (*file_exists)(ldb_env*, const char* fname);
  ldb_status (*get_children)(ldb_env*, const char* dir, ldb_strings* out);
  ldb_status (*remove_file)(ldb_env*, const char* fname);
  ldb_status (*create_dir)(ldb_env*, const char* dirname);
  ldb_status (*delete_dir)(ldb_env*, const char* dirname);
  ldb_status (*get_file_size)(ldb_env*, const char* fname, uint64_t* size);
  ldb_status (*rename_file)(ldb_env*, const char* src, const char* dst);
  ldb_status (*lock_file)(ldb_env*, const char* fname, ldb_file_lock** out);
  ldb_status (*unlock_file)(ldb_env*, ldb_file_lock* lock);
  void (*schedule)(ldb_env*, void (*fn)(void*), void* arg);
  void (*start_thread)(ldb_env*, void (*fn)(void*), void* arg);
  uint64_t (*now_micros)(ldb_env*);
  void (*sleep_for_microseconds)(ldb_env*, int64_t micros);
  ldb_status (*get_test_directory)(ldb_env*, ldb_buffer* path);
  ldb_status (*new_logger)(ldb_env*, const char* fname, ldb_logger** out);
  void (*destroy)(ldb_env*);
} ldb_env_vtbl;

struct ldb_env {
  const ldb_env_vtbl* vtbl;
};

ldb_env* ldb_env_default(void);
// In-memory env for tests (mirrors helpers/memenv).
ldb_env* ldb_memenv_new(void);
void ldb_memenv_destroy(ldb_env* env);
// Convenience wrappers (forward to vtbl).
ldb_status ldb_env_new_sequential_file(ldb_env* e, const char* f,
                                       ldb_seq_file** out);
ldb_status ldb_env_new_random_access_file(ldb_env* e, const char* f,
                                          ldb_rand_file** out);
ldb_status ldb_env_new_writable_file(ldb_env* e, const char* f,
                                     ldb_writable_file** out);
ldb_status ldb_env_new_appendable_file(ldb_env* e, const char* f,
                                       ldb_writable_file** out);
int ldb_env_file_exists(ldb_env* e, const char* f);
ldb_status ldb_env_get_children(ldb_env* e, const char* dir, ldb_strings* out);
ldb_status ldb_env_remove_file(ldb_env* e, const char* f);
ldb_status ldb_env_create_dir(ldb_env* e, const char* d);
ldb_status ldb_env_delete_dir(ldb_env* e, const char* d);
ldb_status ldb_env_get_file_size(ldb_env* e, const char* f, uint64_t* size);
ldb_status ldb_env_rename_file(ldb_env* e, const char* src, const char* dst);
ldb_status ldb_env_lock_file(ldb_env* e, const char* f, ldb_file_lock** out);
ldb_status ldb_env_unlock_file(ldb_env* e, ldb_file_lock* l);
void ldb_env_schedule(ldb_env* e, void (*fn)(void*), void* arg);
void ldb_env_start_thread(ldb_env* e, void (*fn)(void*), void* arg);
uint64_t ldb_env_now_micros(ldb_env* e);
void ldb_env_sleep_for_microseconds(ldb_env* e, int64_t us);
ldb_status ldb_env_get_test_directory(ldb_env* e, ldb_buffer* path);
ldb_status ldb_env_new_logger(ldb_env* e, const char* f, ldb_logger** out);
// Calls the logger's own destroy hook, which is the only thing that can hand
// back the OS file the env opened for it.
void ldb_logger_destroy(ldb_logger* log);

ldb_status ldb_write_string_to_file_sync(ldb_env* env, const ldb_buffer* data,
                                         const char* fname);
ldb_status ldb_read_file_to_string(ldb_env* env, const char* fname,
                                   ldb_buffer* dst);

// ------------------------------------------------------------------ options
enum { LDB_NO_COMPRESSION = 0, LDB_SNAPPY_COMPRESSION = 1 };

typedef struct ldb_filterpolicy ldb_filterpolicy;
typedef struct ldb_cache ldb_cache;

typedef struct ldb_options {
  const ldb_comparator* comparator;  // user comparator
  const ldb_filterpolicy* filter_policy;
  ldb_env* env;
  ldb_logger* info_log;
  ldb_cache* block_cache;
  int create_if_missing;
  int error_if_exists;
  int paranoid_checks;
  int reuse_logs;
  size_t write_buffer_size;
  int max_open_files;
  size_t block_size;
  int block_restart_interval;
  size_t max_file_size;
  int compression;
} ldb_options;

void ldb_options_init(ldb_options* opt);

typedef struct ldb_read_options {
  int verify_checksums;
  int fill_cache;
  const void* snapshot;  // ldb_snapshot_impl*
} ldb_read_options;

void ldb_read_options_init(ldb_read_options* ro);

typedef struct ldb_write_options {
  int sync;
} ldb_write_options;

void ldb_write_options_init(ldb_write_options* wo);

// ------------------------------------------------------------------ filterpolicy
struct ldb_filterpolicy {
  const char* (*name)(const struct ldb_filterpolicy*);
  void (*create_filter)(const struct ldb_filterpolicy*, const ldb_slice* keys, int n,
                        ldb_buffer* dst);
  int (*key_may_match)(const struct ldb_filterpolicy*, const ldb_slice* key,
                       const ldb_slice* filter);
  void (*destroy)(struct ldb_filterpolicy*);  // may be NULL
  void* impl;
};

const ldb_filterpolicy* ldb_new_bloom_filter_policy(int bits_per_key);

// InternalFilterPolicy: operates on user keys extracted from internal keys.
typedef struct ldb_internal_filter_policy {
  ldb_filterpolicy base;
  const ldb_filterpolicy* user_policy;
} ldb_internal_filter_policy;

// `self` is caller-owned storage (leveldb keeps one instance per DBImpl and
// per Repairer); the returned pointer stays valid only while `self` lives, and
// `user_policy` must outlive it.
const ldb_filterpolicy* ldb_init_internal_filter_policy(
    ldb_internal_filter_policy* self,
    const ldb_filterpolicy* user_policy);

// ------------------------------------------------------------------ arena
typedef struct ldb_arena {
  char* alloc_ptr;
  size_t alloc_bytes_remaining;
  char** blocks;
  size_t blocks_count;
  size_t blocks_cap;
  size_t memory_usage;
} ldb_arena;

void ldb_arena_init(ldb_arena* a);
void ldb_arena_destroy(ldb_arena* a);
char* ldb_arena_allocate(ldb_arena* a, size_t bytes);
// Mirrors Arena::AllocateAligned: callers that cast the result to a pointer
// type (skiplist nodes) must not get a bump-pointer address of arbitrary
// alignment.
char* ldb_arena_allocate_aligned(ldb_arena* a, size_t bytes);
size_t ldb_arena_memory_usage(const ldb_arena* a);

// ------------------------------------------------------------------ cache
typedef struct ldb_cache_handle ldb_cache_handle;

struct ldb_cache {
  ldb_cache_handle* (*insert)(ldb_cache*, const ldb_slice* key, void* value,
                              size_t charge, void (*deleter)(const ldb_slice*,
                                                             void* value));
  ldb_cache_handle* (*lookup)(ldb_cache*, const ldb_slice* key);
  void (*release)(ldb_cache*, ldb_cache_handle*);
  void* (*value)(ldb_cache*, ldb_cache_handle*);
  void (*erase)(ldb_cache*, const ldb_slice* key);
  uint64_t (*new_id)(ldb_cache*);
  void (*prune)(ldb_cache*);
  size_t (*total_charge)(ldb_cache*);
  void (*destroy)(ldb_cache*);
};

ldb_cache* ldb_cache_new_lru(size_t capacity);

// ------------------------------------------------------------------ skiplist
typedef struct ldb_skiplist_node {
  const char* key;
  struct ldb_skiplist_node* next[1];
} ldb_skiplist_node;

typedef struct ldb_skiplist ldb_skiplist;
// compare(key_in_skiplist, target) with length-prefixed slices.
typedef int (*ldb_skiplist_cmp)(void* arg, const char* a, const char* b);

ldb_skiplist* ldb_skiplist_new(void* cmp_arg, ldb_skiplist_cmp cmp,
                               ldb_arena* arena);
void ldb_skiplist_insert(ldb_skiplist* list, const char* key);
// Returns 1 and sets *out to an iterator position at first key >= target.
int ldb_skiplist_seek(const ldb_skiplist* list, const char* target,
                      const char** out);
int ldb_skiplist_seek_to_first(const ldb_skiplist* list, const char** out);
int ldb_skiplist_seek_to_last(const ldb_skiplist* list, const char** out);
// From a current position, returns next/prev/valid.
int ldb_skiplist_valid(const ldb_skiplist* list, const char* pos);
const char* ldb_skiplist_next(const ldb_skiplist* list, const char* pos);
const char* ldb_skiplist_prev(const ldb_skiplist* list, const char* pos);
uint64_t ldb_skiplist_approximate_memory(const ldb_skiplist* list);

// ------------------------------------------------------------------ memtable
typedef struct ldb_memtable ldb_memtable;
typedef struct ldb_iterator ldb_iterator;

ldb_memtable* ldb_memtable_new(const ldb_ikc* comparator);
void ldb_memtable_ref(ldb_memtable* m);
void ldb_memtable_unref(ldb_memtable* m);
void ldb_memtable_add(ldb_memtable* m, const ldb_slice* internal_key,
                      const ldb_slice* value);
size_t ldb_memtable_approximate_memory_usage(const ldb_memtable* m);
// Gets value for lookup key; returns 1 if the key was found (value or
// deletion tombstone). For a value, copies into *value; for a deletion,
// sets *s to NotFound and returns 1.
int ldb_memtable_get(const ldb_memtable* m, const ldb_lookup_key* key,
                     ldb_buffer* value, ldb_status* s);
ldb_iterator* ldb_memtable_new_iterator(const ldb_memtable* m);
uint64_t ldb_memtable_first_sequence_number(const ldb_memtable* m);

// ------------------------------------------------------------------ iterators
typedef struct ldb_cleanup_node {
  void (*fn)(void*, void*);
  void* arg1;
  void* arg2;
  struct ldb_cleanup_node* next;
} ldb_cleanup_node;

typedef struct ldb_iterator_methods {
  void (*destroy)(ldb_iterator*);
  int (*valid)(const ldb_iterator*);
  void (*seek_to_first)(ldb_iterator*);
  void (*seek_to_last)(ldb_iterator*);
  void (*seek)(ldb_iterator*, const ldb_slice* target);
  void (*next)(ldb_iterator*);
  void (*prev)(ldb_iterator*);
  ldb_slice (*key)(const ldb_iterator*);
  ldb_slice (*value)(const ldb_iterator*);
  ldb_status (*status)(const ldb_iterator*);
} ldb_iterator_methods;

struct ldb_iterator {
  const ldb_iterator_methods* m;
  void* impl;
  ldb_cleanup_node* cleanups;
};

ldb_iterator* ldb_iterator_alloc(const struct ldb_iterator_methods* m,
                                 void* impl);
void ldb_iterator_destroy(ldb_iterator* it);
void ldb_iterator_register_cleanup(ldb_iterator* it, void (*fn)(void*, void*),
                                   void* arg1, void* arg2);
static inline int ldb_iter_valid(const ldb_iterator* it) {
  return it->m->valid(it);
}
static inline void ldb_iter_seek_to_first(ldb_iterator* it) {
  it->m->seek_to_first(it);
}
static inline void ldb_iter_seek_to_last(ldb_iterator* it) {
  it->m->seek_to_last(it);
}
static inline void ldb_iter_seek(ldb_iterator* it, const ldb_slice* t) {
  it->m->seek(it, t);
}
static inline void ldb_iter_next(ldb_iterator* it) { it->m->next(it); }
static inline void ldb_iter_prev(ldb_iterator* it) { it->m->prev(it); }
static inline ldb_slice ldb_iter_key(const ldb_iterator* it) {
  return it->m->key(it);
}
static inline ldb_slice ldb_iter_value(const ldb_iterator* it) {
  return it->m->value(it);
}
static inline ldb_status ldb_iter_status(const ldb_iterator* it) {
  return it->m->status(it);
}
// Walks to the first entry >= target while running cleanup on each step.
void ldb_iter_seek_run_cleanup(ldb_iterator* it, const ldb_slice* target);

// Empty iterator (invalid always) and error iterator.
ldb_iterator* ldb_new_empty_iterator(void);
ldb_iterator* ldb_new_error_iterator(ldb_status s);

// Merging iterator over children (linear scan like leveldb).
ldb_iterator* ldb_new_merging_iterator(const ldb_ikc* comparator,
                                       ldb_iterator** children, size_t n);

// ------------------------------------------------------------------ snappy
int ldb_snappy_compress(const char* input, size_t input_len, ldb_buffer* out);
int ldb_snappy_get_uncompressed_length(const char* input, size_t input_len,
                                       size_t* result);
int ldb_snappy_uncompress(const char* input, size_t input_len, char* output,
                          size_t output_len);

// ------------------------------------------------------------------ log (WAL)
#define LDB_LOG_BLOCK_SIZE 32768
#define LDB_LOG_HEADER_SIZE (4 + 2 + 1)

enum {
  LDB_LOG_ZERO_TYPE = 0,
  LDB_LOG_FULL_TYPE = 1,
  LDB_LOG_FIRST_TYPE = 2,
  LDB_LOG_MIDDLE_TYPE = 3,
  LDB_LOG_LAST_TYPE = 4,
};

typedef struct ldb_log_writer {
  ldb_writable_file* dest;
  size_t block_offset;  // current offset in block
} ldb_log_writer;

ldb_log_writer* ldb_log_writer_new(ldb_writable_file* dest);
ldb_log_writer* ldb_log_writer_new_at(ldb_writable_file* dest,
                                      uint64_t dest_length);
void ldb_log_writer_destroy(ldb_log_writer* w);
ldb_status ldb_log_writer_add_record(ldb_log_writer* w, const ldb_slice* data);

typedef struct ldb_log_reader ldb_log_reader;
typedef void (*ldb_log_corruption_cb)(void* arg, size_t bytes,
                                      ldb_status status);

ldb_log_reader* ldb_log_reader_new(ldb_seq_file* file, void* reporter,
                                   ldb_log_corruption_cb corruption_cb,
                                   int checksum, uint64_t initial_offset);
void ldb_log_reader_destroy(ldb_log_reader* r);
// Returns 1 and fills *record (points into reader-internal or caller scratch)
// on success; 0 at EOF.
int ldb_log_reader_read_record(ldb_log_reader* r, ldb_slice* record,
                               ldb_buffer* scratch);
// Offset of the beginning of the last record returned by read_record.
uint64_t ldb_log_reader_last_record_offset(const ldb_log_reader* r);

// ------------------------------------------------------------------ write batch
typedef struct ldb_write_batch {
  ldb_buffer rep;
} ldb_write_batch;

void ldb_write_batch_init(ldb_write_batch* b);
void ldb_write_batch_destroy(ldb_write_batch* b);
void ldb_write_batch_clear(ldb_write_batch* b);
ldb_slice ldb_write_batch_contents(const ldb_write_batch* b);
void ldb_write_batch_set_contents(ldb_write_batch* b, const ldb_slice* src);
size_t ldb_write_batch_count(const ldb_write_batch* b);
void ldb_write_batch_set_count(ldb_write_batch* b, size_t n);
uint64_t ldb_write_batch_sequence(const ldb_write_batch* b);
void ldb_write_batch_set_sequence(ldb_write_batch* b, uint64_t seq);
void ldb_write_batch_put(ldb_write_batch* b, const ldb_slice* key,
                         const ldb_slice* value);
void ldb_write_batch_delete(ldb_write_batch* b, const ldb_slice* key);
void ldb_write_batch_append(ldb_write_batch* dst, const ldb_write_batch* src);
size_t ldb_write_batch_byte_size(const ldb_write_batch* b);
// Applies the batch to memtable; sets *last_sequence to the new last sequence
// if non-NULL.
ldb_status ldb_write_batch_insert_into(const ldb_write_batch* b,
                                       ldb_memtable* mem, uint64_t* last_seq);
// Iteration callback style used by the C API and repair.
void ldb_write_batch_iterate(
    const ldb_write_batch* b, void* state,
    void (*put)(void*, const ldb_slice* k, const ldb_slice* v),
    void (*deleted)(void*, const ldb_slice* k));

// ------------------------------------------------------------------ filename
enum {
  LDB_K_LOG_FILE = 0,
  LDB_K_DB_LOCK_FILE = 1,
  LDB_K_TABLE_FILE = 2,
  LDB_K_DESCRIPTOR_FILE = 3,
  LDB_K_CURRENT_FILE = 4,
  LDB_K_TEMP_FILE = 5,
  LDB_K_INFO_LOG_FILE = 6  // either the current one, or an old one
};

char* ldb_log_file_name(const char* dbname, uint64_t number);
char* ldb_table_file_name(const char* dbname, uint64_t number);
char* ldb_sst_table_file_name(const char* dbname, uint64_t number);
char* ldb_descriptor_file_name(const char* dbname, uint64_t number);
char* ldb_current_file_name(const char* dbname);
char* ldb_lock_file_name(const char* dbname);
char* ldb_temp_file_name(const char* dbname, uint64_t number);
char* ldb_info_log_file_name(const char* dbname);
char* ldb_old_info_log_file_name(const char* dbname);
int ldb_parse_file_name(const char* filename, uint64_t* number, int* type);
ldb_status ldb_set_current_file(ldb_env* env, const char* dbname,
                                uint64_t descriptor_number);

// ------------------------------------------------------------------ table formats
#define LDB_BLOCK_TRAILER_SIZE 5
#define LDB_TABLE_MAGIC_NUMBER UINT64_C(0xdb4775248b80fb57)
#define LDB_FOOTER_ENCODED_LENGTH 48
#define LDB_BLOCK_HANDLE_MAX_ENCODED_LENGTH 2 * 10

typedef struct ldb_block_handle {
  uint64_t offset;
  uint64_t size;
} ldb_block_handle;

void ldb_block_handle_init(ldb_block_handle* h);
void ldb_block_handle_encode(const ldb_block_handle* h, ldb_buffer* dst);
ldb_status ldb_block_handle_decode(ldb_block_handle* h, ldb_slice* input);

typedef struct ldb_footer {
  ldb_block_handle metaindex_handle;
  ldb_block_handle index_handle;
} ldb_footer;

void ldb_footer_encode(const ldb_footer* f, ldb_buffer* dst);
ldb_status ldb_footer_decode(ldb_footer* f, ldb_slice* input);

typedef struct ldb_block_contents {
  ldb_slice data;
  char* alloc;  // malloc'ed buffer to free, or NULL
  int cachable;
} ldb_block_contents;

ldb_status ldb_read_block(ldb_rand_file* file, const ldb_read_options* options,
                          const ldb_block_handle* handle,
                          ldb_block_contents* result);
void ldb_block_contents_destroy(ldb_block_contents* c);

// ------------------------------------------------------------------ block builder
typedef struct ldb_block_builder {
  const ldb_options* options;
  ldb_buffer buffer;
  uint32_t* restarts;
  size_t restarts_count;
  size_t restarts_cap;
  int counter;
  int finished;
  ldb_buffer last_key;
} ldb_block_builder;

void ldb_block_builder_init(ldb_block_builder* b, const ldb_options* options);
void ldb_block_builder_reset(ldb_block_builder* b);
void ldb_block_builder_destroy(ldb_block_builder* b);
void ldb_block_builder_add(ldb_block_builder* b, const ldb_slice* key,
                           const ldb_slice* value);
ldb_slice ldb_block_builder_finish(ldb_block_builder* b);
size_t ldb_block_builder_current_size_estimate(const ldb_block_builder* b);
int ldb_block_builder_empty(const ldb_block_builder* b);

// ------------------------------------------------------------------ block reader
typedef struct ldb_block ldb_block;

ldb_block* ldb_block_new(ldb_block_contents* contents);
void ldb_block_destroy(ldb_block* b);
ldb_iterator* ldb_block_new_iterator(const ldb_block* b,
                                     const ldb_comparator* comparator);
size_t ldb_block_size(const ldb_block* b);

// ------------------------------------------------------------------ filter block
typedef struct ldb_filter_block_builder {
  const ldb_filterpolicy* policy;
  size_t base_lg;
  ldb_buffer result;
  ldb_buffer keys_data;  // flattened key contents
  size_t* start;         // start offsets in keys_data for each key
  size_t start_count;
  size_t start_cap;
  uint32_t* filter_offsets;
  size_t filter_offsets_count;
  size_t filter_offsets_cap;
} ldb_filter_block_builder;

void ldb_filter_block_builder_init(ldb_filter_block_builder* b,
                                   const ldb_filterpolicy* policy);
void ldb_filter_block_builder_destroy(ldb_filter_block_builder* b);
void ldb_filter_block_builder_start_block(ldb_filter_block_builder* b,
                                          uint64_t block_offset);
void ldb_filter_block_builder_add_key(ldb_filter_block_builder* b,
                                      const ldb_slice* key);
ldb_slice ldb_filter_block_builder_finish(ldb_filter_block_builder* b);

typedef struct ldb_filter_block_reader {
  const ldb_filterpolicy* policy;
  const char* data;  // pointer to filter data (owned externally)
  size_t base_lg;
  size_t num;
  uint32_t offset_of_offsets;  // offset of the offsets array
} ldb_filter_block_reader;

int ldb_filter_block_reader_init(ldb_filter_block_reader* r,
                                 const ldb_filterpolicy* policy,
                                 const ldb_slice* contents);
int ldb_filter_block_reader_key_may_match(const ldb_filter_block_reader* r,
                                          uint64_t block_offset,
                                          const ldb_slice* key);

// ------------------------------------------------------------------ table
typedef struct ldb_table ldb_table;

ldb_status ldb_table_open(const ldb_options* options, ldb_rand_file* file,
                          uint64_t size, ldb_table** table);
void ldb_table_destroy(ldb_table* t);
ldb_iterator* ldb_table_new_iterator(const ldb_table* t,
                                     const ldb_read_options* options);
// BlockReader callback used by two_level_iterator and table cache.
ldb_iterator* ldb_table_block_reader(void* arg, const ldb_read_options* options,
                                     const ldb_slice* index_value);
ldb_status ldb_table_internal_get(const ldb_table* t,
                                  const ldb_read_options* options,
                                  const ldb_slice* k, void* arg,
                                  void (*handle_result)(void*, const ldb_slice*,
                                                        const ldb_slice*));
uint64_t ldb_table_approximate_offset_of(const ldb_table* t,
                                         const ldb_slice* key);
uint64_t ldb_table_cache_id(const ldb_table* t);
ldb_rand_file* ldb_table_file(const ldb_table* t);

// ------------------------------------------------------------------ table builder
typedef struct ldb_table_builder ldb_table_builder;

ldb_table_builder* ldb_table_builder_new(const ldb_options* options,
                                         ldb_writable_file* file);
void ldb_table_builder_destroy(ldb_table_builder* b);
void ldb_table_builder_add(ldb_table_builder* b, const ldb_slice* key,
                           const ldb_slice* value);
void ldb_table_builder_flush(ldb_table_builder* b);
ldb_status ldb_table_builder_finish(ldb_table_builder* b);
void ldb_table_builder_abandon(ldb_table_builder* b);
uint64_t ldb_table_builder_num_entries(const ldb_table_builder* b);
uint64_t ldb_table_builder_file_size(const ldb_table_builder* b);
ldb_status ldb_table_builder_status(const ldb_table_builder* b);

// ------------------------------------------------------------------ two level iterator
typedef ldb_iterator* (*ldb_block_reader_fn)(void* arg,
                                             const ldb_read_options* options,
                                             const ldb_slice* index_value);
ldb_iterator* ldb_new_two_level_iterator(ldb_iterator* index_iter,
                                         ldb_block_reader_fn block_reader,
                                         void* arg,
                                         const ldb_read_options* options);

// simple uint64 set (sorted dynamic array); used for live file tracking
typedef struct uint64_set_t {
  uint64_t* items;
  size_t count;
  size_t cap;
} uint64_set_t;
void uint64_set_init(uint64_set_t* s);
void uint64_set_destroy(uint64_set_t* s);
void uint64_set_insert(uint64_set_t* s, uint64_t v);
int uint64_set_contains(const uint64_set_t* s, uint64_t v);
void uint64_set_erase(uint64_set_t* s, uint64_t v);
size_t uint64_set_size(const uint64_set_t* s);
uint64_t uint64_set_first(const uint64_set_t* s);  // smallest

// ------------------------------------------------------------------ version edit
typedef struct ldb_file_meta {
  uint64_t number;
  uint64_t file_size;
  ldb_buffer smallest;  // internal key
  ldb_buffer largest;   // internal key
  int refs;
  int allowed_seeks;
  uint64_t seq_of_largest;  // largest sequence in file (for trivial move)
} ldb_file_meta;

void ldb_file_meta_init(ldb_file_meta* f);
void ldb_file_meta_destroy(ldb_file_meta* f);
uint64_t ldb_file_meta_largest_seqno(const ldb_file_meta* f);

typedef struct ldb_compact_pointer_entry {
  int level;
  ldb_buffer key;  // internal key
} ldb_compact_pointer_entry;

typedef struct ldb_new_file_entry {
  int level;
  ldb_file_meta meta;
} ldb_new_file_entry;

typedef struct ldb_version_edit {
  ldb_buffer comparator;
  int has_comparator;
  uint64_t log_number;
  int has_log_number;
  uint64_t prev_log_number;
  int has_prev_log_number;
  uint64_t next_file_number;
  int has_next_file_number;
  uint64_t last_sequence;
  int has_last_sequence;
  ldb_compact_pointer_entry* compact_pointers;
  size_t compact_pointers_count;
  size_t compact_pointers_cap;
  // Deleted files: pairs (level, number)
  int* deleted_levels;
  uint64_t* deleted_numbers;
  size_t deleted_count;
  size_t deleted_cap;
  ldb_new_file_entry* new_files;
  size_t new_files_count;
  size_t new_files_cap;
} ldb_version_edit;

void ldb_version_edit_init(ldb_version_edit* e);
void ldb_version_edit_destroy(ldb_version_edit* e);
void ldb_version_edit_clear(ldb_version_edit* e);
void ldb_version_edit_set_comparator(ldb_version_edit* e, const char* name);
void ldb_version_edit_set_log_number(ldb_version_edit* e, uint64_t n);
void ldb_version_edit_set_prev_log_number(ldb_version_edit* e, uint64_t n);
void ldb_version_edit_set_next_file(ldb_version_edit* e, uint64_t n);
void ldb_version_edit_set_last_sequence(ldb_version_edit* e, uint64_t seq);
void ldb_version_edit_set_compact_pointer(ldb_version_edit* e, int level,
                                          const ldb_buffer* internal_key);
void ldb_version_edit_add_file(ldb_version_edit* e, int level, uint64_t number,
                               uint64_t file_size, const ldb_buffer* smallest,
                               const ldb_buffer* largest);
void ldb_version_edit_remove_file(ldb_version_edit* e, int level,
                                  uint64_t number);
ldb_status ldb_version_edit_encode(const ldb_version_edit* e, ldb_buffer* dst);
ldb_status ldb_version_edit_decode(ldb_version_edit* e, const ldb_slice* src);

// ------------------------------------------------------------------ version set
typedef struct ldb_version ldb_version;
typedef struct ldb_version_set ldb_version_set;
typedef struct ldb_table_cache ldb_table_cache;
typedef struct ldb_compaction ldb_compaction;
typedef struct ldb_db_impl ldb_db_impl;

struct ldb_file_meta;

struct ldb_version {
  ldb_version_set* vset;
  ldb_file_meta** files[LDB_K_NUM_LEVELS];
  size_t nfiles[LDB_K_NUM_LEVELS];
  size_t files_cap[LDB_K_NUM_LEVELS];
  // Next file to compact based on seek stats.
  ldb_file_meta* file_to_compact;
  int file_to_compact_level;
  int compaction_level;
  double compaction_score;
  int refs;
  ldb_version* next;
  ldb_version* prev;
};

ldb_version* ldb_version_new(ldb_version_set* vset);
void ldb_version_destroy(ldb_version* v);
void ldb_version_ref(ldb_version* v);
void ldb_version_unref(ldb_version* v);
int ldb_version_overlap_in_level(const ldb_version* v, int level,
                                 const ldb_slice* smallest_user_key,
                                 const ldb_slice* largest_user_key);
int ldb_version_pick_level_for_memtable_output(const ldb_version* v,
                                               const ldb_slice* smallest,
                                               const ldb_slice* largest);
void ldb_version_get_overlapping_inputs(const ldb_version* v, int level,
                                        const ldb_slice* begin,  // internal key or NULL
                                        const ldb_slice* end,    // internal key or NULL
                                        ldb_file_meta*** inputs,
                                        size_t* count, size_t* cap);
int ldb_version_some_file_overlaps_range(const ldb_version* v, int level,
                                         int disjoint_sorted_files,
                                         const ldb_slice* smallest_user_key,
                                         const ldb_slice* largest_user_key);
// Get implementation: returns status; *value filled if found. stats may be NULL.
ldb_status ldb_version_get(const ldb_version* v, const ldb_read_options* options,
                           const ldb_lookup_key* k, ldb_buffer* value,
                           ldb_file_meta** seek_file, int* seek_file_level);
int ldb_version_update_stats(ldb_version* v, ldb_file_meta* seek_file,
                             int seek_file_level);
int ldb_version_record_read_sample(ldb_version* v, const ldb_slice* ikey);
void ldb_version_add_iterators(const ldb_version* v,
                               const ldb_read_options* options,
                               ldb_iterator*** list, size_t* count,
                               size_t* cap);
void ldb_version_add_live_files(const ldb_version* v, uint64_set_t* set);
char* ldb_version_debug_string(const ldb_version* v);
uint64_t ldb_version_max_next_level_overlapping_bytes(const ldb_version* v);

struct ldb_table_cache {
  ldb_env* env;
  char* dbname;
  const ldb_options* options;  // sanitized options (owned by db)
  ldb_cache* cache;
  const ldb_ikc* icmp;
  uint64_t last_id;
};

ldb_table_cache* ldb_table_cache_new(const char* dbname,
                                     const ldb_options* options, size_t entries);
void ldb_table_cache_destroy(ldb_table_cache* tc);
ldb_iterator* ldb_table_cache_new_iterator(ldb_table_cache* tc,
                                           const ldb_read_options* options,
                                           uint64_t file_number,
                                           uint64_t file_size);
ldb_status ldb_table_cache_get(ldb_table_cache* tc,
                               const ldb_read_options* options,
                               uint64_t file_number, uint64_t file_size,
                               const ldb_slice* k, void* arg,
                               void (*handle_result)(void*, const ldb_slice*,
                                                     const ldb_slice*),
                               ldb_table** tableptr);
void ldb_table_cache_evict(ldb_table_cache* tc, uint64_t file_number);

// BuildTable: writes memtable iterator to a new sstable; fills *meta.
ldb_status ldb_build_table(const char* dbname, ldb_env* env,
                           const ldb_options* options, ldb_table_cache* tc,
                           ldb_iterator* iter, ldb_file_meta* meta);

struct ldb_compaction {
  int level;
  uint64_t max_output_file_size;
  ldb_version* input_version;
  ldb_file_meta** inputs[2];
  size_t inputs_count[2];
  size_t inputs_cap[2];
  ldb_file_meta** grandparents;
  size_t grandparents_count;
  size_t grandparents_cap;
  size_t grandparent_index;
  int seen_key;
  uint64_t overlapped_bytes;
  size_t level_ptrs[LDB_K_NUM_LEVELS];
  ldb_version_edit edit;
};

ldb_compaction* ldb_compaction_new(const ldb_options* options, int level);
void ldb_compaction_destroy(ldb_compaction* c);
int ldb_compaction_is_trivial_move(const ldb_compaction* c);
int ldb_compaction_is_base_level_for_key(ldb_compaction* c,
                                         const ldb_slice* user_key);
int ldb_compaction_should_stop_before(ldb_compaction* c,
                                      const ldb_slice* internal_key);
uint64_t ldb_max_grandparent_overlap_bytes(const ldb_options* options);
uint64_t ldb_expanded_compaction_byte_size_limit(const ldb_options* options);
uint64_t ldb_max_bytes_for_level(const ldb_options* options, int level);
uint64_t ldb_max_file_size_for_level(const ldb_options* options, int level);
uint64_t ldb_total_file_size(ldb_file_meta** files, size_t n);

struct ldb_version_set {
  ldb_env* env;
  char* dbname;
  const ldb_options* options;
  ldb_table_cache* table_cache;
  ldb_ikc icmp;
  uint64_t next_file_number;
  uint64_t manifest_file_number;
  uint64_t last_sequence;
  uint64_t log_number;
  uint64_t prev_log_number;
  ldb_writable_file* descriptor_file;
  ldb_log_writer* descriptor_log;
  ldb_version dummy_versions;  // linked list head; current_ = dummy.prev
  ldb_version* current_;
  ldb_buffer compact_pointer[LDB_K_NUM_LEVELS];
  ldb_mutex* db_mutex;  // DBImpl's mutex (used by LogAndApply)
};

void ldb_version_set_init(ldb_version_set* vs, const char* dbname,
                          const ldb_options* options,
                          ldb_table_cache* table_cache, const ldb_ikc* icmp,
                          ldb_mutex* db_mutex);
void ldb_version_set_destroy(ldb_version_set* vs);
ldb_version* ldb_version_set_current(ldb_version_set* vs);
ldb_status ldb_version_set_recover(ldb_version_set* vs, int* save_manifest);
ldb_status ldb_version_set_log_and_apply(ldb_version_set* vs,
                                         ldb_version_edit* edit);
void ldb_version_set_mark_file_number_used(ldb_version_set* vs, uint64_t n);
uint64_t ldb_version_set_new_file_number(ldb_version_set* vs);
void ldb_version_set_reuse_file_number(ldb_version_set* vs, uint64_t n);
ldb_status ldb_version_set_write_snapshot(ldb_version_set* vs,
                                          ldb_log_writer* log);
int ldb_version_set_num_level_files(const ldb_version_set* vs, int level);
uint64_t ldb_version_set_num_level_bytes(const ldb_version_set* vs, int level);
uint64_t ldb_version_set_approximate_offset_of(ldb_version_set* vs,
                                               ldb_version* v,
                                               const ldb_slice* ikey);
void ldb_version_set_add_live_files(ldb_version_set* vs, uint64_set_t* live);
uint64_t ldb_version_set_max_next_level_overlapping_bytes(
    ldb_version_set* vs);
ldb_compaction* ldb_version_set_pick_compaction(ldb_version_set* vs);
ldb_compaction* ldb_version_set_compact_range(ldb_version_set* vs, int level,
                                              const ldb_slice* begin,
                                              const ldb_slice* end);
ldb_iterator* ldb_version_set_make_input_iterator(ldb_version_set* vs,
                                                  ldb_compaction* c);
char* ldb_version_set_level_summary(const ldb_version_set* vs);
int ldb_version_set_needs_compaction(const ldb_version_set* vs);
void ldb_version_set_finalize(ldb_version_set* vs, ldb_version* v);

// ------------------------------------------------------------------ snapshots
typedef struct ldb_snapshot_impl {
  uint64_t sequence;
  struct ldb_snapshot_impl* prev;
  struct ldb_snapshot_impl* next;
} ldb_snapshot_impl;

typedef struct ldb_snapshot_list {
  ldb_snapshot_impl head;
} ldb_snapshot_list;

void ldb_snapshot_list_init(ldb_snapshot_list* l);
ldb_snapshot_impl* ldb_snapshot_list_new(ldb_snapshot_list* l, uint64_t seq);
void ldb_snapshot_list_delete(ldb_snapshot_list* l, ldb_snapshot_impl* s);
ldb_snapshot_impl* ldb_snapshot_list_oldest(const ldb_snapshot_list* l);
int ldb_snapshot_list_empty(const ldb_snapshot_list* l);
// Not empty: all snapshots in the list, sorted by sequence.
uint64_t ldb_snapshot_list_smallest(const ldb_snapshot_list* l);

// ------------------------------------------------------------------ db impl
struct ldb_db_impl;

// Background compaction state
typedef struct ldb_compaction_output {
  uint64_t number;
  uint64_t file_size;
  ldb_buffer smallest;
  ldb_buffer largest;
} ldb_compaction_output;

typedef struct ldb_compaction_state {
  ldb_compaction* compaction;
  uint64_t smallest_snapshot;
  ldb_compaction_output* outputs;
  size_t outputs_count;
  size_t outputs_cap;
  ldb_writable_file* outfile;
  ldb_table_builder* builder;
  uint64_t total_bytes;
} ldb_compaction_state;

typedef struct ldb_manual_compaction {
  int level;
  ldb_buffer begin;  // internal key; valid if has_begin
  ldb_buffer end;    // internal key; valid if has_end
  int has_begin;
  int has_end;
  int done;
  ldb_buffer tmp_storage;
} ldb_manual_compaction;

// Writers queue node
typedef struct ldb_writer {
  ldb_write_batch* batch;
  int sync;
  int done;
  ldb_status status;
  ldb_cond cv;
  struct ldb_writer* next;
} ldb_writer;

struct ldb_db_impl {
  ldb_env* env;
  ldb_ikc internal_comparator;
  ldb_comparator internal_comparator_adapter;  // ldb_comparator over ikc
  ldb_internal_filter_policy internal_filter_policy;
  ldb_options options;  // sanitized copy (owns info_log/cache if created)
  int owns_info_log;
  int owns_cache;
  char* dbname;
  ldb_table_cache* table_cache;
  ldb_file_lock* db_lock;

  ldb_mutex mutex;
  ldb_atomic_int shutting_down;
  ldb_cond background_work_finished_signal;
  ldb_memtable* mem;
  ldb_memtable* imm;
  ldb_atomic_int has_imm;
  ldb_writable_file* logfile;
  uint64_t logfile_number;
  ldb_log_writer* log;

  uint32_t seed;
  ldb_write_batch tmp_batch;
  ldb_writer* writers;  // singly linked list (front = head)

  int background_compaction_scheduled;
  ldb_manual_compaction* manual_compaction;

  ldb_snapshot_list snapshots;
  uint64_set_t pending_outputs;

  ldb_status bg_error;

  // Per-level compaction stats
  struct {
    int64_t micros;
    int64_t bytes_read;
    int64_t bytes_written;
  } stats[LDB_K_NUM_LEVELS];

  ldb_version_set versions;
};

ldb_db_impl* ldb_db_impl_new(const ldb_options* raw_options,
                             const char* dbname);
void ldb_db_impl_destroy(ldb_db_impl* impl);
ldb_status ldb_db_open(const ldb_options* options, const char* dbname,
                       ldb_db_impl** dbptr);
ldb_status ldb_db_impl_get(ldb_db_impl* impl, const ldb_read_options* options,
                           const ldb_slice* key, ldb_buffer* value);
ldb_status ldb_db_impl_write(ldb_db_impl* impl,
                             const ldb_write_options* options,
                             ldb_write_batch* updates);
ldb_iterator* ldb_db_impl_new_iterator(ldb_db_impl* impl,
                                       const ldb_read_options* options);
const ldb_snapshot_impl* ldb_db_impl_get_snapshot(ldb_db_impl* impl);
void ldb_db_impl_release_snapshot(ldb_db_impl* impl,
                                  const ldb_snapshot_impl* snapshot);
void ldb_db_impl_compact_range(ldb_db_impl* impl, const ldb_slice* begin,
                               const ldb_slice* end);
int ldb_db_impl_get_property(ldb_db_impl* impl, const ldb_slice* property,
                             ldb_buffer* value);
void ldb_db_impl_get_approximate_sizes(ldb_db_impl* impl,
                                       const ldb_slice* start,
                                       const ldb_slice* limit, int n,
                                       uint64_t* sizes);
void ldb_db_impl_record_read_sample(ldb_db_impl* impl, const ldb_slice* key);
// Test hooks (used by ported tests)
ldb_status ldb_db_impl_test_compact_mem_table(ldb_db_impl* impl);
void ldb_db_impl_test_compact_range(ldb_db_impl* impl, int level,
                                    const ldb_slice* begin, const ldb_slice* end);
ldb_iterator* ldb_db_impl_test_new_internal_iterator(ldb_db_impl* impl);
int64_t ldb_db_impl_test_max_next_level_overlapping_bytes(ldb_db_impl* impl);
uint64_t ldb_db_impl_test_last_sequence(ldb_db_impl* impl);

ldb_status ldb_destroy_db(const ldb_options* options, const char* dbname);

// Repair: rebuild a database from existing files.
ldb_status ldb_repair_db(const ldb_options* options, const char* dbname);

// DBIter
ldb_iterator* ldb_new_db_iterator(ldb_db_impl* db,
                                  const ldb_comparator* user_comparator,
                                  ldb_iterator* internal_iter,
                                  uint64_t sequence, uint32_t seed);

#ifdef __cplusplus
}
#endif

#endif  // KVDB_KVDB_H_
