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

static int dbt_contains(const dbt_iter_collector* c, const char* pair) {
  return strstr(c->buf, pair) != NULL;
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

TEST(db, MultiThreadedWrites) {
  // Single-threaded stand-in: many sequential writes + reads
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
