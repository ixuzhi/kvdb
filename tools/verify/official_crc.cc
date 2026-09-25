// Arbitration: official crc32c on an arbitrary block extent (offset,size):
// prints stored vs Mask(Value(type||data)).
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include "util/crc32c.h"
#include "util/coding.h"
int main(int argc, char** argv) {
  if (argc < 4) { printf("usage: %s file off size\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { printf("open failed\n"); return 2; }
  fseek(f, 0, SEEK_END); long total = ftell(f);
  long off = atol(argv[2]); size_t size = (size_t)atol(argv[3]);
  std::vector<char> all(total);
  fseek(f, 0, SEEK_SET);
  if (fread(all.data(), 1, all.size(), f) != all.size()) { fclose(f); return 2; }
  fclose(f);
  uint32_t stored = leveldb::DecodeFixed32(all.data() + off + size - 5);
  uint32_t type = (unsigned char)all[off + size - 1];
  std::string tp;
  tp.push_back((char)type);
  tp.append(all.data() + off, size - 5);
  uint32_t v = leveldb::crc32c::Value(tp.data(), tp.size());
  printf("extent off=%ld size=%zu content=%zu type=%u stored=0x%08x official Value=0x%08x official Mask=0x%08x %s\n",
         off, size, size - 5, type, stored, v, leveldb::crc32c::Mask(v),
         leveldb::crc32c::Mask(v) == stored ? "MATCH" : "*** MISMATCH ***");
  return 0;
}
