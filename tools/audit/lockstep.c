/* lockstep.c — N3 同步锁步差分（doc/14 新体系）
 *
 * 同一随机种子驱动两条完全相同的操作序列，分别在两个引擎进程上执行；
 * 每一步把结果摘要写进 trace 文件，跑完后由 run_lockstep.sh 对两个
 * trace 文件做逐行 diff。任何一步逻辑结果不同（点查值、扫描摘要、
 * seek 窗口、快照读、重开状态）即为跨引擎行为分歧。
 *
 * 进程内同时用排序数组模型自检（自检失败 exit 2），所以这个驱动同时
 * 是"对模型正确"和"对官方引擎逐步等价"的双重证据。
 *
 * 用法: lockstep <seed> <nops> <db> <tracefile>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <leveldb/c.h>

static uint64_t rng_state;
static uint64_t rng_next(void) {
  rng_state ^= rng_state << 13;
  rng_state ^= rng_state >> 7;
  rng_state ^= rng_state << 17;
  return rng_state;
}

/* ---------- 排序数组模型 ---------- */
#define MODEL_MAX 4096
typedef struct { char k[512]; int klen; char v[2200]; int vlen; } mentry;
static mentry model[MODEL_MAX];
static int model_n = 0;

static int model_lower_bound(const char* k, int klen) {
  int lo = 0, hi = model_n;
  while (lo < hi) {
    int mid = (lo + hi) / 2;
    int c = memcmp(model[mid].k, k, (size_t)(model[mid].klen < klen ? model[mid].klen : klen));
    if (c == 0) c = model[mid].klen < klen ? -1 : (model[mid].klen > klen ? 1 : 0);
    if (c < 0) lo = mid + 1; else hi = mid;
  }
  return lo;
}
static int model_find(const char* k, int klen) {
  int i = model_lower_bound(k, klen);
  if (i < model_n && model[i].klen == klen && memcmp(model[i].k, k, (size_t)klen) == 0) return i;
  return -1;
}
static void model_put(const char* k, int klen, const char* v, int vlen) {
  int i = model_find(k, klen);
  if (i >= 0) { memcpy(model[i].v, v, (size_t)vlen); model[i].vlen = vlen; return; }
  i = model_lower_bound(k, klen);
  if (model_n >= MODEL_MAX) { fprintf(stderr, "model full\n"); exit(2); }
  memmove(&model[i + 1], &model[i], (size_t)(model_n - i) * sizeof(mentry));
  model_n++;
  memcpy(model[i].k, k, (size_t)klen); model[i].klen = klen;
  memcpy(model[i].v, v, (size_t)vlen); model[i].vlen = vlen;
}
static void model_del(const char* k, int klen) {
  int i = model_find(k, klen);
  if (i < 0) return;
  memmove(&model[i], &model[i + 1], (size_t)(model_n - 1 - i) * sizeof(mentry));
  model_n--;
}

/* ---------- FNV ---------- */
static uint64_t fnv(uint64_t h, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
  return h;
}

static FILE* g_trace;
static void trace(int opidx, const char* op, uint64_t result) {
  fprintf(g_trace, "%d\t%s\t%016llx\n", opidx, op, (unsigned long long)result);
}
static void die(const char* msg, int opidx) {
  fprintf(stderr, "lockstep INTERNAL-FAIL at op %d: %s\n", opidx, msg);
  exit(2);
}

static leveldb_t* g_db;
static leveldb_options_t* g_opts;
static leveldb_writeoptions_t* g_wo;
static leveldb_readoptions_t* g_ro;
static const char* g_dbpath;

static void db_reopen(void) {
  char* err = NULL;
  if (g_db) leveldb_close(g_db);
  g_db = leveldb_open(g_opts, g_dbpath, &err);
  if (!g_db || err) {
    fprintf(stderr, "reopen failed: %s\n", err ? err : "(null)");
    exit(2);
  }
}

/* key 生成：三种形状（普通/二进制含 \0/前缀族） */
static int make_key(int opidx, char* out) {
  uint64_t r = rng_next() % 1000;
  int n;
  if (r < 700) {
    n = sprintf(out, "k%06d", (int)(rng_next() % 1200));
  } else if (r < 850) {
    out[0] = (char)0x01; out[1] = (char)(rng_next() & 0xff);
    out[2] = (char)(rng_next() & 0xff); out[3] = 0x00;
    out[4] = (char)0xff; out[5] = (char)(rng_next() & 0xff);
    n = 6;
  } else {
    n = sprintf(out, "prefix-a%04d", (int)(rng_next() % 300));
  }
  (void)opidx;
  return n;
}
static int make_val(int opidx, char* out) {
  int len = 1 + (int)(rng_next() % (rng_next() % 50 == 0 ? 2000 : 60));
  int i;
  for (i = 0; i < len; i++) out[i] = (char)('0' + ((opidx + i) % 62) % 62);
  /* 值字符取可打印确定性序列 */
  for (i = 0; i < len; i++) out[i] = (char)(33 + ((opidx * 31 + i * 7) % 90));
  return len;
}

static uint64_t scan_digest(int backward) {
  leveldb_iterator_t* it = leveldb_create_iterator(g_db, g_ro);
  uint64_t h = 1469598103934665603ULL;
  if (backward) {
    leveldb_iter_seek_to_last(it);
    while (leveldb_iter_valid(it)) {
      size_t kl, vl;
      const char* k = leveldb_iter_key(it, &kl);
      const char* v = leveldb_iter_value(it, &vl);
      h = fnv(h, k, kl); h = fnv(h, v, vl);
      leveldb_iter_prev(it);
    }
  } else {
    leveldb_iter_seek_to_first(it);
    while (leveldb_iter_valid(it)) {
      size_t kl, vl;
      const char* k = leveldb_iter_key(it, &kl);
      const char* v = leveldb_iter_value(it, &vl);
      h = fnv(h, k, kl); h = fnv(h, v, vl);
      leveldb_iter_next(it);
    }
  }
  {
    char* errp = NULL;
    leveldb_iter_get_error(it, &errp);
    if (errp) { fprintf(stderr, "scan iter error: %s\n", errp); exit(2); }
  }
  leveldb_iter_destroy(it);
  return h;
}
static uint64_t model_digest(void) {
  uint64_t h = 1469598103934665603ULL;
  int i;
  for (i = 0; i < model_n; i++) {
    h = fnv(h, model[i].k, (size_t)model[i].klen);
    h = fnv(h, model[i].v, (size_t)model[i].vlen);
  }
  return h;
}
static uint64_t model_digest_desc(void) {
  uint64_t h = 1469598103934665603ULL;
  int i;
  for (i = model_n - 1; i >= 0; i--) {
    h = fnv(h, model[i].k, (size_t)model[i].klen);
    h = fnv(h, model[i].v, (size_t)model[i].vlen);
  }
  return h;
}

int main(int argc, char** argv) {
  int nops, i;
  char seedbuf[64];
  if (argc != 5) { fprintf(stderr, "usage: %s <seed> <nops> <db> <trace>\n", argv[0]); return 1; }
  rng_state = (uint64_t)strtoull(argv[1], NULL, 10) * 2654435761ULL + 88172645463325252ULL;
  nops = atoi(argv[2]);
  g_dbpath = argv[3];
  g_trace = fopen(argv[4], "w");
  if (!g_trace) { perror("trace"); return 1; }
  snprintf(seedbuf, sizeof(seedbuf), "%s", argv[1]);

  {
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", g_dbpath);
    if (system(cmd) != 0) return 2;
  }
  g_opts = leveldb_options_create();
  leveldb_options_set_create_if_missing(g_opts, 1);
  /* 官方参考库在本机无 libsnappy（写时降级、读压缩块报 corruption），
   * 跨引擎比对统一 no_compression —— 与 run_golden/run_interop 同纪律 */
  leveldb_options_set_compression(g_opts, leveldb_no_compression);
  g_wo = leveldb_writeoptions_create();
  g_ro = leveldb_readoptions_create();
  db_reopen();

  for (i = 0; i < nops; i++) {
    uint64_t r = rng_next() % 100;
    char k[512], v[2200];
    int klen, vlen;
    klen = make_key(i, k);
    if (r < 35) { /* put */
      char* err = NULL;
      vlen = make_val(i, v);
      leveldb_put(g_db, g_wo, k, (size_t)klen, v, (size_t)vlen, &err);
      if (err) die(err, i);
      model_put(k, klen, v, vlen);
      trace(i, "put", (uint64_t)klen * 1000003u + (uint64_t)vlen);
    } else if (r < 45) { /* delete */
      char* err = NULL;
      leveldb_delete(g_db, g_wo, k, (size_t)klen, &err);
      if (err) die(err, i);
      model_del(k, klen);
      trace(i, "del", 0);
    } else if (r < 62) { /* get：跨引擎等值 + 对模型自检 */
      size_t vlen2 = 0; char* err = NULL;
      char* gv = leveldb_get(g_db, g_ro, k, (size_t)klen, &vlen2, &err);
      int mi = model_find(k, klen);
      if (err) die(err, i);
      if (mi >= 0) {
        if (!gv) die("model says present, db says missing", i);
        if ((int)vlen2 != model[mi].vlen || memcmp(gv, model[mi].v, vlen2) != 0)
          die("value mismatch vs model", i);
      } else if (gv) die("model says missing, db says present", i);
      trace(i, gv ? "getV" : "getM", gv ? fnv(1469598103934665603ULL, gv, vlen2) : 0);
      if (gv) leveldb_free(gv);
    } else if (r < 70) { /* 批量 10 op */
      leveldb_writebatch_t* b = leveldb_writebatch_create();
      int j;
      char* err = NULL;
      for (j = 0; j < 10; j++) {
        int kj = make_key(i, k);
        if (rng_next() & 1) {
          vlen = make_val(i + j, v);
          leveldb_writebatch_put(b, k, (size_t)kj, v, (size_t)vlen);
          model_put(k, kj, v, vlen);
        } else {
          leveldb_writebatch_delete(b, k, (size_t)kj);
          model_del(k, kj);
        }
      }
      leveldb_write(g_db, g_wo, b, &err);
      if (err) die(err, i);
      leveldb_writebatch_destroy(b);
      trace(i, "batch", 0);
    } else if (r < 80) { /* scan（正/反） */
      uint64_t d = scan_digest(0), dm = model_digest();
      if (d != dm) die("forward scan != model", i);
      trace(i, "scan", d);
    } else if (r < 85) { /* seek 窗口（最多 5 条） */
      leveldb_iterator_t* it = leveldb_create_iterator(g_db, g_ro);
      uint64_t h = 1469598103934665603ULL;
      int cnt = 0;
      leveldb_iter_seek(it, k, (size_t)klen);
      while (leveldb_iter_valid(it) && cnt < 5) {
        size_t kl, vl;
        const char* kk = leveldb_iter_key(it, &kl);
        const char* vv = leveldb_iter_value(it, &vl);
        h = fnv(h, kk, kl); h = fnv(h, vv, vl);
        leveldb_iter_next(it); cnt++;
      }
      leveldb_iter_destroy(it);
      trace(i, "seek5", h);
    } else if (r < 88) { /* 快照屏障 */
      const leveldb_snapshot_t* snap = leveldb_create_snapshot(g_db);
      leveldb_readoptions_t* ros = leveldb_readoptions_create();
      uint64_t h = 1469598103934665603ULL;
      leveldb_iterator_t* it;
      int j;
      leveldb_readoptions_set_snapshot(ros, snap);
      for (j = 0; j < 5; j++) { /* 快照内继续写 */
        char* err = NULL;
        int kj = make_key(i, k);
        vlen = make_val(i, v);
        leveldb_put(g_db, g_wo, k, (size_t)kj, v, (size_t)vlen, &err);
        if (err) die(err, i);
        model_put(k, kj, v, vlen);
      }
      it = leveldb_create_iterator(g_db, ros);
      leveldb_iter_seek_to_first(it);
      while (leveldb_iter_valid(it)) {
        size_t kl, vl;
        const char* kk = leveldb_iter_key(it, &kl);
        const char* vv = leveldb_iter_value(it, &vl);
        h = fnv(h, kk, kl); h = fnv(h, vv, vl);
        leveldb_iter_next(it);
      }
      leveldb_iter_destroy(it);
      leveldb_readoptions_destroy(ros);
      leveldb_release_snapshot(g_db, snap);
      trace(i, "snap", h);
    } else if (r < 92) { /* 反向全扫描：摘要按降序模型计算 */
      uint64_t d = scan_digest(1), dm = model_digest_desc();
      if (d != dm) die("backward scan != model", i);
      trace(i, "rscan", d);
    } else if (r < 95) { /* compact_range */
      char* err = NULL;
      leveldb_compact_range(g_db, NULL, 0, NULL, 0);
      (void)err;
      trace(i, "compact", 0);
    } else { /* reopen */
      db_reopen();
      trace(i, "reopen", 0);
    }
  }
  /* 终态全量校验 */
  {
    uint64_t d = scan_digest(0), dm = model_digest();
    if (d != dm) die("final scan != model", nops);
    trace(nops, "final", d);
  }
  leveldb_close(g_db);
  fclose(g_trace);
  return 0;
}
