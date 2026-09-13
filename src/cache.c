// cache.c - sharded LRU cache (mirrors leveldb/util/cache.cc exactly)
#include "kvdb.h"

#include <assert.h>

#define LDB_CACHE_NUM_SHARD_BITS 4
#define LDB_CACHE_NUM_SHARDS (1 << LDB_CACHE_NUM_SHARD_BITS)

typedef struct ldb_lru_handle {
  void* value;
  void (*deleter)(const ldb_slice*, void* value);
  struct ldb_lru_handle* next_hash;
  struct ldb_lru_handle* next;
  struct ldb_lru_handle* prev;
  size_t charge;
  size_t key_length;
  int in_cache;
  uint32_t refs;
  uint32_t hash;
  char key_data[1];  // beginning of key
} ldb_lru_handle;

static ldb_slice lru_key(const ldb_lru_handle* h) {
  return ldb_slice_make(h->key_data, h->key_length);
}

// ------------------------------------------------------------------ handle table
typedef struct ldb_handle_table {
  uint32_t length;
  uint32_t elems;
  ldb_lru_handle** list;
} ldb_handle_table;

static uint32_t hash_slice(const ldb_slice* s) {
  return ldb_hash(s->data, s->size, 0);
}

static void handle_table_init(ldb_handle_table* t) {
  t->length = 4;
  t->elems = 0;
  t->list = (ldb_lru_handle**)malloc(sizeof(ldb_lru_handle*) * t->length);
  assert(t->list);
  memset(t->list, 0, sizeof(ldb_lru_handle*) * t->length);
}

static void handle_table_destroy(ldb_handle_table* t) {
  free(t->list);
  t->list = NULL;
  t->length = 0;
  t->elems = 0;
}

static ldb_lru_handle** handle_table_find_pointer(const ldb_handle_table* t,
                                                  const ldb_slice* key,
                                                  uint32_t hash) {
  ldb_lru_handle** ptr = &t->list[hash & (t->length - 1)];
  while (*ptr != NULL) {
    ldb_slice k = lru_key(*ptr);
    if ((*ptr)->hash == hash && ldb_slice_equals(&k, key)) break;
    ptr = &(*ptr)->next_hash;
  }
  return ptr;
}

static ldb_lru_handle* handle_table_lookup(const ldb_handle_table* t,
                                           const ldb_slice* key,
                                           uint32_t hash) {
  return *handle_table_find_pointer(t, key, hash);
}

static void handle_table_resize(ldb_handle_table* t) {
  uint32_t new_length = 4;
  while (new_length < t->elems) {
    new_length *= 2;
  }
  ldb_lru_handle** new_list =
      (ldb_lru_handle**)malloc(sizeof(ldb_lru_handle*) * new_length);
  assert(new_list);
  memset(new_list, 0, sizeof(ldb_lru_handle*) * new_length);
  uint32_t count = 0;
  for (uint32_t i = 0; i < t->length; i++) {
    ldb_lru_handle* h = t->list[i];
    while (h != NULL) {
      ldb_lru_handle* next = h->next_hash;
      uint32_t bucket = h->hash & (new_length - 1);
      h->next_hash = new_list[bucket];
      new_list[bucket] = h;
      h = next;
      count++;
    }
  }
  assert(t->elems == count);
  free(t->list);
  t->list = new_list;
  t->length = new_length;
}

static ldb_lru_handle* handle_table_insert(ldb_handle_table* t,
                                           ldb_lru_handle* h) {
  ldb_slice hk = lru_key(h);
  ldb_lru_handle** ptr = handle_table_find_pointer(t, &hk, h->hash);
  ldb_lru_handle* old = *ptr;
  h->next_hash = (old == NULL) ? NULL : old->next_hash;
  *ptr = h;
  if (old == NULL) {
    ++t->elems;
    if (t->elems > t->length) {
      handle_table_resize(t);
    }
  }
  return old;
}

static ldb_lru_handle* handle_table_remove(ldb_handle_table* t,
                                           const ldb_slice* key,
                                           uint32_t hash) {
  ldb_lru_handle** ptr = handle_table_find_pointer(t, key, hash);
  ldb_lru_handle* result = *ptr;
  if (result != NULL) {
    *ptr = result->next_hash;
    --t->elems;
  }
  return result;
}

// ------------------------------------------------------------------ per-shard LRU
typedef struct ldb_lru_shard {
  ldb_mutex mu;
  size_t capacity;
  size_t usage;
  ldb_lru_handle lru;     // lru.prev is newest; lru.next is oldest.
                          // Entries have refs==1 and in_cache==1.
  ldb_lru_handle in_use;  // entries with refs >= 2 and in_cache == 1
  ldb_handle_table table;
} ldb_lru_shard;

static void lru_list_init(ldb_lru_handle* head) {
  head->next = head;
  head->prev = head;
}
static void lru_list_remove(ldb_lru_handle* e) {
  e->next->prev = e->prev;
  e->prev->next = e->next;
  e->next = e->prev = e;
}
static void lru_list_append(ldb_lru_handle* list, ldb_lru_handle* e) {
  // Make "e" newest entry by inserting just before *list
  e->next = list;
  e->prev = list->prev;
  e->prev->next = e;
  e->next->prev = e;
}

static void lru_ref(ldb_lru_shard* c, ldb_lru_handle* e) {
  if (e->refs == 1 && e->in_cache) {  // If on lru_ list, move to in_use_ list.
    lru_list_remove(e);
    lru_list_append(&c->in_use, e);
  }
  e->refs++;
}

static void lru_unref(ldb_lru_shard* c, ldb_lru_handle* e) {
  assert(e->refs > 0);
  e->refs--;
  if (e->refs == 0) {  // Deallocate.
    assert(!e->in_cache);
    ldb_slice k = lru_key(e);
    if (e->deleter) e->deleter(&k, e->value);
    free(e);
  } else if (e->in_cache && e->refs == 1) {
    // No longer in use; move to lru_ list.
    lru_list_remove(e);
    lru_list_append(&c->lru, e);
  }
}

// If e != NULL, finish removing *e from the cache; it has already been
// removed from the hash table.
static int lru_finish_erase(ldb_lru_shard* c, ldb_lru_handle* e) {
  if (e != NULL) {
    assert(e->in_cache);
    lru_list_remove(e);
    e->in_cache = 0;
    c->usage -= e->charge;
    lru_unref(c, e);
  }
  return e != NULL;
}

// ------------------------------------------------------------------ shard methods
typedef struct lru_shard_wrap {
  ldb_cache base;
  ldb_lru_shard shards[LDB_CACHE_NUM_SHARDS];
} lru_shard_wrap;

static ldb_cache_handle* lru_insert(ldb_cache* cache, const ldb_slice* key,
                                    void* value, size_t charge,
                                    void (*deleter)(const ldb_slice*, void*)) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  uint32_t hash = hash_slice(key);
  ldb_lru_shard* c = &w->shards[hash >> (32 - LDB_CACHE_NUM_SHARD_BITS)];
  ldb_mutex_lock(&c->mu);

  ldb_lru_handle* e =
      (ldb_lru_handle*)malloc(sizeof(ldb_lru_handle) - 1 + key->size);
  assert(e);
  e->value = value;
  e->deleter = deleter;
  e->charge = charge;
  e->key_length = key->size;
  e->hash = hash;
  e->in_cache = 0;
  e->refs = 1;  // for the returned handle.
  memcpy(e->key_data, key->data, key->size);

  if (c->capacity > 0) {
    e->refs++;  // for the cache's reference.
    e->in_cache = 1;
    lru_list_append(&c->in_use, e);
    c->usage += charge;
    ldb_lru_handle* old = handle_table_insert(&c->table, e);
    lru_finish_erase(c, old);
  } else {  // don't cache. (capacity_==0 is supported and turns off caching.)
    e->next = NULL;
  }
  while (c->usage > c->capacity && c->lru.next != &c->lru) {
    ldb_lru_handle* old = c->lru.next;
    assert(old->refs == 1);
    ldb_slice oldk = lru_key(old);
    ldb_lru_handle* removed =
        handle_table_remove(&c->table, &oldk, old->hash);
    lru_finish_erase(c, removed);
  }

  ldb_mutex_unlock(&c->mu);
  return (ldb_cache_handle*)e;
}

static ldb_cache_handle* lru_lookup(ldb_cache* cache, const ldb_slice* key) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  uint32_t hash = hash_slice(key);
  ldb_lru_shard* c = &w->shards[hash >> (32 - LDB_CACHE_NUM_SHARD_BITS)];
  ldb_mutex_lock(&c->mu);
  ldb_lru_handle* e = handle_table_lookup(&c->table, key, hash);
  if (e != NULL) {
    lru_ref(c, e);
  }
  ldb_mutex_unlock(&c->mu);
  return (ldb_cache_handle*)e;
}

static void lru_release(ldb_cache* cache, ldb_cache_handle* handle) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  ldb_lru_handle* e = (ldb_lru_handle*)handle;
  uint32_t hash = e->hash;
  ldb_lru_shard* c = &w->shards[hash >> (32 - LDB_CACHE_NUM_SHARD_BITS)];
  ldb_mutex_lock(&c->mu);
  lru_unref(c, e);
  ldb_mutex_unlock(&c->mu);
}

static void* lru_value(ldb_cache* cache, ldb_cache_handle* handle) {
  (void)cache;
  return ((ldb_lru_handle*)handle)->value;
}

static void lru_erase(ldb_cache* cache, const ldb_slice* key) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  uint32_t hash = hash_slice(key);
  ldb_lru_shard* c = &w->shards[hash >> (32 - LDB_CACHE_NUM_SHARD_BITS)];
  ldb_mutex_lock(&c->mu);
  ldb_lru_handle* e = handle_table_remove(&c->table, key, hash);
  lru_finish_erase(c, e);
  ldb_mutex_unlock(&c->mu);
}

static uint64_t lru_last_id = 0;

static uint64_t lru_new_id(ldb_cache* cache) {
  (void)cache;
  return ++lru_last_id;
}

static void lru_prune(ldb_cache* cache) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  for (int i = 0; i < LDB_CACHE_NUM_SHARDS; i++) {
    ldb_lru_shard* c = &w->shards[i];
    ldb_mutex_lock(&c->mu);
    while (c->lru.next != &c->lru) {
      ldb_lru_handle* e = c->lru.next;
      assert(e->refs == 1);
      ldb_slice ek = lru_key(e);
      ldb_lru_handle* removed =
          handle_table_remove(&c->table, &ek, e->hash);
      lru_finish_erase(c, removed);
    }
    ldb_mutex_unlock(&c->mu);
  }
}

static size_t lru_total_charge(ldb_cache* cache) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  size_t total = 0;
  for (int i = 0; i < LDB_CACHE_NUM_SHARDS; i++) {
    ldb_mutex_lock(&w->shards[i].mu);
    total += w->shards[i].usage;
    ldb_mutex_unlock(&w->shards[i].mu);
  }
  return total;
}

static void lru_destroy(ldb_cache* cache) {
  lru_shard_wrap* w = (lru_shard_wrap*)cache;
  for (int i = 0; i < LDB_CACHE_NUM_SHARDS; i++) {
    ldb_lru_shard* c = &w->shards[i];
    assert(c->in_use.next == &c->in_use);  // Error if caller has unreleased handle
    for (ldb_lru_handle* e = c->lru.next; e != &c->lru;) {
      ldb_lru_handle* next = e->next;
      assert(e->in_cache);
      e->in_cache = 0;
      assert(e->refs == 1);
      lru_unref(c, e);
      e = next;
    }
    ldb_mutex_destroy(&c->mu);
    handle_table_destroy(&c->table);
  }
  free(w);
}

ldb_cache* ldb_cache_new_lru(size_t capacity) {
  lru_shard_wrap* w = (lru_shard_wrap*)malloc(sizeof(lru_shard_wrap));
  assert(w);
  w->base.insert = lru_insert;
  w->base.lookup = lru_lookup;
  w->base.release = lru_release;
  w->base.value = lru_value;
  w->base.erase = lru_erase;
  w->base.new_id = lru_new_id;
  w->base.prune = lru_prune;
  w->base.total_charge = lru_total_charge;
  w->base.destroy = lru_destroy;
  size_t per_shard =
      (capacity + LDB_CACHE_NUM_SHARDS - 1) / LDB_CACHE_NUM_SHARDS;
  for (int i = 0; i < LDB_CACHE_NUM_SHARDS; i++) {
    ldb_lru_shard* c = &w->shards[i];
    ldb_mutex_init(&c->mu);
    c->capacity = per_shard;
    c->usage = 0;
    lru_list_init(&c->lru);
    lru_list_init(&c->in_use);
    handle_table_init(&c->table);
  }
  return &w->base;
}
