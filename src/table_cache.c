// table_cache.c - open sstable cache and BuildTable helper
// (mirrors leveldb/db/table_cache.cc and db/builder.cc)
#include "kvdb.h"

#include <assert.h>

typedef struct table_and_file {
  ldb_rand_file* file;
  ldb_table* table;
} table_and_file;

static void tc_delete_entry(const ldb_slice* key, void* value) {
  (void)key;
  table_and_file* tf = (table_and_file*)value;
  tf->file->m->destroy(tf->file);
  ldb_table_destroy(tf->table);
  free(tf);
}

static void tc_unref_entry(void* arg1, void* arg2) {
  ldb_cache* cache = (ldb_cache*)arg1;
  ldb_cache_handle* h = (ldb_cache_handle*)arg2;
  cache->release(cache, h);
}

ldb_table_cache* ldb_table_cache_new(const char* dbname,
                                     const ldb_options* options,
                                     size_t entries) {
  ldb_table_cache* tc = (ldb_table_cache*)malloc(sizeof(ldb_table_cache));
  tc->env = options->env;
  tc->dbname = strdup(dbname);
  tc->options = options;
  tc->cache = ldb_cache_new_lru(entries);
  tc->icmp = NULL;  // set by caller if needed
  tc->last_id = 1;
  return tc;
}

void ldb_table_cache_destroy(ldb_table_cache* tc) {
  tc->cache->destroy(tc->cache);
  free(tc->dbname);
  free(tc);
}

static ldb_status tc_find_table(ldb_table_cache* tc, uint64_t file_number,
                                uint64_t file_size,
                                ldb_cache_handle** handle) {
  ldb_status s;
  char buf[16];
  ldb_encode_fixed64(buf, file_number);
  ldb_slice key = ldb_slice_make(buf, sizeof(buf));
  *handle = tc->cache->lookup(tc->cache, &key);
  if (*handle == NULL) {
    char* fname = ldb_table_file_name(tc->dbname, file_number);
    ldb_rand_file* file = NULL;
    ldb_table* table = NULL;
    s = ldb_env_new_random_access_file(tc->env, fname, &file);
    if (!ldb_ok(s)) {
      // Maybe the file was in an old format; try .sst
      char* old_fname = ldb_sst_table_file_name(tc->dbname, file_number);
      ldb_status s2 = ldb_env_new_random_access_file(tc->env, old_fname, &file);
      if (ldb_ok(s2)) {
        ldb_status_destroy(&s);
        s = s2;
        free(old_fname);
      } else {
        ldb_status_destroy(&s2);
        free(fname);
        free(old_fname);
        return s;  // original error
      }
    }
    free(fname);
    s = ldb_table_open(tc->options, file, file_size, &table);
    if (!ldb_ok(s)) {
      file->m->destroy(file);
      // We do not cache error tables so that they can get corrected later
      return s;
    }
    table_and_file* tf = (table_and_file*)malloc(sizeof(table_and_file));
    tf->file = file;
    tf->table = table;
    *handle = tc->cache->insert(tc->cache, &key, tf, 1, tc_delete_entry);
  }
  return ldb_status_ok();
}

ldb_iterator* ldb_table_cache_new_iterator(ldb_table_cache* tc,
                                           const ldb_read_options* options,
                                           uint64_t file_number,
                                           uint64_t file_size) {
  ldb_cache_handle* handle = NULL;
  ldb_status s = tc_find_table(tc, file_number, file_size, &handle);
  if (!ldb_ok(s)) {
    return ldb_new_error_iterator(s);
  }
  ldb_cache* cache = tc->cache;
  ldb_table* t = ((table_and_file*)cache->value(cache, handle))->table;
  ldb_iterator* iter = ldb_table_new_iterator(t, options);
  ldb_iterator_register_cleanup(iter, tc_unref_entry, cache, handle);
  return iter;
}

ldb_status ldb_table_cache_get(ldb_table_cache* tc,
                               const ldb_read_options* options,
                               uint64_t file_number, uint64_t file_size,
                               const ldb_slice* k, void* arg,
                               void (*handle_result)(void*, const ldb_slice*,
                                                     const ldb_slice*),
                               ldb_table** tableptr) {
  ldb_status s;
  ldb_cache_handle* handle = NULL;
  if (tableptr != NULL) {
    *tableptr = NULL;
  }
  s = tc_find_table(tc, file_number, file_size, &handle);
  if (!ldb_ok(s)) {
    return s;
  }
  ldb_cache* cache = tc->cache;
  ldb_table* t = ((table_and_file*)cache->value(cache, handle))->table;
  if (tableptr != NULL) {
    *tableptr = t;
  }
  if (k != NULL) {
    s = ldb_table_internal_get(t, options, k, arg, handle_result);
  }
  cache->release(cache, handle);
  return s;
}

void ldb_table_cache_evict(ldb_table_cache* tc, uint64_t file_number) {
  char buf[16];
  ldb_encode_fixed64(buf, file_number);
  ldb_slice key = ldb_slice_make(buf, sizeof(buf));
  tc->cache->erase(tc->cache, &key);
}

// =================================================================== BuildTable
ldb_status ldb_build_table(const char* dbname, ldb_env* env,
                           const ldb_options* options, ldb_table_cache* tc,
                           ldb_iterator* iter, ldb_file_meta* meta) {
  meta->file_size = 0;
  ldb_iter_seek_to_first(iter);

  char* fname = ldb_table_file_name(dbname, meta->number);
  ldb_status s = ldb_status_ok();
  if (ldb_iter_valid(iter)) {
    ldb_writable_file* file;
    s = ldb_env_new_writable_file(env, fname, &file);
    if (!ldb_ok(s)) {
      free(fname);
      return s;
    }
    ldb_table_builder* builder = ldb_table_builder_new(options, file);
    ldb_slice first_key = ldb_iter_key(iter);
    ldb_buffer_clear(&meta->smallest);
    ldb_buffer_append_slice(&meta->smallest, &first_key);
    for (; ldb_iter_valid(iter); ldb_iter_next(iter)) {
      ldb_slice key = ldb_iter_key(iter);
      ldb_slice value = ldb_iter_value(iter);
      ldb_buffer_clear(&meta->largest);
      ldb_buffer_append_slice(&meta->largest, &key);
      ldb_table_builder_add(builder, &key, &value);
    }
    // Finish and check for builder errors
    ldb_status fs = ldb_table_builder_finish(builder);
    ldb_status_set(&s, fs);
    if (ldb_ok(s)) {
      meta->file_size = ldb_table_builder_file_size(builder);
      assert(meta->file_size > 0);
    }
    ldb_table_builder_destroy(builder);
    // Finish and check for file errors
    if (ldb_ok(s)) {
      ldb_status ss = file->m->sync(file);
      ldb_status_set(&s, ss);
    }
    if (ldb_ok(s)) {
      ldb_status cs = file->m->close(file);
      ldb_status_set(&s, cs);
    }
    file->m->destroy(file);
    if (ldb_ok(s)) {
      // Verify that the table is usable
      ldb_read_options ro;
      ldb_read_options_init(&ro);
      ldb_iterator* it =
          ldb_table_cache_new_iterator(tc, &ro, meta->number, meta->file_size);
      ldb_status is = ldb_iter_status(it);
      ldb_status_set(&s, is);
      ldb_iterator_destroy(it);
    }
  }
  free(fname);

  // Check for input iterator errors
  if (!ldb_ok(iter->m->status(iter))) {
    ldb_status is = ldb_iter_status(iter);
    ldb_status_set(&s, is);
    return s;
  }
  return s;
}
