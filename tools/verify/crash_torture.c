// crash_torture.c - crash-consistency check via PROCESS CRASH (independent
// of the fault-injection-free ported suite).
//
// writer mode: acknowledged (sync=true) puts one by one; after each ack the
// index is appended to a side log and fsync'd; then the process terminates
// ITSELF abruptly (TerminateProcess / SIGKILL - no cleanup, no flush).
// verify mode: reopen and assert the recovered state is EXACTLY a prefix
// [0, max_present] of the acknowledged sequence, with correct values, and
// max_present <= acknowledged count. Torn tail records may be dropped -
// that is allowed; losing or corrupting an acknowledged write is not.
//
// Honest boundary: on Windows this simulates process death, not power loss
// (the OS page cache survives process death). The sync=true path still
// matters - acknowledged writes are fsync'd before we record them. True
// power-loss semantics need a Linux host; see doc/13.
//
// Usage:
//   crash_torture write <db> <side> [ops]   (terminates itself mid-stream)
//   crash_torture verify <db> <side>
// Exit: verify 0 = prefix property holds; 3 = violated; 4 = open failed
// (reported, not silently swallowed).
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L   // fileno/fsync under -std=c11
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <io.h>
#endif

#include "leveldb/c.h"

#if defined(_WIN32)
#include <windows.h>
static void hard_exit(void) { TerminateProcess(GetCurrentProcess(), 0xC0DE); }
#else
#include <signal.h>
#include <unistd.h>
static void hard_exit(void) { signal(SIGKILL, SIG_DFL); raise(SIGKILL); }
#endif

static uint64_t rng;
static uint64_t rnd(void) {
  uint64_t x = rng; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static int do_write(const char* dbpath, const char* sidepath, int maxops) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_writeoptions_t* w = leveldb_writeoptions_create();
  leveldb_writeoptions_set_sync(w, 1);
  leveldb_t* db = NULL; char* err = NULL;
  db = leveldb_open(o, dbpath, &err);
  if (!db) { printf("open failed: %s\n", err ? err : "?"); return 1; }
  FILE* side = fopen(sidepath, "w");
  if (!side) { printf("side log open failed\n"); return 1; }
  int target = 50 + (int)(rnd() % (uint64_t)(maxops > 50 ? maxops - 50 : 1));
  for (int i = 0; i < target; i++) {
    char k[32], v[256];
    int kl = snprintf(k, sizeof(k), "k%08d", i);
    int vl = snprintf(v, sizeof(v), "v%08d.%d", i, (int)(rnd() % 1000));
    leveldb_put(db, w, k, kl, v, vl, &err);
    if (err) { printf("put %d failed: %s\n", i, err); leveldb_free(err); return 1; }
    fprintf(side, "%d\n", i);
    fflush(side);
#if defined(_WIN32)
    _commit(_fileno(side));
#else
    fsync(fileno(side));
#endif
    if (i == target - 1) {
      // crash AFTER acknowledging and recording the last write
      fflush(side);
#if !defined(_WIN32)
      fsync(fileno(side));
#endif
      hard_exit();
    }
  }
  fclose(side);
  leveldb_close(db);
  return 0;
}

static int do_verify(const char* dbpath, const char* sidepath, int drop_k) {
  // acknowledged count from the side log
  FILE* side = fopen(sidepath, "r");
  if (!side) { printf("side log missing\n"); return 3; }
  int ack = -1, line;
  while (fscanf(side, "%d", &line) == 1) ack = line;
  fclose(side);

  leveldb_options_t* o = leveldb_options_create();
  leveldb_options_set_create_if_missing(o, 0);
  leveldb_readoptions_t* r = leveldb_readoptions_create();
  leveldb_t* db = NULL; char* err = NULL;
  db = leveldb_open(o, dbpath, &err);
  if (!db) {
    printf("VERIFY open failed (ack=%d): %s\n", ack, err ? err : "?");
    return 4;
  }
  if (drop_k >= 0) {  // negative control: simulate a lost acknowledged write
    char dk[32]; int dkl = snprintf(dk, sizeof(dk), "k%08d", drop_k);
    char* derr = NULL;
    leveldb_writeoptions_t* dw = leveldb_writeoptions_create();
    leveldb_delete(db, dw, dk, dkl, &derr);
    leveldb_writeoptions_destroy(dw);
    if (derr) { printf("delete for negative control failed: %s\n", derr); return 1; }
  }
  leveldb_iterator_t* it = leveldb_create_iterator(db, r);
  long long count = 0; int maxn = -1; int bad = 0;
  leveldb_iter_seek_to_first(it);
  while (leveldb_iter_valid(it)) {
    size_t kl = 0, vl = 0;
    const char* k = leveldb_iter_key(it, &kl);
    const char* v = leveldb_iter_value(it, &vl);
    int n = -1;
    char expect[64];
    if (sscanf(k, "k%08d", &n) != 1 || n < 0 || n > ack) {
      printf("VERIFY bad key at %lld\n", count); bad = 1; break;
    }
    int evl = snprintf(expect, sizeof(expect), "v%08d.", n);
    if (vl < (size_t)evl || memcmp(v, expect, evl) != 0) {
      printf("VERIFY bad value for k%08d\n", n); bad = 1; break;
    }
    if (n > maxn) maxn = n;
    count++;
    leveldb_iter_next(it);
  }
  leveldb_iter_destroy(it);
  leveldb_close(db);
  leveldb_readoptions_destroy(r);
  leveldb_options_destroy(o);
  if (bad) return 3;
  if (count != (long long)maxn + 1) {
    printf("VERIFY NOT A PREFIX: count=%lld max=%d (gap in [0,%d])\n", count, maxn, maxn);
    return 3;
  }
  // Completeness: every write was acknowledged AFTER fsync, so losing any
  // of them is a violation. The prefix property alone tolerates the EMPTY
  // prefix - which is exactly how a "WAL never written" mutant slipped
  // through the first matrix run (mutation testing found this, not review).
  if (maxn != ack) {
    printf("VERIFY LOST ACKNOWLEDGED WRITES: max=%d ack=%d\n", maxn, ack);
    return 3;
  }
  printf("VERIFY prefix ok: count=%lld max=%d ack=%d\n", count, maxn, ack);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: %s write|verify <db> <side> [ops]\n", argv[0]);
    return 2;
  }
  rng = (uint64_t)time(NULL) * 6364136223846793005ULL + (uint64_t)(uintptr_t)argv[0];
  if (strcmp(argv[1], "write") == 0)
    return do_write(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 600);
  if (strcmp(argv[1], "verify") == 0)
    return do_verify(argv[2], argv[3], -1);
  if (strcmp(argv[1], "verify-drop") == 0)  // negative control: must report 3
    return do_verify(argv[2], argv[3], argc > 4 ? atoi(argv[4]) : 0);
  fprintf(stderr, "unknown mode\n");
  return 2;
}
