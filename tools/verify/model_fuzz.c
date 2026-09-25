// model_fuzz.c - model-based randomized differential test, PUBLIC C API only.
//
// Independence from the ported suite: the oracle is not leveldb's own test
// expectations but the in-memory model maintained here (sorted array +
// binary search, bytewise comparator). Every Get and every full scan is
// cross-checked against the model, so a bug cannot hide by being
// "consistent with the suite". Keys and values are binary (embedded 0x00,
// 0xFF, prefix relationships) to probe paths string-based tests never
// touch. Deterministic: same seed -> same operation sequence.
//
// API contract note: the official C API dereferences every options pointer
// unconditionally (leveldb/db/c.cc: open/write/get/create_iterator/
// destroy_db all do `options->rep`), so NULL is NOT legal for any of them;
// this harness passes real option objects everywhere, like a well-behaved
// consumer.
//
// Usage: model_fuzz <seed> [steps] [dbpath]
// Exit: 0 pass; 3 model mismatch (details on stderr); 1 usage/IO error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "leveldb/c.h"

// ---------------------------------------------------------------- prng
static uint64_t rng_state, orig_seed;
static uint64_t rnd(void) { // xorshift64*
  uint64_t x = rng_state;
  x ^= x >> 12; x ^= x << 25; x ^= x >> 27;
  rng_state = x;
  return x * 0x2545F4914F6CDD1DULL;
}
static uint32_t rnd_n(uint32_t n) { return n ? (uint32_t)(rnd() % n) : 0; }

// ---------------------------------------------------------------- model
typedef struct ent { char* k; size_t klen; char* v; size_t vlen; } ent;
static ent* ents; static size_t nents, ents_cap;

static int kcmp(const char* a, size_t alen, const char* b, size_t blen) {
  size_t n = alen < blen ? alen : blen;
  int r = n ? memcmp(a, b, n) : 0;
  if (r) return r;
  return alen < blen ? -1 : (alen > blen ? 1 : 0);
}
static size_t lower_bound(const char* k, size_t klen) {
  size_t lo = 0, hi = nents;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (kcmp(ents[mid].k, ents[mid].klen, k, klen) < 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}
static ent* model_find(const char* k, size_t klen) {
  size_t i = lower_bound(k, klen);
  if (i < nents && kcmp(ents[i].k, ents[i].klen, k, klen) == 0) return &ents[i];
  return NULL;
}
static void model_put(const char* k, size_t klen, const char* v, size_t vlen) {
  size_t i = lower_bound(k, klen);
  if (i < nents && kcmp(ents[i].k, ents[i].klen, k, klen) == 0) {
    free(ents[i].v); ents[i].v = (char*)malloc(vlen ? vlen : 1);
    memcpy(ents[i].v, v, vlen); ents[i].vlen = vlen; return;
  }
  if (nents == ents_cap) {
    ents_cap = ents_cap ? ents_cap * 2 : 256;
    ents = (ent*)realloc(ents, ents_cap * sizeof(ent));
  }
  memmove(&ents[i + 1], &ents[i], (nents - i) * sizeof(ent));
  ents[i].k = (char*)malloc(klen ? klen : 1);
  memcpy(ents[i].k, k, klen); ents[i].klen = klen;
  ents[i].v = (char*)malloc(vlen ? vlen : 1);
  memcpy(ents[i].v, v, vlen); ents[i].vlen = vlen;
  nents++;
}
static void model_del(const char* k, size_t klen) {
  size_t i = lower_bound(k, klen);
  if (i < nents && kcmp(ents[i].k, ents[i].klen, k, klen) == 0) {
    free(ents[i].k); free(ents[i].v);
    memmove(&ents[i], &ents[i + 1], (nents - i - 1) * sizeof(ent));
    nents--;
  }
}
static void model_clear(void) {
  for (size_t i = 0; i < nents; i++) { free(ents[i].k); free(ents[i].v); }
  free(ents); ents = NULL; nents = ents_cap = 0;
}

// ---------------------------------------------------------------- keys/values
static const char* const k_hot[] = {
  "a", "aa", "a\0a", "a\0a\0", "\0", "\0\0", "\xff", "\xff\xfe", "z",
  "key", "key\0", "key\0longer", "longkey-with-embedded-\0-nul",
};
static const size_t k_hot_len[] = { 1, 2, 3, 4, 1, 2, 1, 2, 1, 3, 4, 8, 29 };
#define N_HOT (sizeof(k_hot) / sizeof(k_hot[0]))

static void gen_key(char** out, size_t* outlen) {
  if (rnd_n(100) < 45) {
    size_t i = rnd_n((uint32_t)N_HOT);
    *outlen = k_hot_len[i]; *out = (char*)malloc(*outlen);
    memcpy(*out, k_hot[i], *outlen);
  } else {
    size_t n = 1 + rnd_n(24);
    *out = (char*)malloc(n);
    for (size_t i = 0; i < n; i++) {
      uint32_t r = rnd_n(100);
      (*out)[i] = r < 15 ? '\0' : (r < 30 ? (char)0xff : (char)(rnd() & 0xff));
    }
    *outlen = n;
  }
}
static void gen_val(char** out, size_t* outlen) {
  size_t n = rnd_n(301);
  *out = (char*)malloc(n ? n : 1);
  for (size_t i = 0; i < n; i++) {
    uint32_t r = rnd_n(100);
    (*out)[i] = r < 20 ? '\0' : (r < 30 ? (char)0xff : (char)(rnd() & 0x7f));
  }
  *outlen = n;
}
static void hexdump(const char* p, size_t n, char* buf, size_t cap) {
  size_t o = 0, i;
  for (i = 0; i < n && o + 4 < cap; i++)
    o += (size_t)snprintf(buf + o, cap - o, "%02x", (unsigned char)p[i]);
  if (i < n && o + 4 < cap) snprintf(buf + o, cap - o, "..");
}

// ---------------------------------------------------------------- handles
// The official C API dereferences every options pointer unconditionally
// (leveldb/db/c.cc: open/write/get/create_iterator/destroy_db all do
// `options->rep`), so NULL is not a legal value for any of them.
static leveldb_t* db;
static leveldb_options_t* GOPT;
static leveldb_readoptions_t* ROPT;
static leveldb_writeoptions_t* WOPT;

static int fail(const char* what, int step, const char* k, size_t klen) {
  char kb[200];
  hexdump(k, klen, kb, sizeof(kb));
  fprintf(stderr, "MODEL MISMATCH seed=%llu step=%d op=%s key=%s model_live=%zu\n",
          (unsigned long long)orig_seed, step, what, kb, nents);
  return 3;
}

// ---------------------------------------------------------------- checks
static int check_get(const char* k, size_t klen, int step) {
  size_t vlen = 0;
  char* verr = NULL;
  char* v = leveldb_get(db, ROPT, k, klen, &vlen, &verr);
  if (verr) { fprintf(stderr, "get error: %s\n", verr); leveldb_free(verr); }
  ent* m = model_find(k, klen);
  if (m) {
    if (!v || vlen != m->vlen || (vlen && memcmp(v, m->v, vlen) != 0)) {
      int rc = fail("get-value", step, k, klen);
      if (v) leveldb_free(v);
      return rc;
    }
    leveldb_free(v);
  } else if (v) {
    leveldb_free(v);
    return fail("get-ghost", step, k, klen);
  }
  return 0;
}

static int check_scan(int step) {
  leveldb_iterator_t* it = leveldb_create_iterator(db, ROPT);
  if (!it) return fail("iterator-create", step, "", 0);
  int rc = 0;
  size_t seen = 0;
  char* prev = NULL; size_t prevlen = 0;
  leveldb_iter_seek_to_first(it);
  while (leveldb_iter_valid(it)) {
    size_t klen = 0, vlen = 0;
    const char* k = leveldb_iter_key(it, &klen);
    const char* v = leveldb_iter_value(it, &vlen);
    ent* m = model_find(k, klen);
    if (!m) { rc = fail("scan-ghost", step, k, klen); break; }
    if (m->vlen != vlen || (vlen && memcmp(m->v, v, vlen) != 0)) {
      rc = fail("scan-value", step, k, klen); break;
    }
    if (prev && kcmp(prev, prevlen, k, klen) >= 0) {
      rc = fail("scan-order", step, k, klen); break;
    }
    free(prev);
    prev = (char*)malloc(klen ? klen : 1);
    memcpy(prev, k, klen); prevlen = klen;
    seen++;
    leveldb_iter_next(it);
  }
  if (!rc) {
    char* err = NULL;
    leveldb_iter_get_error(it, &err);
    if (err) {
      fprintf(stderr, "iterator error: %s\n", err);
      leveldb_free(err);
      rc = fail("scan-status", step, "", 0);
    }
  }
  if (!rc && seen != nents) {
    fprintf(stderr, "count mismatch: db=%zu model=%zu\n", seen, nents);
    rc = fail("scan-count", step, "", 0);
  }
  free(prev);
  leveldb_iter_destroy(it);
  return rc;
}

static int open_db(const char* path, int create) {
  leveldb_options_set_create_if_missing(GOPT, (uint8_t)(create ? 1 : 0));
  char* err = NULL;
  db = leveldb_open(GOPT, path, &err);
  if (!db) {
    fprintf(stderr, "open failed: %s\n", err ? err : "?");
    if (err) leveldb_free(err);
    return 1;
  }
  return 0;
}

static int do_write_region(int delete_bias) {
  int nb = 1 + (int)rnd_n(8);
  leveldb_writebatch_t* wb = leveldb_writebatch_create();
  char* kh[8]; char* vh[8]; size_t kl[8], vl[8]; int isdel[8];
  for (int b = 0; b < nb; b++) {
    gen_key(&kh[b], &kl[b]);
    isdel[b] = delete_bias ? (rnd_n(100) < 55) : 0;
    if (isdel[b]) {
      vl[b] = 0; vh[b] = NULL;
      leveldb_writebatch_delete(wb, kh[b], (int)kl[b]);
    } else {
      gen_val(&vh[b], &vl[b]);
      leveldb_writebatch_put(wb, kh[b], (int)kl[b], vh[b], (int)vl[b]);
    }
  }
  char* werr = NULL;
  leveldb_write(db, WOPT, wb, &werr);
  int bad = werr != NULL;
  if (bad) fprintf(stderr, "write error: %s\n", werr);
  if (werr) leveldb_free(werr);
  leveldb_writebatch_destroy(wb);
  for (int b = 0; b < nb; b++) {
    if (!bad) {
      if (isdel[b]) model_del(kh[b], kl[b]);
      else model_put(kh[b], kl[b], vh[b], vl[b]);
    }
    free(kh[b]); if (vh[b]) free(vh[b]);
  }
  return bad;
}

int main(int argc, char** argv) {
  orig_seed = argc > 1 ? strtoull(argv[1], NULL, 10) : 1;
  int steps = argc > 2 ? atoi(argv[2]) : 30000;
  char path[512];
  snprintf(path, sizeof(path), "%s",
           argc > 3 ? argv[3] : "/tmp/kvdb-model-fuzz");
  rng_state = orig_seed * 0x9E3779B97F4A7C15ULL + 0x1234567;

  GOPT = leveldb_options_create();
  leveldb_options_set_write_buffer_size(GOPT, 256 * 1024);
  ROPT = leveldb_readoptions_create();
  WOPT = leveldb_writeoptions_create();

  char* derr = NULL;
  leveldb_destroy_db(GOPT, path, &derr);
  if (derr) leveldb_free(derr);
  if (open_db(path, 1)) return 1;

  for (int step = 0; step < steps; step++) {
    uint32_t roll = rnd_n(1000);
    int rc = 0;
    if (roll < 420)        rc = do_write_region(0);          // puts
    else if (roll < 540)   rc = do_write_region(1);          // put/del mix
    else if (roll < 700) {                                          // get
      size_t klen; char* k; gen_key(&k, &klen);
      rc = check_get(k, klen, step);
      free(k);
    } else if (roll < 800) {                                        // full scan
      rc = check_scan(step);
    } else if (roll < 820) {                                        // compact all
      leveldb_compact_range(db, NULL, 0, NULL, 0);
    } else if (roll < 850) {                                        // reopen
      leveldb_close(db);
      if (open_db(path, 0)) return 1;
      rc = check_scan(step);
    }
    if (rc == 3) return 3;
    if (rc) return 1;
  }
  {
    int rc = check_scan(-1);
    if (rc) return rc;
  }
  printf("model_fuzz seed=%llu steps=%d live=%zu OK\n",
         (unsigned long long)orig_seed, steps, nents);
  leveldb_close(db);
  model_clear();
  { char* e = NULL; leveldb_destroy_db(GOPT, path, &e); if (e) leveldb_free(e); }
  leveldb_writeoptions_destroy(WOPT);
  leveldb_readoptions_destroy(ROPT);
  leveldb_options_destroy(GOPT);
  return 0;
}
