// dbformat.c - file naming and CURRENT file management
// (mirrors leveldb/db/filename.cc)
#include "kvdb.h"

#include <assert.h>
#include <ctype.h>

static char* make_file_name(const char* dbname, uint64_t number,
                            const char* suffix) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/%06llu.%s", dbname,
           (unsigned long long)number, suffix);
  return strdup(buf);
}

char* ldb_log_file_name(const char* dbname, uint64_t number) {
  assert(number > 0);
  return make_file_name(dbname, number, "log");
}

char* ldb_table_file_name(const char* dbname, uint64_t number) {
  assert(number > 0);
  return make_file_name(dbname, number, "ldb");
}

char* ldb_sst_table_file_name(const char* dbname, uint64_t number) {
  assert(number > 0);
  return make_file_name(dbname, number, "sst");
}

char* ldb_descriptor_file_name(const char* dbname, uint64_t number) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/MANIFEST-%06llu", dbname,
           (unsigned long long)number);
  return strdup(buf);
}

char* ldb_current_file_name(const char* dbname) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/CURRENT", dbname);
  return strdup(buf);
}

char* ldb_lock_file_name(const char* dbname) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/LOCK", dbname);
  return strdup(buf);
}

char* ldb_temp_file_name(const char* dbname, uint64_t number) {
  assert(number > 0);
  return make_file_name(dbname, number, "dbtmp");
}

char* ldb_info_log_file_name(const char* dbname) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/LOG", dbname);
  return strdup(buf);
}

char* ldb_old_info_log_file_name(const char* dbname) {
  char buf[300];
  snprintf(buf, sizeof(buf), "%s/LOG.old", dbname);
  return strdup(buf);
}

int ldb_parse_file_name(const char* filename, uint64_t* number, int* type) {
  ldb_slice rest = ldb_slice_str(filename);
  static const ldb_slice k_current = {"CURRENT", 7};
  static const ldb_slice k_lock = {"LOCK", 4};
  static const ldb_slice k_log_old = {"LOG.old", 7};
  static const ldb_slice k_log = {"LOG", 3};
  static const ldb_slice k_manifest = {"MANIFEST-", 9};
  static const ldb_slice k_dot_log = {".log", 4};
  static const ldb_slice k_dot_sst = {".sst", 4};
  static const ldb_slice k_dot_ldb = {".ldb", 4};
  static const ldb_slice k_dot_dbtmp = {".dbtmp", 6};

  if (ldb_slice_equals(&rest, &k_current)) {
    *number = 0;
    *type = LDB_K_CURRENT_FILE;
  } else if (ldb_slice_equals(&rest, &k_lock)) {
    *number = 0;
    *type = LDB_K_DB_LOCK_FILE;
  } else if (ldb_slice_equals(&rest, &k_log) ||
             ldb_slice_equals(&rest, &k_log_old)) {
    *number = 0;
    *type = LDB_K_INFO_LOG_FILE;
  } else if (ldb_slice_starts_with(&rest, &k_manifest)) {
    rest.data += k_manifest.size;
    rest.size -= k_manifest.size;
    uint64_t num;
    if (!ldb_consume_decimal_number(&rest, &num)) return 0;
    if (rest.size != 0) return 0;
    *type = LDB_K_DESCRIPTOR_FILE;
    *number = num;
  } else {
    uint64_t num;
    if (!ldb_consume_decimal_number(&rest, &num)) return 0;
    ldb_slice suffix = rest;
    if (ldb_slice_equals(&suffix, &k_dot_log)) {
      *type = LDB_K_LOG_FILE;
    } else if (ldb_slice_equals(&suffix, &k_dot_sst) ||
               ldb_slice_equals(&suffix, &k_dot_ldb)) {
      *type = LDB_K_TABLE_FILE;
    } else if (ldb_slice_equals(&suffix, &k_dot_dbtmp)) {
      *type = LDB_K_TEMP_FILE;
    } else {
      return 0;
    }
    *number = num;
  }
  return 1;
}

ldb_status ldb_set_current_file(ldb_env* env, const char* dbname,
                                uint64_t descriptor_number) {
  char* manifest = ldb_descriptor_file_name(dbname, descriptor_number);
  // Strip leading "dbname/" and add newline.
  const char* base = strrchr(manifest, '/');
  base = base ? base + 1 : manifest;
  ldb_buffer contents;
  ldb_buffer_init(&contents);
  ldb_buffer_append_str(&contents, base);
  ldb_buffer_append_str(&contents, "\n");
  char* tmp = ldb_temp_file_name(dbname, descriptor_number);
  ldb_status s = ldb_write_string_to_file_sync(env, &contents, tmp);
  if (ldb_ok(s)) {
    char* cur = ldb_current_file_name(dbname);
    s = ldb_env_rename_file(env, tmp, cur);
    free(cur);
  }
  if (!ldb_ok(s)) {
    ldb_env_remove_file(env, tmp);
  }
  free(tmp);
  free(manifest);
  ldb_buffer_destroy(&contents);
  return s;
}

// =================================================================== internal comparator adapter
// Presents an ldb_ikc through the generic ldb_comparator interface so the
// table layer (TableBuilder etc.) can use it via options->comparator.
static const char* ica_name(const ldb_comparator* c) {
  (void)c;
  return ldb_ikc_name();
}

static int ica_compare(const ldb_comparator* c, const ldb_slice* a,
                       const ldb_slice* b) {
  return ldb_ikc_compare((const ldb_ikc*)c->impl, a, b);
}

static void ica_shortest_separator(const ldb_comparator* c, ldb_buffer* start,
                                   const ldb_slice* limit) {
  ldb_ikc_find_shortest_separator((const ldb_ikc*)c->impl, start, limit);
}

static void ica_short_successor(const ldb_comparator* c, ldb_buffer* key) {
  ldb_ikc_find_short_successor((const ldb_ikc*)c->impl, key);
}

void ldb_internal_comparator_adapter(const ldb_ikc* ikc,
                                     ldb_comparator* adapter) {
  adapter->name = ica_name;
  adapter->compare = ica_compare;
  adapter->find_shortest_separator = ica_shortest_separator;
  adapter->find_short_successor = ica_short_successor;
  adapter->impl = (void*)ikc;
}
