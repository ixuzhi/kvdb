// env_mem.c - in-memory Env implementation (mirrors helpers/memenv/memenv.cc)
// Useful for tests and ephemeral databases.
#include "kvdb.h"

#include <assert.h>
#include <sys/stat.h>

typedef struct mem_file {
  char* name;  // full path, owned
  char* data;
  size_t size;
  size_t cap;
  int refs;  // number of open handles
  int locked;
  struct mem_file* next;
} mem_file;

typedef struct mem_fs {
  mem_file* files;  // singly linked list
  mem_file* graveyard;  // unlinked but still-open files
  uint64_t next_id;
  ldb_mutex mu;
} mem_fs;

typedef struct mem_seq {
  ldb_seq_file base;
  mem_file* f;
  size_t pos;
} mem_seq;

typedef struct mem_rand {
  ldb_rand_file base;
  mem_file* f;
} mem_rand;

typedef struct mem_writable {
  ldb_writable_file base;
  mem_file* f;
} mem_writable;

typedef struct mem_lock {
  ldb_file_lock base;
  mem_file* f;
} mem_lock;

typedef struct mem_logger {
  ldb_logger base;
  ldb_buffer log;
} mem_logger;

typedef struct mem_env {
  ldb_env base;
  mem_fs fs;
} mem_env;

// ------------------------------------------------------------------ fs helpers
static mem_file* fs_find(mem_fs* fs, const char* name) {
  for (mem_file* f = fs->files; f != NULL; f = f->next) {
    if (strcmp(f->name, name) == 0) return f;
  }
  return NULL;
}

// Find nearest existing parent directory entry (directory = file entry
// without data that "exists").
static int fs_dir_exists(mem_fs* fs, const char* dirname) {
  mem_file* f = fs_find(fs, dirname);
  return f != NULL;
}

static mem_file* fs_create(mem_fs* fs, const char* name) {
  mem_file* f = (mem_file*)calloc(1, sizeof(mem_file));
  f->name = strdup(name);
  f->next = fs->files;
  fs->files = f;
  return f;
}

static void fs_delete_file(mem_fs* fs, mem_file* f) {
  assert(f->refs == 0);
  mem_file** p = &fs->files;
  while (*p != NULL && *p != f) p = &(*p)->next;
  assert(*p == f);
  *p = f->next;
  free(f->name);
  free(f->data);
  free(f);
}

static void mem_file_append(mem_file* f, const char* data, size_t n) {
  if (f->size + n > f->cap) {
    size_t newcap = f->cap ? f->cap : 256;
    while (newcap < f->size + n) newcap *= 2;
    f->data = (char*)realloc(f->data, newcap);
    assert(f->data);
    f->cap = newcap;
  }
  memcpy(f->data + f->size, data, n);
  f->size += n;
}

// ------------------------------------------------------------------ seq file
static ldb_status mem_seq_read(ldb_seq_file* base, size_t n,
                               ldb_slice* result, char* scratch) {
  mem_seq* s = (mem_seq*)base;
  size_t avail = (s->pos < s->f->size) ? (s->f->size - s->pos) : 0;
  size_t to_read = (n < avail) ? n : avail;
  if (to_read > 0) {
    memcpy(scratch, s->f->data + s->pos, to_read);
  }
  s->pos += to_read;
  *result = ldb_slice_make(scratch, to_read);
  return ldb_status_ok();
}

static ldb_status mem_seq_skip(ldb_seq_file* base, uint64_t n) {
  mem_seq* s = (mem_seq*)base;
  if (s->pos > s->f->size) {
    s->pos = s->f->size;
  }
  uint64_t avail = s->f->size - s->pos;
  s->pos += (n < avail) ? (size_t)n : (size_t)avail;
  return ldb_status_ok();
}

static void mem_seq_destroy(ldb_seq_file* base) {
  mem_seq* s = (mem_seq*)base;
  s->f->refs--;
  free(s);
}

static const ldb_seq_file_methods g_mem_seq_methods = {mem_seq_destroy,
                                                       mem_seq_read,
                                                       mem_seq_skip};

// ------------------------------------------------------------------ rand file
static ldb_status mem_rand_read(ldb_rand_file* base, uint64_t offset, size_t n,
                                ldb_slice* result, char* scratch) {
  mem_rand* r = (mem_rand*)base;
  if (offset > r->f->size) {
    return ldb_status_io_error("Offset is greater than file size", NULL);
  }
  size_t avail = r->f->size - (size_t)offset;
  size_t to_read = (n < avail) ? n : avail;
  if (to_read > 0) {
    memcpy(scratch, r->f->data + offset, to_read);
  }
  *result = ldb_slice_make(scratch, to_read);
  return ldb_status_ok();
}

static void mem_rand_destroy(ldb_rand_file* base) {
  mem_rand* r = (mem_rand*)base;
  r->f->refs--;
  free(r);
}

static const ldb_rand_file_methods g_mem_rand_methods = {mem_rand_destroy,
                                                         mem_rand_read};

// ------------------------------------------------------------------ writable
static ldb_status mem_writable_append(ldb_writable_file* base,
                                      const ldb_slice* data) {
  mem_writable* w = (mem_writable*)base;
  mem_file_append(w->f, data->data ? data->data : "", data->size);
  return ldb_status_ok();
}

static ldb_status mem_writable_close(ldb_writable_file* base) {
  (void)base;
  return ldb_status_ok();
}

static ldb_status mem_writable_flush(ldb_writable_file* base) {
  (void)base;
  return ldb_status_ok();
}

static ldb_status mem_writable_sync(ldb_writable_file* base) {
  (void)base;
  return ldb_status_ok();
}

static void mem_writable_destroy(ldb_writable_file* base) {
  mem_writable* w = (mem_writable*)base;
  w->f->refs--;
  free(w);
}

static const ldb_writable_file_methods g_mem_writable_methods = {
    mem_writable_destroy, mem_writable_append, mem_writable_close,
    mem_writable_flush, mem_writable_sync};

// ------------------------------------------------------------------ logger
static void mem_logv(ldb_logger* base, const char* fmt, va_list ap) {
  mem_logger* l = (mem_logger*)base;
  va_list ap2;
  va_copy(ap2, ap);
  int n = vsnprintf(NULL, 0, fmt, ap);
  if (n < 0) {
    va_end(ap2);
    return;
  }
  char* buf = (char*)malloc((size_t)n + 1);
  vsnprintf(buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  ldb_buffer_append_str(&l->log, buf);
  ldb_buffer_append_str(&l->log, "\n");
  free(buf);
}

// ------------------------------------------------------------------ env methods
static ldb_status mem_new_sequential_file(ldb_env* base, const char* fname,
                                          ldb_seq_file** out) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    ldb_mutex_unlock(&env->fs.mu);
    return ldb_status_io_error(fname, "file not found");
  }
  f->refs++;
  ldb_mutex_unlock(&env->fs.mu);
  mem_seq* s = (mem_seq*)calloc(1, sizeof(mem_seq));
  s->base.m = &g_mem_seq_methods;
  s->f = f;
  s->pos = 0;
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status mem_new_random_access_file(ldb_env* base, const char* fname,
                                             ldb_rand_file** out) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    ldb_mutex_unlock(&env->fs.mu);
    return ldb_status_io_error(fname, "file not found");
  }
  f->refs++;
  ldb_mutex_unlock(&env->fs.mu);
  mem_rand* r = (mem_rand*)calloc(1, sizeof(mem_rand));
  r->base.m = &g_mem_rand_methods;
  r->f = f;
  *out = &r->base;
  return ldb_status_ok();
}

static ldb_status mem_new_writable(ldb_env* base, const char* fname,
                                   ldb_writable_file** out, int append) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    f = fs_create(&env->fs, fname);
  } else if (!append) {
    f->size = 0;
  }
  f->refs++;
  ldb_mutex_unlock(&env->fs.mu);
  mem_writable* w = (mem_writable*)calloc(1, sizeof(mem_writable));
  w->base.m = &g_mem_writable_methods;
  w->f = f;
  *out = &w->base;
  return ldb_status_ok();
}

static ldb_status mem_new_writable_file(ldb_env* base, const char* fname,
                                        ldb_writable_file** out) {
  return mem_new_writable(base, fname, out, 0);
}

static ldb_status mem_new_appendable_file(ldb_env* base, const char* fname,
                                          ldb_writable_file** out) {
  return mem_new_writable(base, fname, out, 1);
}

static int mem_file_exists(ldb_env* base, const char* fname) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  ldb_mutex_unlock(&env->fs.mu);
  return f != NULL;
}

static ldb_status mem_get_children(ldb_env* base, const char* dir,
                                   ldb_strings* out) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  size_t dirlen = strlen(dir);
  for (mem_file* f = env->fs.files; f != NULL; f = f->next) {
    if (strncmp(f->name, dir, dirlen) == 0 && strlen(f->name) > dirlen &&
        f->name[dirlen] == '/') {
      const char* base_name = f->name + dirlen + 1;
      if (strchr(base_name, '/') == NULL && base_name[0] != '\0') {
        ldb_strings_push(out, strdup(base_name));
      }
    }
  }
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_remove_file(ldb_env* base, const char* fname) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    ldb_mutex_unlock(&env->fs.mu);
    return ldb_status_io_error(fname, "file not found");
  }
  // Unlink from the namespace. If handles are still open, keep the object
  // alive until the last handle closes (mirrors POSIX unlink semantics).
  mem_file** p = &env->fs.files;
  while (*p != NULL && *p != f) p = &(*p)->next;
  if (*p == f) *p = f->next;
  if (f->refs > 0) {
    // deferred free via a graveyard list
    f->next = env->fs.graveyard;
    env->fs.graveyard = f;
  } else {
    free(f->name);
    free(f->data);
    free(f);
  }
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_create_dir(ldb_env* base, const char* dirname) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, dirname);
  if (f == NULL) {
    fs_create(&env->fs, dirname);
  }
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_delete_dir(ldb_env* base, const char* dirname) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, dirname);
  if (f != NULL && f->refs == 0) {
    fs_delete_file(&env->fs, f);
  }
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_get_file_size(ldb_env* base, const char* fname,
                                    uint64_t* size) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    ldb_mutex_unlock(&env->fs.mu);
    return ldb_status_io_error(fname, "file not found");
  }
  *size = f->size;
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_rename_file(ldb_env* base, const char* src,
                                  const char* dst) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, src);
  if (f == NULL) {
    ldb_mutex_unlock(&env->fs.mu);
    return ldb_status_io_error(src, "file not found");
  }
  mem_file* existing = fs_find(&env->fs, dst);
  if (existing != NULL && existing->refs == 0) {
    fs_delete_file(&env->fs, existing);
  }
  free(f->name);
  f->name = strdup(dst);
  ldb_mutex_unlock(&env->fs.mu);
  return ldb_status_ok();
}

static ldb_status mem_lock_file(ldb_env* base, const char* fname,
                                ldb_file_lock** out) {
  mem_env* env = (mem_env*)base;
  ldb_mutex_lock(&env->fs.mu);
  mem_file* f = fs_find(&env->fs, fname);
  if (f == NULL) {
    f = fs_create(&env->fs, fname);
  }
  ldb_mutex_unlock(&env->fs.mu);
  mem_lock* l = (mem_lock*)malloc(sizeof(mem_lock));
  l->base.impl = NULL;
  l->f = f;
  *out = &l->base;
  return ldb_status_ok();
}

static ldb_status mem_unlock_file(ldb_env* base, ldb_file_lock* lock) {
  mem_env* env = (mem_env*)base;
  mem_lock* l = (mem_lock*)lock;
  ldb_mutex_lock(&env->fs.mu);

  ldb_mutex_unlock(&env->fs.mu);
  free(l);
  return ldb_status_ok();
}

static uint64_t mem_now_micros(ldb_env* base) {
  (void)base;
  // Fall back to the platform default env's clock.
  return ldb_env_now_micros(ldb_env_default());
}

static void mem_sleep(ldb_env* base, int64_t micros) {
  ldb_env_sleep_for_microseconds(ldb_env_default(), micros);
  (void)base;
}

static ldb_status mem_get_test_directory(ldb_env* base, ldb_buffer* path) {
  (void)base;
  ldb_buffer_clear(path);
  ldb_buffer_append_str(path, "/memenv");
  return ldb_status_ok();
}

static ldb_status mem_new_logger(ldb_env* base, const char* fname,
                                 ldb_logger** out) {
  (void)base;
  (void)fname;
  mem_logger* l = (mem_logger*)calloc(1, sizeof(mem_logger));
  l->base.logv = mem_logv;
  ldb_buffer_init(&l->log);
  *out = &l->base;
  return ldb_status_ok();
}

static void mem_schedule(ldb_env* base, void (*fn)(void*), void* arg) {
  // In-memory env has no background thread; run inline. DBImpl only
  // schedules one compaction at a time, so this is safe for tests.
  (void)base;
  fn(arg);
}

static void mem_start_thread(ldb_env* base, void (*fn)(void*), void* arg) {
  (void)base;
  fn(arg);
}

static const ldb_env_vtbl g_mem_env_vtbl = {
    mem_new_sequential_file,  mem_new_random_access_file,
    mem_new_writable_file,    mem_new_appendable_file,
    mem_file_exists,          mem_get_children,
    mem_remove_file,          mem_create_dir,
    mem_delete_dir,           mem_get_file_size,
    mem_rename_file,          mem_lock_file,
    mem_unlock_file,          mem_schedule,
    mem_start_thread,         mem_now_micros,
    mem_sleep,                mem_get_test_directory,
    mem_new_logger,           NULL};

ldb_env* ldb_memenv_new(void) {
  mem_env* env = (mem_env*)calloc(1, sizeof(mem_env));
  env->base.vtbl = &g_mem_env_vtbl;
  env->fs.files = NULL;

  ldb_mutex_init(&env->fs.mu);
  return &env->base;
}

void ldb_memenv_destroy(ldb_env* env) {
  mem_env* e = (mem_env*)env;
  mem_file* f = e->fs.files;
  while (f != NULL) {
    mem_file* next = f->next;
    free(f->name);
    free(f->data);
    free(f);
    f = next;
  }
  f = e->fs.graveyard;
  while (f != NULL) {
    mem_file* next = f->next;
    free(f->name);
    free(f->data);
    free(f);
    f = next;
  }
  ldb_mutex_destroy(&e->fs.mu);
  free(e);
}
