// c_api.c - C bindings compatible with leveldb/c.h (mirrors leveldb/db/c.cc)
#include <stdlib.h>
#include <string.h>

#include "leveldb/c.h"
#include <assert.h>

#include "kvdb.h"

static char* copy_string(const char* src, size_t n) {
  char* out = (char*)malloc(n + 1);
  memcpy(out, src ? src : "", n);
  out[n] = '\0';
  return out;
}

static void save_error(char** errptr, const ldb_status* s) {
  if (errptr == NULL) return;
  if (ldb_ok(*s)) {
    return;
  }
  if (*errptr != NULL) {
    free(*errptr);
  }
  *errptr = ldb_status_to_string(*s);
}

// =================================================================== comparator wrapper
struct leveldb_comparator_t {
  void* state;
  void (*destructor)(void*);
  int (*compare)(void*, const char* a, size_t alen, const char* b,
                 size_t blen);
  const char* (*name)(void*);
  ldb_comparator base;  // adapter for the core library
};

static const char* cmpw_name(const ldb_comparator* c) {
  const leveldb_comparator_t* lc = (const leveldb_comparator_t*)c->impl;
  return lc->name(lc->state);
}

static int cmpw_compare(const ldb_comparator* c, const ldb_slice* a,
                        const ldb_slice* b) {
  const leveldb_comparator_t* lc = (const leveldb_comparator_t*)c->impl;
  return lc->compare(lc->state, a->data, a->size, b->data, b->size);
}

// The C API does not support key shortening; the no-op defaults apply.
static void cmpw_shortest(const ldb_comparator* c, ldb_buffer* start,
                          const ldb_slice* limit) {
  (void)c;
  (void)start;
  (void)limit;
}

static void cmpw_successor(const ldb_comparator* c, ldb_buffer* key) {
  (void)c;
  (void)key;
}

static const ldb_comparator* to_ldb_comparator(
    const leveldb_comparator_t* lc) {
  if (lc == NULL) return ldb_bytewise_comparator();
  return &lc->base;
}

leveldb_comparator_t* leveldb_comparator_create(
    void* state, void (*destructor)(void*),
    int (*compare)(void*, const char* a, size_t alen, const char* b,
                   size_t blen),
    const char* (*name)(void*)) {
  leveldb_comparator_t* result =
      (leveldb_comparator_t*)calloc(1, sizeof(leveldb_comparator_t));
  result->state = state;
  result->destructor = destructor;
  result->compare = compare;
  result->name = name;
  result->base.name = cmpw_name;
  result->base.compare = cmpw_compare;
  result->base.find_shortest_separator = cmpw_shortest;
  result->base.find_short_successor = cmpw_successor;
  result->base.impl = result;
  return result;
}

void leveldb_comparator_destroy(leveldb_comparator_t* cmp) {
  if (cmp->destructor) {
    cmp->destructor(cmp->state);
  }
  free(cmp);
}

// =================================================================== filter policy wrapper
struct leveldb_filterpolicy_t {
  void* state;
  void (*destructor)(void*);
  char* (*create_filter)(void*, const char* const* key_array,
                         const size_t* key_length_array, int num_keys,
                         size_t* filter_length);
  uint8_t (*key_may_match)(void*, const char* key, size_t length,
                           const char* filter, size_t filter_length);
  const char* (*name)(void*);
  ldb_filterpolicy base;  // adapter for the core library
};

static const char* fpw_name(const ldb_filterpolicy* p) {
  const leveldb_filterpolicy_t* lp = (const leveldb_filterpolicy_t*)p->impl;
  return lp->name(lp->state);
}

static void fpw_create(const ldb_filterpolicy* p, const ldb_slice* keys,
                       int n, ldb_buffer* dst) {
  const leveldb_filterpolicy_t* lp = (const leveldb_filterpolicy_t*)p->impl;
  const char** key_array =
      (const char**)malloc(sizeof(char*) * (size_t)(n > 0 ? n : 1));
  size_t* key_length_array =
      (size_t*)malloc(sizeof(size_t) * (size_t)(n > 0 ? n : 1));
  for (int i = 0; i < n; i++) {
    key_array[i] = keys[i].data;
    key_length_array[i] = keys[i].size;
  }
  size_t filter_length = 0;
  char* filter = lp->create_filter(lp->state, key_array, key_length_array, n,
                                   &filter_length);
  if (filter != NULL) {
    ldb_buffer_append(dst, filter, filter_length);
    free(filter);
  }
  free(key_array);
  free(key_length_array);
}

static int fpw_match(const ldb_filterpolicy* p, const ldb_slice* key,
                     const ldb_slice* filter) {
  const leveldb_filterpolicy_t* lp = (const leveldb_filterpolicy_t*)p->impl;
  return lp->key_may_match(lp->state, key->data, key->size, filter->data,
                           filter->size) != 0;
}

static const ldb_filterpolicy* to_ldb_filterpolicy(
    const leveldb_filterpolicy_t* lp) {
  if (lp == NULL) return NULL;
  return &lp->base;
}

leveldb_filterpolicy_t* leveldb_filterpolicy_create(
    void* state, void (*destructor)(void*),
    char* (*create_filter)(void*, const char* const* key_array,
                           const size_t* key_length_array, int num_keys,
                           size_t* filter_length),
    uint8_t (*key_may_match)(void*, const char* key, size_t length,
                             const char* filter, size_t filter_length),
    const char* (*name)(void*)) {
  leveldb_filterpolicy_t* result =
      (leveldb_filterpolicy_t*)calloc(1, sizeof(leveldb_filterpolicy_t));
  result->state = state;
  result->destructor = destructor;
  result->create_filter = create_filter;
  result->key_may_match = key_may_match;
  result->name = name;
  result->base.name = fpw_name;
  result->base.create_filter = fpw_create;
  result->base.key_may_match = fpw_match;
  result->base.destroy = NULL;
  result->base.impl = result;
  return result;
}

void leveldb_filterpolicy_destroy(leveldb_filterpolicy_t* filter) {
  if (filter->destructor) {
    filter->destructor(filter->state);
  }
  free(filter);
}

// Builtin bloom policy exposed through the C API: we delegate to the core
// bloom implementation via the same adapter paths, with the C-API function
// pointers backed by it.
typedef struct bloom_bridge {
  const ldb_filterpolicy* internal;
} bloom_bridge;

static const ldb_filterpolicy* bloom_internal(void* state) {
  bloom_bridge* b = (bloom_bridge*)state;
  return b->internal;
}

static char* bloom_create(void* state, const char* const* key_array,
                          const size_t* key_length_array, int num_keys,
                          size_t* filter_length) {
  const ldb_filterpolicy* internal = bloom_internal(state);
  ldb_slice* keys =
      (ldb_slice*)malloc(sizeof(ldb_slice) * (size_t)(num_keys > 0 ? num_keys : 1));
  for (int i = 0; i < num_keys; i++) {
    keys[i] = ldb_slice_make(key_array[i], key_length_array[i]);
  }
  ldb_buffer dst;
  ldb_buffer_init(&dst);
  internal->create_filter(internal, keys, num_keys, &dst);
  free(keys);
  *filter_length = dst.size;
  char* out = (char*)malloc(dst.size ? dst.size : 1);
  if (dst.size) memcpy(out, dst.data, dst.size);
  ldb_buffer_destroy(&dst);
  return out;
}

static uint8_t bloom_match(void* state, const char* key, size_t length,
                           const char* filter, size_t filter_length) {
  const ldb_filterpolicy* internal = bloom_internal(state);
  ldb_slice k = ldb_slice_make(key, length);
  ldb_slice f = ldb_slice_make(filter, filter_length);
  return (uint8_t)(internal->key_may_match(internal, &k, &f) != 0);
}

static const char* bloom_name(void* state) {
  const ldb_filterpolicy* internal = bloom_internal(state);
  return internal->name(internal);
}

static void bloom_bridge_destructor(void* state) {
  bloom_bridge* b = (bloom_bridge*)state;
  if (b->internal->destroy) {
    b->internal->destroy((ldb_filterpolicy*)b->internal);
  }
  free(b);
}

leveldb_filterpolicy_t* leveldb_filterpolicy_create_bloom(int bits_per_key) {
  // The returned wrapper is its own heap allocation; the bridge state is
  // allocated separately so that leveldb_filterpolicy_destroy can free it.
  bloom_bridge* b = (bloom_bridge*)malloc(sizeof(bloom_bridge));
  b->internal = ldb_new_bloom_filter_policy(bits_per_key);
  leveldb_filterpolicy_t* api =
      (leveldb_filterpolicy_t*)malloc(sizeof(leveldb_filterpolicy_t));
  api->state = b;
  api->destructor = bloom_bridge_destructor;
  api->create_filter = bloom_create;
  api->key_may_match = bloom_match;
  api->name = bloom_name;
  api->base.name = fpw_name;
  api->base.create_filter = fpw_create;
  api->base.key_may_match = fpw_match;
  api->base.destroy = NULL;
  api->base.impl = api;
  return api;
}

// =================================================================== cache / env / logger wrappers
struct leveldb_cache_t {
  ldb_cache* rep;
};

leveldb_cache_t* leveldb_cache_create_lru(size_t capacity) {
  leveldb_cache_t* c = (leveldb_cache_t*)malloc(sizeof(leveldb_cache_t));
  c->rep = ldb_cache_new_lru(capacity);
  return c;
}

void leveldb_cache_destroy(leveldb_cache_t* cache) {
  cache->rep->destroy(cache->rep);
  free(cache);
}

struct leveldb_logger_t {
  ldb_logger* rep;
};

struct leveldb_env_t {
  ldb_env* rep;
};

leveldb_env_t* leveldb_create_default_env(void) {
  leveldb_env_t* e = (leveldb_env_t*)malloc(sizeof(leveldb_env_t));
  e->rep = ldb_env_default();
  return e;
}

void leveldb_env_destroy(leveldb_env_t* env) { free(env); }

char* leveldb_env_get_test_directory(leveldb_env_t* env) {
  ldb_buffer path;
  ldb_buffer_init(&path);
  ldb_status s = ldb_env_get_test_directory(env->rep, &path);
  int ok = ldb_ok(s);
  ldb_status_destroy(&s);
  if (ok) {
    char* out = (char*)malloc(path.size + 1);
    memcpy(out, path.data ? path.data : "", path.size);
    out[path.size] = '\0';
    ldb_buffer_destroy(&path);
    return out;
  }
  ldb_buffer_destroy(&path);
  return NULL;
}

// =================================================================== options
struct leveldb_options_t {
  const leveldb_comparator_t* comparator;
  const leveldb_filterpolicy_t* filter_policy;
  const leveldb_cache_t* block_cache;
  const leveldb_env_t* env;
  leveldb_logger_t* info_log;
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
};

leveldb_options_t* leveldb_options_create(void) {
  leveldb_options_t* o =
      (leveldb_options_t*)calloc(1, sizeof(leveldb_options_t));
  o->compression = LDB_SNAPPY_COMPRESSION;
  o->write_buffer_size = 4 * 1024 * 1024;
  o->max_open_files = 1000;
  o->block_size = 4 * 1024;
  o->block_restart_interval = 16;
  o->max_file_size = 2 * 1024 * 1024;
  return o;
}

void leveldb_options_destroy(leveldb_options_t* options) { free(options); }

void leveldb_options_set_comparator(leveldb_options_t* opt,
                                    leveldb_comparator_t* cmp) {
  opt->comparator = cmp;
}
void leveldb_options_set_filter_policy(leveldb_options_t* opt,
                                       leveldb_filterpolicy_t* policy) {
  opt->filter_policy = policy;
}
void leveldb_options_set_create_if_missing(leveldb_options_t* opt, uint8_t v) {
  opt->create_if_missing = (int)v;
}
void leveldb_options_set_error_if_exists(leveldb_options_t* opt, uint8_t v) {
  opt->error_if_exists = (int)v;
}
void leveldb_options_set_paranoid_checks(leveldb_options_t* opt, uint8_t v) {
  opt->paranoid_checks = (int)v;
}
void leveldb_options_set_env(leveldb_options_t* opt, leveldb_env_t* env) {
  opt->env = env;
}
void leveldb_options_set_info_log(leveldb_options_t* opt,
                                  leveldb_logger_t* info_log) {
  opt->info_log = info_log;
}
void leveldb_options_set_write_buffer_size(leveldb_options_t* opt, size_t v) {
  opt->write_buffer_size = v;
}
void leveldb_options_set_max_open_files(leveldb_options_t* opt, int v) {
  opt->max_open_files = v;
}
void leveldb_options_set_cache(leveldb_options_t* opt, leveldb_cache_t* cache) {
  opt->block_cache = cache;
}
void leveldb_options_set_block_size(leveldb_options_t* opt, size_t v) {
  opt->block_size = v;
}
void leveldb_options_set_block_restart_interval(leveldb_options_t* opt,
                                                int v) {
  opt->block_restart_interval = v;
}
void leveldb_options_set_max_file_size(leveldb_options_t* opt, size_t v) {
  opt->max_file_size = v;
}
void leveldb_options_set_compression(leveldb_options_t* opt, int v) {
  opt->compression = v;
}

static void to_ldb_options(const leveldb_options_t* lopt,
                           ldb_options* opt) {
  ldb_options_init(opt);
  opt->comparator = to_ldb_comparator(lopt->comparator);
  opt->filter_policy = to_ldb_filterpolicy(lopt->filter_policy);
  opt->env = lopt->env ? lopt->env->rep : NULL;
  opt->info_log = lopt->info_log ? lopt->info_log->rep : NULL;
  opt->block_cache = lopt->block_cache ? lopt->block_cache->rep : NULL;
  opt->create_if_missing = lopt->create_if_missing;
  opt->error_if_exists = lopt->error_if_exists;
  opt->paranoid_checks = lopt->paranoid_checks;
  opt->reuse_logs = lopt->reuse_logs;
  opt->write_buffer_size = lopt->write_buffer_size;
  opt->max_open_files = lopt->max_open_files;
  opt->block_size = lopt->block_size;
  opt->block_restart_interval = lopt->block_restart_interval;
  opt->max_file_size = lopt->max_file_size;
  opt->compression = lopt->compression;
}

// =================================================================== read/write options
struct leveldb_readoptions_t {
  int verify_checksums;
  int fill_cache;
  const leveldb_snapshot_t* snapshot;
};

leveldb_readoptions_t* leveldb_readoptions_create(void) {
  leveldb_readoptions_t* r =
      (leveldb_readoptions_t*)calloc(1, sizeof(leveldb_readoptions_t));
  r->fill_cache = 1;
  return r;
}

void leveldb_readoptions_destroy(leveldb_readoptions_t* opt) { free(opt); }

void leveldb_readoptions_set_verify_checksums(leveldb_readoptions_t* opt,
                                              uint8_t v) {
  opt->verify_checksums = (int)v;
}

void leveldb_readoptions_set_fill_cache(leveldb_readoptions_t* opt, uint8_t v) {
  opt->fill_cache = (int)v;
}

void leveldb_readoptions_set_snapshot(leveldb_readoptions_t* opt,
                                      const leveldb_snapshot_t* snap) {
  opt->snapshot = snap;
}

struct leveldb_writeoptions_t {
  int sync;
};

leveldb_writeoptions_t* leveldb_writeoptions_create(void) {
  return (leveldb_writeoptions_t*)calloc(1, sizeof(leveldb_writeoptions_t));
}

void leveldb_writeoptions_destroy(leveldb_writeoptions_t* opt) { free(opt); }

void leveldb_writeoptions_set_sync(leveldb_writeoptions_t* opt, uint8_t v) {
  opt->sync = (int)v;
}

struct leveldb_snapshot_t {
  const ldb_snapshot_impl* rep;
};

static ldb_read_options to_ldb_read_options(
    const leveldb_readoptions_t* ropt) {
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  if (ropt != NULL) {
    ro.verify_checksums = ropt->verify_checksums;
    ro.fill_cache = ropt->fill_cache;
    ro.snapshot = ropt->snapshot ? ropt->snapshot->rep : NULL;
  }
  return ro;
}

static ldb_write_options to_ldb_write_options(
    const leveldb_writeoptions_t* wopt) {
  ldb_write_options wo;
  ldb_write_options_init(&wo);
  if (wopt != NULL) {
    wo.sync = wopt->sync;
  }
  return wo;
}

// =================================================================== db
struct leveldb_writebatch_t {
  ldb_write_batch rep;
};

struct leveldb_t {
  ldb_db_impl* rep;
  ldb_options opts;  // converted options copy used at open time
};

leveldb_t* leveldb_open(const leveldb_options_t* options, const char* name,
                        char** errptr) {
  leveldb_t* result = (leveldb_t*)calloc(1, sizeof(leveldb_t));
  to_ldb_options(options, &result->opts);
  ldb_db_impl* db = NULL;
  ldb_status s = ldb_db_open(&result->opts, name, &db);
  if (ldb_ok(s)) {
    result->rep = db;
    ldb_status_destroy(&s);
  } else {
    save_error(errptr, &s);
    ldb_status_destroy(&s);
    free(result);
    result = NULL;
  }
  return result;
}

void leveldb_close(leveldb_t* db) {
  ldb_db_impl_destroy(db->rep);
  free(db);
}

void leveldb_put(leveldb_t* db, const leveldb_writeoptions_t* options,
                 const char* key, size_t keylen, const char* val,
                 size_t vallen, char** errptr) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_slice k = ldb_slice_make(key, keylen);
  ldb_slice v = ldb_slice_make(val, vallen);
  ldb_write_batch_put(&batch, &k, &v);
  ldb_write_options wo = to_ldb_write_options(options);
  ldb_status s = ldb_db_impl_write(db->rep, &wo, &batch);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
  ldb_write_batch_destroy(&batch);
}

void leveldb_delete(leveldb_t* db, const leveldb_writeoptions_t* options,
                    const char* key, size_t keylen, char** errptr) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_slice k = ldb_slice_make(key, keylen);
  ldb_write_batch_delete(&batch, &k);
  ldb_write_options wo = to_ldb_write_options(options);
  ldb_status s = ldb_db_impl_write(db->rep, &wo, &batch);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
  ldb_write_batch_destroy(&batch);
}

void leveldb_write(leveldb_t* db, const leveldb_writeoptions_t* options,
                   leveldb_writebatch_t* batch, char** errptr) {
  ldb_write_options wo = to_ldb_write_options(options);
  ldb_status s = ldb_db_impl_write(db->rep, &wo, &batch->rep);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
}

char* leveldb_get(leveldb_t* db, const leveldb_readoptions_t* options,
                  const char* key, size_t keylen, size_t* vallen,
                  char** errptr) {
  char* result = NULL;
  ldb_buffer tmp;
  ldb_buffer_init(&tmp);
  ldb_read_options ro = to_ldb_read_options(options);
  ldb_slice k = ldb_slice_make(key, keylen);
  ldb_status s = ldb_db_impl_get(db->rep, &ro, &k, &tmp);
  if (ldb_ok(s)) {
    result = copy_string(tmp.data ? tmp.data : "", tmp.size);
    if (vallen != NULL) {
      *vallen = tmp.size;
    }
  } else {
    if (vallen != NULL) {
      *vallen = 0;
    }
    if (s.code != LDB_NOTFOUND && errptr != NULL) {
      save_error(errptr, &s);
    }
  }
  ldb_status_destroy(&s);
  ldb_buffer_destroy(&tmp);
  return result;
}

// =================================================================-- iterator
struct leveldb_iterator_t {
  ldb_iterator* rep;
};

leveldb_iterator_t* leveldb_create_iterator(leveldb_t* db,
                                            const leveldb_readoptions_t* options) {
  leveldb_iterator_t* result =
      (leveldb_iterator_t*)malloc(sizeof(leveldb_iterator_t));
  ldb_read_options ro = to_ldb_read_options(options);
  result->rep = ldb_db_impl_new_iterator(db->rep, &ro);
  return result;
}

void leveldb_iter_destroy(leveldb_iterator_t* iter) {
  ldb_iterator_destroy(iter->rep);
  free(iter);
}

uint8_t leveldb_iter_valid(const leveldb_iterator_t* iter) {
  return (uint8_t)ldb_iter_valid(iter->rep);
}

void leveldb_iter_seek_to_first(leveldb_iterator_t* iter) {
  ldb_iter_seek_to_first(iter->rep);
}

void leveldb_iter_seek_to_last(leveldb_iterator_t* iter) {
  ldb_iter_seek_to_last(iter->rep);
}

void leveldb_iter_seek(leveldb_iterator_t* iter, const char* k, size_t klen) {
  ldb_slice target = ldb_slice_make(k, klen);
  ldb_iter_seek(iter->rep, &target);
}

void leveldb_iter_next(leveldb_iterator_t* iter) { ldb_iter_next(iter->rep); }

void leveldb_iter_prev(leveldb_iterator_t* iter) { ldb_iter_prev(iter->rep); }

const char* leveldb_iter_key(const leveldb_iterator_t* iter, size_t* klen) {
  ldb_slice s = ldb_iter_key(iter->rep);
  *klen = s.size;
  return s.data;
}

const char* leveldb_iter_value(const leveldb_iterator_t* iter, size_t* vlen) {
  ldb_slice s = ldb_iter_value(iter->rep);
  *vlen = s.size;
  return s.data;
}

void leveldb_iter_get_error(const leveldb_iterator_t* iter, char** errptr) {
  ldb_status s = ldb_iter_status(iter->rep);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
}

// =================================================================== snapshot
const leveldb_snapshot_t* leveldb_create_snapshot(leveldb_t* db) {
  leveldb_snapshot_t* s =
      (leveldb_snapshot_t*)malloc(sizeof(leveldb_snapshot_t));
  s->rep = ldb_db_impl_get_snapshot(db->rep);
  return s;
}

void leveldb_release_snapshot(leveldb_t* db,
                              const leveldb_snapshot_t* snapshot) {
  ldb_db_impl_release_snapshot(db->rep, snapshot->rep);
  free((void*)snapshot);
}

// =================================================================== writebatch
static void wbi_put(void* state, const ldb_slice* k, const ldb_slice* v);
static void wbi_deleted(void* state, const ldb_slice* k);

leveldb_writebatch_t* leveldb_writebatch_create(void) {
  leveldb_writebatch_t* b =
      (leveldb_writebatch_t*)malloc(sizeof(leveldb_writebatch_t));
  ldb_write_batch_init(&b->rep);
  return b;
}

void leveldb_writebatch_destroy(leveldb_writebatch_t* b) {
  ldb_write_batch_destroy(&b->rep);
  free(b);
}

void leveldb_writebatch_clear(leveldb_writebatch_t* b) {
  ldb_write_batch_clear(&b->rep);
}

void leveldb_writebatch_put(leveldb_writebatch_t* b, const char* key,
                            size_t klen, const char* val, size_t vlen) {
  ldb_slice k = ldb_slice_make(key, klen);
  ldb_slice v = ldb_slice_make(val, vlen);
  ldb_write_batch_put(&b->rep, &k, &v);
}

void leveldb_writebatch_delete(leveldb_writebatch_t* b, const char* key,
                               size_t klen) {
  ldb_slice k = ldb_slice_make(key, klen);
  ldb_write_batch_delete(&b->rep, &k);
}

void leveldb_writebatch_iterate(
    const leveldb_writebatch_t* b, void* state,
    void (*put)(void*, const char* k, size_t klen, const char* v, size_t vlen),
    void (*deleted)(void*, const char* k, size_t klen)) {
  // adapter around the ldb-style callbacks
  typedef struct cb {
    void* state;
    void (*put)(void*, const char*, size_t, const char*, size_t);
    void (*deleted)(void*, const char*, size_t);
  } cb;
  cb c;
  c.state = state;
  c.put = put;
  c.deleted = deleted;
  ldb_write_batch_iterate(&b->rep, &c, wbi_put, wbi_deleted);
}

static void wbi_put(void* state, const ldb_slice* k, const ldb_slice* v) {
  typedef struct cb {
    void* state;
    void (*put)(void*, const char*, size_t, const char*, size_t);
    void (*deleted)(void*, const char*, size_t);
  } cb;
  cb* c = (cb*)state;
  if (c->put) c->put(c->state, k->data, k->size, v->data, v->size);
}

static void wbi_deleted(void* state, const ldb_slice* k) {
  typedef struct cb {
    void* state;
    void (*put)(void*, const char*, size_t, const char*, size_t);
    void (*deleted)(void*, const char*, size_t);
  } cb;
  cb* c = (cb*)state;
  if (c->deleted) c->deleted(c->state, k->data, k->size);
}

void leveldb_writebatch_append(leveldb_writebatch_t* destination,
                               const leveldb_writebatch_t* source) {
  ldb_write_batch_append(&destination->rep, &source->rep);
}

// =================================================================== misc
char* leveldb_property_value(leveldb_t* db, const char* propname) {
  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_slice prop = ldb_slice_str(propname);
  if (!ldb_db_impl_get_property(db->rep, &prop, &value)) {
    ldb_buffer_destroy(&value);
    return NULL;
  }
  char* out = copy_string(value.data ? value.data : "", value.size);
  ldb_buffer_destroy(&value);
  return out;
}

void leveldb_approximate_sizes(
    leveldb_t* db, int num_ranges, const char* const* range_start_key,
    const size_t* range_start_key_len, const char* const* range_limit_key,
    const size_t* range_limit_key_len, uint64_t* sizes) {
  for (int i = 0; i < num_ranges; i++) {
    ldb_slice start = range_start_key[i]
                          ? ldb_slice_make(range_start_key[i], range_start_key_len[i])
                          : ldb_slice_make("", 0);
    ldb_slice limit = range_limit_key[i]
                          ? ldb_slice_make(range_limit_key[i], range_limit_key_len[i])
                          : ldb_slice_make("", 0);
    // compute one range via the core helper
    ldb_db_impl_get_approximate_sizes(db->rep, &start, &limit, 1, &sizes[i]);
  }
}

void leveldb_compact_range(leveldb_t* db, const char* start_key,
                           size_t start_key_len, const char* limit_key,
                           size_t limit_key_len) {
  ldb_slice a, b;
  const ldb_slice* pa = NULL;
  const ldb_slice* pb = NULL;
  if (start_key != NULL) {
    a = ldb_slice_make(start_key, start_key_len);
    pa = &a;
  }
  if (limit_key != NULL) {
    b = ldb_slice_make(limit_key, limit_key_len);
    pb = &b;
  }
  ldb_db_impl_compact_range(db->rep, pa, pb);
}

void leveldb_destroy_db(const leveldb_options_t* options, const char* name,
                        char** errptr) {
  ldb_options opt;
  to_ldb_options(options, &opt);
  ldb_status s = ldb_destroy_db(&opt, name);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
}

void leveldb_repair_db(const leveldb_options_t* options, const char* name,
                       char** errptr) {
  ldb_options opt;
  to_ldb_options(options, &opt);
  ldb_status s = ldb_repair_db(&opt, name);
  save_error(errptr, &s);
  ldb_status_destroy(&s);
}

void leveldb_free(void* ptr) { free(ptr); }

int leveldb_major_version(void) { return 1; }

int leveldb_minor_version(void) { return 23; }
