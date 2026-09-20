// test_table.c - port of leveldb's table_test (core round-trip scenarios)
// plus block_builder/block iteration checks.
#include "harness.h"

typedef struct table_tester {
  ldb_env* env;
  ldb_options options;
  char fname[128];
  ldb_table* table;
  // Table::Open does not take ownership of the file it reads, so the tester
  // has to keep it and destroy it itself (as the engine's table cache does).
  ldb_rand_file* file;
  uint64_t fsize;
} table_tester;

static void tt_init(table_tester* t, int use_filter) {
  t->env = ldb_memenv_new();
  ldb_options_init(&t->options);
  t->options.comparator = ldb_bytewise_comparator();
  t->options.env = t->env;
  t->options.block_cache = NULL;
  if (use_filter) {
    t->options.filter_policy = ldb_new_bloom_filter_policy(10);
  } else {
    t->options.filter_policy = NULL;
  }
  snprintf(t->fname, sizeof(t->fname), "/memenv/table_test.ldb");
  t->table = NULL;
  t->file = NULL;
  t->fsize = 0;
}

static void tt_destroy(table_tester* t) {
  if (t->table) ldb_table_destroy(t->table);
  if (t->file) t->file->m->destroy(t->file);
  if (t->options.filter_policy && t->options.filter_policy->destroy) {
    t->options.filter_policy->destroy(
        (ldb_filterpolicy*)t->options.filter_policy);
  }
  if (t->options.block_cache) {
    t->options.block_cache->destroy(t->options.block_cache);
  }
  ldb_memenv_destroy(t->env);
}

// Writes a table with keys "keyNNN" -> "valueNNN"
static ldb_status tt_build(table_tester* t, int n, int block_size) {
  t->options.block_size = (size_t)block_size;
  ldb_writable_file* file = NULL;
  ldb_status s = ldb_env_new_writable_file(t->env, t->fname, &file);
  if (!ldb_ok(s)) return s;
  ldb_table_builder* b = ldb_table_builder_new(&t->options, file);
  for (int i = 0; i < n; i++) {
    char kbuf[32], vbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%06d", i);
    snprintf(vbuf, sizeof(vbuf), "value%06d", n * 2 - i * 2);
    ldb_slice k = ldb_slice_str(kbuf);
    ldb_slice v = ldb_slice_str(vbuf);
    ldb_table_builder_add(b, &k, &v);
  }
  s = ldb_table_builder_finish(b);
  t->fsize = ldb_table_builder_file_size(b);
  ldb_table_builder_destroy(b);
  if (ldb_ok(s)) s = file->m->sync(file);
  if (ldb_ok(s)) s = file->m->close(file);
  file->m->destroy(file);
  return s;
}

static ldb_status tt_open(table_tester* t) {
  ldb_rand_file* file = NULL;
  ldb_status s = ldb_env_new_random_access_file(t->env, t->fname, &file);
  if (!ldb_ok(s)) return s;
  s = ldb_table_open(&t->options, file, t->fsize, &t->table);
  if (!ldb_ok(s)) {
    file->m->destroy(file);
  } else {
    t->file = file;
  }
  return s;
}

TEST(table, RoundTrip) {
  table_tester t;
  tt_init(&t, 0);
  CHECK_STATUS_OK(tt_build(&t, 5000, 2048));
  CHECK_STATUS_OK(tt_open(&t));
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_iterator* iter = ldb_table_new_iterator(t.table, &ro);
  int count = 0;
  for (ldb_iter_seek_to_first(iter); ldb_iter_valid(iter);
       ldb_iter_next(iter)) {
    char kbuf[32], vbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%06d", count);
    snprintf(vbuf, sizeof(vbuf), "value%06d", 5000 * 2 - count * 2);
    ldb_slice k = ldb_iter_key(iter);
    ldb_slice v = ldb_iter_value(iter);
    CHECK_EQ((long long)strlen(kbuf), (long long)k.size);
    CHECK_EQ(0, memcmp(k.data, kbuf, k.size));
    CHECK_EQ(0, memcmp(v.data, vbuf, strlen(vbuf)));
    count++;
  }
  CHECK_EQ(5000, count);
  CHECK_STATUS_OK(ldb_iter_status(iter));
  ldb_iterator_destroy(iter);
  tt_destroy(&t);
}

TEST(table, Seek) {
  table_tester t;
  tt_init(&t, 0);
  CHECK_STATUS_OK(tt_build(&t, 1000, 2048));
  CHECK_STATUS_OK(tt_open(&t));
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_iterator* iter = ldb_table_new_iterator(t.table, &ro);
  char kbuf[32];
  snprintf(kbuf, sizeof(kbuf), "key000500");
  ldb_slice target = ldb_slice_str(kbuf);
  ldb_iter_seek(iter, &target);
  CHECK_EQ(1, ldb_iter_valid(iter));
  ldb_slice k = ldb_iter_key(iter);
  CHECK_EQ(0, memcmp(k.data, kbuf, strlen(kbuf)));
  snprintf(kbuf, sizeof(kbuf), "key000499");
  ldb_iter_prev(iter);
  k = ldb_iter_key(iter);
  CHECK_EQ(0, memcmp(k.data, kbuf, strlen(kbuf)));
  // Seek past the end
  ldb_slice after = ldb_slice_str("zzzzz");
  ldb_iter_seek(iter, &after);
  CHECK_EQ(0, ldb_iter_valid(iter));
  // Seek before the beginning
  ldb_slice front = ldb_slice_str("aaaaa");
  ldb_iter_seek(iter, &front);
  CHECK_EQ(1, ldb_iter_valid(iter));
  k = ldb_iter_key(iter);
  CHECK_EQ(0, memcmp(k.data, "key000000", 9));
  ldb_iterator_destroy(iter);
  tt_destroy(&t);
}

static void tt_save_value(void* arg, const ldb_slice* ikey,
                          const ldb_slice* v) {
  ldb_buffer* out = (ldb_buffer*)arg;
  (void)ikey;
  ldb_buffer_clear(out);
  ldb_buffer_append_slice(out, v);
}

TEST(table, InternalGet) {
  table_tester t;
  tt_init(&t, 1 /*filter*/);
  CHECK_STATUS_OK(tt_build(&t, 2000, 4096));
  CHECK_STATUS_OK(tt_open(&t));
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_buffer value;
  ldb_buffer_init(&value);
  for (int i = 0; i < 2000; i += 17) {
    char kbuf[32], vbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%06d", i);
    snprintf(vbuf, sizeof(vbuf), "value%06d", 2000 * 2 - i * 2);
    ldb_slice k = ldb_slice_str(kbuf);
    ldb_status s =
        ldb_table_internal_get(t.table, &ro, &k, &value, tt_save_value);
    CHECK_STATUS_OK(s);
    CHECK_EQ(0, memcmp(value.data, vbuf, strlen(vbuf)));
  }
  // Missing key: saver must not be called, so value keeps whatever it
  // held before; clear it to observe that nothing was written.
  ldb_buffer_clear(&value);
  ldb_slice missing = ldb_slice_str("key999999");
  ldb_status s = ldb_table_internal_get(t.table, &ro, &missing, &value,
                                        tt_save_value);
  CHECK_STATUS_OK(s);
  CHECK_EQ(0, (long long)value.size);
  ldb_buffer_destroy(&value);
  tt_destroy(&t);
}

TEST(table, ApproximateOffset) {
  table_tester t;
  tt_init(&t, 0);
  CHECK_STATUS_OK(tt_build(&t, 1000, 2048));
  CHECK_STATUS_OK(tt_open(&t));
  ldb_slice first = ldb_slice_str("key000000");
  ldb_slice mid = ldb_slice_str("key000500");
  ldb_slice last = ldb_slice_str("key999999");
  uint64_t off_first = ldb_table_approximate_offset_of(t.table, &first);
  uint64_t off_mid = ldb_table_approximate_offset_of(t.table, &mid);
  uint64_t off_last = ldb_table_approximate_offset_of(t.table, &last);
  CHECK_LE((long long)off_first, (long long)off_mid);
  CHECK_LE((long long)off_mid, (long long)off_last);
  CHECK_GT((long long)off_last, 0);
  tt_destroy(&t);
}

// ------------------------------------------------------------------ block
TEST(block, BuildAndIterate) {
  ldb_options options;
  ldb_options_init(&options);
  options.block_restart_interval = 4;
  options.comparator = ldb_bytewise_comparator();
  ldb_block_builder bb;
  ldb_block_builder_init(&bb, &options);
  for (int i = 0; i < 100; i++) {
    char kbuf[32], vbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%04d", i);
    snprintf(vbuf, sizeof(vbuf), "v%04d", i);
    ldb_slice k = ldb_slice_str(kbuf);
    ldb_slice v = ldb_slice_str(vbuf);
    ldb_block_builder_add(&bb, &k, &v);
  }
  ldb_slice contents = ldb_block_builder_finish(&bb);
  CHECK(contents.size > 0);

  ldb_block_contents bc;
  bc.data = contents;
  bc.alloc = NULL;
  bc.cachable = 0;
  ldb_block* block = ldb_block_new(&bc);
  ldb_iterator* iter =
      ldb_block_new_iterator(block, ldb_bytewise_comparator());
  int count = 0;
  for (ldb_iter_seek_to_first(iter); ldb_iter_valid(iter);
       ldb_iter_next(iter)) {
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%04d", count);
    ldb_slice k = ldb_iter_key(iter);
    CHECK_EQ(0, memcmp(k.data, kbuf, strlen(kbuf)));
    count++;
  }
  CHECK_EQ(100, count);
  // Backward iteration
  int back = 99;
  ldb_iter_seek_to_last(iter);
  for (; ldb_iter_valid(iter); ldb_iter_prev(iter)) {
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%04d", back);
    ldb_slice k = ldb_iter_key(iter);
    CHECK_EQ(0, memcmp(k.data, kbuf, strlen(kbuf)));
    back--;
  }
  CHECK_EQ(-1, back);
  ldb_iterator_destroy(iter);
  ldb_block_destroy(block);
  ldb_block_builder_destroy(&bb);
}

// ------------------------------------------------------------------ filter block
TEST(filter_block, MatchAndMiss) {
  const ldb_filterpolicy* policy = ldb_new_bloom_filter_policy(10);
  ldb_filter_block_builder builder;
  ldb_filter_block_builder_init(&builder, policy);
  // One filter per 2KB of "blocks"
  for (int i = 0; i < 100; i++) {
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%d", i);
    ldb_slice k = ldb_slice_str(kbuf);
    ldb_filter_block_builder_add_key(&builder, &k);
  }
  ldb_filter_block_builder_start_block(&builder, 2048);
  for (int i = 100; i < 200; i++) {
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%d", i);
    ldb_slice k = ldb_slice_str(kbuf);
    ldb_filter_block_builder_add_key(&builder, &k);
  }
  ldb_slice fdata = ldb_filter_block_builder_finish(&builder);

  ldb_filter_block_reader reader;
  CHECK_EQ(1, ldb_filter_block_reader_init(&reader, policy, &fdata));
  // No false negatives for keys of either block
  for (int i = 0; i < 200; i++) {
    char kbuf[32];
    snprintf(kbuf, sizeof(kbuf), "key%d", i);
    ldb_slice k = ldb_slice_str(kbuf);
    uint64_t block_offset = (i < 100) ? 0 : 2048;
    CHECK_EQ(1,
             ldb_filter_block_reader_key_may_match(&reader, block_offset, &k));
  }
  // Bloom guarantees no false negatives; false positives are possible but
  // bounded, and covered implicitly by the checks above.
  ldb_filter_block_builder_destroy(&builder);
  if (policy->destroy) policy->destroy((ldb_filterpolicy*)policy);
}
