// env.c - common Env wrappers shared by all platform backends.
#include "kvdb.h"

#include <assert.h>

// ------------------------------------------------------------------ strings
void ldb_strings_init(ldb_strings* v) {
  v->items = NULL;
  v->count = 0;
  v->cap = 0;
}

void ldb_strings_destroy(ldb_strings* v) {
  for (size_t i = 0; i < v->count; i++) free(v->items[i]);
  free(v->items);
  ldb_strings_init(v);
}

void ldb_strings_push(ldb_strings* v, char* s) {
  if (v->count == v->cap) {
    v->cap = v->cap ? v->cap * 2 : 8;
    v->items = (char**)realloc(v->items, sizeof(char*) * v->cap);
    assert(v->items);
  }
  v->items[v->count++] = s;
}

// ------------------------------------------------------------------ wrappers
ldb_status ldb_env_new_sequential_file(ldb_env* e, const char* f,
                                       ldb_seq_file** out) {
  return e->vtbl->new_sequential_file(e, f, out);
}
ldb_status ldb_env_new_random_access_file(ldb_env* e, const char* f,
                                          ldb_rand_file** out) {
  return e->vtbl->new_random_access_file(e, f, out);
}
ldb_status ldb_env_new_writable_file(ldb_env* e, const char* f,
                                     ldb_writable_file** out) {
  return e->vtbl->new_writable_file(e, f, out);
}
ldb_status ldb_env_new_appendable_file(ldb_env* e, const char* f,
                                       ldb_writable_file** out) {
  return e->vtbl->new_appendable_file(e, f, out);
}
int ldb_env_file_exists(ldb_env* e, const char* f) {
  return e->vtbl->file_exists(e, f);
}
ldb_status ldb_env_get_children(ldb_env* e, const char* dir, ldb_strings* out) {
  return e->vtbl->get_children(e, dir, out);
}
ldb_status ldb_env_remove_file(ldb_env* e, const char* f) {
  return e->vtbl->remove_file(e, f);
}
ldb_status ldb_env_create_dir(ldb_env* e, const char* d) {
  return e->vtbl->create_dir(e, d);
}
ldb_status ldb_env_delete_dir(ldb_env* e, const char* d) {
  return e->vtbl->delete_dir(e, d);
}
ldb_status ldb_env_get_file_size(ldb_env* e, const char* f, uint64_t* size) {
  return e->vtbl->get_file_size(e, f, size);
}
ldb_status ldb_env_rename_file(ldb_env* e, const char* src, const char* dst) {
  return e->vtbl->rename_file(e, src, dst);
}
ldb_status ldb_env_lock_file(ldb_env* e, const char* f, ldb_file_lock** out) {
  return e->vtbl->lock_file(e, f, out);
}
ldb_status ldb_env_unlock_file(ldb_env* e, ldb_file_lock* l) {
  return e->vtbl->unlock_file(e, l);
}
void ldb_env_schedule(ldb_env* e, void (*fn)(void*), void* arg) {
  e->vtbl->schedule(e, fn, arg);
}
void ldb_env_start_thread(ldb_env* e, void (*fn)(void*), void* arg) {
  e->vtbl->start_thread(e, fn, arg);
}
uint64_t ldb_env_now_micros(ldb_env* e) { return e->vtbl->now_micros(e); }
void ldb_env_sleep_for_microseconds(ldb_env* e, int64_t us) {
  e->vtbl->sleep_for_microseconds(e, us);
}
ldb_status ldb_env_get_test_directory(ldb_env* e, ldb_buffer* path) {
  return e->vtbl->get_test_directory(e, path);
}
ldb_status ldb_env_new_logger(ldb_env* e, const char* f, ldb_logger** out) {
  return e->vtbl->new_logger(e, f, out);
}

ldb_status ldb_write_string_to_file_sync(ldb_env* env, const ldb_buffer* data,
                                         const char* fname) {
  ldb_writable_file* file;
  ldb_status s = ldb_env_new_writable_file(env, fname, &file);
  if (!ldb_ok(s)) return s;
  ldb_slice dslice = ldb_buffer_slice(data);
  s = file->m->append(file, &dslice);
  if (ldb_ok(s)) {
    s = file->m->sync(file);
  }
  if (ldb_ok(s)) {
    s = file->m->close(file);
  }
  file->m->destroy(file);
  return s;
}

ldb_status ldb_read_file_to_string(ldb_env* env, const char* fname,
                                   ldb_buffer* dst) {
  ldb_seq_file* file;
  ldb_status s = ldb_env_new_sequential_file(env, fname, &file);
  if (!ldb_ok(s)) return s;
  static const size_t kBufferSize = 8192;
  char* scratch = (char*)malloc(kBufferSize);
  for (;;) {
    ldb_slice fragment;
    s = file->m->read(file, kBufferSize, &fragment, scratch);
    if (!ldb_ok(s)) break;
    ldb_buffer_append_slice(dst, &fragment);
    if (fragment.size < kBufferSize) {
      break;  // Done
    }
  }
  free(scratch);
  file->m->destroy(file);
  return s;
}
