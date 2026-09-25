// test_db.c - port of leveldb's db_test (core scenarios)
#include "harness.h"

typedef struct db_tester {
  ldb_options options;
  ldb_db_impl* db;
  char path[300];
} db_tester;

static void dbt_init_options(db_tester* t) {
  ldb_options_init(&t->options);
  t->options.create_if_missing = 1;
  t->options.env = ldb_memenv_new();
  t->options.write_buffer_size = 64 * 1024;  // small so compactions happen
  t->db = NULL;
}

static void dbt_reopen(db_tester* t) {
  if (t->db) {
    ldb_db_impl_destroy(t->db);
    t->db = NULL;
  }
  ldb_status s = ldb_db_open(&t->options, t->path, &t->db);
  CHECK_STATUS_OK(s);
}

static void dbt_destroy_open(db_tester* t) {
  if (t->db) {
    ldb_db_impl_destroy(t->db);
    t->db = NULL;
  }
}

static void dbt_teardown(db_tester* t) {
  dbt_destroy_open(t);
  if (t->options.block_cache) t->options.block_cache->destroy(t->options.block_cache);
  ldb_memenv_destroy(t->options.env);
  t->options.env = NULL;
  t->options.block_cache = NULL;
}

static void dbt_put(db_tester* t, const char* k, const char* v) {
  ldb_write_options wo;
  ldb_write_options_init(&wo);
  ldb_write_batch b;
  ldb_write_batch_init(&b);
  ldb_slice ks = ldb_slice_str(k);
  ldb_slice vs = ldb_slice_str(v);
  ldb_write_batch_put(&b, &ks, &vs);
  ldb_status s = ldb_db_impl_write(t->db, &wo, &b);
  CHECK_STATUS_OK(s);
  ldb_write_batch_destroy(&b);
}

static void dbt_delete(db_tester* t, const char* k) {
  ldb_write_options wo;
  ldb_write_options_init(&wo);
  ldb_write_batch b;
  ldb_write_batch_init(&b);
  ldb_slice ks = ldb_slice_str(k);
  ldb_write_batch_delete(&b, &ks);
  ldb_status s = ldb_db_impl_write(t->db, &wo, &b);
  CHECK_STATUS_OK(s);
  ldb_write_batch_destroy(&b);
}

static int dbt_get(db_tester* t, const char* k, char* out, size_t out_size) {
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_slice ks = ldb_slice_str(k);
  ldb_status s = ldb_db_impl_get(t->db, &ro, &ks, &value);
  int found = ldb_ok(s);
  if (found) {
    snprintf(out, out_size, "%.*s", (int)value.size,
             value.data ? value.data : "");
  } else {
    out[0] = '\0';
  }
  ldb_status_destroy(&s);
  ldb_buffer_destroy(&value);
  return found;
}

static void dbt_check_get(db_tester* t, const char* k, const char* expected) {
  char buf[1024];
  int found = dbt_get(t, k, buf, sizeof(buf));
  if (expected == NULL) {
    if (found) {
      ldb_test_fail(__FILE__, __LINE__, "expected missing key %s, got '%s'", k,
                    buf);
    }
  } else if (!found) {
    ldb_test_fail(__FILE__, __LINE__, "expected key %s = '%s', missing", k,
                  expected);
  } else if (strcmp(buf, expected) != 0) {
    ldb_test_fail(__FILE__, __LINE__, "expected key %s = '%s', got '%s'", k,
                  expected, buf);
  }
}

// iterator helper: collects visible (key,value) pairs into a string
typedef struct dbt_iter_collector {
  char buf[8192];
  size_t len;
} dbt_iter_collector;

static void dbt_collect(db_tester* t, dbt_iter_collector* c,
                        const ldb_read_options* ro) {
  c->len = 0;
  c->buf[0] = '\0';
  ldb_iterator* it = ldb_db_impl_new_iterator(t->db, ro);
  for (ldb_iter_seek_to_first(it); ldb_iter_valid(it); ldb_iter_next(it)) {
    ldb_slice k = ldb_iter_key(it);
    ldb_slice v = ldb_iter_value(it);
    c->len += (size_t)snprintf(c->buf + c->len, sizeof(c->buf) - c->len,
                               "%.*s=%.*s;", (int)k.size, k.data, (int)v.size,
                               v.data);
  }
  CHECK_STATUS_OK(ldb_iter_status(it));
  ldb_iterator_destroy(it);
}

// ------------------------------------------------------------------ tests
TEST(db, Empty) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_empty", t.path, sizeof(t.path));
  dbt_reopen(&t);
  char buf[64];
  CHECK_EQ(0, dbt_get(&t, "missing", buf, sizeof(buf)));
  dbt_teardown(&t);
}

TEST(db, ReadWrite) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_readwrite", t.path, sizeof(t.path));
  dbt_reopen(&t);
  char buf[1024];
  CHECK_EQ(0, dbt_get(&t, "foo", buf, sizeof(buf)));

  dbt_put(&t, "foo", "bar");
  dbt_check_get(&t, "foo", "bar");
  dbt_check_get(&t, "foo", "bar");  // repeated read

  dbt_put(&t, "foo", "baz");
  dbt_check_get(&t, "foo", "baz");
  dbt_teardown(&t);
}

TEST(db, PutDeleteGet) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_putdeleteget", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "bar");
  dbt_delete(&t, "foo");
  dbt_check_get(&t, "foo", NULL);
  dbt_teardown(&t);
}

TEST(db, GetFromVersions) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_getfromversions", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "v1");
  dbt_reopen(&t);  // force flush/recovery
  dbt_check_get(&t, "foo", "v1");
  dbt_put(&t, "foo", "v2");
  dbt_check_get(&t, "foo", "v2");
  dbt_reopen(&t);
  dbt_check_get(&t, "foo", "v2");
  dbt_teardown(&t);
}

TEST(db, GetSnapshots) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_getsnapshots", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "v1");
  const ldb_snapshot_impl* s1 = ldb_db_impl_get_snapshot(t.db);
  dbt_put(&t, "foo", "v2");
  const ldb_snapshot_impl* s2 = ldb_db_impl_get_snapshot(t.db);
  dbt_put(&t, "foo", "v3");
  const ldb_snapshot_impl* s3 = ldb_db_impl_get_snapshot(t.db);

  dbt_put(&t, "foo", "v4");
  dbt_check_get(&t, "foo", "v4");

  // Read from each snapshot
  {
    ldb_read_options ro;
    ldb_read_options_init(&ro);
    ldb_buffer value;
    ldb_buffer_init(&value);
    ldb_slice k = ldb_slice_str("foo");

    ro.snapshot = s1;
    ldb_status st = ldb_db_impl_get(t.db, &ro, &k, &value);
    CHECK_STATUS_OK(st);
    CHECK_BUF_EQ("v1", value);
    ro.snapshot = s2;
    st = ldb_db_impl_get(t.db, &ro, &k, &value);
    CHECK_STATUS_OK(st);
    CHECK_BUF_EQ("v2", value);
    ro.snapshot = s3;
    st = ldb_db_impl_get(t.db, &ro, &k, &value);
    CHECK_STATUS_OK(st);
    CHECK_BUF_EQ("v3", value);

    ldb_buffer_destroy(&value);
  }

  ldb_db_impl_release_snapshot(t.db, s1);
  ldb_db_impl_release_snapshot(t.db, s2);
  ldb_db_impl_release_snapshot(t.db, s3);
  dbt_teardown(&t);
}

TEST(db, IterEmpty) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_iterempty", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_iter_collector c;
  dbt_collect(&t, &c, NULL);
  CHECK_STR_EQ("", c.buf);
  dbt_teardown(&t);
}

TEST(db, IterSingle) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_itersingle", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "a", "va");
  dbt_iter_collector c;
  dbt_collect(&t, &c, NULL);
  CHECK_STR_EQ("a=va;", c.buf);
  dbt_teardown(&t);
}

TEST(db, IterMulti) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_itermulti", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "b", "vb");
  dbt_put(&t, "c", "vc");
  dbt_put(&t, "a", "va");
  dbt_put(&t, "d", "vd");
  dbt_iter_collector c;
  dbt_collect(&t, &c, NULL);
  CHECK_STR_EQ("a=va;b=vb;c=vc;d=vd;", c.buf);
  dbt_teardown(&t);
}

TEST(db, IterMultiWithDelete) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_itermultidelete", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "b", "vb");
  dbt_put(&t, "c", "vc");
  dbt_delete(&t, "b");
  dbt_put(&t, "a", "va");
  dbt_delete(&t, "c");
  dbt_put(&t, "d", "vd");
  dbt_iter_collector c;
  dbt_collect(&t, &c, NULL);
  CHECK_STR_EQ("a=va;d=vd;", c.buf);
  dbt_teardown(&t);
}

TEST(db, IterPrev) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_iterprev", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "b", "vb");
  dbt_put(&t, "c", "vc");
  dbt_put(&t, "a", "va");
  dbt_put(&t, "d", "vd");

  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_iterator* it = ldb_db_impl_new_iterator(t.db, &ro);
  // Seek to c, then Prev repeatedly
  ldb_slice target = ldb_slice_str("c");
  ldb_iter_seek(it, &target);
  CHECK_EQ(1, ldb_iter_valid(it));
  ldb_slice k = ldb_iter_key(it);
  CHECK_EQ('c', k.data[0]);
  ldb_iter_prev(it);
  CHECK_EQ(1, ldb_iter_valid(it));
  k = ldb_iter_key(it);
  CHECK_EQ('b', k.data[0]);
  ldb_iter_prev(it);
  CHECK_EQ(1, ldb_iter_valid(it));
  k = ldb_iter_key(it);
  CHECK_EQ('a', k.data[0]);
  ldb_iter_prev(it);
  CHECK_EQ(0, ldb_iter_valid(it));
  // SeekToLast then walk backward
  ldb_iter_seek_to_last(it);
  CHECK_EQ(1, ldb_iter_valid(it));
  k = ldb_iter_key(it);
  CHECK_EQ('d', k.data[0]);
  ldb_iter_prev(it);
  k = ldb_iter_key(it);
  CHECK_EQ('c', k.data[0]);
  ldb_iterator_destroy(it);
  dbt_teardown(&t);
}

TEST(db, IterWithMultipleVersions) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_iterversions", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "k", "v1");
  dbt_put(&t, "k", "v2");
  dbt_put(&t, "k", "v3");
  dbt_iter_collector c;
  dbt_collect(&t, &c, NULL);
  CHECK_STR_EQ("k=v3;", c.buf);
  // Snapshot at earlier sequence
  dbt_put(&t, "k", "v4");
  dbt_iter_collector c2;
  dbt_collect(&t, &c2, NULL);
  CHECK_STR_EQ("k=v4;", c2.buf);
  dbt_teardown(&t);
}

TEST(db, HiddenFromNewSnapshots) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_hidden", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "0");
  const ldb_snapshot_impl* s = ldb_db_impl_get_snapshot(t.db);
  dbt_put(&t, "foo", "1");
  dbt_put(&t, "bar", "2");
  // Snapshot still sees old state
  {
    ldb_read_options ro;
    ldb_read_options_init(&ro);
    ro.snapshot = s;
    ldb_buffer value;
    ldb_buffer_init(&value);
    ldb_slice k = ldb_slice_str("foo");
    ldb_status st = ldb_db_impl_get(t.db, &ro, &k, &value);
    CHECK_STATUS_OK(st);
    CHECK_BUF_EQ("0", value);
    ldb_slice k2 = ldb_slice_str("bar");
    st = ldb_db_impl_get(t.db, &ro, &k2, &value);
    CHECK_EQ(0, ldb_ok(st));
    ldb_buffer_destroy(&value);
    ldb_status_destroy(&st);
  }
  ldb_db_impl_release_snapshot(t.db, s);
  dbt_teardown(&t);
}

TEST(db, CompactionTriggers) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_compaction", t.path, sizeof(t.path));
  t.options.write_buffer_size = 64 * 1024;
  dbt_reopen(&t);
  // Write enough data to trigger multiple memtable flushes and compactions
  char value[1000];
  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  char key[32];
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_put(&t, key, value);
  }
  // Verify all values survive compactions
  for (int i = 0; i < 500; i += 13) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_check_get(&t, key, value);
  }
  // Overwrite all keys, force another round of compactions
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_put(&t, key, "short");
  }
  dbt_reopen(&t);
  for (int i = 0; i < 500; i += 17) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_check_get(&t, key, "short");
  }
  dbt_teardown(&t);
}

TEST(db, DeletesAcrossCompactions) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_deletes", t.path, sizeof(t.path));
  t.options.write_buffer_size = 64 * 1024;
  dbt_reopen(&t);
  char value[500];
  memset(value, 'y', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  char key[32];
  for (int i = 0; i < 200; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_put(&t, key, value);
  }
  for (int i = 0; i < 200; i += 2) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_delete(&t, key);
  }
  for (int i = 0; i < 200; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    if (i % 2 == 0) {
      dbt_check_get(&t, key, NULL);
    } else {
      dbt_check_get(&t, key, value);
    }
  }
  // Reopen: recovery must preserve state
  dbt_reopen(&t);
  for (int i = 0; i < 200; i += 50) {
    snprintf(key, sizeof(key), "key%06d", i);
    if (i % 2 == 0) {
      dbt_check_get(&t, key, NULL);
    } else {
      dbt_check_get(&t, key, value);
    }
  }
  dbt_teardown(&t);
}

TEST(db, RecoveryEmptyLog) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_recovery", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foobar", "5");
  // Simulate crash: destroy handle without closing cleanly is not possible
  // here, so just reopen.
  dbt_reopen(&t);
  dbt_check_get(&t, "foobar", "5");
  dbt_teardown(&t);
}

TEST(db, DBOpenNotExist) {
  db_tester t;
  dbt_init_options(&t);
  t.options.create_if_missing = 0;
  ldb_test_make_db_path("db_test_notexist", t.path, sizeof(t.path));
  ldb_db_impl* db = NULL;
  ldb_status s = ldb_db_open(&t.options, t.path, &db);
  CHECK_STATUS_ERR(s);
  ldb_status_destroy(&s);
  dbt_teardown(&t);
}

TEST(db, DBErrorIfExists) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_errorifexists", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_destroy_open(&t);
  t.options.error_if_exists = 1;
  t.options.create_if_missing = 1;
  ldb_db_impl* db = NULL;
  ldb_status s = ldb_db_open(&t.options, t.path, &db);
  CHECK_STATUS_ERR(s);
  ldb_status_destroy(&s);
  t.options.error_if_exists = 0;
  dbt_teardown(&t);
}

TEST(db, DestroyEmptyDir) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_destroy", t.path, sizeof(t.path));
  ldb_options opt;
  ldb_options_init(&opt);
  opt.env = t.options.env;
  CHECK_STATUS_OK(ldb_destroy_db(&opt, t.path));
  dbt_teardown(&t);
}

TEST(db, DestroyOpenDB) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_destroyopen", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "v1");
  ldb_options opt;
  ldb_options_init(&opt);
  opt.env = t.options.env;
  CHECK_STATUS_OK(ldb_destroy_db(&opt, t.path));
  // The DB object still works (like leveldb), data was deleted under it.
  dbt_teardown(&t);
}

TEST(db, CompactRange) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_compactrange", t.path, sizeof(t.path));
  t.options.write_buffer_size = 64 * 1024;
  dbt_reopen(&t);
  char value[300];
  memset(value, 'z', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  char key[32];
  for (int i = 0; i < 300; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_put(&t, key, value);
  }
  // Manual full compaction
  ldb_db_impl_compact_range(t.db, NULL, NULL);
  for (int i = 0; i < 300; i += 11) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_check_get(&t, key, value);
  }
  // Compaction with a range
  ldb_slice a = ldb_slice_str("key000100");
  ldb_slice b = ldb_slice_str("key000200");
  ldb_db_impl_compact_range(t.db, &a, &b);
  dbt_check_get(&t, "key000150", value);
  dbt_teardown(&t);
}

TEST(db, ApproximateSizes) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_approx", t.path, sizeof(t.path));
  t.options.write_buffer_size = 64 * 1024;
  dbt_reopen(&t);
  char value[1000];
  for (int j = 0; j < (int)sizeof(value) - 1; j++) value[j] = (char)(0x21 + ((j * 7919) % 90));
  value[sizeof(value) - 1] = '\0';
  char key[32];
  for (int i = 0; i < 100; i++) {
    snprintf(key, sizeof(key), "key%06d", i);
    dbt_put(&t, key, value);
  }
  ldb_slice start = ldb_slice_str("key000000");
  ldb_slice limit = ldb_slice_str("key999999");
  uint64_t sizes[1];
  ldb_db_impl_get_approximate_sizes(t.db, &start, &limit, 1, sizes);
  // Size of the whole range should be nonzero and within the file sizes.
  CHECK_GE((long long)sizes[0], (long long)1);
  ldb_slice start2 = ldb_slice_str("zzzzz");
  uint64_t sizes2[1];
  ldb_db_impl_get_approximate_sizes(t.db, &start2, &limit, 1, sizes2);
  CHECK_EQ(0, (long long)sizes2[0]);
  dbt_teardown(&t);
}

TEST(db, GetProperty) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_property", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "bar");
  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_slice prop = ldb_slice_str("leveldb.stats");
  CHECK_EQ(1, ldb_db_impl_get_property(t.db, &prop, &value));
  CHECK(value.size > 0);
  ldb_buffer_clear(&value);
  ldb_slice prop2 = ldb_slice_str("leveldb.sstables");
  CHECK_EQ(1, ldb_db_impl_get_property(t.db, &prop2, &value));
  CHECK(value.size > 0);
  ldb_buffer_clear(&value);
  ldb_slice prop3 = ldb_slice_str("leveldb.num-files-at-level0");
  CHECK_EQ(1, ldb_db_impl_get_property(t.db, &prop3, &value));
  CHECK(value.size > 0);
  ldb_buffer_clear(&value);
  ldb_slice prop4 = ldb_slice_str("leveldb.bogus");
  CHECK_EQ(0, ldb_db_impl_get_property(t.db, &prop4, &value));
  ldb_buffer_destroy(&value);
  dbt_teardown(&t);
}

TEST(db, SequenceNumberRecovery) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_seqnum", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "v1");
  dbt_reopen(&t);
  dbt_put(&t, "bar", "v2");
  // If sequence numbers were not recovered, the second put would shadow
  // the first one's visibility.
  dbt_check_get(&t, "foo", "v1");
  dbt_check_get(&t, "bar", "v2");
  dbt_reopen(&t);
  dbt_check_get(&t, "foo", "v1");
  dbt_teardown(&t);
}

// ------------------------------------------------------------------ multithreaded
// Port of leveldb db_test MultiThreaded: N threads share one DB handle,
// randomly writing values of the form "<key>.<writer>.<counter>" or
// reading and verifying that the encoded counter never exceeds the
// writer's published progress. Exercises writer-queue / memtable-flush /
// background-compaction / Get interleavings that single-threaded tests
// cannot reach. Keep the old sequential coverage as DBWriteHeavy.
#define MT_NUM_THREADS 4
#define MT_NUM_KEYS 1000
#define MT_OPS_PER_THREAD 2000

typedef struct mt_state {
  db_tester* t;
  ldb_atomic_int counter[MT_NUM_THREADS];
  ldb_atomic_int thread_done[MT_NUM_THREADS];
} mt_state;

typedef struct mt_thread {
  mt_state* state;
  int id;
} mt_thread;

static uint64_t mt_rnd_next(uint64_t* seed) {
  // MINSTD, same generator family as leveldb's Random.
  *seed = (*seed * 16807ull) % 2147483647ull;
  return *seed;
}

static void mt_thread_body(void* arg) {
  mt_thread* th = (mt_thread*)arg;
  mt_state* st = th->state;
  db_tester* t = st->t;
  uint64_t seed = 1000 + (uint64_t)th->id;
  int counter = 0;
  for (int op = 0; op < MT_OPS_PER_THREAD; op++) {
    // Publish progress before the next operation, like leveldb.
    ldb_atomic_store(&st->counter[th->id], counter);

    int key = (int)(mt_rnd_next(&seed) % MT_NUM_KEYS);
    char keybuf[24];
    snprintf(keybuf, sizeof(keybuf), "%016d", key);
    ldb_slice ks = ldb_slice_str(keybuf);

    if (mt_rnd_next(&seed) % 2 == 0) {
      // Write values of the form <key, my id, counter>; the padding
      // forces memtable flushes and compactions during the test.
      char val[1100];
      int n = snprintf(val, sizeof(val), "%d.%d.%-1000d", key, th->id,
                       counter);
      ldb_write_options wo;
      ldb_write_options_init(&wo);
      ldb_write_batch b;
      ldb_write_batch_init(&b);
      ldb_slice vs = ldb_slice_make(val, (size_t)n);
      ldb_write_batch_put(&b, &ks, &vs);
      ldb_status s = ldb_db_impl_write(t->db, &wo, &b);
      if (!ldb_ok(s)) {
        char* msg = ldb_status_to_string(s);
        ldb_test_fail(__FILE__, __LINE__, "thread %d put failed: %s", th->id,
                      msg);
      }
      ldb_write_batch_destroy(&b);
    } else {
      ldb_read_options ro;
      ldb_read_options_init(&ro);
      ldb_buffer value;
      ldb_buffer_init(&value);
      ldb_status s = ldb_db_impl_get(t->db, &ro, &ks, &value);
      if (ldb_ok(s)) {
        // value may not be NUL-terminated; copy before sscanf.
        char* tmp = (char*)malloc(value.size + 1);
        memcpy(tmp, value.data ? value.data : "", value.size);
        tmp[value.size] = '\0';
        int k, w, c;
        if (sscanf(tmp, "%d.%d.%d", &k, &w, &c) != 3 || k != key || w < 0 ||
            w >= MT_NUM_THREADS ||
            c > ldb_atomic_load(&st->counter[w])) {
          ldb_test_fail(__FILE__, __LINE__,
                        "thread %d: bad value '%s' for key %d", th->id, tmp,
                        key);
        }
        free(tmp);
      } else if (s.code == LDB_NOTFOUND) {
        // Key has not yet been written.
      } else {
        char* msg = ldb_status_to_string(s);
        ldb_test_fail(__FILE__, __LINE__, "thread %d get failed: %s", th->id,
                      msg);
      }
      ldb_status_destroy(&s);
      ldb_buffer_destroy(&value);
    }
    counter++;
  }
  ldb_atomic_store(&st->thread_done[th->id], 1);
}

TEST(db, MultiThreaded) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_mt", t.path, sizeof(t.path));
  dbt_reopen(&t);

  mt_state st;
  st.t = &t;
  for (int i = 0; i < MT_NUM_THREADS; i++) {
    ldb_atomic_store(&st.counter[i], 0);
    ldb_atomic_store(&st.thread_done[i], 0);
  }
  mt_thread th[MT_NUM_THREADS];
  for (int i = 0; i < MT_NUM_THREADS; i++) {
    th[i].state = &st;
    th[i].id = i;
    ldb_start_thread(mt_thread_body, &th[i]);
  }

  // Wait for all workers; they are iteration-bounded so this terminates,
  // but the wait is bounded too so a lost wakeup fails instead of hanging.
  int done = 0;
  uint64_t deadline_micros =
      ldb_env_now_micros(t.options.env) + 120 * 1000000ull;
  while (done < MT_NUM_THREADS) {
    done = 0;
    for (int i = 0; i < MT_NUM_THREADS; i++) {
      done += ldb_atomic_load(&st.thread_done[i]);
    }
    if (done < MT_NUM_THREADS) {
      if (ldb_env_now_micros(t.options.env) > deadline_micros) {
        ldb_test_fail(__FILE__, __LINE__,
                      "MT workers did not finish within 120s (done=%d/%d)",
                      done, MT_NUM_THREADS);
      }
      ldb_env_sleep_for_microseconds(t.options.env, 20000);
    }
  }

  dbt_teardown(&t);
}

TEST(db, WriteHeavySequential) {
  // Single-threaded stand-in for sustained write coverage: many sequential
  // writes + reads across multiple "rounds" (overwrite pattern).
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_multi", t.path, sizeof(t.path));
  dbt_reopen(&t);
  for (int round = 0; round < 3; round++) {
    for (int i = 0; i < 100; i++) {
      char key[32], val[32];
      snprintf(key, sizeof(key), "key%04d", i);
      snprintf(val, sizeof(val), "value%d_%d", round, i);
      dbt_put(&t, key, val);
    }
  }
  for (int i = 0; i < 100; i++) {
    char key[32], expected[32];
    snprintf(key, sizeof(key), "key%04d", i);
    snprintf(expected, sizeof(expected), "value2_%d", i);
    dbt_check_get(&t, key, expected);
  }
  dbt_teardown(&t);
}

// ------------------------------------------------------------------ repair
TEST(db, RepairOpenDB) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_repair", t.path, sizeof(t.path));
  dbt_reopen(&t);
  dbt_put(&t, "foo", "bar");
  dbt_put(&t, "baz", "quux");
  dbt_destroy_open(&t);  // close, then damage the metadata files
  // Delete CURRENT and MANIFEST to force repair to rebuild from logs/tables.
  ldb_strings files;
  ldb_strings_init(&files);
  CHECK_STATUS_OK(ldb_env_get_children(t.options.env, t.path, &files));
  for (size_t i = 0; i < files.count; i++) {
    uint64_t number;
    int type;
    if (ldb_parse_file_name(files.items[i], &number, &type) &&
        (type == LDB_K_DESCRIPTOR_FILE || type == LDB_K_CURRENT_FILE)) {
      char* full = (char*)malloc(strlen(t.path) + strlen(files.items[i]) + 2);
      sprintf(full, "%s/%s", t.path, files.items[i]);
      CHECK_STATUS_OK(ldb_env_remove_file(t.options.env, full));
      free(full);
    }
  }
  ldb_strings_destroy(&files);

  // Repair
  ldb_options ropt;
  ldb_options_init(&ropt);
  ropt.env = t.options.env;
  CHECK_STATUS_OK(ldb_repair_db(&ropt, t.path));

  // Reopen and verify data
  dbt_reopen(&t);
  dbt_check_get(&t, "foo", "bar");
  dbt_check_get(&t, "baz", "quux");
  dbt_teardown(&t);
}

// ------------------------------------------------------------------ repair fidelity
// A repaired database must be readable by the ordinary path, which means two
// things the WAL->table conversion has to get right: the filter block has to be
// keyed the way a normal table keys it (user keys, through the internal filter
// policy), and the new descriptor's last_sequence has to cover the table that
// was just converted.  Either mistake leaves the data in the SST but invisible
// to Get, so the checks below are the assertion.
TEST(db, RepairWalWithBloomFilter) {
  db_tester t;
  dbt_init_options(&t);
  const ldb_filterpolicy* bloom = ldb_new_bloom_filter_policy(10);
  t.options.filter_policy = bloom;
  ldb_test_make_db_path("db_test_repair_bloom", t.path, sizeof(t.path));
  dbt_reopen(&t);
  char key[32], val[32];
  for (int i = 0; i < 200; i++) {
    snprintf(key, sizeof(key), "key%04d", i);
    snprintf(val, sizeof(val), "value%04d", i);
    dbt_put(&t, key, val);
  }
  dbt_destroy_open(&t);  // no flush: everything is still in the WAL

  ldb_options ropt;
  ldb_options_init(&ropt);
  ropt.env = t.options.env;
  ropt.filter_policy = bloom;
  CHECK_STATUS_OK(ldb_repair_db(&ropt, t.path));

  // Drop the WAL, so the converted table is the only copy of the data.  While
  // the log is around a reopen replays it and papers over both mistakes.
  ldb_strings files;
  ldb_strings_init(&files);
  CHECK_STATUS_OK(ldb_env_get_children(t.options.env, t.path, &files));
  for (size_t i = 0; i < files.count; i++) {
    uint64_t number;
    int type;
    if (ldb_parse_file_name(files.items[i], &number, &type) &&
        type == LDB_K_LOG_FILE) {
      char* full = (char*)malloc(strlen(t.path) + strlen(files.items[i]) + 2);
      sprintf(full, "%s/%s", t.path, files.items[i]);
      CHECK_STATUS_OK(ldb_env_remove_file(t.options.env, full));
      free(full);
    }
  }
  ldb_strings_destroy(&files);

  dbt_reopen(&t);
  char buf[64];
  for (int i = 0; i < 200; i++) {
    snprintf(key, sizeof(key), "key%04d", i);
    snprintf(val, sizeof(val), "value%04d", i);
    CHECK_EQ(1, dbt_get(&t, key, buf, sizeof(buf)));
    CHECK_STR_EQ(val, buf);
  }
  dbt_teardown(&t);
  ((ldb_filterpolicy*)bloom)->destroy((ldb_filterpolicy*)bloom);
}

// ------------------------------------------------------------------ memenv rename
static void dbt_env_write(ldb_env* e, const char* path, const char* what) {
  ldb_writable_file* w = NULL;
  CHECK_STATUS_OK(ldb_env_new_writable_file(e, path, &w));
  ldb_slice d = ldb_slice_str(what);
  CHECK_STATUS_OK(w->m->append(w, &d));
  CHECK_STATUS_OK(w->m->close(w));
  w->m->destroy(w);
}

static void dbt_env_check_read(ldb_env* e, const char* path, const char* expect) {
  ldb_seq_file* f = NULL;
  CHECK_STATUS_OK(ldb_env_new_sequential_file(e, path, &f));
  char buf[64];
  ldb_slice out;
  CHECK_STATUS_OK(f->m->read(f, (uint64_t)strlen(expect), &out, buf));
  CHECK_EQ(strlen(expect), out.size);
  CHECK(memcmp(buf, expect, out.size) == 0);
  f->m->destroy(f);
}

static int dbt_env_child_count(ldb_env* e, const char* dir) {
  ldb_strings files;
  ldb_strings_init(&files);
  CHECK_STATUS_OK(ldb_env_get_children(e, dir, &files));
  int n = (int)files.count;
  ldb_strings_destroy(&files);
  return n;
}

// rename(2) semantics the POSIX backend gives for free: renaming a file onto
// itself succeeds and changes nothing, and renaming onto a file that still has
// open handles unlinks the target without pulling it out from under them.
TEST(db, MemenvRenameSemantics) {
  ldb_env* e = ldb_memenv_new();
  ldb_status s = ldb_env_create_dir(e, "/r");
  ldb_status_destroy(&s);
  dbt_env_write(e, "/r/a", "hello");
  dbt_env_write(e, "/r/b", "world");

  CHECK_STATUS_OK(ldb_env_rename_file(e, "/r/a", "/r/a"));
  CHECK_EQ(2, dbt_env_child_count(e, "/r"));
  dbt_env_check_read(e, "/r/a", "hello");

  ldb_seq_file* open_b = NULL;
  CHECK_STATUS_OK(ldb_env_new_sequential_file(e, "/r/b", &open_b));
  CHECK_STATUS_OK(ldb_env_rename_file(e, "/r/a", "/r/b"));
  CHECK_EQ(1, dbt_env_child_count(e, "/r"));
  dbt_env_check_read(e, "/r/b", "hello");
  open_b->m->destroy(open_b);
  CHECK_EQ(1, dbt_env_child_count(e, "/r"));
  dbt_env_check_read(e, "/r/b", "hello");
  ldb_memenv_destroy(e);
}

// ------------------------------------------------------------------ empty slices
static void dbt_noop_deleter(const ldb_slice* key, void* value) {
  (void)key;
  (void)value;
}

// {NULL,0} slices and zero-length values reach memcpy in the memtable, the
// block cache and the range-overlap binary search.  memcpy's nonnull contract
// is violated even when the length is zero, so only a sanitizer build turns
// this into a failure; the value checks make sure the data really round-trips.
TEST(db, EmptySlicesWithFilesInHigherLevels) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_empty_slices", t.path, sizeof(t.path));
  dbt_reopen(&t);

  char key[32], val[32];
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "key%04d", i);
    snprintf(val, sizeof(val), "value%04d", i);
    dbt_put(&t, key, val);
  }
  ldb_write_options wo;
  ldb_write_options_init(&wo);
  ldb_write_batch b;
  ldb_write_batch_init(&b);
  ldb_slice empty = {NULL, 0};
  ldb_write_batch_put(&b, &empty, &empty);
  ldb_write_batch_delete(&b, &empty);
  ldb_write_batch_put(&b, &empty, &empty);
  CHECK_STATUS_OK(ldb_db_impl_write(t.db, &wo, &b));
  ldb_write_batch_destroy(&b);

  // Put files in level 1 so the binary-search branch of the overlap test runs.
  ldb_db_impl_test_compact_range(t.db, 0, NULL, NULL);
  ldb_db_impl_compact_range(t.db, &empty, &empty);

  char buf[64];
  for (int i = 0; i < 500; i++) {
    snprintf(key, sizeof(key), "key%04d", i);
    snprintf(val, sizeof(val), "value%04d", i);
    CHECK_EQ(1, dbt_get(&t, key, buf, sizeof(buf)));
    CHECK_STR_EQ(val, buf);
  }
  dbt_teardown(&t);

  ldb_cache* c = ldb_cache_new_lru(1024);
  char v[] = "v";  // insert() takes void*, so a literal would need a const-cast
  ldb_cache_handle* h = c->insert(c, &empty, v, 1, dbt_noop_deleter);
  CHECK(h != NULL);
  c->release(c, h);
  ldb_cache_handle* h2 = c->lookup(c, &empty);
  CHECK(h2 != NULL);
  c->release(c, h2);
  c->destroy(c);
}

// ------------------------------------------------------------------ large values
// Values > the 4KB write-batch heap-buffer threshold and > the 4KB block
// size; key > 4096 exercises the heap path in batch replay.
TEST(db, LargeValueRoundtrip) {
  db_tester t;
  dbt_init_options(&t);
  ldb_test_make_db_path("db_test_large", t.path, sizeof(t.path));
  dbt_reopen(&t);

  const size_t kBigKey = 8 * 1024;      // > 4096: heap path
  const size_t kBigValue = 100 * 1024;  // > block size and > write buffer
  char* kbuf = (char*)malloc(kBigKey);
  char* vbuf = (char*)malloc(kBigValue);
  for (size_t i = 0; i < kBigKey; i++) kbuf[i] = (char)(i * 31u + 1);
  for (size_t i = 0; i < kBigValue; i++) vbuf[i] = (char)(i * 31u + 2);
  ldb_slice key = ldb_slice_make(kbuf, kBigKey);
  ldb_slice val = ldb_slice_make(vbuf, kBigValue);

  ldb_write_options wo;
  ldb_write_options_init(&wo);
  ldb_write_batch b;
  ldb_write_batch_init(&b);
  ldb_write_batch_put(&b, &key, &val);
  CHECK_STATUS_OK(ldb_db_impl_write(t.db, &wo, &b));
  ldb_write_batch_destroy(&b);

  ldb_read_options ro;
  ldb_read_options_init(&ro);
  ldb_buffer got;
  ldb_buffer_init(&got);
  CHECK_STATUS_OK(ldb_db_impl_get(t.db, &ro, &key, &got));
  CHECK_EQ((long long)kBigValue, (long long)got.size);
  CHECK_EQ(0, memcmp(got.data, vbuf, kBigValue));
  ldb_buffer_destroy(&got);

  // Visible through an iterator with identical content.
  ldb_iterator* it = ldb_db_impl_new_iterator(t.db, &ro);
  ldb_iter_seek_to_first(it);
  CHECK_EQ(1, ldb_iter_valid(it));
  ldb_slice iter_key = ldb_iter_key(it);
  CHECK_EQ(1, ldb_slice_equals(&key, &iter_key));
  CHECK_EQ((long long)kBigValue, (long long)ldb_iter_value(it).size);
  CHECK_EQ(0, memcmp(ldb_iter_value(it).data, vbuf, kBigValue));
  CHECK_STATUS_OK(ldb_iter_status(it));
  ldb_iterator_destroy(it);

  // Survives reopen (WAL replay of a large batch) and compaction.
  dbt_reopen(&t);
  ldb_buffer_init(&got);
  CHECK_STATUS_OK(ldb_db_impl_get(t.db, &ro, &key, &got));
  CHECK_EQ((long long)kBigValue, (long long)got.size);
  CHECK_EQ(0, memcmp(got.data, vbuf, kBigValue));
  ldb_buffer_destroy(&got);

  ldb_db_impl_compact_range(t.db, NULL, NULL);
  ldb_buffer_init(&got);
  CHECK_STATUS_OK(ldb_db_impl_get(t.db, &ro, &key, &got));
  CHECK_EQ((long long)kBigValue, (long long)got.size);
  CHECK_EQ(0, memcmp(got.data, vbuf, kBigValue));
  ldb_buffer_destroy(&got);

  free(kbuf);
  free(vbuf);
  dbt_teardown(&t);
}
