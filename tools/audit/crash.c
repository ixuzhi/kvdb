/* crash.c — N6 随机崩溃点 + 跨引擎恢复（doc/14 新体系）
 *
 * write  模式：连续 put，每 32 条把进度写进 ack 文件并 fflush+fsync，
 *        然后被外部脚本在随机时刻 kill -9。
 * verify 模式：重开目录，检查 [0, ack) 全部在场且值正确、
 *        [ack, ack+65536) 只允许出现值正确的键（未确认写可能幸存），
 *        违反前缀性质 exit 3。
 * scan   模式：输出恢复出的键集摘要（两引擎恢复同一目录必须同集）。
 *
 * 用法: crash  write  <db> <ackfile> <nops>
 *       crash  verify <db> <ack>
 *       crash  scan   <db>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <leveldb/c.h>

static uint64_t fnv(uint64_t h, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
  return h;
}
static void val_for(int k, char* out) {
  int j;
  for (j = 0; j < 40; j++) out[j] = (char)(33 + ((k * 37 + j * 11) % 90));
  out[40] = 0;
}

static int mode_write(const char* db, const char* ackfile, int nops) {
  char* err = NULL;
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* d;
  FILE* ack;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  /* 本机参考库无 libsnappy，跨引擎恢复比对统一 no_compression */
  leveldb_options_set_compression(o, leveldb_no_compression);
  d = leveldb_open(o, db, &err);
  if (!d) { fprintf(stderr, "open: %s\n", err ? err : "?"); return 2; }
  ack = fopen(ackfile, "w");
  if (!ack) { perror("ackfile"); return 2; }
  for (i = 0; i < nops; i++) {
    char k[32], v[41];
    char* errp = NULL;
    snprintf(k, sizeof(k), "c%07d", i);
    val_for(i, v);
    leveldb_put(d, wo, k, strlen(k), v, 40, &errp);
    if (errp) { fprintf(stderr, "put: %s\n", errp); return 2; }
    if (i % 32 == 31) {
      fprintf(ack, "%d\n", i + 1);
      fflush(ack);
      fsync(fileno(ack));
    }
  }
  fprintf(ack, "DONE\n");
  fclose(ack);
  leveldb_close(d);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
  return 0;
}

static int mode_verify(const char* db, int ack) {
  char* err = NULL;
  leveldb_options_t* o = leveldb_options_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_t* d;
  int i;
  int extras = 0;
  leveldb_options_set_create_if_missing(o, 0);
  /* 只读打开也会触发恢复期建表，压缩选项必须与写侧一致 */
  leveldb_options_set_compression(o, leveldb_no_compression);
  d = leveldb_open(o, db, &err);
  if (!d) {
    fprintf(stderr, "reopen failed (may be legal after corruption): %s\n",
            err ? err : "?");
    return 5;
  }
  for (i = 0; i < ack; i++) { /* 已确认前缀必须全在场且值正确 */
    char k[32], v[41];
    size_t vlen = 0;
    char* errp = NULL;
    char* gv;
    snprintf(k, sizeof(k), "c%07d", i);
    val_for(i, v);
    gv = leveldb_get(d, ro, k, strlen(k), &vlen, &errp);
    if (errp) {
      fprintf(stderr, "GET-ERROR on acked key %s: %s\n", k, errp);
      return 3;
    }
    if (!gv) { fprintf(stderr, "MISSING acked key %s\n", k); return 3; }
    if (vlen != 40 || memcmp(gv, v, 40) != 0) {
      fprintf(stderr, "VALUE mismatch on acked key %s\n", k);
      return 3;
    }
    leveldb_free(gv);
  }
  for (i = ack; i < ack + 65536; i++) { /* 未确认区：值正确或缺失 */
    char k[32], v[41];
    size_t vlen = 0;
    char* errp = NULL;
    char* gv;
    snprintf(k, sizeof(k), "c%07d", i);
    val_for(i, v);
    gv = leveldb_get(d, ro, k, strlen(k), &vlen, &errp);
    if (errp) { fprintf(stderr, "GET-ERROR on extra key %s: %s\n", k, errp); return 3; }
    if (gv) {
      extras++;
      if (vlen != 40 || memcmp(gv, v, 40) != 0) {
        fprintf(stderr, "VALUE mismatch on unacked key %s\n", k);
        return 3;
      }
      leveldb_free(gv);
    }
  }
  leveldb_close(d);
  leveldb_readoptions_destroy(ro);
  leveldb_options_destroy(o);
  printf("verify ok ack=%d extras=%d\n", ack, extras);
  return 0;
}

static int mode_scan(const char* db) {
  char* err = NULL;
  leveldb_options_t* o = leveldb_options_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_iterator_t* it;
  leveldb_t* d;
  uint64_t h = 1469598103934665603ULL;
  int count = 0;
  leveldb_options_set_compression(o, leveldb_no_compression);
  d = leveldb_open(o, db, &err);
  if (!d) { fprintf(stderr, "open failed: %s\n", err ? err : "?"); return 5; }
  it = leveldb_create_iterator(d, ro);
  leveldb_iter_seek_to_first(it);
  while (leveldb_iter_valid(it)) {
    size_t kl, vl;
    const char* k = leveldb_iter_key(it, &kl);
    const char* v = leveldb_iter_value(it, &vl);
    h = fnv(h, k, kl); h = fnv(h, v, vl);
    count++;
    leveldb_iter_next(it);
  }
  {
    char* errp = NULL;
    leveldb_iter_get_error(it, &errp);
    if (errp) { fprintf(stderr, "iter: %s\n", errp); return 3; }
  }
  leveldb_iter_destroy(it);
  leveldb_readoptions_destroy(ro);
  leveldb_close(d);
  leveldb_options_destroy(o);
  printf("recovered=%d digest=%016llx\n", count, (unsigned long long)h);
  return 0;
}

int main(int argc, char** argv) {
  if (argc < 2) return 1;
  if (!strcmp(argv[1], "write") && argc == 5) return mode_write(argv[2], argv[3], atoi(argv[4]));
  if (!strcmp(argv[1], "verify") && argc == 4) return mode_verify(argv[2], atoi(argv[3]));
  if (!strcmp(argv[1], "scan") && argc == 3) return mode_scan(argv[2]);
  fprintf(stderr, "usage: crash write <db> <ackfile> <n> | verify <db> <ack> | scan <db>\n");
  return 1;
}
