/* option_matrix.c — N2 选项×行为全矩阵（doc/14 新体系）
 *
 * 目的：以官方公共头 c.h 的选项清单为准（而不是我们自己的测试清单），
 * 逐项给每个公开旋钮造一个确定性场景；同一场景 id 在两个引擎上各跑一遍，
 * 输出机器可读的字段行，由 run_option_matrix.sh 做跨引擎等值比对。
 *
 * 编译纪律（本体系的常设 ABI 替换证明）：本文件只 include 官方
 * leveldb/c.h，编译出的单个 .o 分别链官方库与 kvdb 库。若 kvdb 的
 * 导出面缺任何符号，链接当场失败。
 *
 * 用法: option_matrix <label> <workdir> <scenario>
 *   每行输出 "scenario<TAB>field<TAB>value"。
 *   退出码 0=场景自身断言全部成立；2=场景内部断言失败（引擎缺陷或
 *   预期写错，跑分脚本会单独标记）；1=用法错误。
 *   跨引擎等值判定不在这里做。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <leveldb/c.h>

static const char* g_label;
static const char* g_dir;
static const char* g_scn;

static void fail(const char* msg) {
  fprintf(stderr, "[%s/%s] INTERNAL-FAIL: %s\n", g_label, g_scn, msg);
  exit(2);
}
static void check(int ok, const char* msg) { if (!ok) fail(msg); }

/* ---------- 输出 ---------- */
static void emit(const char* field, const char* value) {
  printf("%s\t%s\t%s\n", g_scn, field, value);
}
static void emit_u64(const char* field, unsigned long long v) {
  printf("%s\t%s\t%llu\n", g_scn, field, v);
}
static void emit_s64(const char* field, long long v) {
  printf("%s\t%s\t%lld\n", g_scn, field, v);
}

/* ---------- FNV-1a 64 摘要 ---------- */
static uint64_t fnv(uint64_t h, const char* p, size_t n) {
  size_t i;
  for (i = 0; i < n; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
  return h;
}
static uint64_t fnv_str(uint64_t h, const char* s) {
  return fnv(h, s, strlen(s));
}

/* ---------- 独立 CRC32C（leveldb 语义，用于伪造 WAL 记录） ---------- */
static uint32_t crc32c_raw(uint32_t crc_in, const uint8_t* d, size_t n) {
  uint32_t l = ~crc_in;
  size_t i; int k;
  for (i = 0; i < n; i++) {
    l ^= d[i];
    for (k = 0; k < 8; k++)
      l = (l >> 1) ^ (0x82F63B78u & (uint32_t)(-(int32_t)(l & 1)));
  }
  return ~l;
}
static uint32_t crc32c_mask(uint32_t crc) {
  const uint32_t kMaskDelta = 0xa282ead8u;
  return ((crc >> 15) | (crc << 17)) + kMaskDelta;
}
static void put_fixed32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
  p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static void put_fixed64(uint8_t* p, uint64_t v) {
  int i; for (i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i));
}
static void put_varint32(uint8_t* p, int* off, uint32_t v) {
  while (v >= 128) { p[(*off)++] = (uint8_t)(v | 128); v >>= 7; }
  p[(*off)++] = (uint8_t)v;
}

/* ---------- 简易文件操作 ---------- */
static void rm_rf(const char* path) {
  char cmd[2048];
  int n = snprintf(cmd, sizeof(cmd), "rm -rf \"%s\"", path);
  if (n < 0 || n >= (int)sizeof(cmd)) fail("rm_rf path too long");
  if (system(cmd) != 0) fail("rm -rf failed");
}
/* 找目录里指定后缀的最大编号文件（leveldb 文件名 %06llu.ext） */
static int find_largest_file(const char* dir, const char* ext, char* out, size_t outn) {
  DIR* d = opendir(dir);
  struct dirent* e;
  long best = -1;
  if (!d) return 0;
  while ((e = readdir(d)) != NULL) {
    const char* dot = strrchr(e->d_name, '.');
    if (!dot || strcmp(dot + 1, ext) != 0) continue;
    long num = strtol(e->d_name, NULL, 10);
    if (num > best) best = num;
  }
  closedir(d);
  if (best < 0) return 0;
  snprintf(out, outn, "%s/%06ld.%s", dir, best, ext);
  return 1;
}
static void flip_byte_at(const char* path, long frac_pct) {
  FILE* f = fopen(path, "r+b");
  long size, off;
  int c;
  check(f != NULL, "flip_byte_at: open failed");
  fseek(f, 0, SEEK_END);
  size = ftell(f);
  off = size * frac_pct / 100;
  if (off < 0) off = 0;
  if (off >= size) off = size - 1;
  fseek(f, off, SEEK_SET);
  c = fgetc(f);
  fseek(f, off, SEEK_SET);
  fputc(c ^ 0x40, f);
  fclose(f);
}
static long file_size(const char* path) {
  struct stat st;
  if (stat(path, &st) != 0) return -1;
  return (long)st.st_size;
}

/* ---------- DB 帮手 ---------- */
static void check_err(char** errptr, const char* what) {
  if (*errptr) {
    fprintf(stderr, "[%s/%s] unexpected error at %s: %s\n",
            g_label, g_scn, what, *errptr);
    exit(2);
  }
}
static void count_files(const char* dir, const char* ext, int* count, long* bytes) {
  DIR* d = opendir(dir);
  struct dirent* e;
  *count = 0; *bytes = 0;
  if (!d) return;
  while ((e = readdir(d)) != NULL) {
    const char* dot = strrchr(e->d_name, '.');
    char full[1024];
    if (!dot || strcmp(dot + 1, ext) != 0) continue;
    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
    long sz = file_size(full);
    (*count)++;
    if (sz > 0) *bytes += sz;
  }
  closedir(d);
}

/* 全量正/反扫描摘要（把 key/value 对依次喂进 FNV） */
static uint64_t scan_digest(leveldb_t* db, int backward) {
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_iterator_t* it = leveldb_create_iterator(db, ro);
  uint64_t h = 1469598103934665603ULL;
  leveldb_iter_seek_to_first(it);
  if (backward) {
    leveldb_iter_seek_to_last(it);
    while (leveldb_iter_valid(it)) {
      size_t klen, vlen;
      const char* k = leveldb_iter_key(it, &klen);
      const char* v = leveldb_iter_value(it, &vlen);
      h = fnv(h, k, klen); h = fnv(h, v, vlen);
      leveldb_iter_prev(it);
    }
  } else {
    while (leveldb_iter_valid(it)) {
      size_t klen, vlen;
      const char* k = leveldb_iter_key(it, &klen);
      const char* v = leveldb_iter_value(it, &vlen);
      h = fnv(h, k, klen); h = fnv(h, v, vlen);
      leveldb_iter_next(it);
    }
  }
  {
    char err[512] = {0};
    char* errp = NULL;
    leveldb_iter_get_error(it, &errp);
    if (errp) { snprintf(err, sizeof(err), "iter error: %s", errp); leveldb_free(errp); check(0, err); }
  }
  leveldb_iter_destroy(it);
  leveldb_readoptions_destroy(ro);
  return h;
}
/* 点查摘要：probe 集为 "key000".."keyNNN" 中留 3 的倍数（含缺失样本） */
static uint64_t gets_digest(leveldb_t* db, int nkeys) {
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  uint64_t h = 1469598103934665603ULL;
  int i;
  for (i = 0; i < nkeys; i += 3) {
    char k[64]; size_t vlen = 0; char* err = NULL;
    char* v;
    snprintf(k, sizeof(k), "key%03d", i);
    v = leveldb_get(db, ro, k, strlen(k), &vlen, &err);
    if (err) { h = fnv_str(h, "ERR"); leveldb_free(err); continue; }
    if (!v) { h = fnv_str(h, "MISS"); continue; }
    h = fnv(h, v, vlen);
    leveldb_free(v);
  }
  leveldb_readoptions_destroy(ro);
  return h;
}
static void put_kn(leveldb_t* db, leveldb_writeoptions_t* wo, int i, const char* val) {
  char k[64]; char* err = NULL;
  snprintf(k, sizeof(k), "key%03d", i);
  leveldb_put(db, wo, k, strlen(k), val, strlen(val), &err);
  check_err(&err, "put");
}

/* ---------- 场景 ---------- */
static void open_ok(leveldb_options_t* o, const char* path, leveldb_t** out) {
  char* err = NULL;
  *out = leveldb_open(o, path, &err);
  if (*out == NULL || err) {
    fprintf(stderr, "[%s/%s] open failed: %s\n", g_label, g_scn, err ? err : "(null)");
    exit(2);
  }
}
/* 尝试打开，只报成败不 abort（错误场景用） */
static int open_class(leveldb_options_t* o, const char* path) {
  char* err = NULL;
  leveldb_t* db = leveldb_open(o, path, &err);
  if (db) { leveldb_close(db); return 0; }
  leveldb_free(err);
  return 1; /* 1 = open errored */
}

static void sc_defaults(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 50; i++) put_kn(db, wo, i, "val");
  leveldb_close(db);
  open_ok(o, g_dir, &db);
  emit_u64("scan", scan_digest(db, 0));
  emit_u64("rscan", scan_digest(db, 1));
  emit_u64("gets", gets_digest(db, 50));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_open_missing(void) {
  leveldb_options_t* o = leveldb_options_create();
  int cls;
  leveldb_options_set_create_if_missing(o, 0);
  rm_rf(g_dir);
  cls = open_class(o, g_dir);
  emit_s64("errclass", cls); /* 期望 1：两边都必须拒绝 */
  check(cls == 1, "open without create_if_missing must fail");
  leveldb_options_destroy(o);
}
static void sc_error_if_exists(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_t* db;
  int cls;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  leveldb_close(db);
  leveldb_options_set_error_if_exists(o, 1);
  cls = open_class(o, g_dir);
  emit_s64("errclass", cls); /* 期望 1 */
  check(cls == 1, "reopen with error_if_exists must fail");
  leveldb_options_destroy(o);
}
static void sc_paranoid_healthy(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_paranoid_checks(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 30; i++) put_kn(db, wo, i, "pv");
  leveldb_close(db);
  open_ok(o, g_dir, &db);
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
/* 往最新 WAL 追加一条手工记录：先零填充到块边界（读者当 trailer 丢弃），
 * 再写一条 FULL 记录，CRC 按官方语义（Mask(crc32c(type‖payload))）。
 * 用于伪造特定形状的恢复输入（M-1/F-2 探针）。 */
static void append_wal_record(const char* dir, const uint8_t* payload, int plen) {
  char logpath[1024];
  FILE* f;
  long size;
  int pad;
  check(find_largest_file(dir, "log", logpath, sizeof(logpath)), "no WAL found");
  size = file_size(logpath);
  check(size >= 0, "stat WAL failed");
  pad = (int)(32768 - (size % 32768)) % 32768;
  f = fopen(logpath, "ab");
  check(f != NULL, "append WAL open failed");
  if (pad > 0) {
    static const uint8_t zeros[4096] = { 0 };
    while (pad > 0) {
      int n = pad > 4096 ? 4096 : pad;
      fwrite(zeros, 1, (size_t)n, f);
      pad -= n;
    }
  }
  {
    uint8_t t = 1; /* FULL */
    uint32_t crc = crc32c_raw(crc32c_raw(0, &t, 1), payload, (size_t)plen);
    uint8_t header[7];
    put_fixed32(header, crc32c_mask(crc));
    header[4] = (uint8_t)(plen & 0xff);
    header[5] = (uint8_t)((plen >> 8) & 0xff);
    header[6] = t;
    fwrite(header, 1, 7, f);
    fwrite(payload, 1, (size_t)plen, f);
  }
  fclose(f);
}

/* M-1 探针：paranoid=1 + 一条 size<12、CRC 合法的 WAL 记录。
 * 官方 db_impl.cc:433 走 reporter.Corruption("log record too small")，
 * paranoid 下 Open 失败；kvdb db.c:456 把 status 重置为 OK 静默丢弃。
 * 期望跨引擎 errclass 分歧。 */
static void sc_paranoid_small_record(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i, cls;
  uint8_t payload[9];
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_paranoid_checks(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 30; i++) put_kn(db, wo, i, "pv");
  leveldb_close(db);
  memset(payload, 0, sizeof(payload)); /* 9B：不足 WriteBatch 12B 头 */
  append_wal_record(g_dir, payload, (int)sizeof(payload));
  cls = open_class(o, g_dir);
  emit_s64("errclass", cls);
  leveldb_options_destroy(o);
  leveldb_writeoptions_destroy(wo);
}
/* F-2 探针：往 WAL 追加一条 count==0 但带一个 put 负载的批次（CRC 正确）。
 * 官方 Parse 以 found != Count 判 corruption；paranoid=1 时 Open 失败。
 * 若对方 count==0 提前返回 OK → Open 成功 → errclass 分歧（doc/10 F-2）。 */
static void sc_paranoid_count0_wal(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i, cls;
  const char* ck = "crafted";
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_paranoid_checks(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 10; i++) put_kn(db, wo, i, "cv");
  leveldb_close(db);
  {
    /* WriteBatch rep: seq=200, count=0, 一个 put 条目 */
    uint8_t payload[256];
    int plen = 0;
    put_fixed64(payload, 200);
    put_fixed32(payload + 8, 0);
    plen = 12;
    payload[plen++] = 1; /* kTypeValue */
    put_varint32(payload, &plen, (uint32_t)strlen(ck));
    memcpy(payload + plen, ck, strlen(ck)); plen += (int)strlen(ck);
    put_varint32(payload, &plen, 5);
    memcpy(payload + plen, "cval!", 5); plen += 5;
    append_wal_record(g_dir, payload, plen);
  }
  cls = open_class(o, g_dir);
  emit_s64("errclass", cls);
  leveldb_options_destroy(o);
}
static void sc_wbuf_tiny(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i, nt; long nb;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_write_buffer_size(o, 1024);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 300; i++) put_kn(db, wo, i, "val-32-bytes-32-bytes-32-bytes-xx");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_close(db);
  open_ok(o, g_dir, &db);
  emit_u64("scan", scan_digest(db, 0));
  emit_u64("gets", gets_digest(db, 300));
  count_files(g_dir, "ldb", &nt, &nb);
  emit_s64("many_tables", nt >= 2);
  count_files(g_dir, "log", &nt, &nb);
  emit_s64("logs_nonempty", nt);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_maxfile_tiny(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i, nt; long nb;
  char val[120];
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_max_file_size(o, 8192);
  memset(val, 'x', sizeof(val) - 1);
  val[sizeof(val) - 1] = 0;
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 500; i++) put_kn(db, wo, i, val);
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_close(db);
  open_ok(o, g_dir, &db);
  emit_u64("scan", scan_digest(db, 0));
  count_files(g_dir, "ldb", &nt, &nb);
  emit_s64("many_tables", nt >= 3);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_block_small(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  char val[48];
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_block_size(o, 128);
  leveldb_options_set_block_restart_interval(o, 1);
  memset(val, 'y', sizeof(val) - 1); val[sizeof(val) - 1] = 0;
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 100; i++) put_kn(db, wo, i, val);
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_bloom(int bits) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_filter_policy(o, leveldb_filterpolicy_create_bloom(bits));
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 200; i++) {
    if (i % 10 == 7) continue; /* 留缺失样本 */
    put_kn(db, wo, i, "bv");
  }
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  emit_u64("gets", gets_digest(db, 200)); /* 含 MISS 项 */
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_options_destroy(o);
  leveldb_writeoptions_destroy(wo);
}
static void sc_cache(size_t cap) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_cache(o, leveldb_cache_create_lru(cap));
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 100; i++) put_kn(db, wo, i, "cachefill-40-bytes-40-bytes-40-xxxxx");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  emit_u64("scan", scan_digest(db, 0));
  emit_u64("gets", gets_digest(db, 100));
  leveldb_close(db);
  leveldb_options_destroy(o);
  leveldb_writeoptions_destroy(wo);
}
static void sc_ro_verify_checksums(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_t* db;
  int i, nt; long nb, before;
  char ldbs[64][1024]; int nl = 0;
  DIR* d; struct dirent* e;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 50; i++) put_kn(db, wo, i, "rv-24-bytes-24-bytes-24-byy");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_close(db);
  /* 翻转最大 .ldb 中部 1 字节（数据区） */
  d = opendir(g_dir);
  check(d != NULL, "opendir");
  while ((e = readdir(d)) != NULL) {
    const char* dot = strrchr(e->d_name, '.');
    if (!dot || (strcmp(dot + 1, "ldb") != 0 && strcmp(dot + 1, "sst") != 0)) continue;
    snprintf(ldbs[nl], sizeof(ldbs[0]), "%s/%s", g_dir, e->d_name);
    nl++;
    check(nl < 64, "too many tables");
  }
  closedir(d);
  check(nl > 0, "no table to corrupt");
  before = -1;
  for (i = 0; i < nl; i++) {
    long sz = file_size(ldbs[i]);
    if (sz > before) { before = sz; strcpy(ldbs[63], ldbs[i]); }
  }
  flip_byte_at(ldbs[63], 50);
  leveldb_readoptions_set_verify_checksums(ro, 1);
  open_ok(o, g_dir, &db);
  {
    /* 点查在损坏字节上的结果只记录类别（命中/缺失/报错），不做断言：
     * 翻转的字节可能落在 filter/index 区，合法结果因引擎而异——
     * 但跨引擎应同类别（相同字节 → 相同行为）。 */
    size_t vlen; char* err = NULL;
    char* v = leveldb_get(db, ro, "key010", 6, &vlen, &err);
    int cls2 = err ? 2 : (v ? 0 : 1);
    emit_s64("get_class", cls2);
    if (err) leveldb_free(err);
    if (v) leveldb_free(v);
  }
  /* 全扫描必须报错：迭代器 get_error */
  {
    leveldb_iterator_t* it = leveldb_create_iterator(db, ro);
    char* errp = NULL;
    leveldb_iter_seek_to_first(it);
    while (leveldb_iter_valid(it)) leveldb_iter_next(it);
    leveldb_iter_get_error(it, &errp);
    emit_s64("scan_errclass", errp ? 1 : 0);
    check(errp != NULL, "verify_checksums scan must report corruption");
    if (errp) leveldb_free(errp);
    leveldb_iter_destroy(it);
  }
  leveldb_close(db);
  count_files(g_dir, "ldb", &nt, &nb);
  emit_s64("tables", nt);
  leveldb_readoptions_destroy(ro);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_wo_sync(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_writeoptions_set_sync(wo, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 50; i++) put_kn(db, wo, i, "sv");
  leveldb_close(db);
  open_ok(o, g_dir, &db);
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_snapshot(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_readoptions_t* ros = leveldb_readoptions_create();
  leveldb_t* db;
  const leveldb_snapshot_t* snap;
  uint64_t h1 = 1469598103934665603ULL, h2 = 1469598103934665603ULL;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 20; i++) put_kn(db, wo, i, "one");
  snap = leveldb_create_snapshot(db);
  leveldb_readoptions_set_snapshot(ros, snap);
  for (i = 0; i < 20; i++) put_kn(db, wo, i, "two");
  for (i = 20; i < 30; i++) put_kn(db, wo, i, "post");
  /* 快照读 */
  for (i = 0; i < 30; i++) {
    char k[64]; size_t vlen; char* err = NULL; char* v;
    snprintf(k, sizeof(k), "key%03d", i);
    v = leveldb_get(db, ros, k, strlen(k), &vlen, &err);
    check_err(&err, "snapshot get");
    if (v) { h1 = fnv(h1, k, strlen(k)); h1 = fnv(h1, v, vlen); leveldb_free(v); }
    else h1 = fnv_str(h1, "MISS");
  }
  /* 迭代器走快照 */
  {
    leveldb_iterator_t* it = leveldb_create_iterator(db, ros);
    leveldb_iter_seek_to_first(it);
    while (leveldb_iter_valid(it)) {
      size_t klen, vlen;
      const char* k = leveldb_iter_key(it, &klen);
      const char* v = leveldb_iter_value(it, &vlen);
      h2 = fnv(h2, k, klen); h2 = fnv(h2, v, vlen);
      leveldb_iter_next(it);
    }
    leveldb_iter_destroy(it);
  }
  leveldb_release_snapshot(db, snap);
  emit_u64("snapget", h1);
  emit_u64("snapiter", h2);
  emit_u64("scan", scan_digest(db, 0));
  leveldb_readoptions_destroy(ros);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_iterator_seeks(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_t* db;
  leveldb_iterator_t* it;
  uint64_t h = 1469598103934665603ULL;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 30; i++) put_kn(db, wo, i, "iv");
  it = leveldb_create_iterator(db, ro);
  /* 齐全的 seek 形状：命中 / 间隙 / 越前 / 越后 / 首尾边界 / 方向切换 */
  {
    static const char* probes[] = { "key015", "key015a", "aaa", "zzz",
                                    "key000", "key029", "key100" };
    size_t pi;
    for (pi = 0; pi < sizeof(probes)/sizeof(probes[0]); pi++) {
      leveldb_iter_seek(it, probes[pi], strlen(probes[pi]));
      h = fnv_str(h, probes[pi]);
      h = fnv_str(h, leveldb_iter_valid(it) ? "V" : "I");
      if (leveldb_iter_valid(it)) {
        size_t klen, vlen2;
        const char* k = leveldb_iter_key(it, &klen);
        const char* v = leveldb_iter_value(it, &vlen2);
        h = fnv(h, k, klen); h = fnv(h, v, vlen2);
      }
    }
  }
  leveldb_iter_seek_to_first(it);
  leveldb_iter_prev(it); /* first 之前 prev → invalid */
  h = fnv_str(h, leveldb_iter_valid(it) ? "V" : "I");
  leveldb_iter_seek_to_last(it);
  leveldb_iter_next(it); /* last 之后 next → invalid */
  h = fnv_str(h, leveldb_iter_valid(it) ? "V" : "I");
  /* 方向切换链 */
  leveldb_iter_seek_to_first(it);
  leveldb_iter_next(it); leveldb_iter_next(it); leveldb_iter_prev(it); leveldb_iter_prev(it);
  if (leveldb_iter_valid(it)) {
    size_t klen;
    const char* k = leveldb_iter_key(it, &klen);
    h = fnv(h, k, klen);
  } else h = fnv_str(h, "I");
  leveldb_iter_destroy(it);
  emit_u64("walk", h);
  leveldb_readoptions_destroy(ro);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_batch(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  leveldb_writebatch_t* b1 = leveldb_writebatch_create();
  leveldb_writebatch_t* b2 = leveldb_writebatch_create();
  int i;
  char* werr = NULL;
  char k[64];
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  /* 1000 条大批次：put/delete 交错 */
  for (i = 0; i < 1000; i++) {
    snprintf(k, sizeof(k), "key%03d", i % 400);
    if (i % 5 == 4) { char* err = NULL; leveldb_writebatch_delete(b1, k, strlen(k)); (void)err; }
    else {
      char v[80]; char* err = NULL;
      snprintf(v, sizeof(v), "b1-%d", i);
      leveldb_writebatch_put(b1, k, strlen(k), v, strlen(v));
      (void)err;
    }
  }
  leveldb_write(db, wo, b1, &werr);
  check_err(&werr, "write b1");
  emit_u64("scan1", scan_digest(db, 0));
  /* append 合并 */
  for (i = 400; i < 430; i++) {
    char v[40]; char* err = NULL;
    snprintf(v, sizeof(v), "b2-%d", i);
    snprintf(k, sizeof(k), "key%03d", i);
    leveldb_writebatch_put(b2, k, strlen(k), v, strlen(v));
    (void)err;
  }
  leveldb_writebatch_append(b1, b2);
  leveldb_write(db, wo, b1, &werr);
  check_err(&werr, "write b1+appended b2");
  emit_u64("scan2", scan_digest(db, 0));
  /* clear 复用 */
  leveldb_writebatch_clear(b1);
  leveldb_writebatch_put(b1, "fresh", 5, "freshval", 8);
  leveldb_write(db, wo, b1, &werr);
  check_err(&werr, "write cleared b1");
  emit_u64("scan3", scan_digest(db, 0));
  leveldb_writebatch_destroy(b1);
  leveldb_writebatch_destroy(b2);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_empty_batch(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  leveldb_writebatch_t* b = leveldb_writebatch_create();
  char* werr = NULL;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  put_kn(db, wo, 1, "x");
  emit_u64("before", scan_digest(db, 0));
  leveldb_write(db, wo, b, &werr); /* 空批次必须无害；官方对 NULL errptr 断言 */
  check_err(&werr, "empty batch write");
  emit_u64("after", scan_digest(db, 0));
  leveldb_writebatch_destroy(b);
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_approx(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  const char* starts[3] = { "key000", "key100", "zzzz" };
  const char* limits[3] = { "key100", "key200", "zzzzz" };
  size_t sl[3] = { 6, 6, 4 }, ll[3] = { 6, 6, 5 };
  uint64_t sizes[3];
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 200; i++) put_kn(db, wo, i, "approx-40-bytes-40-bytes-40-bytes-40-xx");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  leveldb_approximate_sizes(db, 3, starts, sl, limits, ll, sizes);
  for (i = 0; i < 3; i++) {
    check(sizes[i] <= (uint64_t)-1, "impossible");
    emit_u64("size", sizes[i]); /* 实现定义，仅记录 */
  }
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_properties(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 60; i++) put_kn(db, wo, i, "prop-30-bytes-30-bytes-30-bytes-xx");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  {
    char* s = leveldb_property_value(db, "leveldb.stats");
    emit_s64("stats_present", s != NULL);
    if (s) leveldb_free(s);
    s = leveldb_property_value(db, "leveldb.sstables");
    emit_s64("sstables_present", s != NULL);
    if (s) leveldb_free(s);
    s = leveldb_property_value(db, "leveldb.no-such-property");
    emit_s64("unknown_absent", s == NULL);
    if (s) leveldb_free(s);
  }
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_compact_range(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 80; i++) put_kn(db, wo, i, "cr-20-bytes-20-bytes-20-bytes");
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  emit_u64("scan_full", scan_digest(db, 0));
  for (i = 80; i < 100; i++) put_kn(db, wo, i, "cr-20-bytes-20-bytes-20-bytes");
  leveldb_compact_range(db, "key010", 6, "key020", 6);
  emit_u64("scan_ranged", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_destroy_open(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  char* err = NULL;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  put_kn(db, wo, 0, "dv");
  leveldb_destroy_db(o, g_dir, &err);
  emit_s64("errclass", err ? 1 : 0);
  if (err) leveldb_free(err);
  leveldb_close(db); /* 之后关闭必须不崩（句柄归还纪律） */
  leveldb_options_destroy(o);
}
static void sc_repair_healthy(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  uint64_t pre;
  int i;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_filter_policy(o, leveldb_filterpolicy_create_bloom(10));
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 40; i++) {
    if (i % 5 == 4) continue;
    put_kn(db, wo, i, "rp");
  }
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  pre = scan_digest(db, 0);
  leveldb_close(db);
  {
    char* err = NULL;
    leveldb_repair_db(o, g_dir, &err);
    check_err(&err, "repair_db");
  }
  open_ok(o, g_dir, &db);
  emit_u64("pre", pre);
  emit_u64("post", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static int rev_compare(void* st, const char* a, size_t al, const char* b, size_t bl) {
  (void)st;
  {
    size_t n = al < bl ? al : bl;
    int c = memcmp(a, b, n);
    if (c) return -c; /* 反向 */
    return al < bl ? 1 : (al > bl ? -1 : 0);
  }
}
static const char* rev_name(void* st) { (void)st; return "audit.reverse1"; }
static void rev_destructor(void* st) { (void)st; }
static void sc_comparator(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_comparator_t* rc =
      leveldb_comparator_create(NULL, rev_destructor, rev_compare, rev_name);
  leveldb_t* db;
  int i, cls;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_comparator(o, rc);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 20; i++) put_kn(db, wo, i, "cv");
  emit_u64("scan", scan_digest(db, 0)); /* 迭代序 = 比较器序，两边同比较器 → 可比 */
  leveldb_close(db);
  /* 比较器名不匹配必须拒绝打开（用 bytewise 写的目录） */
  {
    char dir2[1024];
    leveldb_options_t* o2 = leveldb_options_create();
    leveldb_t* db2;
    snprintf(dir2, sizeof(dir2), "%s-bw", g_dir);
    rm_rf(dir2);
    leveldb_options_set_create_if_missing(o2, 1);
    open_ok(o2, dir2, &db2);
    leveldb_close(db2);
    cls = open_class(o, dir2);
    emit_s64("mismatch_errclass", cls); /* 期望 1 */
    check(cls == 1, "comparator name mismatch must fail");
    leveldb_options_destroy(o2);
    rm_rf(dir2);
  }
  leveldb_options_destroy(o);
  leveldb_writeoptions_destroy(wo);
}
static void sc_env_default(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_env_t* env = leveldb_create_default_env();
  leveldb_t* db;
  int i;
  char* td;
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_env(o, env);
  td = leveldb_env_get_test_directory(env);
  emit_s64("testdir_present", td != NULL);
  if (td) leveldb_free(td);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 20; i++) put_kn(db, wo, i, "ev");
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_env_destroy(env);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}
static void sc_empty_and_free(void) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_t* db;
  int i;
  char* perr = NULL;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  /* 空值、空键、缺失键的返回契约 */
  leveldb_put(db, wo, "", 0, "", 0, &perr);
  leveldb_put(db, wo, "k1", 2, "", 0, &perr);
  {
    size_t vl = 999; char* err = NULL;
    char* v = leveldb_get(db, ro, "", 0, &vl, &err);
    check_err(&err, "get empty key");
    emit_s64("empty_key_found", v != NULL);
    if (v) { emit_s64("empty_key_len0", vl == 0); leveldb_free(v); }
    else emit_s64("empty_key_len0", -1);
    v = leveldb_get(db, ro, "k1", 2, &vl, &err);
    check_err(&err, "get k1");
    emit_s64("empty_val_found", v != NULL);
    if (v) { emit_s64("empty_val_len0", vl == 0); leveldb_free(v); }
    else emit_s64("empty_val_len0", -1);
    v = leveldb_get(db, ro, "absent", 6, &vl, &err);
    emit_s64("missing_null_noerr", v == NULL && err == NULL);
    if (v) leveldb_free(v);
    if (err) leveldb_free(err);
  }
  for (i = 0; i < 10; i++) {
    char k[8]; snprintf(k, sizeof(k), "e%02d", i);
    leveldb_put(db, wo, k, 3, "", 0, &perr);
    check_err(&perr, "put empty val");
  }
  emit_u64("scan", scan_digest(db, 0));
  leveldb_close(db);
  leveldb_readoptions_destroy(ro);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}

/* 新发现探针（doc/14 首轮）：官方 env_posix 有进程内持锁集合，
 * kvdb env_posix 只有 fcntl：同进程二次 open / destroy-while-open 行为分歧 */
static void sc_lock_same_process(void) {
  leveldb_options_t* o = leveldb_options_create();
  char dir2[1024];
  char* err = NULL;
  leveldb_t *db1, *db2;
  leveldb_options_set_create_if_missing(o, 1);
  rm_rf(g_dir);
  db1 = leveldb_open(o, g_dir, &err);
  check(db1 != NULL, "first open failed");
  db2 = leveldb_open(o, g_dir, &err);
  emit_s64("double_open_errclass", db2 ? 0 : 1);
  if (db2) leveldb_close(db2);
  leveldb_close(db1);
  (void)err;
  /* destroy-while-open：官方报 IO error（锁被持有），目录保留 */
  rm_rf(g_dir);
  db1 = leveldb_open(o, g_dir, &err);
  check(db1 != NULL, "second first open failed");
  err = NULL;
  leveldb_destroy_db(o, g_dir, &err);
  emit_s64("destroy_open_errclass", err ? 1 : 0);
  if (err) leveldb_free(err);
  leveldb_close(db1);
  (void)dir2;
  leveldb_options_destroy(o);
}
static void sc_version(void) {
  int maj = leveldb_major_version();
  int min = leveldb_minor_version();
  emit_s64("major", maj);
  emit_s64("minor", min);
  check(maj == 1 && min == 23, "version must be 1.23");
}
static void sc_compression(int snappy) {
  leveldb_options_t* o = leveldb_options_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  leveldb_t* db;
  int i;
  char val[300];
  memset(val, 'a', sizeof(val) - 1); val[sizeof(val) - 1] = 0; /* 可压缩 */
  leveldb_options_set_create_if_missing(o, 1);
  leveldb_options_set_compression(o, snappy ? leveldb_snappy_compression
                                            : leveldb_no_compression);
  rm_rf(g_dir);
  open_ok(o, g_dir, &db);
  for (i = 0; i < 200; i++) put_kn(db, wo, i, val);
  leveldb_compact_range(db, NULL, 0, NULL, 0);
  emit_u64("scan", scan_digest(db, 0));
  emit_u64("gets", gets_digest(db, 200));
  leveldb_close(db);
  leveldb_writeoptions_destroy(wo);
  leveldb_options_destroy(o);
}

static void sc_bloom1(void) { sc_bloom(1); }
static void sc_bloom10(void) { sc_bloom(10); }
static void sc_bloom30(void) { sc_bloom(30); }
static void sc_cache_zero(void) { sc_cache(0); }
static void sc_cache_tiny(void) { sc_cache(512); }
static void sc_compression_off(void) { sc_compression(0); }
static void sc_compression_snappy(void) { sc_compression(1); }

struct scenario { const char* id; void (*fn)(void); };
static const struct scenario kScenarios[] = {
  { "defaults", sc_defaults },
  { "open-missing-nocreate", sc_open_missing },
  { "error-if-exists", sc_error_if_exists },
  { "paranoid-healthy", sc_paranoid_healthy },
  { "paranoid-small-record", sc_paranoid_small_record },
  { "paranoid-count0-wal", sc_paranoid_count0_wal },
  { "wbuf-tiny", sc_wbuf_tiny },
  { "maxfile-tiny", sc_maxfile_tiny },
  { "block-small-ri1", sc_block_small },
  { "bloom1", sc_bloom1 },
  { "bloom10", sc_bloom10 },
  { "bloom30", sc_bloom30 },
  { "cache-zero", sc_cache_zero },
  { "cache-tiny", sc_cache_tiny },
  { "ro-verify-corrupt", sc_ro_verify_checksums },
  { "wo-sync", sc_wo_sync },
  { "snapshot", sc_snapshot },
  { "iterator-seeks", sc_iterator_seeks },
  { "batch-big", sc_batch },
  { "empty-batch", sc_empty_batch },
  { "approx-sizes", sc_approx },
  { "properties", sc_properties },
  { "compact-range", sc_compact_range },
  { "destroy-open", sc_destroy_open },
  { "repair-healthy", sc_repair_healthy },
  { "comparator", sc_comparator },
  { "env-default", sc_env_default },
  { "empty-and-free", sc_empty_and_free },
  { "lock-same-process", sc_lock_same_process },
  { "version", sc_version },
  { "compression-off", sc_compression_off },
  { "compression-snappy", sc_compression_snappy },
};

int main(int argc, char** argv) {
  int i;
  if (argc == 2 && strcmp(argv[1], "list") == 0) {
    for (i = 0; i < (int)(sizeof(kScenarios)/sizeof(kScenarios[0])); i++)
      printf("%s\n", kScenarios[i].id);
    return 0;
  }
  if (argc != 4) {
    fprintf(stderr, "usage: %s <label> <workdir> <scenario>\n", argv[0]);
    return 1;
  }
  g_label = argv[1]; g_dir = argv[2]; g_scn = argv[3];
  for (i = 0; i < (int)(sizeof(kScenarios)/sizeof(kScenarios[0])); i++) {
    if (strcmp(kScenarios[i].id, g_scn) == 0) {
      kScenarios[i].fn();
      return 0;
    }
  }
  fprintf(stderr, "unknown scenario %s\n", g_scn);
  return 1;
}
