/* Public C API only; compile ONCE, link into two independent executables.
 * Usage: driver DB create|read|update|final wal|sst basic|edge
 * All reads compare a deterministic model (including complete ordered scans).
 * WAL means graceful close without explicit flush, NOT crash/power-loss testing.
 * Reopening a WAL may itself create SSTables. The runner checks physical files.
 * No Bloom, no compression; checksum verification and synchronous writes enabled.
 */
#include "leveldb/c.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BULK 80
#define MAX_ROWS 90
struct row { const char *key, *value; size_t kn, vn; int live; };
static struct row rows[MAX_ROWS];
static char keys[BULK][20], values[BULK][600];
static char large_value[40000]; /* Force WAL fragmentation across 32KiB blocks. */
static const char binary_key[] = {0, (char)255, 0, 'x'};
static const char binary_value[] = {0, (char)128, (char)255, 0, 'y'};
static int count, checks;
static const char *stage;

static void require(int ok, const char *what, int index) {
  ++checks;
  if (!ok) {
    fprintf(stderr, "FAIL stage=%s check=%s row=%d checks=%d\n",
            stage, what, index, checks);
    exit(1);
  }
}
static void no_error(char *err, const char *what) {
  if (err) {
    fprintf(stderr, "FAIL stage=%s operation=%s error=%s\n", stage, what, err);
    leveldb_free(err);
    exit(1);
  }
  ++checks;
}
static void add(const char *k, size_t kn, const char *v, size_t vn) {
  rows[count++] = (struct row){k, v, kn, vn, 1};
}
static int compare(const void *a, const void *b) {
  const struct row *x = a, *y = b;
  size_t n = x->kn < y->kn ? x->kn : y->kn;
  int c = memcmp(x->key, y->key, n);
  return c ? c : (x->kn > y->kn) - (x->kn < y->kn);
}
static void model(int edge, int updated) {
  int i;
  count = 0;
  for (i = 0; i < BULK; ++i) {
    snprintf(keys[i], sizeof(keys[i]), "key-%03d", i);
    memset(values[i], 'A' + i % 26, sizeof(values[i]));
    values[i][0] = (char)i;
    values[i][599] = (char)(255-i);
    add(keys[i], strlen(keys[i]), values[i], sizeof(values[i]));
    if (i == 10) { rows[count-1].value = "overwritten"; rows[count-1].vn = 11; }
    if (i == 20 || (updated && i == 30)) rows[count-1].live = 0;
    if (updated && i == 10) { rows[count-1].value = "peer-update"; rows[count-1].vn = 11; }
  }
  memset(large_value, 'L', sizeof(large_value));
  large_value[0] = 0; large_value[39999] = (char)255;
  add("large", 5, large_value, sizeof(large_value));
  add("peer-added", 10, "new", 3);
  rows[count-1].live = updated;
  if (edge) {
    add("", 0, updated ? "" : "empty-key", updated ? 0 : 9);
    add("empty-value", 11, "", 0);
    add(binary_key, sizeof(binary_key), binary_value, sizeof(binary_value));
    if (updated) { rows[count-1].value = "\xff\0\x80"; rows[count-1].vn = 3; }
  }
  qsort(rows, (size_t)count, sizeof(rows[0]), compare);
}
static void iter_row(leveldb_iterator_t *it, int i) {
  size_t n;
  const char *s;
  require(leveldb_iter_valid(it), "iterator missing entry", i);
  s = leveldb_iter_key(it, &n);
  require(n == rows[i].kn && !memcmp(s, rows[i].key, n), "iterator key/order", i);
  s = leveldb_iter_value(it, &n);
  require(n == rows[i].vn && !memcmp(s, rows[i].value, n), "iterator value", i);
}
static void verify(leveldb_t *db, leveldb_readoptions_t *ro) {
  int i, live = 0;
  char *err = NULL;
  leveldb_iterator_t *it;
  for (i = 0; i < count; ++i) {
    size_t n = 0;
    char *v = leveldb_get(db, ro, rows[i].key, rows[i].kn, &n, &err);
    no_error(err, "get");
    if (rows[i].live) {
      require(v != NULL, "get present (including empty value)", i);
      require(n == rows[i].vn && !memcmp(v, rows[i].value, n), "get value", i);
      ++live;
    } else require(v == NULL, "deleted/absent key", i);
    leveldb_free(v);
  }
  it = leveldb_create_iterator(db, ro);
  leveldb_iter_seek_to_first(it);
  for (i = 0; i < count; ++i) if (rows[i].live) {
    iter_row(it, i); leveldb_iter_next(it);
  }
  require(!leveldb_iter_valid(it), "forward scan exhausted", count);
  leveldb_iter_get_error(it, &err); no_error(err, "forward iterator");
  leveldb_iter_seek_to_last(it);
  for (i = count-1; i >= 0; --i) if (rows[i].live) {
    iter_row(it, i); leveldb_iter_prev(it);
  }
  require(!leveldb_iter_valid(it), "reverse scan exhausted", count);
  leveldb_iter_get_error(it, &err); no_error(err, "reverse iterator");
  for (i = 0; i < count; ++i) if (rows[i].live) {
    leveldb_iter_seek(it, rows[i].key, rows[i].kn); iter_row(it, i);
  }
  /* Missing key seeks to the following key, not just exact matches. */
  leveldb_iter_seek(it, "key-010x", 8);
  for (i = 0; i < count; ++i) if (rows[i].kn == 7 && !memcmp(rows[i].key, "key-011", 7)) iter_row(it, i);
  leveldb_iter_get_error(it, &err); no_error(err, "seek iterator");
  leveldb_iter_destroy(it);
  printf("verified live=%d get+forward+reverse+seek\n", live);
}
int main(int argc, char **argv) {
  leveldb_options_t *opt;
  leveldb_readoptions_t *ro;
  leveldb_writeoptions_t *wo;
  leveldb_t *db;
  char *err = NULL;
  int create, update, final, sst, edge, i;
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc != 5) {
    fprintf(stderr, "usage: %s DB create|read|update|final wal|sst basic|edge\n", argv[0]);
    return 2;
  }
  stage = argv[2];
  create = !strcmp(stage, "create"); update = !strcmp(stage, "update"); final = !strcmp(stage, "final");
  if (!create && !update && !final && strcmp(stage, "read")) return 2;
  if (strcmp(argv[3], "wal") && strcmp(argv[3], "sst")) return 2;
  if (strcmp(argv[4], "basic") && strcmp(argv[4], "edge")) return 2;
  sst = !strcmp(argv[3], "sst"); edge = !strcmp(argv[4], "edge");
  printf("START stage=%s storage=%s data=%s db=%s\n", stage, argv[3], argv[4], argv[1]);
  opt = leveldb_options_create(); ro = leveldb_readoptions_create(); wo = leveldb_writeoptions_create();
  leveldb_options_set_create_if_missing(opt, (unsigned char)create);
  leveldb_options_set_error_if_exists(opt, (unsigned char)create);
  leveldb_options_set_compression(opt, leveldb_no_compression);
  leveldb_options_set_write_buffer_size(opt, 8u << 20);
  leveldb_options_set_block_size(opt, 1024);
  leveldb_options_set_paranoid_checks(opt, 1);
  leveldb_readoptions_set_verify_checksums(ro, 1);
  leveldb_writeoptions_set_sync(wo, 1);
  db = leveldb_open(opt, argv[1], &err); no_error(err, "open"); require(db != NULL, "open handle", -1);
  model(edge, final);
  if (create) {
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    leveldb_iter_seek_to_first(it); require(!leveldb_iter_valid(it), "empty db", -1);
    leveldb_iter_get_error(it, &err); no_error(err, "empty iterator"); leveldb_iter_destroy(it);
    for (i = 0; i < count; ++i) if (rows[i].live) {
      leveldb_put(db, wo, rows[i].key, rows[i].kn, rows[i].value, rows[i].vn, &err); no_error(err, "put");
    }
    leveldb_put(db, wo, "key-020", 7, "deleted", 7, &err); no_error(err, "put deleted key");
    leveldb_delete(db, wo, "key-020", 7, &err); no_error(err, "delete");
    leveldb_put(db, wo, "key-010", 7, "old", 3, &err); no_error(err, "overwrite old");
    leveldb_put(db, wo, "key-010", 7, "overwritten", 11, &err); no_error(err, "overwrite new");
  } else verify(db, ro); /* Cross-engine read must pass BEFORE mutation. */
  if (update) {
    leveldb_writebatch_t *batch = leveldb_writebatch_create();
    leveldb_writebatch_put(batch, "peer-added", 10, "discard", 7);
    leveldb_writebatch_clear(batch);
    leveldb_writebatch_put(batch, "key-010", 7, "peer-update", 11);
    leveldb_writebatch_put(batch, "peer-added", 10, "temporary", 9);
    leveldb_writebatch_delete(batch, "peer-added", 10);
    leveldb_writebatch_put(batch, "peer-added", 10, "new", 3);
    leveldb_writebatch_delete(batch, "key-030", 7);
    if (edge) {
      leveldb_writebatch_put(batch, "", 0, "", 0);
      leveldb_writebatch_put(batch, binary_key, sizeof(binary_key), "\xff\0\x80", 3);
    }
    leveldb_write(db, wo, batch, &err); no_error(err, "write batch");
    leveldb_writebatch_destroy(batch);
    model(edge, 1);
  }
  if (create || update) {
    verify(db, ro);
    if (sst) {
      printf("COMPACT begin\n");
      leveldb_compact_range(db, NULL, 0, NULL, 0);
      printf("COMPACT returned\n");
      verify(db, ro);
    }
  }
  leveldb_close(db);
  leveldb_options_destroy(opt); leveldb_readoptions_destroy(ro); leveldb_writeoptions_destroy(wo);
  printf("PASS stage=%s checks=%d\n", stage, checks);
  return 0;
}
