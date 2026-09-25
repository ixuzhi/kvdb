// reopen_probe.c - minimal black-box open+scan used by the corruption fuzzer.
// Exit codes (the fuzzer's crash oracle depends on this contract):
//   0  opened and full scan completed (status clean or reported on stdout)
//   4  open refused (corruption / IO error) - a legitimate answer
//   2  usage/other error
// A crash shows up as an abnormal exit (>=128 in msys bash) or a timeout -
// both are FAILURES for the fuzzer, whatever the engine intended.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "leveldb/c.h"
int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <db>\n", argv[0]); return 2; }
  leveldb_options_t* o = leveldb_options_create();
  leveldb_options_set_create_if_missing(o, 0);
  leveldb_readoptions_t* r = leveldb_readoptions_create();
  leveldb_t* db = NULL; char* err = NULL;
  db = leveldb_open(o, argv[1], &err);
  if (!db) {
    printf("OPEN_REFUSED %s\n", err ? err : "?");
    if (err) leveldb_free(err);
    leveldb_readoptions_destroy(r); leveldb_options_destroy(o);
    return 4;
  }
  leveldb_iterator_t* it = leveldb_create_iterator(db, r);
  long long n = 0;
  leveldb_iter_seek_to_first(it);
  while (leveldb_iter_valid(it)) { n++; leveldb_iter_next(it); }
  char* ierr = NULL;
  leveldb_iter_get_error(it, &ierr);
  printf("OPEN_OK entries=%lld iter_status=%s\n", n, ierr ? ierr : "OK");
  if (ierr) leveldb_free(ierr);
  leveldb_iter_destroy(it);
  leveldb_close(db);
  leveldb_readoptions_destroy(r);
  leveldb_options_destroy(o);
  return 0;
}
