/* Golden-workload driver for byte-level on-disk format comparison.
 *
 * ONE deterministic C program, compiled against the official leveldb/c.h and
 * linked twice (real leveldb, kvdb). scripts/run_golden.sh runs it per engine,
 * snapshots the produced DB directory and compares every file byte by byte,
 * then asks BOTH engines to verify the other's files and prints a content
 * digest. That covers both directions of binary compatibility:
 *   - identical bytes for identical logical operations (format writer)
 *   - identical logical reads of the other engine's bytes (format reader)
 *
 * usage: golden_driver create DB mode
 *        golden_driver verify DB mode
 *
 * Determinism: fixed options, no random data, no clock. The strict modes keep
 * every write in the memtable and drain it with one explicit
 * leveldb_compact_range, so no background-compaction race can reorder file
 * numbering. "levels" is deliberately exploratory (it triggers automatic
 * background compactions) and its byte layout may legitimately differ.
 */
#include "leveldb/c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_ROWS 4096

typedef struct {
  char key[24];
  size_t kn;
  char* val;
  size_t vn;
} row;

typedef struct {
  row rows[MAX_ROWS];
  int n;
  int deleted[MAX_ROWS];
  int delete_count;
  int compact; /* drain through one explicit manual compaction */
  struct {
    size_t block_size;
    int restart_interval;
    size_t write_buffer;
    size_t max_file_size;
    int bloom_bits;
    int sync;
  } cfg;
  leveldb_filterpolicy_t* bloom;
} work;

static char* err;

static void die(const char* what) {
  fprintf(stderr, "FAIL op=%s error=%s\n", what, err ? err : "(null)");
  exit(1);
}

static void check(const char* what) {
  if (err != NULL) die(what);
}

/* Compressible-but-non-trivial payloads that also cover every byte value, so
 * block prefix/delta encoding, checksums and bloom keys see real binary data.
 */
static size_t make_value(char* out, int i, size_t n) {
  if (n == 0) return 0;
  for (size_t j = 0; j < n; j++) {
    int k = (int)(((unsigned)i * 7u + j * 13u) & 0xffu);
    out[j] = (j % 11 == 0) ? (char)k : (char)('a' + (k % 26));
  }
  out[0] = (char)(i & 0x7f);
  out[n - 1] = (char)0xff;
  return n;
}

static size_t make_key(char* out, int i) {
  return (size_t)snprintf(out, 24, "golden-%06d-key", i);
}

static void plan_delete(work* w, int index);

static void add_row(work* w, int i, size_t value_len) {
  if (w->n >= MAX_ROWS) {
    fprintf(stderr, "too many rows\n");
    exit(2);
  }
  row* r = &w->rows[w->n++];
  r->kn = make_key(r->key, i);
  r->val = (char*)malloc(value_len ? value_len : 1);
  r->vn = make_value(r->val, i, value_len);
}

/* Rows are inserted in generation order; the bytewise comparator orders these
 * fixed-width keys identically in both engines.
 */
static void cfg_defaults(work* w) {
  w->cfg.block_size = 4096;
  w->cfg.restart_interval = 16;
  w->cfg.write_buffer = 8u << 20;
  w->cfg.max_file_size = 2u << 20;
  w->cfg.bloom_bits = 0;
  w->cfg.sync = 1;
  w->compact = 1;
  w->bloom = NULL;
}

static void build(work* w, const char* mode) {
  cfg_defaults(w);
  if (!strcmp(mode, "sst")) {
    w->cfg.block_size = 512;
    for (int i = 0; i < 400; i++) add_row(w, i, 64);
  } else if (!strcmp(mode, "sst-bloom")) {
    w->cfg.block_size = 512;
    w->cfg.bloom_bits = 10;
    for (int i = 0; i < 800; i++) add_row(w, i, 96);
  } else if (!strcmp(mode, "sst-restart1")) {
    w->cfg.block_size = 128;
    w->cfg.restart_interval = 1;
    for (int i = 0; i < 300; i++) add_row(w, i, 48);
  } else if (!strcmp(mode, "sst-bigblock")) {
    w->cfg.block_size = 8192;
    w->cfg.bloom_bits = 16;
    for (int i = 0; i < 500; i++) add_row(w, i, 220);
  } else if (!strcmp(mode, "wal")) {
    w->cfg.sync = 0;
    w->compact = 0;
    for (int i = 0; i < 400; i++) add_row(w, i, 64);
  } else if (!strcmp(mode, "wal-big")) {
    w->cfg.sync = 0;
    w->compact = 0;
    /* One record far beyond the 32KiB WAL block: FIRST/MIDDLE/LAST framing. */
    add_row(w, 1, 100000);
    for (int i = 2; i < 12; i++) add_row(w, i, 32);
    add_row(w, 12, 32768 - 7); /* exactly one full WAL block */
    add_row(w, 13, 32768 - 8); /* one byte short of a full block */
  } else if (!strcmp(mode, "wal-frag")) {
    w->cfg.sync = 0;
    w->compact = 0;
    /* Alternating sizes push records across block boundaries. */
    for (int i = 0; i < 60; i++) add_row(w, i, (size_t)(i * 613));
  } else if (!strcmp(mode, "edge")) {
    w->cfg.block_size = 512;
    w->cfg.bloom_bits = 10;
    row* r = &w->rows[w->n++];
    r->kn = 0; /* empty key with empty value */
    r->val = (char*)malloc(1);
    r->vn = 0;
    r = &w->rows[w->n++];
    r->kn = 4; /* binary key: 00 ff 00 78 */
    memcpy(r->key, "\0\xff\0x", 4);
    r->val = (char*)malloc(4);
    memcpy(r->val, "\0\xff\x80Z", 4);
    r->vn = 4;
    for (int i = 0; i < 20; i++) add_row(w, i + 100, 300);
    plan_delete(w, 1);
    plan_delete(w, 4);
  } else if (!strcmp(mode, "tomb")) {
    w->cfg.block_size = 512;
    for (int i = 0; i < 600; i++) add_row(w, i, 64);
    for (int i = 0; i < 600; i += 2) plan_delete(w, i);
  } else if (!strcmp(mode, "levels")) {
    w->cfg.block_size = 256;
    w->cfg.write_buffer = 16u << 10;
    w->cfg.max_file_size = 32u << 10;
    w->compact = 1;
    for (int i = 0; i < 900; i++) add_row(w, i, 200);
  } else {
    fprintf(stderr, "unknown mode %s\n", mode);
    exit(2);
  }
}

static void plan_delete(work* w, int index) {
  if (index < 0 || index >= w->n) {
    fprintf(stderr, "bad delete index %d\n", index);
    exit(2);
  }
  w->deleted[w->delete_count++] = index;
}

static void apply(work* w, leveldb_options_t* o, int create) {
  leveldb_options_set_create_if_missing(o, (unsigned char)create);
  leveldb_options_set_error_if_exists(o, (unsigned char)create);
  leveldb_options_set_compression(o, leveldb_no_compression);
  leveldb_options_set_block_size(o, w->cfg.block_size);
  leveldb_options_set_block_restart_interval(o, w->cfg.restart_interval);
  leveldb_options_set_write_buffer_size(o, w->cfg.write_buffer);
  leveldb_options_set_max_file_size(o, w->cfg.max_file_size);
  leveldb_options_set_paranoid_checks(o, 1);
  if (w->cfg.bloom_bits > 0) {
    // leveldb_options_destroy does not take ownership of the policy, so keep
    // the handle for the caller-side destroy in main.
    w->bloom = leveldb_filterpolicy_create_bloom(w->cfg.bloom_bits);
    leveldb_options_set_filter_policy(o, w->bloom);
  }
}

static void create_db(const char* path, work* w) {
  leveldb_options_t* o = leveldb_options_create();
  apply(w, o, 1);
  err = NULL;
  leveldb_t* db = leveldb_open(o, path, &err);
  check("open");
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_writeoptions_set_sync(wo, (unsigned char)w->cfg.sync);
  for (int i = 0; i < w->n; i++) {
    err = NULL;
    leveldb_put(db, wo, w->rows[i].key, w->rows[i].kn, w->rows[i].val,
                w->rows[i].vn, &err);
    check("put");
  }
  for (int i = 0; i < w->delete_count; i++) {
    const row* r = &w->rows[w->deleted[i]];
    err = NULL;
    leveldb_delete(db, wo, r->key, r->kn, &err);
    check("delete");
  }
  if (w->compact) {
    leveldb_compact_range(db, NULL, 0, NULL, 0);
  }
  leveldb_writeoptions_destroy(wo);
  leveldb_close(db);
  leveldb_options_destroy(o);
}

static unsigned long long fnv(unsigned long long h, const char* p, size_t n) {
  for (size_t i = 0; i < n; i++) {
    h = (h ^ (unsigned char)p[i]) * 1099511628211ULL;
  }
  return h;
}

static void verify_db(const char* path, work* w) {
  leveldb_options_t* o = leveldb_options_create();
  apply(w, o, 0);
  err = NULL;
  leveldb_t* db = leveldb_open(o, path, &err);
  check("reopen");
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_readoptions_set_verify_checksums(ro, 1);

  unsigned long long digest = 1469598103934665603ULL; /* FNV-1a 64 offset */
  unsigned long long live = 0;
  int deleted[MAX_ROWS];
  memset(deleted, 0, sizeof(deleted));
  for (int i = 0; i < w->delete_count; i++) deleted[w->deleted[i]] = 1;

  /* Point lookups, including the "absent" case that must not report an error. */
  for (int i = 0; i < w->n; i++) {
    size_t n = 0;
    err = NULL;
    char* v = leveldb_get(db, ro, w->rows[i].key, w->rows[i].kn, &n, &err);
    check("get");
    if (deleted[i]) {
      if (v != NULL || n != 0) {
        fprintf(stderr, "FAIL deleted key %d still present\n", i);
        exit(1);
      }
    } else {
      if (v == NULL || n != w->rows[i].vn ||
          memcmp(v, w->rows[i].val, n) != 0) {
        fprintf(stderr, "FAIL value mismatch at row %d (n=%zu want %zu)\n", i,
                n, w->rows[i].vn);
        exit(1);
      }
      digest = fnv(digest, v, n);
      digest = fnv(digest, w->rows[i].key, w->rows[i].kn);
      live++;
    }
    leveldb_free(v);
  }

  /* Full ordered scan both ways: catches index/restart/block layout issues. */
  leveldb_iterator_t* it = leveldb_create_iterator(db, ro);
  unsigned long long fdigest = 1469598103934665603ULL;
  unsigned long long fwd = 0;
  size_t expect_total = 0;
  for (int i = 0; i < w->n; i++)
    if (!deleted[i]) expect_total++;
  for (leveldb_iter_seek_to_first(it); leveldb_iter_valid(it);
       leveldb_iter_next(it)) {
    size_t kn, vn;
    const char* k = leveldb_iter_key(it, &kn);
    const char* v = leveldb_iter_value(it, &vn);
    fdigest = fnv(fdigest, k, kn);
    fdigest = fnv(fdigest, v, vn);
    fwd++;
  }
  if (fwd != expect_total) {
    fprintf(stderr, "FAIL forward scan count %llu want %zu\n", fwd,
            expect_total);
    exit(1);
  }
  err = NULL;
  leveldb_iter_get_error(it, &err);
  check("forward iterator");
  unsigned long long rdigest = 1469598103934665603ULL;
  unsigned long long rev = 0;
  for (leveldb_iter_seek_to_last(it); leveldb_iter_valid(it);
       leveldb_iter_prev(it)) {
    size_t kn, vn;
    /* Bind before use: the key/value lengths are output parameters, so they
     * must not be read as arguments of the same call that sets them.
     */
    const char* k = leveldb_iter_key(it, &kn);
    const char* v = leveldb_iter_value(it, &vn);
    rdigest = fnv(rdigest, k, kn);
    rdigest = fnv(rdigest, v, vn);
    rev++;
  }
  if (rev != expect_total) {
    fprintf(stderr, "FAIL reverse scan count %llu want %zu\n", rev,
            expect_total);
    exit(1);
  }
  err = NULL;
  leveldb_iter_get_error(it, &err);
  check("reverse iterator");
  leveldb_iter_destroy(it);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_options_destroy(o);
  printf("VERIFY live=%llu get-digest=%016llx fwd=%016llx rev=%016llx\n", live,
         digest, fdigest, rdigest);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s create|verify DB mode\n", argv[0]);
    return 2;
  }
  work w;
  memset(&w, 0, sizeof(w));
  build(&w, argv[3]);
  if (!strcmp(argv[1], "create")) {
    create_db(argv[2], &w);
    printf("CREATE ok rows=%d dels=%d mode=%s\n", w.n, w.delete_count, argv[3]);
  } else if (!strcmp(argv[1], "verify")) {
    verify_db(argv[2], &w);
  } else {
    fprintf(stderr, "bad phase %s\n", argv[1]);
    return 2;
  }
  for (int i = 0; i < w.n; i++) free(w.rows[i].val);
  if (w.bloom != NULL) leveldb_filterpolicy_destroy(w.bloom);
  return 0;
}
