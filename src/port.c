// port.c - threading primitives implementation
#include "port.h"

#include <assert.h>
#include <stdlib.h>

void ldb_mutex_init(ldb_mutex* mu) {
#if defined(_WIN32)
  InitializeCriticalSection(&mu->cs);  // Win32 CS is recursive
#else
  // Recursive: DBImpl re-enters its mutex when inline-scheduled background
  // work (e.g. the memenv env) runs on the calling thread.
  pthread_mutexattr_t attr;
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
  pthread_mutex_init(&mu->mu, &attr);
  pthread_mutexattr_destroy(&attr);
#endif
}

void ldb_mutex_destroy(ldb_mutex* mu) {
#if defined(_WIN32)
  DeleteCriticalSection(&mu->cs);
#else
  pthread_mutex_destroy(&mu->mu);
#endif
}

void ldb_mutex_lock(ldb_mutex* mu) {
#if defined(_WIN32)
  EnterCriticalSection(&mu->cs);
#else
  pthread_mutex_lock(&mu->mu);
#endif
}

void ldb_mutex_unlock(ldb_mutex* mu) {
#if defined(_WIN32)
  LeaveCriticalSection(&mu->cs);
#else
  pthread_mutex_unlock(&mu->mu);
#endif
}

void ldb_cond_init(ldb_cond* cv, ldb_mutex* mu) {
  (void)mu;
#if defined(_WIN32)
  InitializeConditionVariable(&cv->cv);
#else
  pthread_cond_init(&cv->cv, NULL);
#endif
}

void ldb_cond_destroy(ldb_cond* cv) {
#if !defined(_WIN32)
  pthread_cond_destroy(&cv->cv);
#else
  (void)cv;
#endif
}

void ldb_cond_wait(ldb_cond* cv, ldb_mutex* mu) {
#if defined(_WIN32)
  SleepConditionVariableCS(&cv->cv, &mu->cs, INFINITE);
#else
  pthread_cond_wait(&cv->cv, &mu->mu);
#endif
}

void ldb_cond_signal(ldb_cond* cv) {
#if defined(_WIN32)
  WakeConditionVariable(&cv->cv);
#else
  pthread_cond_signal(&cv->cv);
#endif
}

void ldb_cond_signal_all(ldb_cond* cv) {
#if defined(_WIN32)
  WakeAllConditionVariable(&cv->cv);
#else
  pthread_cond_broadcast(&cv->cv);
#endif
}

void ldb_atomic_store(ldb_atomic_int* a, int v) {
#if defined(_WIN32)
  InterlockedExchange(&a->value, (LONG)v);
#else
  __atomic_store_n(&a->value, v, __ATOMIC_SEQ_CST);
#endif
}

int ldb_atomic_load(ldb_atomic_int* a) {
#if defined(_WIN32)
  return (int)InterlockedCompareExchange(&a->value, 0, 0);
#else
  return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
#endif
}

void ldb_atomic_store_relaxed(ldb_atomic_int* a, int v) {
#if defined(_WIN32)
  a->value = (LONG)v;
#else
  __atomic_store_n(&a->value, v, __ATOMIC_RELAXED);
#endif
}

int ldb_atomic_load_relaxed(ldb_atomic_int* a) {
#if defined(_WIN32)
  return (int)a->value;
#else
  return __atomic_load_n(&a->value, __ATOMIC_RELAXED);
#endif
}

typedef struct thread_arg {
  void (*fn)(void*);
  void* raw;
} thread_arg;

#if defined(_WIN32)
static DWORD WINAPI win_thread_main(LPVOID p) {
  thread_arg* a = (thread_arg*)p;
  a->fn(a->raw);
  free(a);
  return 0;
}
#else
static void* posix_thread_main(void* p) {
  thread_arg* a = (thread_arg*)p;
  a->fn(a->raw);
  free(a);
  return NULL;
}
#endif

void ldb_start_thread(void (*fn)(void*), void* arg) {
  thread_arg* a = (thread_arg*)malloc(sizeof(thread_arg));
  a->fn = fn;
  a->raw = arg;
#if defined(_WIN32)
  HANDLE h = CreateThread(NULL, 0, win_thread_main, a, 0, NULL);
  if (h != NULL) {
    CloseHandle(h);
  } else {
    free(a);
  }
#else
  pthread_t tid;
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&tid, &attr, posix_thread_main, a) != 0) {
    free(a);
  }
  pthread_attr_destroy(&attr);
#endif
}
