// official_bloom_check.cc - the bloom membership oracle. Reads a kvdb-written
// .ldb's filter block and every user key, then asks the OFFICIAL
// FilterPolicy::KeyMayMatch whether each key is a maybe-match in the filter
// bytes the engine produced. A "no" is a FALSE NEGATIVE: the reader would
// skip a live key. This replaces a JS re-implementation that cried wolf on
// the healthy engine four times (doc/13 §4) - the reference implementation is
// both independent of kvdb and actually correct.
//
// Input (binary, written by the JS probe): magic "BLOOMORC1", then per
// filter block: u32 block_key, u32 filter_len, filter bytes, then u32
// nkeys, then nkeys × (u16 klen, key bytes).
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <cstring>
#include "leveldb/filter_policy.h"
#include "leveldb/slice.h"
int main(int argc, char** argv) {
  if (argc < 2) { fprintf(stderr, "usage: %s <oracle-input>\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { fprintf(stderr, "open %s failed\n", argv[1]); return 2; }
  char magic[8];
  if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "BLOOMORC", 8) != 0) {
    fprintf(stderr, "bad magic\n"); fclose(f); return 2;
  }
  const leveldb::FilterPolicy* policy = leveldb::NewBloomFilterPolicy(10);
  int blocks = 0, checked = 0, misses = 0;
  for (;;) {
    uint32_t bk = 0, flen = 0;
    if (fread(&bk, 4, 1, f) != 1) break;
    if (fread(&flen, 4, 1, f) != 1) break;
    std::vector<char> filt(flen);
    if (flen && fread(filt.data(), 1, flen, f) != flen) { fclose(f); return 2; }
    uint32_t nk = 0;
    if (fread(&nk, 4, 1, f) != 1) break;
    blocks++;
    for (uint32_t i = 0; i < nk; i++) {
      uint16_t kl = 0;
      if (fread(&kl, 2, 1, f) != 1) { fclose(f); return 2; }
      std::vector<char> key(kl);
      if (kl && fread(key.data(), 1, kl, f) != kl) { fclose(f); return 2; }
      leveldb::Slice fs(filt.data(), filt.size());
      if (!policy->KeyMayMatch(leveldb::Slice(key.data(), key.size()), fs)) {
        if (misses < 5) {
          printf("false negative: block_key=%u key=", bk);
          for (uint16_t j = 0; j < kl; j++) printf("%02x", (unsigned char)key[j]);
          printf("\n");
        }
        misses++;
      }
      checked++;
    }
  }
  fclose(f);
  delete policy;
  printf("official_bloom_check: blocks=%d checked=%d false_negatives=%d\n",
         blocks, checked, misses);
  return misses ? 3 : 0;
}
