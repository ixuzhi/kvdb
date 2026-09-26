/* lifecycle.c — N5 交替生命周期 + 跨引擎修复（doc/14 新体系）
 *
 * 一个目录在两个引擎之间交替交接：官方写一程、kvdb 写一程、再官方、
 * 再 kvdb……每一程做一批确定性写/删/覆写/压缩；跑完 N 程后用
 * verify 校验目录内容与"纯函数重放的模型"一致。修复场景把 repair_db
 * 与 WAL/表文件的确定性破坏插进交接链。
 *
 * 用法: lifecycle <db> <phase> step      — 应用第 phase 程的确定性操作
 *       lifecycle <db> <phases> verify   — 重放 0..phases-1 得期望摘要并校验
 *       lifecycle <db> <phase> stepcompact — step 后追加 compact_range
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <leveldb/c.h>

static uint64_t fnv(uint64_t h, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
  return h;
}

#define PHASE_KEYS 100
#define MAX_MODEL 8192
typedef struct { int k; char v[32]; int alive; } mrow;
static mrow model[MAX_MODEL];
static int model_n = 0;

static int model_find(int k) {
  int i;
  for (i = 0; i < model_n; i++) if (model[i].k == k) return i;
  return -1;
}
static void model_set(int k, const char* v) {
  int i = model_find(k);
  if (i >= 0) { snprintf(model[i].v, sizeof(model[i].v), "%s", v); model[i].alive = 1; return; }
  model[model_n].k = k;
  snprintf(model[model_n].v, sizeof(model[model_n].v), "%s", v);
  model[model_n].alive = 1;
  model_n++;
}
static void model_del(int k) {
  int i = model_find(k);
  if (i >= 0) model[i].alive = 0;
}

static void phase_ops(leveldb_t* db, leveldb_writeoptions_t* wo, int p) {
  int k;
  for (k = PHASE_KEYS * p; k < PHASE_KEYS * (p + 1); k++) {
    char kk[32], v[32];
    snprintf(kk, sizeof(kk), "k%06d", k);
    snprintf(v, sizeof(v), "p%d", k);
    if (db) {
      char* errp = NULL;
      leveldb_put(db, wo, kk, strlen(kk), v, strlen(v), &errp);
      if (errp) { fprintf(stderr, "put: %s\n", errp); exit(2); }
    }
    model_set(k, v);
  }
  if (p > 0) {
    for (k = PHASE_KEYS * (p - 1); k < PHASE_KEYS * p; k++) {
      char kk[32];
      snprintf(kk, sizeof(kk), "k%06d", k);
      if (k % 5 == 0) {
        if (db) {
          char* errp = NULL;
          leveldb_delete(db, wo, kk, strlen(kk), &errp);
          if (errp) { fprintf(stderr, "del: %s\n", errp); exit(2); }
        }
        model_del(k);
      } else if (k % 3 == 0) {
        char v[32];
        snprintf(v, sizeof(v), "u%d", k);
        if (db) {
          char* errp = NULL;
          leveldb_put(db, wo, kk, strlen(kk), v, strlen(v), &errp);
          if (errp) { fprintf(stderr, "put: %s\n", errp); exit(2); }
        }
        model_set(k, v);
      }
    }
  }
}

static void do_step(const char* db, int phase, int compact) {
  char* err = NULL;
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* d;
  leveldb_options_set_create_if_missing(o, 1);
  /* 本机参考库无 libsnappy，跨引擎交接统一 no_compression */
  leveldb_options_set_compression(o, leveldb_no_compression);
  d = leveldb_open(o, db, &err);
  if (!d) { fprintf(stderr, "open %s: %s\n", db, err ? err : "?"); exit(2); }
  phase_ops(d, wo, phase);
  if (compact) leveldb_compact_range(d, NULL, 0, NULL, 0);
  leveldb_close(d);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}

static void do_verify(const char* db, int phases) {
  int p, k;
  uint64_t expect = 1469598103934665603ULL;
  for (p = 0; p < phases; p++) phase_ops(NULL, NULL, p); /* 只喂模型 */
  for (k = 0; k < PHASE_KEYS * phases; k++) {
    char kk[32];
    int i = model_find(k);
    if (i < 0) continue;
    if (!model[i].alive) continue;
    snprintf(kk, sizeof(kk), "k%06d", k);
    expect = fnv(expect, kk, strlen(kk));
    expect = fnv(expect, model[i].v, strlen(model[i].v));
  }
  /* 只读扫描实际目录 */
  {
    char* err = NULL;
    leveldb_options_t* o = leveldb_options_create();
    leveldb_readoptions_t* ro = leveldb_readoptions_create();
    leveldb_iterator_t* it;
    leveldb_options_set_compression(o, leveldb_no_compression);
    leveldb_t* d = leveldb_open(o, db, &err);
    uint64_t actual = 1469598103934665603ULL;
    int count = 0;
    if (!d) { fprintf(stderr, "open %s: %s\n", db, err ? err : "?"); exit(3); }
    it = leveldb_create_iterator(d, ro);
    leveldb_iter_seek_to_first(it);
    while (leveldb_iter_valid(it)) {
      size_t kl, vl;
      const char* key = leveldb_iter_key(it, &kl);
      const char* val = leveldb_iter_value(it, &vl);
      actual = fnv(actual, key, kl);
      actual = fnv(actual, val, vl);
      count++;
      leveldb_iter_next(it);
    }
    {
      char* errp = NULL;
      leveldb_iter_get_error(it, &errp);
      if (errp) { fprintf(stderr, "iter: %s\n", errp); exit(3); }
    }
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    leveldb_close(d);
    leveldb_options_destroy(o);
    printf("phases=%d entries=%d digest=%016llx\n", phases, count,
           (unsigned long long)actual);
    if (actual != expect) {
      fprintf(stderr, "VERIFY-FAIL: digest %016llx != expected %016llx\n",
              (unsigned long long)actual, (unsigned long long)expect);
      exit(4);
    }
  }
}

int main(int argc, char** argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s <db> <phase|phases> step|stepcompact|verify\n", argv[0]);
    return 1;
  }
  if (!strcmp(argv[3], "step")) do_step(argv[1], atoi(argv[2]), 0);
  else if (!strcmp(argv[3], "stepcompact")) do_step(argv[1], atoi(argv[2]), 1);
  else if (!strcmp(argv[3], "verify")) do_verify(argv[1], atoi(argv[2]));
  else return 1;
  return 0;
}
