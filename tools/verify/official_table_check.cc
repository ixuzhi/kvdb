// Ground truth: read a .ldb with the OFFICIAL table reader and checksum
// verification ENABLED (leveldb's default is verify_checksums=false, which is
// exactly why a bad block CRC can hide from every default-config test).
#include <cstdio>
#include "leveldb/env.h"
#include "leveldb/options.h"
#include "leveldb/table.h"
#include "leveldb/iterator.h"
int main(int argc, char** argv) {
  leveldb::RandomAccessFile* file;
  uint64_t size;
  leveldb::Env* env = leveldb::Env::Default();
  leveldb::Status s = env->GetFileSize(argv[1], &size);
  if (!s.ok()) { printf("size: %s\n", s.ToString().c_str()); return 2; }
  s = env->NewRandomAccessFile(argv[1], &file);
  if (!s.ok()) { printf("open: %s\n", s.ToString().c_str()); return 2; }
  leveldb::Table* table;
  s = leveldb::Table::Open(leveldb::Options(), file, size, &table);
  printf("table open: %s\n", s.ToString().c_str());
  if (!s.ok()) { delete file; return 1; }
  leveldb::ReadOptions ro;
  ro.verify_checksums = true;   // <- the check defaults skip
  leveldb::Iterator* it = table->NewIterator(ro);
  int n = 0;
  for (it->SeekToFirst(); it->Valid(); it->Next()) n++;
  printf("entries=%d status=%s\n", n, it->status().ToString().c_str());
  delete it; delete table; delete file;
  return 0;
}
