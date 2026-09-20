// repair.c - database repair (mirrors leveldb/db/repair.cc, simplified:
// converts logs into tables and re-registers readable tables into a new
// MANIFEST).
#include "kvdb.h"

#include <assert.h>

typedef struct repairer {
  ldb_env* env;
  const char* dbname;
  ldb_options options;  // sanitized copy
  int owns_info_log;
  int owns_cache;
  ldb_table_cache* table_cache;
  ldb_ikc icmp;
  ldb_comparator adapter;
  ldb_file_meta** tables;
  size_t tables_count;
  size_t tables_cap;
  uint64_t next_file_number;
} repairer;

static ldb_status scan_table(repairer* r, uint64_t number,
                             ldb_file_meta* meta, uint64_t* max_sequence) {
  char* fname = ldb_table_file_name(r->dbname, number);
  uint64_t fsize = 0;
  ldb_status s = ldb_env_get_file_size(r->env, fname, &fsize);
  ldb_rand_file* file = NULL;
  if (ldb_ok(s)) {
    s = ldb_env_new_random_access_file(r->env, fname, &file);
  }
  if (ldb_ok(s)) {
    ldb_table* t = NULL;
    s = ldb_table_open(&r->options, file, fsize, &t);
    if (ldb_ok(s)) {
      *max_sequence = 0;
      meta->number = number;
      meta->file_size = fsize;
      ldb_read_options ro;
      ldb_read_options_init(&ro);
      ldb_iterator* iter = ldb_table_new_iterator(t, &ro);
      int ok = 1;
      ldb_iter_seek_to_first(iter);
      if (ldb_iter_valid(iter)) {
        ldb_slice k = ldb_iter_key(iter);
        ldb_buffer_clear(&meta->smallest);
        ldb_buffer_append_slice(&meta->smallest, &k);
        for (; ldb_iter_valid(iter); ldb_iter_next(iter)) {
          ldb_slice kk = ldb_iter_key(iter);
          ldb_buffer_clear(&meta->largest);
          ldb_buffer_append_slice(&meta->largest, &kk);
          ldb_parsed_internal_key ikey;
          if (!ldb_parse_internal_key(&kk, &ikey)) {
            ok = 0;
            break;
          }
          if (ikey.sequence > *max_sequence) *max_sequence = ikey.sequence;
        }
      } else {
        ok = 0;  // empty table: drop it
      }
      ldb_status is = ldb_iter_status(iter);
      if (!ldb_ok(is)) {
        ldb_status_set(&s, is);
        ok = 0;
      }
      ldb_iterator_destroy(iter);
      if (!ok && ldb_ok(s)) {
        ldb_status_destroy(&s);
        s = ldb_status_corruption("cannot parse", NULL);
      }
      ldb_table_destroy(t);
    }
    file->m->destroy(file);
  }
  free(fname);
  return s;
}

static ldb_status convert_log_to_table(repairer* r, uint64_t log_number) {
  char* fname = ldb_log_file_name(r->dbname, log_number);
  ldb_seq_file* file = NULL;
  ldb_status s = ldb_env_new_sequential_file(r->env, fname, &file);
  free(fname);
  if (!ldb_ok(s)) return s;
  ldb_log_reader* reader = ldb_log_reader_new(file, NULL, NULL, 1, 0);

  ldb_memtable* mem = ldb_memtable_new(&r->icmp);
  ldb_memtable_ref(mem);
  ldb_buffer scratch;
  ldb_buffer_init(&scratch);
  ldb_slice record;
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  while (ldb_log_reader_read_record(reader, &record, &scratch)) {
    if (record.size < 12) continue;
    ldb_write_batch_set_contents(&batch, &record);
    // Ignore error since the state is already gone
    ldb_status_release(ldb_write_batch_insert_into(&batch, mem, NULL));
  }
  ldb_write_batch_destroy(&batch);
  ldb_buffer_destroy(&scratch);
  ldb_log_reader_destroy(reader);
  file->m->destroy(file);

  if (ldb_memtable_approximate_memory_usage(mem) > 0) {
    // Write the memtable contents to a new table.
    uint64_t tnumber = r->next_file_number++;
    ldb_file_meta meta;
    ldb_file_meta_init(&meta);
    meta.number = tnumber;
    ldb_iterator* iter = ldb_memtable_new_iterator(mem);
    s = ldb_build_table(r->dbname, r->env, &r->options, r->table_cache, iter,
                        &meta);
    ldb_iterator_destroy(iter);
    if (ldb_ok(s) && meta.file_size > 0) {
      if (r->tables_count == r->tables_cap) {
        r->tables_cap = r->tables_cap ? r->tables_cap * 2 : 4;
        r->tables = (ldb_file_meta**)realloc(
            r->tables, sizeof(ldb_file_meta*) * r->tables_cap);
        assert(r->tables);
      }
      ldb_file_meta* stored = (ldb_file_meta*)malloc(sizeof(ldb_file_meta));
      *stored = meta;
      r->tables[r->tables_count++] = stored;
    } else {
      ldb_file_meta_destroy(&meta);
    }
  }
  ldb_memtable_unref(mem);
  return s;
}

ldb_status ldb_repair_db(const ldb_options* options, const char* dbname) {
  ldb_env* env = options->env ? options->env : ldb_env_default();
  ldb_status s = ldb_status_ok();

  repairer r;
  memset(&r, 0, sizeof(r));
  r.env = env;
  r.dbname = dbname;
  ldb_ikc_init(&r.icmp, options->comparator);
  ldb_internal_comparator_adapter(&r.icmp, &r.adapter);

  // Sanitize options for building/reading tables.
  r.options = *options;
  r.options.comparator = &r.adapter;
  r.options.env = env;
  if (r.options.block_cache == NULL) {
    r.options.block_cache = ldb_cache_new_lru((size_t)8 << 20);
    r.owns_cache = 1;
  }
  r.table_cache =
      ldb_table_cache_new(dbname, &r.options, r.options.max_open_files - 10);

  // Phase 1: scan all files.
  ldb_strings filenames;
  ldb_strings_init(&filenames);
  s = ldb_env_get_children(env, dbname, &filenames);
  uint64_t* logs = NULL;
  size_t logs_count = 0, logs_cap = 0;
  uint64_t* tables = NULL;
  size_t tables_count = 0, tables_cap = 0;
  for (size_t i = 0; ldb_ok(s) && i < filenames.count; i++) {
    uint64_t number;
    int type;
    if (ldb_parse_file_name(filenames.items[i], &number, &type)) {
      if (type == LDB_K_LOG_FILE) {
        if (logs_count == logs_cap) {
          logs_cap = logs_cap ? logs_cap * 2 : 4;
          logs = (uint64_t*)realloc(logs, sizeof(uint64_t) * logs_cap);
          assert(logs);
        }
        logs[logs_count++] = number;
      } else if (type == LDB_K_TABLE_FILE) {
        if (tables_count == tables_cap) {
          tables_cap = tables_cap ? tables_cap * 2 : 4;
          tables = (uint64_t*)realloc(tables, sizeof(uint64_t) * tables_cap);
          assert(tables);
        }
        tables[tables_count++] = number;
      } else {
        char* path =
            (char*)malloc(strlen(dbname) + strlen(filenames.items[i]) + 2);
        sprintf(path, "%s/%s", dbname, filenames.items[i]);
        ldb_status_release(ldb_env_remove_file(env, path));
        free(path);
      }
    }
  }
  if (ldb_ok(s)) {
    // Assign file numbers so new tables do not collide.
    for (size_t i = 0; i < logs_count; i++) {
      if (logs[i] >= r.next_file_number) r.next_file_number = logs[i] + 1;
    }
    for (size_t i = 0; i < tables_count; i++) {
      if (tables[i] >= r.next_file_number) r.next_file_number = tables[i] + 1;
    }
    r.next_file_number += 1;  // headroom for newly built tables

    // Phase 2: convert logs to tables.
    for (size_t i = 0; ldb_ok(s) && i < logs_count; i++) {
      s = convert_log_to_table(&r, logs[i]);
    }
    r.next_file_number = (r.next_file_number > 2) ? r.next_file_number : 2;

    // Phase 3: verify existing tables and register the good ones.
    uint64_t max_sequence = 1;
    for (size_t i = 0; ldb_ok(s) && i < tables_count; i++) {
      uint64_t number = tables[i];
      ldb_file_meta meta;
      ldb_file_meta_init(&meta);
      uint64_t table_max_seq = 0;
      ldb_status ts = scan_table(&r, number, &meta, &table_max_seq);
      if (ldb_ok(ts)) {
        if (table_max_seq > max_sequence) max_sequence = table_max_seq;
        ldb_file_meta* stored = (ldb_file_meta*)malloc(sizeof(ldb_file_meta));
        *stored = meta;
        if (r.tables_count == r.tables_cap) {
          r.tables_cap = r.tables_cap ? r.tables_cap * 2 : 4;
          r.tables = (ldb_file_meta**)realloc(
              r.tables, sizeof(ldb_file_meta*) * r.tables_cap);
          assert(r.tables);
        }
        r.tables[r.tables_count++] = stored;
      } else {
        char* tname = ldb_table_file_name(dbname, number);
        ldb_status_release(ldb_env_remove_file(env, tname));
        free(tname);
        ldb_status_destroy(&ts);
      }
    }

    // Phase 4: write new manifest + CURRENT.
    if (ldb_ok(s)) {
      uint64_t manifest_number = r.next_file_number + 1;
      char* manifest = ldb_descriptor_file_name(dbname, manifest_number);
      ldb_writable_file* file = NULL;
      s = ldb_env_new_writable_file(env, manifest, &file);
      if (ldb_ok(s)) {
        ldb_log_writer* lw = ldb_log_writer_new(file);
        ldb_buffer record;
        ldb_buffer_init(&record);
        // VersionEdit: comparator, log number, next file, last sequence,
        // then all files at level 0.
        {
          ldb_version_edit edit;
          ldb_version_edit_init(&edit);
          // Record the USER comparator name (same as leveldb's NewDB).
          ldb_version_edit_set_comparator(
              &edit,
              r.icmp.user_comparator->name(r.icmp.user_comparator));
          ldb_version_edit_set_log_number(&edit, 0);
          ldb_version_edit_set_prev_log_number(&edit, 0);
          ldb_version_edit_set_next_file(&edit, manifest_number + 1);
          uint64_t last_sequence = max_sequence;
          for (size_t i = 0; i < r.tables_count; i++) {
            ldb_file_meta* f = r.tables[i];
            ldb_version_edit_add_file(&edit, 0, f->number, f->file_size,
                                      &f->smallest, &f->largest);
          }
          ldb_version_edit_set_last_sequence(&edit, last_sequence);
          ldb_version_edit_encode(&edit, &record);
          ldb_version_edit_destroy(&edit);
        }
        ldb_slice rs = ldb_buffer_slice(&record);
        s = ldb_log_writer_add_record(lw, &rs);
        ldb_buffer_destroy(&record);
        ldb_log_writer_destroy(lw);
        if (ldb_ok(s)) s = file->m->sync(file);
        if (ldb_ok(s)) s = file->m->close(file);
      }
      if (file) file->m->destroy(file);
      if (ldb_ok(s)) {
        s = ldb_set_current_file(env, dbname, manifest_number);
      } else {
        ldb_status_release(ldb_env_remove_file(env, manifest));
      }
      free(manifest);
    }
  }

  free(logs);
  free(tables);
  for (size_t i = 0; i < r.tables_count; i++) {
    ldb_file_meta_destroy(r.tables[i]);
    free(r.tables[i]);
  }
  free(r.tables);
  ldb_strings_destroy(&filenames);
  ldb_table_cache_destroy(r.table_cache);
  if (r.owns_cache && r.options.block_cache) {
    r.options.block_cache->destroy(r.options.block_cache);
  }
  if (r.owns_info_log && r.options.info_log) {
    ldb_logger_destroy(r.options.info_log);
  }
  return s;
}
