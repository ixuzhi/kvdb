// env_posix.c - POSIX implementation of the Env interface.
//
// Executed on native Linux (glibc) and on the POSIX-emulating Windows layers
// (MSYS2's msys target); the backend is selected by the !_WIN32 test below, so
// a host that defines _WIN32 while offering POSIX semantics must be excluded
// there or it silently links the Windows Env instead.
#include "kvdb.h"

#if !defined(_WIN32)

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include <assert.h>
#include <errno.h>

static ldb_status posix_status(const char* context) {
  return ldb_status_from_errno(errno, context);
}

// ------------------------------------------------------------------ seq file
typedef struct posix_seq_file {
  ldb_seq_file base;
  int fd;
} posix_seq_file;

static void seq_destroy(ldb_seq_file* f) {
  posix_seq_file* s = (posix_seq_file*)f;
  if (s->fd >= 0) close(s->fd);
  free(s);
}

static ldb_status seq_read(ldb_seq_file* f, size_t n, ldb_slice* result,
                           char* scratch) {
  posix_seq_file* s = (posix_seq_file*)f;
  size_t got = 0;
  while (got < n) {
    ssize_t r = read(s->fd, scratch + got, n - got);
    if (r < 0) {
      if (errno == EINTR) continue;
      *result = ldb_slice_make("", 0);
      return posix_status("read failed");
    }
    if (r == 0) break;  // EOF
    got += (size_t)r;
  }
  *result = ldb_slice_make(scratch, got);
  return ldb_status_ok();
}

static ldb_status seq_skip(ldb_seq_file* f, uint64_t n) {
  posix_seq_file* s = (posix_seq_file*)f;
  if (lseek(s->fd, (off_t)n, SEEK_CUR) == (off_t)-1) {
    return posix_status("lseek failed");
  }
  return ldb_status_ok();
}

static const ldb_seq_file_methods g_seq_methods = {seq_destroy, seq_read,
                                                   seq_skip};

// ------------------------------------------------------------------ rand file
typedef struct posix_rand_file {
  ldb_rand_file base;
  int fd;
} posix_rand_file;

static void rand_destroy(ldb_rand_file* f) {
  posix_rand_file* s = (posix_rand_file*)f;
  if (s->fd >= 0) close(s->fd);
  free(s);
}

static ldb_status rand_read(ldb_rand_file* f, uint64_t offset, size_t n,
                            ldb_slice* result, char* scratch) {
  posix_rand_file* s = (posix_rand_file*)f;
  size_t got = 0;
  while (got < n) {
    ssize_t r = pread(s->fd, scratch + got, n - got, (off_t)(offset + got));
    if (r < 0) {
      if (errno == EINTR) continue;
      *result = ldb_slice_make("", 0);
      return posix_status("pread failed");
    }
    if (r == 0) break;  // EOF
    got += (size_t)r;
  }
  *result = ldb_slice_make(scratch, got);
  return ldb_status_ok();
}

static const ldb_rand_file_methods g_rand_methods = {rand_destroy,
                                                     rand_read};

// ------------------------------------------------------------------ writable
typedef struct posix_writable {
  ldb_writable_file base;
  int fd;
  char* fname;
} posix_writable;

static void writable_destroy(ldb_writable_file* f) {
  posix_writable* s = (posix_writable*)f;
  if (s->fd >= 0) close(s->fd);
  free(s->fname);
  free(s);
}

static ldb_status writable_append(ldb_writable_file* f, const ldb_slice* data) {
  posix_writable* s = (posix_writable*)f;
  size_t off = 0;
  while (off < data->size) {
    ssize_t w = write(s->fd, data->data + off, data->size - off);
    if (w < 0) {
      if (errno == EINTR) continue;
      return posix_status("write failed");
    }
    off += (size_t)w;
  }
  return ldb_status_ok();
}

static ldb_status writable_close(ldb_writable_file* f) {
  posix_writable* s = (posix_writable*)f;
  if (s->fd >= 0) {
    if (close(s->fd) != 0) {
      s->fd = -1;
      return posix_status("close failed");
    }
    s->fd = -1;
  }
  return ldb_status_ok();
}

static ldb_status writable_flush(ldb_writable_file* f) {
  (void)f;
  return ldb_status_ok();  // data goes straight to the OS
}

static ldb_status writable_sync(ldb_writable_file* f) {
  posix_writable* s = (posix_writable*)f;
  if (fsync(s->fd) != 0) return posix_status("fsync failed");
  return ldb_status_ok();
}

static const ldb_writable_file_methods g_writable_methods = {
    writable_destroy, writable_append, writable_close, writable_flush,
    writable_sync};

// ------------------------------------------------------------------ env
typedef struct posix_env {
  ldb_env base;
} posix_env;

static ldb_status posix_new_sequential_file(ldb_env* env, const char* fname,
                                            ldb_seq_file** out) {
  (void)env;
  int fd = open(fname, O_RDONLY);
  if (fd < 0) return posix_status(fname);
  posix_seq_file* s = (posix_seq_file*)malloc(sizeof(posix_seq_file));
  s->base.m = &g_seq_methods;
  s->fd = fd;
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status posix_new_random_access_file(ldb_env* env, const char* fname,
                                               ldb_rand_file** out) {
  (void)env;
  int fd = open(fname, O_RDONLY);
  if (fd < 0) return posix_status(fname);
  posix_rand_file* s = (posix_rand_file*)malloc(sizeof(posix_rand_file));
  s->base.m = &g_rand_methods;
  s->fd = fd;
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status posix_new_writable(ldb_env* env, const char* fname,
                                     ldb_writable_file** out, int appendable) {
  (void)env;
  int flags = O_WRONLY | O_CREAT;
  flags |= appendable ? O_APPEND : O_TRUNC;
  int fd = open(fname, flags, 0644);
  if (fd < 0) return posix_status(fname);
  posix_writable* s = (posix_writable*)malloc(sizeof(posix_writable));
  s->base.m = &g_writable_methods;
  s->fd = fd;
  s->fname = strdup(fname);
  *out = &s->base;
  return ldb_status_ok();
}

static ldb_status posix_new_writable_file(ldb_env* env, const char* fname,
                                          ldb_writable_file** out) {
  return posix_new_writable(env, fname, out, 0);
}

static ldb_status posix_new_appendable_file(ldb_env* env, const char* fname,
                                            ldb_writable_file** out) {
  return posix_new_writable(env, fname, out, 1);
}

static int posix_file_exists(ldb_env* env, const char* fname) {
  (void)env;
  return access(fname, F_OK) == 0;
}

static ldb_status posix_get_children(ldb_env* env, const char* dir,
                                     ldb_strings* out) {
  (void)env;
  DIR* d = opendir(dir);
  if (d == NULL) return posix_status(dir);
  struct dirent* entry;
  while ((entry = readdir(d)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    ldb_strings_push(out, strdup(entry->d_name));
  }
  closedir(d);
  return ldb_status_ok();
}

static ldb_status posix_remove_file(ldb_env* env, const char* fname) {
  (void)env;
  if (unlink(fname) != 0) return posix_status(fname);
  return ldb_status_ok();
}

static ldb_status posix_create_dir(ldb_env* env, const char* dirname) {
  (void)env;
  if (mkdir(dirname, 0755) != 0) {
    if (errno == EEXIST) return ldb_status_ok();
    return posix_status(dirname);
  }
  return ldb_status_ok();
}

static ldb_status posix_delete_dir(ldb_env* env, const char* dirname) {
  (void)env;
  if (rmdir(dirname) != 0) return posix_status(dirname);
  return ldb_status_ok();
}

static ldb_status posix_get_file_size(ldb_env* env, const char* fname,
                                      uint64_t* size) {
  (void)env;
  struct stat st;
  if (stat(fname, &st) != 0) return posix_status(fname);
  *size = (uint64_t)st.st_size;
  return ldb_status_ok();
}

static ldb_status posix_rename_file(ldb_env* env, const char* src,
                                    const char* dst) {
  (void)env;
  if (rename(src, dst) != 0) return posix_status(src);
  return ldb_status_ok();
}

static ldb_status posix_lock_file(ldb_env* env, const char* fname,
                                  ldb_file_lock** out) {
  (void)env;
  int fd = open(fname, O_RDWR | O_CREAT, 0644);
  if (fd < 0) return posix_status(fname);
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;  // whole file
  if (fcntl(fd, F_SETLK, &fl) != 0) {
    close(fd);
    return posix_status(fname);
  }
  ldb_file_lock* lock = (ldb_file_lock*)malloc(sizeof(ldb_file_lock));
  lock->impl = (void*)(intptr_t)fd;
  *out = lock;
  return ldb_status_ok();
}

static ldb_status posix_unlock_file(ldb_env* env, ldb_file_lock* lock) {
  (void)env;
  int fd = (int)(intptr_t)lock->impl;
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_UNLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  if (fd >= 0) {
    fcntl(fd, F_SETLK, &fl);  // best effort
    close(fd);
  }
  free(lock);
  return ldb_status_ok();
}

static uint64_t posix_now_micros(ldb_env* env) {
  (void)env;
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)(ts.tv_nsec / 1000);
}

static void posix_sleep_for_microseconds(ldb_env* env, int64_t micros) {
  (void)env;
  struct timespec ts;
  ts.tv_sec = (time_t)(micros / 1000000);
  ts.tv_nsec = (long)(micros % 1000000) * 1000L;
  nanosleep(&ts, NULL);
}

static ldb_status posix_get_test_directory(ldb_env* env, ldb_buffer* path) {
  (void)env;
  const char* tmp = getenv("TMPDIR");
  if (tmp == NULL || tmp[0] == '\0') tmp = "/tmp";
  ldb_buffer_clear(path);
  ldb_buffer_append_str(path, tmp);
  ldb_buffer_append_str(path, "/leveldbtest-");
  char buf[32];
  snprintf(buf, sizeof(buf), "%d", (int)getpid());
  ldb_buffer_append_str(path, buf);
  return ldb_status_ok();
}

// ------------------------------------------------------------------ logger
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
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  struct tm tm_time;
  localtime_r(&ts.tv_sec, &tm_time);
  fprintf(l->f, "%04d/%02d/%02d-%02d:%02d:%02d.%06ld %s\n",
          tm_time.tm_year + 1900, tm_time.tm_mon + 1, tm_time.tm_mday,
          tm_time.tm_hour, tm_time.tm_min, tm_time.tm_sec,
          (long)(ts.tv_nsec / 1000), buf);
  fflush(l->f);
  free(buf);
}

// Mirrors ~POSIXFileLogger in leveldb: the FILE* belongs to the logger.
static void file_logger_destroy(ldb_logger* logger) {
  typedef struct file_logger {
    ldb_logger base;
    FILE* f;
  } file_logger;
  file_logger* l = (file_logger*)logger;
  fclose(l->f);
  free(l);
}

static ldb_status posix_new_logger(ldb_env* env, const char* fname,
                                   ldb_logger** out) {
  (void)env;
  typedef struct file_logger {
    ldb_logger base;
    FILE* f;
  } file_logger;
  FILE* f = fopen(fname, "a");
  if (f == NULL) return posix_status(fname);
  file_logger* l = (file_logger*)malloc(sizeof(file_logger));
  l->base.logv = file_logv;
  l->base.destroy = file_logger_destroy;
  l->f = f;
  *out = &l->base;
  return ldb_status_ok();
}

static void posix_schedule(ldb_env* env, void (*fn)(void*), void* arg) {
  (void)env;
  ldb_start_thread(fn, arg);
}

static void posix_start_thread(ldb_env* env, void (*fn)(void*), void* arg) {
  (void)env;
  ldb_start_thread(fn, arg);
}

static const ldb_env_vtbl g_posix_env_vtbl = {
    posix_new_sequential_file,  posix_new_random_access_file,
    posix_new_writable_file,    posix_new_appendable_file,
    posix_file_exists,          posix_get_children,
    posix_remove_file,          posix_create_dir,
    posix_delete_dir,           posix_get_file_size,
    posix_rename_file,          posix_lock_file,
    posix_unlock_file,          posix_schedule,
    posix_start_thread,         posix_now_micros,
    posix_sleep_for_microseconds, posix_get_test_directory,
    posix_new_logger,           NULL};

ldb_env* ldb_env_default(void) {
  static posix_env default_env;
  static int initialized = 0;
  if (!initialized) {
    default_env.base.vtbl = &g_posix_env_vtbl;
    initialized = 1;
  }
  return &default_env.base;
}

#else
typedef int posix_env_dummy_t;  // avoid empty translation unit on Windows
#endif  // !_WIN32
