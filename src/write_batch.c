// write_batch.c - WriteBatch encoding and memtable application
// (mirrors leveldb/db/write_batch.cc and write_batch_internal.h)
#include "kvdb.h"

#include <assert.h>

#define WB_HEADER_SIZE 12

enum { WB_TYPE_DELETION = 0, WB_TYPE_VALUE = 1 };

// WriteBatch::rep_ :=
//    sequence: fixed64
//    count: fixed32
//    data: record[count]
// record := kTypeValue varstring varstring | kTypeDeletion varstring

void ldb_write_batch_init(ldb_write_batch* b) { ldb_buffer_init(&b->rep); }

void ldb_write_batch_destroy(ldb_write_batch* b) { ldb_buffer_destroy(&b->rep); }

void ldb_write_batch_clear(ldb_write_batch* b) {
  ldb_buffer_clear(&b->rep);
  ldb_buffer_resize(&b->rep, WB_HEADER_SIZE);
  memset(b->rep.data, 0, WB_HEADER_SIZE);
}

static void wb_maybe_init(ldb_write_batch* b) {
  if (b->rep.cap == 0 || b->rep.size < WB_HEADER_SIZE) {
    ldb_buffer_destroy(&b->rep);
    ldb_write_batch_clear(b);
  }
}

ldb_slice ldb_write_batch_contents(const ldb_write_batch* b) {
  return ldb_buffer_slice((ldb_buffer*)&b->rep);
}

void ldb_write_batch_set_contents(ldb_write_batch* b, const ldb_slice* src) {
  assert(src->size >= WB_HEADER_SIZE);
  b->rep.size = 0;
  ldb_buffer_append(&b->rep, src->data, src->size);
}

size_t ldb_write_batch_count(const ldb_write_batch* b) {
  wb_maybe_init((ldb_write_batch*)b);
  return (size_t)ldb_decode_fixed32(b->rep.data + 8);
}

void ldb_write_batch_set_count(ldb_write_batch* b, size_t n) {
  wb_maybe_init(b);
  ldb_encode_fixed32(b->rep.data + 8, (uint32_t)n);
}

uint64_t ldb_write_batch_sequence(const ldb_write_batch* b) {
  wb_maybe_init((ldb_write_batch*)b);
  return ldb_decode_fixed64(b->rep.data);
}

void ldb_write_batch_set_sequence(ldb_write_batch* b, uint64_t seq) {
  wb_maybe_init(b);
  ldb_encode_fixed64(b->rep.data, seq);
}

static const size_t kHeader = WB_HEADER_SIZE;

void ldb_write_batch_put(ldb_write_batch* b, const ldb_slice* key,
                         const ldb_slice* value) {
  wb_maybe_init(b);
  ldb_write_batch_set_count(b, ldb_write_batch_count(b) + 1);
  ldb_put_varint32(&b->rep, WB_TYPE_VALUE);
  ldb_put_length_prefixed_slice(&b->rep, key);
  ldb_put_length_prefixed_slice(&b->rep, value);
}

void ldb_write_batch_delete(ldb_write_batch* b, const ldb_slice* key) {
  wb_maybe_init(b);
  ldb_write_batch_set_count(b, ldb_write_batch_count(b) + 1);
  ldb_put_varint32(&b->rep, WB_TYPE_DELETION);
  ldb_put_length_prefixed_slice(&b->rep, key);
}

void ldb_write_batch_append(ldb_write_batch* dst, const ldb_write_batch* src) {
  wb_maybe_init(dst);
  wb_maybe_init((ldb_write_batch*)src);
  ldb_write_batch_set_count(dst, ldb_write_batch_count(dst) +
                                     ldb_write_batch_count(src));
  assert(dst->rep.size >= kHeader);
  ldb_slice tail =
      ldb_slice_make(src->rep.data + kHeader, src->rep.size - kHeader);
  ldb_buffer_append(&dst->rep, tail.data, tail.size);
}

size_t ldb_write_batch_byte_size(const ldb_write_batch* b) {
  return b->rep.size;
}

// ------------------------------------------------------------------ iterate
void ldb_write_batch_iterate(
    const ldb_write_batch* b, void* state,
    void (*put)(void*, const ldb_slice* k, const ldb_slice* v),
    void (*deleted)(void*, const ldb_slice* k)) {
  wb_maybe_init((ldb_write_batch*)b);
  const char* p = b->rep.data + kHeader;
  const char* limit = b->rep.data + b->rep.size;
  int found = 0;
  while (p < limit) {
    found = 0;
    uint32_t tag;
    if (!ldb_get_varint32(&p, limit, &tag)) break;
    switch (tag) {
      case WB_TYPE_VALUE: {
        ldb_slice key, value;
        if (!ldb_get_length_prefixed_slice(&p, limit, &key) ||
            !ldb_get_length_prefixed_slice(&p, limit, &value)) {
          found = 1;
          break;
        }
        if (put) put(state, &key, &value);
        break;
      }
      case WB_TYPE_DELETION: {
        ldb_slice key;
        if (!ldb_get_length_prefixed_slice(&p, limit, &key)) {
          found = 1;
          break;
        }
        if (deleted) deleted(state, &key);
        break;
      }
      default:
        found = 1;
        break;
    }
    if (found) break;
  }
}

// ------------------------------------------------------------------ insert into memtable
typedef struct memtable_inserter {
  ldb_memtable* mem;
  uint64_t sequence;  // sequence of the next op
} memtable_inserter;

static void inserter_put(void* p, const ldb_slice* k, const ldb_slice* v) {
  memtable_inserter* mi = (memtable_inserter*)p;
  char ikey_buf[4096];
  char* heap = NULL;
  char* ikey = ikey_buf;
  if (k->size + 8 > sizeof(ikey_buf)) {
    heap = (char*)malloc(k->size + 8);
    ikey = heap;
  }
  memcpy(ikey, k->data, k->size);
  ldb_encode_fixed64(ikey + k->size,
                     ldb_pack_sequence_and_type(mi->sequence, LDB_TYPE_VALUE));
  ldb_slice ikey_slice = ldb_slice_make(ikey, k->size + 8);
  ldb_memtable_add(mi->mem, &ikey_slice, v);
  mi->sequence++;
  free(heap);
}

static void inserter_delete(void* p, const ldb_slice* k) {
  memtable_inserter* mi = (memtable_inserter*)p;
  char ikey_buf[4096];
  char* heap = NULL;
  char* ikey = ikey_buf;
  if (k->size + 8 > sizeof(ikey_buf)) {
    heap = (char*)malloc(k->size + 8);
    ikey = heap;
  }
  memcpy(ikey, k->data, k->size);
  ldb_encode_fixed64(
      ikey + k->size,
      ldb_pack_sequence_and_type(mi->sequence, LDB_TYPE_DELETION));
  ldb_slice ikey_slice = ldb_slice_make(ikey, k->size + 8);
  ldb_slice empty = ldb_slice_make("", 0);
  ldb_memtable_add(mi->mem, &ikey_slice, &empty);
  mi->sequence++;
  free(heap);
}

ldb_status ldb_write_batch_insert_into(const ldb_write_batch* b,
                                       ldb_memtable* mem, uint64_t* last_seq) {
  int count = (int)ldb_write_batch_count(b);
  if (count == 0) {
    if (last_seq) *last_seq = ldb_write_batch_sequence(b) + count - 1;
    return ldb_status_ok();
  }
  memtable_inserter mi;
  mi.mem = mem;
  mi.sequence = ldb_write_batch_sequence(b);
  ldb_write_batch_iterate(b, &mi, inserter_put, inserter_delete);
  if (mi.sequence != ldb_write_batch_sequence(b) + (uint64_t)count) {
    return ldb_status_corruption("invalid WriteBatch (bad count)", NULL);
  }
  if (last_seq) *last_seq = mi.sequence - 1;
  return ldb_status_ok();
}

