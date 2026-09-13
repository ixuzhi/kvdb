// port.h - threading primitives for kvdb (mirrors leveldb/port/port_stdcxx.h)
#ifndef KVDB_PORT_H_
#define KVDB_PORT_H_

#include <stdint.h>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <sched.h>
#endif

// ---------------- Mutex ----------------
typedef struct ldb_mutex {
#if defined(_WIN32)
  CRITICAL_SECTION cs;
#else
  pthread_mutex_t mu;
#endif
} ldb_mutex;

void ldb_mutex_init(ldb_mutex* mu);
void ldb_mutex_destroy(ldb_mutex* mu);
void ldb_mutex_lock(ldb_mutex* mu);
void ldb_mutex_unlock(ldb_mutex* mu);

// ---------------- CondVar ----------------
typedef struct ldb_cond {
#if defined(_WIN32)
  CONDITION_VARIABLE cv;
#else
  pthread_cond_t cv;
#endif
} ldb_cond;

void ldb_cond_init(ldb_cond* cv, ldb_mutex* mu);
void ldb_cond_destroy(ldb_cond* cv);
void ldb_cond_wait(ldb_cond* cv, ldb_mutex* mu);
void ldb_cond_signal(ldb_cond* cv);
void ldb_cond_signal_all(ldb_cond* cv);

// ---------------- Atomics ----------------
typedef struct ldb_atomic_int {
#if defined(_WIN32)
  volatile LONG value;
#else
  volatile int value;
#endif
} ldb_atomic_int;

void ldb_atomic_store(ldb_atomic_int* a, int v);
int ldb_atomic_load(ldb_atomic_int* a);
void ldb_atomic_store_relaxed(ldb_atomic_int* a, int v);
int ldb_atomic_load_relaxed(ldb_atomic_int* a);

// ---------------- Threads ----------------
// Runs fn(arg) on a new detached thread.
void ldb_start_thread(void (*fn)(void*), void* arg);

#endif  // KVDB_PORT_H_
