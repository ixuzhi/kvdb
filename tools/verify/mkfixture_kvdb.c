// Fixture through the kvdb C API, WITH a bloom filter so the independent
// format probe has filter bytes to validate (the false-negative class the
// static audit flagged lives exactly on this path).
#include <stdio.h>
#include <string.h>
#include "leveldb/c.h"
int main(int argc, char** argv) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_filterpolicy_t* bloom = leveldb_filterpolicy_create_bloom(10);
  leveldb_options_set_filter_policy(o, bloom);
  leveldb_t* db = NULL; char* err = NULL;
  db = leveldb_open(o, argv[1], &err);
  if (!db) { printf("open: %s\n", err ? err : "?"); return 1; }
  for (int i = 0; i < 20; i++) {
    char k[32], v[64];
    snprintf(k, sizeof(k), "key-%03d", i);
    snprintf(v, sizeof(v), "value-%03d", i);
    leveldb_put(db, NULL, k, strlen(k), v, strlen(v), &err);
    if (err) { printf("put: %s\n", err); return 1; }
  }
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_close(db);
  leveldb_filterpolicy_destroy(bloom);
  leveldb_options_destroy(o);
  printf("kvdb fixture written\n");
  return 0;
}
