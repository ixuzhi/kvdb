// Ground truth: decode the first records of a leveldb log file with the
// OFFICIAL log::Reader, so the independent JS probe can be checked against
// the reference implementation itself.
#include <cstdio>
#include <cstdlib>
#include <string>
#include "db/log_reader.h"
#include "leveldb/env.h"
#include "leveldb/slice.h"
int main(int argc, char** argv) {
  leveldb::Env* env = leveldb::Env::Default();
  leveldb::SequentialFile* file;
  leveldb::Status s = env->NewSequentialFile(argv[1], &file);
  if (!s.ok()) { printf("open %s: %s\n", argv[1], s.ToString().c_str()); return 2; }
  int n = 0;
  leveldb::log::Reader reader(file, nullptr, true, 0);
  leveldb::Slice rec; std::string scratch;
  while (reader.ReadRecord(&rec, &scratch) && n < 3) {
    printf("record %d: offset=%llu size=%zu bytes=", n,
           (unsigned long long)reader.LastRecordOffset(), rec.size());
    size_t lim = rec.size() < 48 ? rec.size() : 48;
    for (size_t i = 0; i < lim; i++) printf("%02x", (unsigned char)rec.data()[i]);
    printf("%s\n", rec.size() > lim ? "..." : "");
    n++;
  }
  delete file;
  printf("records dumped: %d\n", n);
  return 0;
}
