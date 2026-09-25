// iterator_snapshot.c - independent verification of SNAPSHOT ISOLATION
// semantics (leveldb db_test Iterator/ConcurrentSnapshot spirit, written
// against an independent model rather than ported assertions).
//
// Scenario: a writer thread runs a deterministic put/delete stream with a
// model array under a mutex; a reader takes the model snapshot at a barrier,
// creates a DB iterator, and the writer keeps mutating (crossing memtable
// flushes and compactions). The reader's iteration must equal the model
// snapshot EXACTLY - not the final state. A reader that returns torn or
// post-snapshot data fails here even though most functional tests would
// not notice (they iterate a quiescent DB).
//
// Build (from repo root):
//   gcc -std=c11 -O2 -Iinclude tools/verify/iterator_snapshot.c \
//       build/libleveldb.a -o build/verify/iterator_snapshot.exe -lpthread
// Run: iterator_snapshot <seed> [keys] [rounds]
// Exit: 0 pass; 3 mismatch; 1 error.
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "leveldb/c.h"

typedef struct kv { char* k; size_t klen; char* v; size_t vlen; } kv;

static int kcmp(const char* a, size_t al, const char* b, size_t bl) {
  size_t n = al < bl ? al : bl;
  int r = n ? memcmp(a, b, n) : 0;
  if (r) return r;
  return al < bl ? -1 : (al > bl ? 1 : 0);
}

static uint64_t rng;
static uint64_t rnd(void) {
  uint64_t x = rng; x ^= x >> 12; x ^= x << 25; x ^= x >> 27; rng = x;
  return x * 0x2545F4914F6CDD1DULL;
}

static int NKEYS;
static leveldb_t* db;
static leveldb_options_t* OPT;
static leveldb_writeoptions_t* WOPT;
static leveldb_readoptions_t* ROPT;

// live model (sorted)
static kv* model; static size_t mcount;
static size_t lower_bound(const char* k, size_t kl) {
  size_t lo = 0, hi = mcount;
  while (lo < hi) { size_t mid = (lo + hi) / 2;
    if (kcmp(model[mid].k, model[mid].klen, k, kl) < 0) lo = mid + 1; else hi = mid; }
  return lo;
}
static void model_put(const char* k, size_t kl, const char* v, size_t vl) {
  size_t i = lower_bound(k, kl);
  if (i < mcount && kcmp(model[i].k, model[i].klen, k, kl) == 0) {
    free(model[i].v); model[i].v = malloc(vl ? vl : 1); memcpy(model[i].v, v, vl);
    model[i].vlen = vl; return;
  }
  model = realloc(model, (mcount + 1) * sizeof(kv));
  memmove(&model[i + 1], &model[i], (mcount - i) * sizeof(kv));
  model[i].k = malloc(kl ? kl : 1); memcpy(model[i].k, k, kl); model[i].klen = kl;
  model[i].v = malloc(vl ? vl : 1); memcpy(model[i].v, v, vl); model[i].vlen = vl;
  mcount++;
}
static void model_del(const char* k, size_t kl) {
  size_t i = lower_bound(k, kl);
  if (i < mcount && kcmp(model[i].k, model[i].klen, k, kl) == 0) {
    free(model[i].k); free(model[i].v);
    memmove(&model[i], &model[i + 1], (mcount - i - 1) * sizeof(kv));
    mcount--;
  }
}
static kv* model_copy(void) {
  kv* c = malloc(mcount * sizeof(kv) + 1);
  for (size_t i = 0; i < mcount; i++) {
    c[i].k = malloc(model[i].klen); memcpy(c[i].k, model[i].k, model[i].klen); c[i].klen = model[i].klen;
    c[i].v = malloc(model[i].vlen ? model[i].vlen : 1); memcpy(c[i].v, model[i].v, model[i].vlen); c[i].vlen = model[i].vlen;
  }
  return c;
}
static void model_free(kv* c, size_t n) {
  for (size_t i = 0; i < n; i++) { free(c[i].k); free(c[i].v); }
  free(c);
}

static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cv = PTHREAD_COND_INITIALIZER;
static int writer_done;
static kv* snapshot; static size_t snap_count; static int snap_ready;
static int fail_code; static char fail_msg[256];

static void fail(int code, const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  pthread_mutex_lock(&mu);
  if (!fail_code) { fail_code = code; vsnprintf(fail_msg, sizeof(fail_msg), fmt, ap); }
  pthread_mutex_unlock(&mu);
  va_end(ap);
}

static void* writer_main(void* arg) {
  (void)arg;
  for (int op = 0; op < 4000; op++) {
    pthread_mutex_lock(&mu);
    int k = (int)(rnd() % NKEYS);
    char kb[32], vb[1200];
    int kl = snprintf(kb, sizeof(kb), "key%06d", k);
    int del = (rnd() % 100) < 25;
    int vl = 0;
    if (!del) vl = snprintf(vb, sizeof(vb), "v%06d.%d.%-1000d", k, op, op);
    leveldb_writebatch_t* wb = leveldb_writebatch_create();
    if (del) leveldb_writebatch_delete(wb, kb, kl);
    else leveldb_writebatch_put(wb, kb, kl, vb, vl);
    char* werr = NULL;
    leveldb_write(db, WOPT, wb, &werr);
    leveldb_writebatch_destroy(wb);
    if (werr) { fail(1, "writer: %s", werr); leveldb_free(werr); }
    if (del) model_del(kb, (size_t)kl); else model_put(kb, (size_t)kl, vb, (size_t)vl);
    pthread_mutex_unlock(&mu);
  }
  pthread_mutex_lock(&mu); writer_done = 1; pthread_cond_broadcast(&cv);
  pthread_mutex_unlock(&mu);
  return NULL;
}

int main(int argc, char** argv) {
  rng = (argc > 1 ? strtoull(argv[1], NULL, 10) : 7) * 0x9E3779B97F4A7C15ULL + 5;
  NKEYS = argc > 2 ? atoi(argv[2]) : 2000;
  const char* path = argc > 3 ? argv[3] : "/tmp/kvdb-iter-snap";
  (void)argc;

  char* err = NULL;
  leveldb_destroy_db(OPT = leveldb_options_create(), path, &err);
  if (err) leveldb_free(err);
  leveldb_options_set_create_if_missing(OPT, 1);
  leveldb_options_set_write_buffer_size(OPT, 64 * 1024);  // force flushes
  ROPT = leveldb_readoptions_create();
  WOPT = leveldb_writeoptions_create();
  db = leveldb_open(OPT, path, &err);
  if (!db) { printf("open: %s\n", err ? err : "?"); return 1; }

  pthread_t wt;
  pthread_create(&wt, NULL, writer_main, NULL);

  // barrier: take the model snapshot and create the DB iterator while the
  // writer is mid-stream; then let the writer run to completion.
  pthread_mutex_lock(&mu);
  while (mcount < 50 && !writer_done) pthread_cond_wait(&cv, &mu);
  snapshot = model_copy(); snap_count = mcount;
  leveldb_iterator_t* it = leveldb_create_iterator(db, ROPT);  // captures its own snapshot
  snap_ready = 1;
  pthread_mutex_unlock(&mu);
  if (!it) { printf("iterator create failed\n"); return 1; }

  pthread_join(wt, NULL);

  // The iterator must now yield EXACTLY the barrier-time model.
  size_t seen = 0;
  leveldb_iter_seek_to_first(it);
  while (leveldb_iter_valid(it)) {
    size_t kl = 0, vl = 0;
    const char* k = leveldb_iter_key(it, &kl);
    const char* v = leveldb_iter_value(it, &vl);
    if (seen >= snap_count) { fail(3, "iterator yielded more entries than the snapshot (%zu > %zu)", seen + 1, snap_count); break; }
    if (kcmp(k, kl, snapshot[seen].k, snapshot[seen].klen) != 0) {
      fail(3, "snapshot entry %zu: key mismatch", seen); break;
    }
    if (vl != snapshot[seen].vlen || (vl && memcmp(v, snapshot[seen].v, vl) != 0)) {
      fail(3, "snapshot entry %zu: value mismatch (len %zu vs %zu)", seen, vl, snapshot[seen].vlen); break;
    }
    seen++;
    leveldb_iter_next(it);
  }
  if (!fail_code && seen != snap_count) fail(3, "iterator yielded %zu of %zu snapshot entries", seen, snap_count);
  // Reverse pass over the SAME snapshot: catches visibility bugs that only
  // show when walking backwards (old versions leaking out of the snapshot
  // are invisible to a forward scan - the snap_leak mutant's blind spot).
  if (!fail_code) {
    size_t rseen = snap_count;
    leveldb_iter_seek_to_last(it);
    while (leveldb_iter_valid(it) && rseen > 0) {
      size_t kl = 0, vl = 0;
      const char* k = leveldb_iter_key(it, &kl);
      const char* v = leveldb_iter_value(it, &vl);
      rseen--;
      if (kcmp(k, kl, snapshot[rseen].k, snapshot[rseen].klen) != 0 ||
          vl != snapshot[rseen].vlen || (vl && memcmp(v, snapshot[rseen].v, vl) != 0)) {
        fail(3, "reverse snapshot entry %zu mismatch", rseen);
        break;
      }
      leveldb_iter_prev(it);
    }
    if (!fail_code && rseen == 0 && leveldb_iter_valid(it))
      fail(3, "reverse scan yielded entries beyond the snapshot");
    if (!fail_code && rseen != 0) fail(3, "reverse scan yielded %zu of %zu entries", snap_count - rseen, snap_count);
  }
  leveldb_iter_destroy(it);

  if (fail_code) {
    printf("ITERATOR SNAPSHOT MISMATCH seed rng state: %s\n", fail_msg);
    return fail_code;
  }
  printf("iterator_snapshot OK (snapshot=%zu entries, writer ran 4000 ops across flushes)\n", snap_count);
  leveldb_close(db);
  leveldb_destroy_db(OPT, path, &err);
  if (err) leveldb_free(err);
  model_free(snapshot, snap_count);
  model_free(model, mcount);
  leveldb_writeoptions_destroy(WOPT);
  leveldb_readoptions_destroy(ROPT);
  leveldb_options_destroy(OPT);
  return 0;
}
