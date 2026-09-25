// Controlled fixture: OFFICIAL engine writes a small DB with NO filter
// policy, so the metaindex block is empty - the exact case under dispute.
#include <cstdio>
#include <string>
#include "leveldb/db.h"
#include "leveldb/options.h"
#include "leveldb/write_batch.h"
int main(int argc, char** argv) {
  leveldb::Options o;               // no filter_policy
  o.create_if_missing = true;
  leveldb::DB* db;
  leveldb::Status s = leveldb::DB::Open(o, argv[1], &db);
  if (!s.ok()) { printf("%s\n", s.ToString().c_str()); return 1; }
  for (int i = 0; i < 20; i++) {
    char k[32], v[64];
    snprintf(k, sizeof(k), "key-%03d", i);
    snprintf(v, sizeof(v), "value-%03d", i);
    s = db->Put(leveldb::WriteOptions(), k, v);
    if (!s.ok()) { printf("%s\n", s.ToString().c_str()); return 1; }
  }
  db->CompactRange(nullptr, nullptr);
  delete db;
  printf("official fixture written\n");
  return 0;
}
