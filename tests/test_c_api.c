// test_c_api.c - port of leveldb's c_test (core scenarios)
#include "harness.h"

#include "leveldb/c.h"

TEST(c_api, OpenOnNonExistentDB) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_env_t* env = leveldb_create_default_env();
  leveldb_options_set_env(options, env);
  leveldb_options_set_create_if_missing(options, 0);
  char path[300];
  ldb_test_make_db_path("c_api_notexist", path, sizeof(path));
  char* err2 = NULL;
  leveldb_t* db = leveldb_open(options, path, &err2);
  CHECK(db == NULL);
  CHECK(err2 != NULL);
  leveldb_free(err2);
  leveldb_options_destroy(options);
  leveldb_env_destroy(env);
}

TEST(c_api, PutGetDelete) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_putgetdelete", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);
  CHECK(err == NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  const char* key = "key";
  const char* val = "value";
  leveldb_put(db, wo, key, 3, val, 5, &err);
  CHECK(err == NULL);
  leveldb_delete(db, wo, key, 3, &err);
  CHECK(err == NULL);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  size_t vallen = 12345;
  char* got = leveldb_get(db, ro, key, 3, &vallen, &err);
  CHECK(got == NULL);
  CHECK_EQ(0, (long long)vallen);
  CHECK(err == NULL);
  leveldb_put(db, wo, key, 3, val, 5, &err);
  CHECK(err == NULL);
  got = leveldb_get(db, ro, key, 3, &vallen, &err);
  CHECK(got != NULL);
  CHECK_EQ(5, (long long)vallen);
  CHECK_STR_EQ("value", got);
  leveldb_free(got);

  leveldb_writeoptions_destroy(wo);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, WriteBatch) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_writebatch", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_writebatch_t* wb = leveldb_writebatch_create();
  leveldb_writebatch_put(wb, "one", 3, "1", 1);
  leveldb_writebatch_put(wb, "two", 3, "2", 1);
  leveldb_writebatch_put(wb, "three", 5, "3", 1);
  leveldb_writebatch_delete(wb, "one", 3);
  leveldb_write(db, wo, wb, &err);
  CHECK(err == NULL);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  size_t vallen = 0;
  char* got = leveldb_get(db, ro, "one", 3, &vallen, &err);
  CHECK(got == NULL);
  CHECK(err == NULL);
  got = leveldb_get(db, ro, "two", 3, &vallen, &err);
  CHECK(got != NULL);
  CHECK_STR_EQ("2", got);
  leveldb_free(got);
  got = leveldb_get(db, ro, "three", 5, &vallen, &err);
  CHECK(got != NULL);
  CHECK_STR_EQ("3", got);
  leveldb_free(got);

  leveldb_writebatch_destroy(wb);
  leveldb_writeoptions_destroy(wo);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, Iterators) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_iterators", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_put(db, wo, "a", 1, "va", 2, &err);
  CHECK(err == NULL);
  leveldb_put(db, wo, "b", 1, "vb", 2, &err);
  CHECK(err == NULL);
  leveldb_put(db, wo, "c", 1, "vc", 2, &err);
  CHECK(err == NULL);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_iterator_t* it = leveldb_create_iterator(db, ro);
  CHECK(it != NULL);
  leveldb_iter_seek_to_first(it);
  CHECK_EQ(1, leveldb_iter_valid(it));
  size_t klen = 0, vlen = 0;
  const char* k = leveldb_iter_key(it, &klen);
  const char* v = leveldb_iter_value(it, &vlen);
  CHECK_EQ(1, (long long)klen);
  CHECK_EQ('a', k[0]);
  CHECK_EQ(2, (long long)vlen);
  CHECK_EQ('v', v[0]);

  leveldb_iter_next(it);
  CHECK_EQ(1, leveldb_iter_valid(it));
  k = leveldb_iter_key(it, &klen);
  CHECK_EQ('b', k[0]);

  leveldb_iter_next(it);
  CHECK_EQ(1, leveldb_iter_valid(it));
  k = leveldb_iter_key(it, &klen);
  CHECK_EQ('c', k[0]);

  leveldb_iter_next(it);
  CHECK_EQ(0, leveldb_iter_valid(it));

  leveldb_iter_seek_to_last(it);
  CHECK_EQ(1, leveldb_iter_valid(it));
  k = leveldb_iter_key(it, &klen);
  CHECK_EQ('c', k[0]);

  leveldb_iter_prev(it);
  k = leveldb_iter_key(it, &klen);
  CHECK_EQ('b', k[0]);

  // Seek
  const char* seek_key = "b";
  leveldb_iter_seek(it, seek_key, 1);
  CHECK_EQ(1, leveldb_iter_valid(it));
  k = leveldb_iter_key(it, &klen);
  CHECK_EQ('b', k[0]);

  leveldb_iter_get_error(it, &err);
  CHECK(err == NULL);
  leveldb_iter_destroy(it);

  leveldb_writeoptions_destroy(wo);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, Snapshot) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_snapshot", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_put(db, wo, "k", 1, "1", 1, &err);
  CHECK(err == NULL);

  const leveldb_snapshot_t* snap = leveldb_create_snapshot(db);
  CHECK(snap != NULL);

  leveldb_put(db, wo, "k", 1, "2", 1, &err);
  CHECK(err == NULL);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  size_t vallen = 0;
  char* got = leveldb_get(db, ro, "k", 1, &vallen, &err);
  CHECK_STR_EQ("2", got);
  leveldb_free(got);
  err = NULL;

  leveldb_readoptions_set_snapshot(ro, snap);
  got = leveldb_get(db, ro, "k", 1, &vallen, &err);
  CHECK(got != NULL);
  CHECK_STR_EQ("1", got);
  leveldb_free(got);
  err = NULL;

  leveldb_readoptions_set_snapshot(ro, NULL);
  leveldb_release_snapshot(db, snap);
  leveldb_readoptions_destroy(ro);
  leveldb_writeoptions_destroy(wo);
  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, PropertyValue) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_property", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  char* val = leveldb_property_value(db, "leveldb.stats");
  CHECK(val != NULL);
  leveldb_free(val);
  val = leveldb_property_value(db, "leveldb.bogus");
  CHECK(val == NULL);

  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, CompactRange) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_compactrange", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  for (int i = 0; i < 100; i++) {
    char key[32], val[32];
    snprintf(key, sizeof(key), "key%06d", i);
    snprintf(val, sizeof(val), "value%06d", i);
    leveldb_put(db, wo, key, strlen(key), val, strlen(val), &err);
    CHECK(err == NULL);
  }
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_writeoptions_destroy(wo);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  size_t vallen = 0;
  char* got = leveldb_get(db, ro, "key000050", 9, &vallen, &err);
  CHECK(got != NULL);
  CHECK_STR_EQ("value000050", got);
  leveldb_free(got);
  leveldb_readoptions_destroy(ro);

  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, Versions) {
  CHECK_EQ(1, leveldb_major_version());
  CHECK_EQ(23, leveldb_minor_version());
}

TEST(c_api, BloomFilterOption) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_filterpolicy_t* policy = leveldb_filterpolicy_create_bloom(10);
  leveldb_options_set_filter_policy(options, policy);
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_bloom", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);
  CHECK(err == NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  for (int i = 0; i < 500; i++) {
    char key[32], val[32];
    snprintf(key, sizeof(key), "key%06d", i);
    snprintf(val, sizeof(val), "value%06d", i);
    leveldb_put(db, wo, key, strlen(key), val, strlen(val), &err);
    CHECK(err == NULL);
  }
  leveldb_writeoptions_destroy(wo);

  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  size_t vallen = 0;
  for (int i = 0; i < 500; i += 9) {
    char key[32], expected[32];
    snprintf(key, sizeof(key), "key%06d", i);
    snprintf(expected, sizeof(expected), "value%06d", i);
    char* got = leveldb_get(db, ro, key, strlen(key), &vallen, &err);
    CHECK(got != NULL);
    CHECK_STR_EQ(expected, got);
    leveldb_free(got);
  }
  leveldb_readoptions_destroy(ro);

  leveldb_close(db);
  leveldb_filterpolicy_destroy(policy);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}

TEST(c_api, ApproximateSizes) {
  leveldb_options_t* options = leveldb_options_create();
  leveldb_options_set_create_if_missing(options, 1);
  char path[300];
  ldb_test_make_db_path("c_api_approx", path, sizeof(path));
  char* err = NULL;
  leveldb_t* db = leveldb_open(options, path, &err);
  CHECK(db != NULL);

  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  for (int i = 0; i < 100; i++) {
    char key[32], val[64];
    snprintf(key, sizeof(key), "key%06d", i);
    memset(val, 'x', sizeof(val) - 1);
    val[sizeof(val) - 1] = '\0';
    leveldb_put(db, wo, key, strlen(key), val, sizeof(val) - 1, &err);
    CHECK(err == NULL);
  }
  leveldb_writeoptions_destroy(wo);

  const char* start_keys[1] = {"key000000"};
  const size_t start_lens[1] = {9};
  const char* limit_keys[1] = {"key999999"};
  const size_t limit_lens[1] = {9};
  uint64_t sizes[1] = {0};
  leveldb_approximate_sizes(db, 1, start_keys, start_lens, limit_keys,
                            limit_lens, sizes);
  CHECK((long long)sizes[0] >= 0);
  leveldb_close(db);
  leveldb_options_destroy(options);
  ldb_test_destroy_dir(path);
}
