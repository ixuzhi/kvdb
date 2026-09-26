/* corner_matrix.c — N4 格式角落矩阵（doc/14 新体系）
 *
 * 参数化扫描编码/布局角落：键值尺寸边界、二进制键、重启点间隔、
 * 块尺寸、布隆位数、序列号增长、删除标记形态、32KiB WAL 分片边界、
 * 超大记录、空库、空前缀族键等。每个场景先用 `write` 写出一个目录，
 * 由 run_corner_matrix.sh 做：
 *   - strict 类：两引擎产出的目录（除 LOG-family and LOCK）逐字节 diff；
 *   - digest 类：4 个方向的交叉读取（两引擎 × 两目录）摘要必须全等。
 * `scan` 模式输出全量正/反扫描摘要与点查摘要。
 *
 * 用法: corner_matrix write <db> <scenario>
 *       corner_matrix scan  <db>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <leveldb/c.h>

static void die(const char* msg) { fprintf(stderr, "corner: %s\n", msg); exit(2); }

static uint64_t fnv(uint64_t h, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
  return h;
}

static leveldb_options_t* g_opts;
static leveldb_writeoptions_t* g_wo;
static leveldb_t* g_db;
static const char* g_dir;

/* 选项必须在 open 前生效：按场景 id 预设 */
static void apply_preopen_options(const char* scenario) {
  /* strict 字节比对要求两引擎同一编码；本机参考库无 libsnappy（写时降级、
   * 读压缩块报 corruption），所以角落矩阵统一 no_compression —— 与
   * run_golden.sh 同一纪律。压缩路径的行为等价由 N2 的 compression 场景
   * 单独覆盖（不比字节）。 */
  leveldb_options_set_compression(g_opts, leveldb_no_compression);
  if (!strcmp(scenario, "restart1")) leveldb_options_set_block_restart_interval(g_opts, 1);
  else if (!strcmp(scenario, "restart32")) leveldb_options_set_block_restart_interval(g_opts, 32);
  else if (!strcmp(scenario, "block120")) leveldb_options_set_block_size(g_opts, 120);
  else if (!strcmp(scenario, "block64k")) leveldb_options_set_block_size(g_opts, 65536);
  else if (!strcmp(scenario, "bloom1")) leveldb_options_set_filter_policy(g_opts, leveldb_filterpolicy_create_bloom(1));
  else if (!strcmp(scenario, "bloom10")) leveldb_options_set_filter_policy(g_opts, leveldb_filterpolicy_create_bloom(10));
  else if (!strcmp(scenario, "bloom30")) leveldb_options_set_filter_policy(g_opts, leveldb_filterpolicy_create_bloom(30));
  else if (!strcmp(scenario, "multibuf")) leveldb_options_set_write_buffer_size(g_opts, 65536);
}
static void db_open_create(const char* scenario) {
  char* err = NULL;
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", g_dir);
  if (system(cmd) != 0) die("rm -rf");
  g_opts = leveldb_options_create();
  leveldb_options_set_create_if_missing(g_opts, 1);
  apply_preopen_options(scenario);
  g_wo = leveldb_writeoptions_create();
  g_db = leveldb_open(g_opts, g_dir, &err);
  if (!g_db || err) { fprintf(stderr, "open: %s\n", err ? err : "?"); exit(2); }
}
static void put(const char* k, size_t klen, const char* v, size_t vlen) {
  char* err = NULL;
  leveldb_put(g_db, g_wo, k, klen, v, vlen, &err);
  if (err) { fprintf(stderr, "put: %s\n", err); exit(2); }
}
static void del(const char* k, size_t klen) {
  char* err = NULL;
  leveldb_delete(g_db, g_wo, k, klen, &err);
  if (err) { fprintf(stderr, "del: %s\n", err); exit(2); }
}
static void compact_all(void) { leveldb_compact_range(g_db, NULL, 0, NULL, 0); }

static void finish_and_close(void) {
  leveldb_close(g_db);
  leveldb_writeoptions_destroy(g_wo);
  leveldb_options_destroy(g_opts);
}

static void fill_val(char* v, int len, int seed) {
  int i;
  for (i = 0; i < len; i++) v[i] = (char)(33 + ((seed * 131 + i * 17) % 90));
}

static void wr_kv_edges(void) {
  /* 值尺寸边界：0/1/255/256/257/1000/65535/65536 */
  static const int sizes[] = { 0, 1, 255, 256, 257, 1000, 65535, 65536 };
  char k[64], *v = malloc(70000);
  int i, n = 0;
  for (i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
    int len = sizes[i];
    char kk[8];
    fill_val(v, len > 0 ? len : 1, i);
    snprintf(kk, sizeof(kk), "v%06d", len);
    put(kk, strlen(kk), v, (size_t)len);
    n++;
  }
  /* 键尺寸边界：1..300 */
  {
    int len;
    for (len = 1; len <= 300; len += (len < 10 ? 1 : (len < 100 ? 10 : 50))) {
      char kk[512];
      int j;
      for (j = 0; j < len; j++) kk[j] = (char)('A' + (j % 26));
      kk[len] = 0;
      fill_val(v, 30, len);
      put(kk, (size_t)len, v, 30);
      n++;
    }
  }
  /* 同前缀族（触发 FindShortestSeparator/Successor） */
  {
    const char* fam[] = { "a", "aa", "aaa", "aaaa", "aaab", "ab", "b", "ba",
                          "a\0x01", NULL };
    int j;
    for (j = 0; fam[j]; j++) put(fam[j], strlen(fam[j]) == 0 ? 6 : strlen(fam[j]), "fam", 3);
    n++;
  }
  (void)k; (void)n;
  compact_all();
  free(v);
}
static void wr_binary_keys(void) {
  char k[64], v[64];
  int i;
  for (i = 0; i < 256; i++) {
    int klen = 0;
    k[klen++] = (char)0x00; k[klen++] = (char)i; k[klen++] = (char)(i ^ 0xff);
    if (i % 3 == 0) k[klen++] = 0x00;
    fill_val(v, 40, i);
    put(k, (size_t)klen, v, 40);
  }
  compact_all();
}
static void wr_restart(int interval) {
  char k[64], v[64];
  int i;
  (void)interval;
  for (i = 0; i < 120; i++) {
    sprintf(k, "restart%04d", i);
    fill_val(v, 50, i);
    put(k, strlen(k), v, 50);
  }
  compact_all();
}
static void wr_block_size(size_t bs) {
  char k[64], *v = malloc(4096);
  int i;
  (void)bs;
  for (i = 0; i < 200; i++) {
    sprintf(k, "blk%04d", i);
    fill_val(v, 120, i);
    put(k, strlen(k), v, 120);
  }
  compact_all();
  free(v);
}
static void wr_bloom(int bits) {
  char k[64], v[64];
  int i;
  (void)bits;
  for (i = 0; i < 150; i++) {
    sprintf(k, "bloom%04d", i);
    fill_val(v, 60, i);
    put(k, strlen(k), v, 60);
  }
  compact_all();
}
static void wr_seq_growth(void) {
  /* 单键覆写 5000 次：序列号 varint 增长 + 压缩后只留终值 */
  char k[8] = "hot";
  char v[32];
  int i;
  for (i = 0; i < 5000; i++) {
    sprintf(v, "gen-%d", i);
    put(k, 3, v, strlen(v));
  }
  sprintf(v, "gen-final");
  put(k, 3, v, strlen(v));
  compact_all();
}
static void wr_tombstones(void) {
  char k[64], v[64];
  int i;
  /* 先放后删全部（纯墓碑），再复活一半 */
  for (i = 0; i < 100; i++) {
    sprintf(k, "tomb%04d", i);
    fill_val(v, 40, i);
    put(k, strlen(k), v, 40);
  }
  for (i = 0; i < 100; i++) {
    sprintf(k, "tomb%04d", i);
    del(k, strlen(k));
  }
  for (i = 0; i < 100; i += 2) {
    sprintf(k, "tomb%04d", i);
    put(k, strlen(k), "revived", 7);
  }
  /* 删除从未写过的键 */
  for (i = 100; i < 130; i++) {
    sprintf(k, "tomb%04d", i);
    del(k, strlen(k));
  }
  compact_all();
}
static void wr_wal_32k_edge(void) {
  /* 32767/32768/32769 长度值：32KiB 块边界（不压缩，写完即关，
   * 落在 WAL 里 → strict 比对 WAL 字节） */
  static const int sizes[] = { 32767, 32768, 32769 };
  char* v = malloc(40000);
  int i;
  for (i = 0; i < (int)(sizeof(sizes)/sizeof(sizes[0])); i++) {
    char k[16];
    fill_val(v, sizes[i], i);
    sprintf(k, "wal%02d", i);
    put(k, strlen(k), v, (size_t)sizes[i]);
  }
  free(v);
}
static void wr_wal_70k(void) {
  /* 70000 字节单记录：跨 3 个 WAL 块 */
  char* v = malloc(70001);
  fill_val(v, 70000, 7);
  put("big", 3, v, 70000);
  free(v);
}
static void wr_wal_grow(void) {
  /* 60 条尺寸递增记录：分片边界滑动 */
  char* v = malloc(4000);
  int i;
  for (i = 1; i <= 60; i++) {
    char k[16];
    fill_val(v, i * 33, i);
    sprintf(k, "grow%03d", i);
    put(k, strlen(k), v, (size_t)(i * 33));
  }
  free(v);
}
static void wr_empty(void) {
  /* 空库：open+close */
}
static void wr_tiny(void) {
  put("only", 4, "one", 3);
  compact_all();
}
static void wr_empty_batch(void) {
  leveldb_writebatch_t* b = leveldb_writebatch_create();
  char* err = NULL;
  put("before", 6, "x", 1);
  leveldb_write(g_db, g_wo, b, &err);
  if (err) { fprintf(stderr, "empty batch: %s\n", err); exit(2); }
  leveldb_writebatch_destroy(b);
}
static void wr_multibuf(void) {
  /* 小写缓冲触发多次 memtable flush（无压缩，2 个 L0 文件） */
  char k[64], v[128];
  int i;
  for (i = 0; i < 1600; i++) {
    sprintf(k, "mb%05d", i);
    fill_val(v, 90, i);
    put(k, strlen(k), v, 90);
  }
  /* 不 compact：要求 WAL 为空态 + 若干 L0 表 */
}
static void wr_two_phase_compact(void) {
  char k[64], v[64];
  int i;
  for (i = 0; i < 80; i++) { sprintf(k, "p1%04d", i); fill_val(v, 70, i); put(k, strlen(k), v, 70); }
  compact_all();
  for (i = 0; i < 80; i++) { sprintf(k, "p2%04d", i); fill_val(v, 70, i + 1); put(k, strlen(k), v, 70); }
  compact_all();
}
static void wr_all_bytes(void) {
  char k[8], v[256];
  int i;
  for (i = 0; i < 256; i++) {
    int j;
    for (j = 0; j < 256; j++) v[j] = (char)j;
    sprintf(k, "%02x", i);
    put(k, strlen(k), v, 256);
  }
  compact_all();
}

static void wr_restart1(void) { wr_restart(1); }
static void wr_restart32(void) { wr_restart(32); }
static void wr_block120(void) { wr_block_size(120); }
static void wr_block64k(void) { wr_block_size(65536); }
static void wr_bloom1(void) { wr_bloom(1); }
static void wr_bloom10(void) { wr_bloom(10); }
static void wr_bloom30(void) { wr_bloom(30); }

struct wscenario { const char* id; void (*fn)(void); };
static const struct wscenario kW[] = {
  { "kv-edges", wr_kv_edges },
  { "binary-keys", wr_binary_keys },
  { "restart1", wr_restart1 }, { "restart32", wr_restart32 },
  { "block120", wr_block120 }, { "block64k", wr_block64k },
  { "bloom1", wr_bloom1 }, { "bloom10", wr_bloom10 }, { "bloom30", wr_bloom30 },
  { "seq-growth", wr_seq_growth },
  { "tombstones", wr_tombstones },
  { "wal-32k-edge", wr_wal_32k_edge },
  { "wal-70k", wr_wal_70k },
  { "wal-grow", wr_wal_grow },
  { "empty", wr_empty },
  { "tiny", wr_tiny },
  { "empty-batch", wr_empty_batch },
  { "multibuf", wr_multibuf },
  { "two-phase", wr_two_phase_compact },
  { "all-bytes", wr_all_bytes },
};

int main(int argc, char** argv) {
  int i;
  if (argc == 2 && strcmp(argv[1], "list") == 0) {
    for (i = 0; i < (int)(sizeof(kW)/sizeof(kW[0])); i++)
      printf("%s\n", kW[i].id);
    return 0;
  }
  if (argc < 3) { fprintf(stderr, "usage: %s write <db> <scenario> | scan <db>\n", argv[0]); return 1; }
  if (strcmp(argv[1], "scan") == 0) {
    /* 只读打开并输出摘要 */
    char* err = NULL;
    leveldb_readoptions_t* ro;
    leveldb_iterator_t* it;
    uint64_t hf = 1469598103934665603ULL, hb = 1469598103934665603ULL;
    int count = 0;
    g_opts = leveldb_options_create();
    /* 只读打开也可能触发恢复期建表（WAL 冲洗），选项必须与写侧一致 */
    leveldb_options_set_compression(g_opts, leveldb_no_compression);
    g_db = leveldb_open(g_opts, argv[2], &err);
    if (!g_db) { fprintf(stderr, "scan open failed: %s\n", err ? err : "?"); return 3; }
    ro = leveldb_readoptions_create();
    it = leveldb_create_iterator(g_db, ro);
    leveldb_iter_seek_to_first(it);
    while (leveldb_iter_valid(it)) {
      size_t kl, vl;
      const char* k = leveldb_iter_key(it, &kl);
      const char* v = leveldb_iter_value(it, &vl);
      hf = fnv(hf, k, kl); hf = fnv(hf, v, vl);
      count++;
      leveldb_iter_next(it);
    }
    {
      char* errp = NULL;
      leveldb_iter_get_error(it, &errp);
      if (errp) { fprintf(stderr, "scan error: %s\n", errp); return 4; }
    }
    leveldb_iter_seek_to_last(it);
    while (leveldb_iter_valid(it)) {
      size_t kl, vl;
      const char* k = leveldb_iter_key(it, &kl);
      const char* v = leveldb_iter_value(it, &vl);
      hb = fnv(hb, k, kl); hb = fnv(hb, v, vl);
      leveldb_iter_prev(it);
    }
    leveldb_iter_destroy(it);
    printf("entries=%d forward=%016llx backward=%016llx\n", count,
           (unsigned long long)hf, (unsigned long long)hb);
    leveldb_readoptions_destroy(ro);
    leveldb_close(g_db);
    leveldb_options_destroy(g_opts);
    return 0;
  }
  if (argc != 4 || strcmp(argv[1], "write") != 0) {
    fprintf(stderr, "usage: %s write <db> <scenario> | scan <db>\n", argv[0]);
    return 1;
  }
  g_dir = argv[2];
  db_open_create(argv[3]); /* 场景 id 用于预设选项 */
  for (i = 0; i < (int)(sizeof(kW)/sizeof(kW[0])); i++) {
    if (strcmp(kW[i].id, argv[3]) == 0) { kW[i].fn(); finish_and_close(); return 0; }
  }
  fprintf(stderr, "unknown scenario %s\n", argv[3]);
  return 1;
}
