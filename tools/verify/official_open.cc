#include <cstdio>
#include "leveldb/db.h"
#include "leveldb/options.h"
#include "leveldb/iterator.h"
int main(int argc, char** argv) {
  leveldb::Options o; o.create_if_missing = false;
  leveldb::DB* db = nullptr;
  leveldb::Status s = leveldb::DB::Open(o, argv[1], &db);
  printf("open: %s\n", s.ToString().c_str());
  if (!s.ok()) return 1;
  int n = 0; leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
  for (it->SeekToFirst(); it->Valid(); it->Next()) n++;
  printf("entries=%d iter_status=%s\n", n, it->status().ToString().c_str());
  delete it; delete db; return 0;
}
