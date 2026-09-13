// test_skiplist.c - port of leveldb's skiplist_test and memtable_test
// (single-threaded core scenarios)
#include "harness.h"

static int skip_cmp(void* arg, const char* a, const char* b) {
  ldb_ikc* icmp = (ldb_ikc*)arg;
  const char* ap = a;
  const char* bp = b;
  ldb_slice as, bs;
  ldb_get_length_prefixed_slice_unbounded(&ap, &as);
  ldb_get_length_prefixed_slice_unbounded(&bp, &bs);
  return ldb_ikc_compare(icmp, &as, &bs);
}

typedef struct skip_tester {
  ldb_ikc icmp;
  ldb_arena arena;
  ldb_skiplist* list;
  ldb_buffer storage;  // keeps encoded keys alive
} skip_tester;

static void skip_init(skip_tester* t) {
  ldb_ikc_init(&t->icmp, ldb_bytewise_comparator());
  ldb_arena_init(&t->arena);
  t->list = ldb_skiplist_new(&t->icmp, skip_cmp, &t->arena);
  ldb_buffer_init(&t->storage);
  // Skiplist nodes keep raw pointers into storage, so storage must never
  // realloc after the first insert; reserve for the largest test upfront.
  ldb_buffer_reserve(&t->storage, 1 << 20);
}

static void skip_destroy(skip_tester* t) {
  free(t->list);
  ldb_arena_destroy(&t->arena);
  ldb_buffer_destroy(&t->storage);
}

// Inserts internal key (user, seq, value type) as a length-prefixed entry.
static void skip_insert(skip_tester* t, const char* user, uint64_t seq) {
  size_t ulen = strlen(user);
  size_t needed = 5 + ulen + 8 + 1;
  char* buf = (char*)malloc(needed);
  char* p = buf;
  p = ldb_encode_varint32(p, (uint32_t)(ulen + 8));
  memcpy(p, user, ulen);
  p += ulen;
  ldb_encode_fixed64(p, ldb_pack_sequence_and_type(seq, LDB_TYPE_VALUE));
  p += 8;
  size_t entry_len = (size_t)(p - buf);
  size_t offset = t->storage.size;
  ldb_buffer_append(&t->storage, buf, entry_len);
  ldb_skiplist_insert(t->list, t->storage.data + offset);
  free(buf);
}

static long long skip_count_all(skip_tester* t) {
  long long count = 0;
  const char* pos;
  if (ldb_skiplist_seek_to_first(t->list, &pos)) {
    count++;
    while ((pos = ldb_skiplist_next(t->list, pos)) != NULL) {
      count++;
    }
  }
  return count;
}

TEST(skiplist, Empty) {
  skip_tester t;
  skip_init(&t);
  const char* pos;
  CHECK_EQ(0, ldb_skiplist_seek_to_first(t.list, &pos));
  CHECK_EQ(0, ldb_skiplist_seek_to_last(t.list, &pos));
  CHECK_EQ(0, skip_count_all(&t));
  skip_destroy(&t);
}

TEST(skiplist, InsertAndLookup) {
  skip_tester t;
  skip_init(&t);
  const int N = 2000;
  ldb_buffer_reserve(&t.storage, N * 32);
  for (int i = N; i > 0; i--) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%06d", i);
    skip_insert(&t, buf, (uint64_t)i);
  }
  CHECK_EQ(N, skip_count_all(&t));
  const char* pos;
  for (int i = 1; i <= N; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%06d", i);
    ldb_slice user = ldb_slice_str(buf);
    ldb_lookup_key lk;
    ldb_lookup_key_init(&lk, &user, 1000000);
    CHECK_EQ(1, ldb_skiplist_seek(t.list, lk.memtable_key.data, &pos));
    const ldb_skiplist_node* node = (const ldb_skiplist_node*)pos;
    const char* p = node->key;
    ldb_slice entry;
    ldb_get_length_prefixed_slice_unbounded(&p, &entry);
    ldb_slice eu = ldb_extract_user_key(&entry);
    if (!ldb_slice_equals(&eu, &user)) {
      fprintf(stderr, "DBG seek i=%d eu=%.*s user=%s\n", i, (int)eu.size,
              eu.data, buf);
    }
    CHECK_EQ(1, ldb_slice_equals(&eu, &user));
    ldb_lookup_key_destroy(&lk);
  }
  skip_destroy(&t);
}

TEST(skiplist, InsertDuplicates) {
  skip_tester t;
  skip_init(&t);
  skip_insert(&t, "a", 1);
  skip_insert(&t, "a", 2);  // different sequence => different internal key
  skip_insert(&t, "a", 3);
  CHECK_EQ(3, skip_count_all(&t));
  skip_destroy(&t);
}

TEST(skiplist, Prev) {
  skip_tester t;
  skip_init(&t);
  skip_insert(&t, "a", 1);
  skip_insert(&t, "b", 2);
  skip_insert(&t, "c", 3);
  const char* pos;
  CHECK_EQ(1, ldb_skiplist_seek_to_last(t.list, &pos));
  const char* prev = ldb_skiplist_prev(t.list, pos);
  CHECK(prev != NULL);
  const char* first;
  CHECK_EQ(1, ldb_skiplist_seek_to_first(t.list, &first));
  CHECK(prev != first);
  CHECK_EQ((const char*)((const ldb_skiplist_node*)prev)->next[0], pos);
  CHECK_EQ(NULL, ldb_skiplist_prev(t.list, first));  // nothing before first
  skip_destroy(&t);
}

// ------------------------------------------------------------------ memtable
static void mt_add_value(ldb_memtable* mem, const char* key, uint64_t seq) {
  char ibuf[64];
  size_t klen = strlen(key);
  memcpy(ibuf, key, klen);
  ldb_encode_fixed64(ibuf + klen,
                     ldb_pack_sequence_and_type(seq, LDB_TYPE_VALUE));
  ldb_slice ikey = ldb_slice_make(ibuf, klen + 8);
  char vbuf[64];
  memcpy(vbuf, ibuf, klen + 8);
  ldb_slice v = ldb_slice_make(vbuf, klen + 8);  // value mirrors the tag
  ldb_memtable_add(mem, &ikey, &v);
}

TEST(memtable, Simple) {
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_memtable* mem = ldb_memtable_new(&icmp);
  ldb_memtable_ref(mem);
  mt_add_value(mem, "test", 1);

  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_status s = ldb_status_ok();
  ldb_slice k = ldb_slice_str("test");
  ldb_lookup_key lk;
  ldb_lookup_key_init(&lk, &k, 1);
  CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));
  CHECK_EQ(12, (long long)value.size);
  ldb_lookup_key_destroy(&lk);
  ldb_buffer_destroy(&value);
  ldb_status_destroy(&s);
  ldb_memtable_unref(mem);
}

TEST(memtable, SequenceOrdering) {
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_memtable* mem = ldb_memtable_new(&icmp);
  ldb_memtable_ref(mem);
  mt_add_value(mem, "key", 10);
  mt_add_value(mem, "key", 20);
  mt_add_value(mem, "key", 30);

  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_status s = ldb_status_ok();
  ldb_slice user = ldb_slice_str("key");
  ldb_lookup_key lk;
  ldb_lookup_key_init(&lk, &user, 30);
  CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));
  uint64_t tag = ldb_decode_fixed64(value.data + value.size - 8);
  CHECK_EQ(30, (long long)(tag >> 8));
  // Get at seq 25 sees the seq-20 version
  ldb_lookup_key lk2;
  ldb_lookup_key_init(&lk2, &user, 25);
  CHECK_EQ(1, ldb_memtable_get(mem, &lk2, &value, &s));
  tag = ldb_decode_fixed64(value.data + value.size - 8);
  CHECK_EQ(20, (long long)(tag >> 8));
  ldb_lookup_key_destroy(&lk);
  ldb_lookup_key_destroy(&lk2);
  ldb_buffer_destroy(&value);
  ldb_status_destroy(&s);
  ldb_memtable_unref(mem);
}

TEST(memtable, Deletion) {
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_memtable* mem = ldb_memtable_new(&icmp);
  ldb_memtable_ref(mem);
  char ibuf[64];
  memcpy(ibuf, "key", 3);
  ldb_encode_fixed64(ibuf + 3, ldb_pack_sequence_and_type(5, LDB_TYPE_DELETION));
  ldb_slice ikey = ldb_slice_make(ibuf, 11);
  ldb_slice empty = ldb_slice_make("", 0);
  ldb_memtable_add(mem, &ikey, &empty);

  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_status s = ldb_status_ok();
  ldb_slice user = ldb_slice_str("key");
  ldb_lookup_key lk;
  ldb_lookup_key_init(&lk, &user, 10);
  CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));  // found (deleted)
  CHECK_EQ(0, ldb_ok(s));
  ldb_lookup_key_destroy(&lk);
  ldb_status_destroy(&s);
  ldb_buffer_destroy(&value);
  ldb_memtable_unref(mem);
}

TEST(memtable, ApproximateMemoryUsage) {
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_memtable* mem = ldb_memtable_new(&icmp);
  ldb_memtable_ref(mem);
  size_t before = ldb_memtable_approximate_memory_usage(mem);
  mt_add_value(mem, "key", 1);
  size_t after = ldb_memtable_approximate_memory_usage(mem);
  CHECK_GT((long long)after, (long long)before);
  ldb_memtable_unref(mem);
}
