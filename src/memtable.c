// memtable.c - memtable over skiplist (mirrors leveldb/db/memtable.cc)
// Entry format: varint32(klen) | internal key | varint32(vlen) | value
#include "kvdb.h"

#include <assert.h>

struct ldb_memtable {
  ldb_ikc comparator;
  int refs;
  ldb_arena arena;
  ldb_skiplist* table;
};

// skiplist comparator: compares length-prefixed slices with the internal
// comparator.
static int memtable_key_cmp(void* arg, const char* a, const char* b) {
  ldb_ikc* icmp = (ldb_ikc*)arg;
  const char* ap = a;
  const char* bp = b;
  // Length prefixes are always within 5 bytes for these entries.
  ldb_slice as, bs;
  int ok1 = ldb_get_length_prefixed_slice_unbounded(&ap, &as);
  int ok2 = ldb_get_length_prefixed_slice_unbounded(&bp, &bs);
  assert(ok1 && ok2);
  (void)ok1;
  (void)ok2;
  return ldb_ikc_compare(icmp, &as, &bs);
}

ldb_memtable* ldb_memtable_new(const ldb_ikc* comparator) {
  ldb_memtable* m = (ldb_memtable*)malloc(sizeof(ldb_memtable));
  m->comparator = *comparator;
  m->refs = 0;
  ldb_arena_init(&m->arena);
  m->table = ldb_skiplist_new(&m->comparator, memtable_key_cmp, &m->arena);
  return m;
}

void ldb_memtable_ref(ldb_memtable* m) { m->refs++; }

void ldb_memtable_unref(ldb_memtable* m) {
  m->refs--;
  if (m->refs <= 0) {
    if (m->table) free(m->table);
    ldb_arena_destroy(&m->arena);
    free(m);
  }
}

size_t ldb_memtable_approximate_memory_usage(const ldb_memtable* m) {
  return ldb_arena_memory_usage(&m->arena);
}

uint64_t ldb_memtable_first_sequence_number(const ldb_memtable* m) {
  (void)m;
  return 0;
}

void ldb_memtable_add(ldb_memtable* m, const ldb_slice* internal_key,
                      const ldb_slice* value) {
  // Format: varint32(klen) | key | varint32(vlen) | value
  size_t klen = internal_key->size;
  size_t vlen = value->size;
  size_t needed =
      klen + vlen + 5 /* varint klen */ + 5 /* varint vlen */ + 1;
  char* buf = (char*)ldb_arena_allocate(&m->arena, needed);
  char* p = buf;
  p = ldb_encode_varint32(p, (uint32_t)klen);
  memcpy(p, internal_key->data, klen);
  p += klen;
  p = ldb_encode_varint32(p, (uint32_t)vlen);
  memcpy(p, value->data, vlen);
  p += vlen;
  assert((size_t)(p - buf) <= needed);
  ldb_skiplist_insert(m->table, buf);
}

int ldb_memtable_get(const ldb_memtable* m, const ldb_lookup_key* key,
                     ldb_buffer* value, ldb_status* s) {
  const char* memkey = key->memtable_key.data;
  const char* pos;
  if (!ldb_skiplist_seek(m->table, memkey, &pos)) {
    return 0;  // not found
  }
  // Entry format:
  //    klength  varint32
  //    userkey  char[klength]
  //    tag      uint64
  //    vlength  varint32
  //    value    char[vlength]
  // Check that it belongs to same user key. We do not check the sequence
  // number since the Seek() call above should have skipped all entries with
  // overly large sequence numbers.
  const ldb_skiplist_node* node = (const ldb_skiplist_node*)pos;
  const char* entry = node->key;
  const char* entry_limit = entry + 5;
  uint32_t key_length;
  const char* key_ptr = entry;
  if (!ldb_get_varint32(&key_ptr, entry_limit, &key_length)) {
    return 0;
  }
  ldb_slice ukey = ldb_slice_make(key_ptr, key_length - 8);
  if (ldb_cmp(m->comparator.user_comparator, &ukey, &key->user_key) == 0) {
    // Correct user key
    const uint64_t tag = ldb_decode_fixed64(key_ptr + key_length - 8);
    switch ((int)(tag & 0xff)) {
      case LDB_TYPE_VALUE: {
        const char* vp = key_ptr + key_length;
        ldb_slice v;
        if (!ldb_get_length_prefixed_slice_unbounded(&vp, &v)) {
          return 0;
        }
        ldb_buffer_clear(value);
        ldb_buffer_append_slice(value, &v);
        return 1;
      }
      case LDB_TYPE_DELETION:
        ldb_status_destroy(s);
        *s = ldb_status_notfound("", NULL);
        return 1;
      default:
        return 0;
    }
  }
  return 0;
}

// ------------------------------------------------------------------ iterator
typedef struct memtable_iter {
  ldb_iterator base;
  const ldb_memtable* m;
  const char* node;  // skiplist node pointer or NULL
  ldb_buffer tmp;    // scratch for encoded seek target
} memtable_iter;

static void mi_destroy(ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  ldb_buffer_destroy(&mi->tmp);
  free(mi);
}

static int mi_valid(const ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  return mi->node != NULL;
}

static void mi_seek_to_first(ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  const char* pos;
  if (ldb_skiplist_seek_to_first(mi->m->table, &pos)) {
    mi->node = pos;
  } else {
    mi->node = NULL;
  }
}

static void mi_seek_to_last(ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  const char* pos;
  if (ldb_skiplist_seek_to_last(mi->m->table, &pos)) {
    mi->node = pos;
  } else {
    mi->node = NULL;
  }
}

static void mi_seek(ldb_iterator* it, const ldb_slice* target) {
  memtable_iter* mi = (memtable_iter*)it;
  // Encode target as length-prefixed slice into scratch
  ldb_buffer_clear(&mi->tmp);
  ldb_put_length_prefixed_slice(&mi->tmp, target);
  const char* pos;
  if (ldb_skiplist_seek(mi->m->table, mi->tmp.data, &pos)) {
    mi->node = pos;
  } else {
    mi->node = NULL;
  }
}

static void mi_next(ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  assert(mi->node != NULL);
  mi->node = ldb_skiplist_next(mi->m->table, mi->node);
}

static void mi_prev(ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  assert(mi->node != NULL);
  mi->node = ldb_skiplist_prev(mi->m->table, mi->node);
}

// key(): length-prefixed slice starting at node key.
static ldb_slice mi_key(const ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  const ldb_skiplist_node* node = (const ldb_skiplist_node*)mi->node;
  const char* p = node->key;
  ldb_slice result;
  ldb_get_length_prefixed_slice_unbounded(&p, &result);
  return result;
}

// value(): second length-prefixed slice, after the first.
static ldb_slice mi_value(const ldb_iterator* it) {
  memtable_iter* mi = (memtable_iter*)it;
  const ldb_skiplist_node* node = (const ldb_skiplist_node*)mi->node;
  const char* p = node->key;
  ldb_slice key_slice;
  ldb_get_length_prefixed_slice_unbounded(&p, &key_slice);
  // p now points just past the key data, i.e. at the value length varint.
  const char* vp = p;
  ldb_slice value;
  ldb_get_length_prefixed_slice_unbounded(&vp, &value);
  return value;
}

static ldb_status mi_status(const ldb_iterator* it) {
  (void)it;
  return ldb_status_ok();
}

static const ldb_iterator_methods g_memtable_iter_methods = {
    mi_destroy, mi_valid,  mi_seek_to_first, mi_seek_to_last, mi_seek,
    mi_next,    mi_prev,   mi_key,           mi_value,        mi_status};

ldb_iterator* ldb_memtable_new_iterator(const ldb_memtable* m) {
  memtable_iter* mi = (memtable_iter*)malloc(sizeof(memtable_iter));
  mi->base.m = &g_memtable_iter_methods;
  mi->base.impl = mi;
  mi->base.cleanups = NULL;
  mi->m = m;
  mi->node = NULL;
  ldb_buffer_init(&mi->tmp);
  return &mi->base;
}
