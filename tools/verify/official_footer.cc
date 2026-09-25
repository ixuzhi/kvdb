// Ground truth: decode the 48-byte footer with the OFFICIAL Footer class.
#include <cstdio>
#include <string>
#include <vector>
#include "table/format.h"
int main(int argc, char** argv) {
  FILE* f = fopen(argv[1], "rb");
  if (!f) return 2;
  fseek(f, 0, SEEK_END); long total = ftell(f); fseek(f, 0, SEEK_SET);
  std::vector<char> all(total);
  if (fread(all.data(), 1, all.size(), f) != all.size()) { fclose(f); return 2; }
  fclose(f);
  leveldb::Slice input(all.data() + total - leveldb::Footer::kEncodedLength,
                       leveldb::Footer::kEncodedLength);
  leveldb::Footer footer;
  leveldb::Status s = footer.DecodeFrom(&input);
  printf("footer: %s metaindex=(%llu,%llu) index=(%llu,%llu) remaining=%d\n",
         s.ToString().c_str(),
         (unsigned long long)footer.metaindex_handle().offset(),
         (unsigned long long)footer.metaindex_handle().size(),
         (unsigned long long)footer.index_handle().offset(),
         (unsigned long long)footer.index_handle().size(),
         (int)input.size());
  return 0;
}
