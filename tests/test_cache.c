// test_cache.c - port of leveldb's cache_test (core scenarios)
#include "harness.h"

// Callback counters
static int g_deletes = 0;

static void cache_test_deleter(const ldb_slice* key, void* value) {
  (void)key;
  (void)value;
  g_deletes++;
}

static ldb_slice cache_key(int k) {
  char buf[32];
  snprintf(buf, sizeof(buf), "key%d", k);
  return ldb_slice_str(buf);
}

// Note: keys passed to insert must outlive the handle; use static storage.
static char g_key_storage[512][32];
static void cache_key_into(int k, ldb_slice* out) {
  snprintf(g_key_storage[k % 512], 32, "key%d", k);
  *out = ldb_slice_str(g_key_storage[k % 512]);
}

TEST(cache, HitAndMiss) {
  ldb_cache* c = ldb_cache_new_lru(100);
  ldb_slice key;
  cache_key_into(100, &key);
  ldb_cache_handle* h = c->insert(c, &key, (void*)"v100", 1, cache_test_deleter);
  CHECK(h != NULL);
  CHECK_STR_EQ("v100", (const char*)c->value(c, h));
  c->release(c, h);

  h = c->lookup(c, &key);
  CHECK(h != NULL);
  CHECK_STR_EQ("v100", (const char*)c->value(c, h));
  c->release(c, h);

  ldb_slice missing;
  cache_key_into(200, &missing);
  CHECK(c->lookup(c, &missing) == NULL);
  c->destroy(c);
}

TEST(cache, EmptyInsert) {
  ldb_cache* c = ldb_cache_new_lru(0);  // caching disabled
  ldb_slice key;
  cache_key_into(100, &key);
  ldb_cache_handle* h = c->insert(c, &key, (void*)"v100", 1, cache_test_deleter);
  CHECK(h != NULL);
  c->release(c, h);
  CHECK(c->lookup(c, &key) == NULL);
  c->destroy(c);
}

TEST(cache, Erase) {
  ldb_cache* c = ldb_cache_new_lru(100);
  ldb_slice key;
  cache_key_into(100, &key);
  g_deletes = 0;
  ldb_cache_handle* h =
      c->insert(c, &key, (void*)"v100", 1, cache_test_deleter);
  c->release(c, h);  // only the cache reference remains
  c->erase(c, &key);  // drops the cache reference and runs the deleter
  CHECK(c->lookup(c, &key) == NULL);
  c->destroy(c);
  CHECK_EQ(1, g_deletes);
}

TEST(cache, Eviction) {
  // Like leveldb's cache_test: capacity 100, insert 1000, the first 100
  // must be gone in every shard.
  ldb_cache* c = ldb_cache_new_lru(100);
  ldb_slice key;
  for (int i = 0; i < 1000; i++) {
    cache_key_into(i, &key);
    ldb_cache_handle* h =
        c->insert(c, &key, (void*)(size_t)(i + 1000), 1, cache_test_deleter);
    c->release(c, h);
  }
  for (int i = 0; i < 100; i++) {
    cache_key_into(i, &key);
    CHECK(c->lookup(c, &key) == NULL);
  }
  c->destroy(c);
}

TEST(cache, Prune) {
  ldb_cache* c = ldb_cache_new_lru(100);
  ldb_slice key;
  for (int i = 0; i < 5; i++) {
    cache_key_into(i, &key);
    ldb_cache_handle* h =
        c->insert(c, &key, (void*)(size_t)(i + 1000), 1, cache_test_deleter);
    c->release(c, h);
  }
  c->prune(c);
  for (int i = 0; i < 5; i++) {
    cache_key_into(i, &key);
    CHECK(c->lookup(c, &key) == NULL);
  }
  c->destroy(c);
}

TEST(cache, ZeroSizeCacheOffered) {
  ldb_cache* c = ldb_cache_new_lru(0);
  ldb_slice key;
  cache_key_into(1, &key);
  ldb_cache_handle* h = c->insert(c, &key, (void*)"v", 1, cache_test_deleter);
  c->release(c, h);
  CHECK(c->lookup(c, &key) == NULL);
  c->destroy(c);
}

TEST(cache, NewId) {
  ldb_cache* c = ldb_cache_new_lru(100);
  uint64_t a = c->new_id(c);
  uint64_t b = c->new_id(c);
  CHECK_EQ(1, (long long)(b - a));
  c->destroy(c);
  ldb_cache* d = ldb_cache_new_lru(100);
  uint64_t e = d->new_id(d);
  CHECK_EQ(1, (long long)(e - b));
  d->destroy(d);
}

TEST(cache, TotalCharge) {
  ldb_cache* c = ldb_cache_new_lru(1000);
  ldb_slice key;
  cache_key_into(1, &key);
  ldb_cache_handle* h1 = c->insert(c, &key, (void*)"v", 100, cache_test_deleter);
  c->release(c, h1);
  cache_key_into(2, &key);
  ldb_cache_handle* h2 = c->insert(c, &key, (void*)"v", 200, cache_test_deleter);
  c->release(c, h2);
  CHECK_EQ(300, (long long)c->total_charge(c));
  c->destroy(c);
}
