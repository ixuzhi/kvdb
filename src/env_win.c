// env_win.c - Windows implementation of the Env interface.
#include "kvdb.h"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <assert.h>
#include <io.h>

// ------------------------------------------------------------------ helpers
static ldb_status win_status(const char* context) {
  DWORD err = GetLastError();
  char msg[256];
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                 NULL, err, 0, msg, sizeof(msg), NULL);
  size_t l = strlen(msg);
  while (l > 0 && (msg[l - 1] == '\r' || msg[l - 1] == '\n' ||
                   msg[l - 1] == ' ')) {
    msg[--l] = '\0';
  }
  char buf[512];
  snprintf(buf, sizeof(buf), "%s: %s (win32 error %lu)", context, msg,
           (unsigned long)err);
  return ldb_status_io_error(buf, NULL);
}

// ------------------------------------------------------------------ seq file
typedef struct win_seq_file {
  ldb_seq_file base;
  HANDLE h;
  char* fname;
} win_seq_file;

static void seq_destroy(ldb_seq_file* f) {
  win_seq_file* s = (win_seq_file*)f;
  if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
  free(s->fname);
  free(s);
}

static ldb_status seq_read(ldb_seq_file* f, size_t n, ldb_slice* result,
                           char* scratch) {
  win_seq_file* s = (win_seq_file*)f;
  DWORD got = 0;
  if (n > 0 && !ReadFile(s->h, scratch, (DWORD)n, &got, NULL)) {
    *result = ldb_slice_make("", 0);
    return win_status("ReadFile failed");
  }
  *result = ldb_slice_make(scratch, (size_t)got);
  return ldb_status_ok();
}

static ldb_status seq_skip(ldb_seq_file* f, uint64_t n) {
  win_seq_file* s = (win_seq_file*)f;
  LARGE_INTEGER li;
  li.QuadPart = (LONGLONG)n;
  if (!SetFilePointerEx(s->h, li, NULL, FILE_CURRENT)) {
    return win_status("SetFilePointerEx failed");
  }
  return ldb_status_ok();
}

static const ldb_seq_file_methods g_seq_methods = {seq_destroy, seq_read,
                                                   seq_skip};

// ------------------------------------------------------------------ rand file
typedef struct win_rand_file {
  ldb_rand_file base;
  HANDLE h;
  char* fname;
} win_rand_file;

static void rand_destroy(ldb_rand_file* f) {
  win_rand_file* s = (win_rand_file*)f;
  if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
  free(s->fname);
  free(s);
}

static ldb_status rand_read(ldb_rand_file* f, uint64_t offset, size_t n,
                            ldb_slice* result, char* scratch) {
  win_rand_file* s = (win_rand_file*)f;
  OVERLAPPED ov;
  memset(&ov, 0, sizeof(ov));
  ov.Offset = (DWORD)(offset & 0xffffffffu);
  ov.OffsetHigh = (DWORD)(offset >> 32);
  DWORD got = 0;
  if (n > 0 && !ReadFile(s->h, scratch, (DWORD)n, &got, &ov)) {
    DWORD err = GetLastError();
    if (err != ERROR_HANDLE_EOF) {
      *result = ldb_slice_make("", 0);
      return win_status("ReadFile failed");
    }
  }
  *result = ldb_slice_make(scratch, (size_t)got);
  return ldb_status_ok();
}

static const ldb_rand_file_methods g_rand_methods = {rand_destroy, rand_read};

// ------------------------------------------------------------------ writable
typedef struct win_writable {
  ldb_writable_file base;
  HANDLE h;
  char* fname;
} win_writable;

static void writable_destroy(ldb_writable_file* f) {
  win_writable* s = (win_writable*)f;
  if (s->h != INVALID_HANDLE_VALUE) CloseHandle(s->h);
  free(s->fname);
  free(s);
}

static ldb_status writable_append(ldb_writable_file* f, const ldb_slice* data) {
  win_writable* s = (win_writable*)f;
  size_t off = 0;
  while (off < data->size) {
    DWORD to_write = (DWORD)((data->size - off) > 0x40000000
                                 ? 0x40000000
                                 : (data->size - off));
    DWORD written = 0;
    if (!WriteFile(s->h, data->data + off, to_write, &written, NULL)) {
      return win_status("WriteFile failed");
    }
    off += written;
  }
  return ldb_status_ok();
}

static ldb_status writable_close(ldb_writable_file* f) {
  win_writable* s = (win_writable*)f;
  if (s->h != INVALID_HANDLE_VALUE) {
    if (!CloseHandle(s->h)) return win_status("CloseHandle failed");
    s->h = INVALID_HANDLE_VALUE;
  }
  return ldb_status_ok();
}

static ldb_status writable_flush(ldb_writable_file* f) {
  (void)f;
  // Data is handed directly to the OS; durability is provided by Sync().
  return ldb_status_ok();
}

static ldb_status writable_sync(ldb_writable_file* f) {
  win_writable* s = (win_writable*)f;
  if (s->h != INVALID_HANDLE_VALUE && !FlushFileBuffers(s->h)) {
    return win_status("FlushFileBuffers failed");
  }
  return ldb_status_ok();
}

static const ldb_writable_file_methods g_writable_methods = {
    writable_destroy, writable_append, writable_close, writable_flush,
    writable_sync};

// ------------------------------------------------------------------ logger
// (stderr fallback logger; the DB uses the file logger below)

// ------------------------------------------------------------------ background scheduler
#define LDB_BG_QUEUE_MAX 64

typedef struct bg_item {
  void (*fn)(void*);
  void* arg;
} bg_item;

typedef struct win_env {
  ldb_env base;
  ldb_mutex mu;
  ldb_cond cv;
  bg_item queue[LDB_BG_QUEUE_MAX];
  size_t q_head;
  size_t q_count;
  int started;
  int shutting_down;
} win_env;

static DWORD WINAPI bg_thread_main(LPVOID p) {
  win_env* e = (win_env*)p;
  for (;;) {
    ldb_mutex_lock(&e->mu);
    while (e->q_count == 0 && !e->shutting_down) {
      ldb_cond_wait(&e->cv, &e->mu);
    }
    if (e->q_count == 0 && e->shutting_down) {
      ldb_mutex_unlock(&e->mu);
      return 0;
    }
    bg_item item = e->queue[e->q_head];
    e->q_head = (e->q_head + 1) % LDB_BG_QUEUE_MAX;
    e->q_count--;
    ldb_mutex_unlock(&e->mu);
    item.fn(item.arg);
  }
}

static void win_schedule(ldb_env* env, void (*fn)(void*), void* arg) {
  win_env* e = (win_env*)env;
  ldb_mutex_lock(&e->mu);
  if (!e->started) {
    e->started = 1;
    HANDLE h = CreateThread(NULL, 0, bg_thread_main, e, 0, NULL);
    if (h) CloseHandle(h);
  }
  assert(e->q_count < LDB_BG_QUEUE_MAX);
  size_t pos = (e->q_head + e->q_count) % LDB_BG_QUEUE_MAX;
  e->queue[pos].fn = fn;
  e->queue[pos].arg = arg;
  e->q_count++;
  ldb_cond_signal(&e->cv);
  ldb_mutex_unlock(&e->mu);
}

// ------------------------------------------------------------------ env methods
static ldb_status win_new_sequential_file(ldb_env* env, const char* fname,
                                          ldb_seq_file** out) {
  (void)env;
  HANDLE h = CreateFileA(
      fname, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return win_status(fname);
  win_seq_file* s = (win_seq_file*)malloc(sizeof(win_seq_file));
  s->base.m = &g_seq_methods;
  s->h = h;
  s->fname = strdup(fname);
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status win_new_random_access_file(ldb_env* env, const char* fname,
                                             ldb_rand_file** out) {
  (void)env;
  HANDLE h = CreateFileA(
      fname, GENERIC_READ,
      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h == INVALID_HANDLE_VALUE) return win_status(fname);
  win_rand_file* s = (win_rand_file*)malloc(sizeof(win_rand_file));
  s->base.m = &g_rand_methods;
  s->h = h;
  s->fname = strdup(fname);
  *out = &s->base;
  return ldb_status_ok();
}

static HANDLE open_writable(const char* fname, int appendable) {
  DWORD creation = appendable ? OPEN_ALWAYS : CREATE_ALWAYS;
  HANDLE h = CreateFileA(fname, GENERIC_WRITE,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         NULL, creation, FILE_ATTRIBUTE_NORMAL, NULL);
  if (h != INVALID_HANDLE_VALUE && appendable) {
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    SetFilePointerEx(h, zero, NULL, FILE_END);
  }
  return h;
}

static ldb_status win_new_writable_file(ldb_env* env, const char* fname,
                                        ldb_writable_file** out) {
  (void)env;
  HANDLE h = open_writable(fname, 0);
  if (h == INVALID_HANDLE_VALUE) return win_status(fname);
  win_writable* s = (win_writable*)malloc(sizeof(win_writable));
  s->base.m = &g_writable_methods;
  s->h = h;
  s->fname = strdup(fname);
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status win_new_appendable_file(ldb_env* env, const char* fname,
                                          ldb_writable_file** out) {
  (void)env;
  HANDLE h = open_writable(fname, 1);
  if (h == INVALID_HANDLE_VALUE) return win_status(fname);
  win_writable* s = (win_writable*)malloc(sizeof(win_writable));
  s->base.m = &g_writable_methods;
  s->h = h;
  s->fname = strdup(fname);
  *out = &s->base;
  return ldb_status_ok();
}

static int win_file_exists(ldb_env* env, const char* fname) {
  (void)env;
  DWORD attrs = GetFileAttributesA(fname);
  return attrs != INVALID_FILE_ATTRIBUTES;
}

static ldb_status win_get_children(ldb_env* env, const char* dir,
                                   ldb_strings* out) {
  (void)env;
  char pattern[MAX_PATH];
  snprintf(pattern, sizeof(pattern), "%s\\*", dir);
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    DWORD err = GetLastError();
    if (err == ERROR_FILE_NOT_FOUND || err == ERROR_NO_MORE_FILES) {
      return ldb_status_ok();
    }
    return win_status(dir);
  }
  do {
    if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0) {
      continue;
    }
    ldb_strings_push(out, strdup(fd.cFileName));
  } while (FindNextFileA(h, &fd));
  FindClose(h);
  return ldb_status_ok();
}

static ldb_status win_remove_file(ldb_env* env, const char* fname) {
  (void)env;
  if (!DeleteFileA(fname)) return win_status(fname);
  return ldb_status_ok();
}

static ldb_status win_create_dir(ldb_env* env, const char* dirname) {
  (void)env;
  if (!CreateDirectoryA(dirname, NULL)) {
    DWORD err = GetLastError();
    if (err == ERROR_ALREADY_EXISTS) return ldb_status_ok();
    return win_status(dirname);
  }
  return ldb_status_ok();
}

static ldb_status win_delete_dir(ldb_env* env, const char* dirname) {
  (void)env;
  if (!RemoveDirectoryA(dirname)) return win_status(dirname);
  return ldb_status_ok();
}

static ldb_status win_get_file_size(ldb_env* env, const char* fname,
                                    uint64_t* size) {
  (void)env;
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(fname, GetFileExInfoStandard, &fad)) {
    return win_status(fname);
  }
  *size = ((uint64_t)fad.nFileSizeHigh << 32) | fad.nFileSizeLow;
  return ldb_status_ok();
}

static ldb_status win_rename_file(ldb_env* env, const char* src,
                                  const char* dst) {
  (void)env;
  if (!MoveFileExA(src, dst, MOVEFILE_REPLACE_EXISTING)) {
    return win_status(src);
  }
  return ldb_status_ok();
}

// Track locks held by this process so that a second LockFile call on the
// same file (e.g., DestroyDB while the DB is open) behaves like the POSIX
// per-process locks used by leveldb.
typedef struct win_lock_entry {
  char* fname;
  HANDLE h;
  int count;
  struct win_lock_entry* next;
} win_lock_entry;

static win_lock_entry* g_win_locks = NULL;
static ldb_mutex g_win_locks_mu;

static win_lock_entry* win_find_lock(const char* fname) {
  for (win_lock_entry* e = g_win_locks; e != NULL; e = e->next) {
    if (strcmp(e->fname, fname) == 0) return e;
  }
  return NULL;
}

static ldb_status win_lock_file(ldb_env* env, const char* fname,
                                ldb_file_lock** out) {
  (void)env;
  static int mu_init = 0;
  if (!mu_init) {
    ldb_mutex_init(&g_win_locks_mu);
    mu_init = 1;
  }
  ldb_mutex_lock(&g_win_locks_mu);
  win_lock_entry* entry = win_find_lock(fname);
  if (entry != NULL) {
    // Same-process re-lock: succeed without a new OS lock (mirrors the
    // per-process semantics of leveldb's POSIX fcntl locks).
    entry->count++;
  } else {
    // Open with no sharing: a second locker (other process) fails.
    HANDLE h = CreateFileA(fname, GENERIC_READ | GENERIC_WRITE,
                           0 /* no sharing */, NULL, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
      ldb_mutex_unlock(&g_win_locks_mu);
      return win_status(fname);
    }
    entry = (win_lock_entry*)calloc(1, sizeof(win_lock_entry));
    entry->fname = strdup(fname);
    entry->h = h;
    entry->count = 1;
    entry->next = g_win_locks;
    g_win_locks = entry;
  }
  ldb_mutex_unlock(&g_win_locks_mu);
  ldb_file_lock* lock = (ldb_file_lock*)malloc(sizeof(ldb_file_lock));
  lock->impl = entry;
  *out = lock;
  return ldb_status_ok();
}

static ldb_status win_unlock_file(ldb_env* env, ldb_file_lock* lock) {
  (void)env;
  static int mu_init = 0;
  if (!mu_init) {
    ldb_mutex_init(&g_win_locks_mu);
    mu_init = 1;
  }
  ldb_mutex_lock(&g_win_locks_mu);
  win_lock_entry* entry = (win_lock_entry*)lock->impl;
  entry->count--;
  if (entry->count == 0) {
    CloseHandle(entry->h);
    // unlink
    win_lock_entry** p = &g_win_locks;
    while (*p != NULL && *p != entry) p = &(*p)->next;
    if (*p == entry) {
      *p = entry->next;
      free(entry->fname);
      free(entry);
    }
  }
  ldb_mutex_unlock(&g_win_locks_mu);
  free(lock);
  return ldb_status_ok();
}

static void win_start_thread(ldb_env* env, void (*fn)(void*), void* arg) {
  (void)env;
  ldb_start_thread(fn, arg);
}

static uint64_t win_now_micros(ldb_env* env) {
  (void)env;
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  // FILETIME is 100ns ticks since 1601-01-01; subtract to Unix epoch.
  return (uint64_t)((t - UINT64_C(116444736000000000)) / 10);
}

static void win_sleep_for_microseconds(ldb_env* env, int64_t micros) {
  (void)env;
  Sleep((DWORD)((micros + 999) / 1000));
}

static ldb_status win_get_test_directory(ldb_env* env, ldb_buffer* path) {
  (void)env;
  char temp_path[MAX_PATH];
  if (!GetTempPathA(sizeof(temp_path), temp_path)) {
    strcpy(temp_path, ".");
  }
  size_t l = strlen(temp_path);
  while (l > 0 && (temp_path[l - 1] == '/' || temp_path[l - 1] == '\\')) {
    temp_path[--l] = '\0';
  }
  ldb_buffer_clear(path);
  ldb_buffer_append_str(path, temp_path);
  ldb_buffer_append_str(path, "/leveldbtest-");
  {
    char buf[32];
    snprintf(buf, sizeof(buf), "%lu", (unsigned long)GetCurrentProcessId());
    ldb_buffer_append_str(path, buf);
  }
  return ldb_status_ok();
}

// Logger writing into the opened LOG file with leveldb's timestamp format.
static void file_logv(ldb_logger* logger, const char* fmt, va_list ap) {
  typedef struct file_logger {
    ldb_logger base;
    FILE* f;
  } file_logger;
  file_logger* l = (file_logger*)logger;
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
  SYSTEMTIME st;
  FILETIME ft;
  GetSystemTimeAsFileTime(&ft);
  FileTimeToSystemTime(&ft, &st);
  uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
  uint64_t micros = (t / 10) % 1000000;
  fprintf(l->f, "%04d/%02d/%02d-%02d:%02d:%02d.%06llu %s\n", st.wYear,
          st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond,
          (unsigned long long)micros, buf);
  fflush(l->f);
  free(buf);
}

// An open FILE* keeps LOG exclusively locked on Windows, so the handle has to
// go back before the file can be renamed or deleted.
static void file_logger_destroy(ldb_logger* logger) {
  typedef struct file_logger {
    ldb_logger base;
    FILE* f;
  } file_logger;
  file_logger* l = (file_logger*)logger;
  fclose(l->f);
  free(l);
}

static ldb_status win_new_logger(ldb_env* env, const char* fname,
                                 ldb_logger** out) {
  (void)env;
  typedef struct file_logger {
    ldb_logger base;
    FILE* f;
  } file_logger;
  FILE* f = fopen(fname, "a");
  if (f == NULL) return win_status(fname);
  file_logger* l = (file_logger*)malloc(sizeof(file_logger));
  l->base.logv = file_logv;
  l->base.destroy = file_logger_destroy;
  l->f = f;
  *out = &l->base;
  return ldb_status_ok();
}

static void win_env_destroy(ldb_env* env) {
  win_env* e = (win_env*)env;
  ldb_mutex_lock(&e->mu);
  e->shutting_down = 1;
  ldb_cond_signal_all(&e->cv);
  ldb_mutex_unlock(&e->mu);
  // default env is a process-lifetime singleton; do not free.
}

static const ldb_env_vtbl g_win_env_vtbl = {
    win_new_sequential_file,  win_new_random_access_file,
    win_new_writable_file,    win_new_appendable_file,
    win_file_exists,          win_get_children,
    win_remove_file,          win_create_dir,
    win_delete_dir,           win_get_file_size,
    win_rename_file,          win_lock_file,
    win_unlock_file,          win_schedule,
    win_start_thread,         win_now_micros,
    win_sleep_for_microseconds, win_get_test_directory,
    win_new_logger,           win_env_destroy};

ldb_env* ldb_env_default(void) {
  static win_env default_env;
  static int initialized = 0;
  if (!initialized) {
    default_env.base.vtbl = &g_win_env_vtbl;
    ldb_mutex_init(&default_env.mu);
    ldb_cond_init(&default_env.cv, &default_env.mu);
    default_env.q_head = 0;
    default_env.q_count = 0;
    default_env.started = 0;
    default_env.shutting_down = 0;
    initialized = 1;
  }
  return &default_env.base;
}

#else
// Avoid "empty translation unit" warnings when building for other platforms.
typedef int ldb_env_win_dummy_t;
#endif  // _WIN32
