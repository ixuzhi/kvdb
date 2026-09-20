// db.c - DBImpl: write path, recovery, background compaction
// (mirrors leveldb/db/db_impl.cc)
#include "kvdb.h"

#include <assert.h>

#define LDB_NUM_NON_TABLE_CACHE_FILES 10

static void record_background_error(ldb_db_impl* impl, ldb_status s);
static void compact_mem_table(ldb_db_impl* impl);
static void background_compaction(ldb_db_impl* impl);
static void cleanup_compaction(ldb_db_impl* impl, ldb_compaction_state* compact);
static ldb_status do_compaction_work(ldb_db_impl* impl, ldb_compaction_state* compact);
ldb_status ldb_write_level0_table(ldb_db_impl* impl, ldb_memtable* mem, ldb_version_edit* edit, ldb_version* base);

// ------------------------------------------------------------------ sanitize
static void clip_to_range_size(size_t* ptr, size_t minv, size_t maxv) {
  if (*ptr > maxv) *ptr = maxv;
  if (*ptr < minv) *ptr = minv;
}

static void clip_to_range_int(int* ptr, int minv, int maxv) {
  if (*ptr > maxv) *ptr = maxv;
  if (*ptr < minv) *ptr = minv;
}

static size_t table_cache_size(const ldb_options* sanitized) {
  // Reserve ten files or so for other uses and give the rest to TableCache.
  return (size_t)(sanitized->max_open_files - LDB_NUM_NON_TABLE_CACHE_FILES);
}

// ------------------------------------------------------------------ ctor/dtor
ldb_db_impl* ldb_db_impl_new(const ldb_options* raw_options,
                             const char* dbname) {
  ldb_db_impl* impl = (ldb_db_impl*)calloc(1, sizeof(ldb_db_impl));
  impl->env = raw_options->env ? raw_options->env : ldb_env_default();
  impl->dbname = strdup(dbname);
  ldb_ikc_init(&impl->internal_comparator, raw_options->comparator);
  ldb_internal_comparator_adapter(&impl->internal_comparator,
                                  &impl->internal_comparator_adapter);

  // SanitizeOptions
  ldb_options* o = &impl->options;
  *o = *raw_options;
  o->comparator = &impl->internal_comparator_adapter;
  o->env = impl->env;
  if (raw_options->filter_policy != NULL) {
    o->filter_policy = ldb_init_internal_filter_policy(
        &impl->internal_filter_policy, raw_options->filter_policy);
  } else {
    o->filter_policy = NULL;
  }
  clip_to_range_int(&o->max_open_files, 64 + LDB_NUM_NON_TABLE_CACHE_FILES,
                    50000);
  clip_to_range_size(&o->write_buffer_size, (size_t)64 << 10, (size_t)1 << 30);
  clip_to_range_size(&o->max_file_size, (size_t)1 << 20, (size_t)1 << 30);
  clip_to_range_size(&o->block_size, (size_t)1 << 10, (size_t)4 << 20);
  if (o->info_log == NULL) {
    // Open a log file in the same directory as the db
    // (the directory may not exist yet)
    ldb_status_release(ldb_env_create_dir(impl->env, dbname));
    char* old_log = ldb_old_info_log_file_name(dbname);
    char* log_name = ldb_info_log_file_name(dbname);
    ldb_status_release(ldb_env_rename_file(impl->env, log_name, old_log));
    ldb_logger* logger = NULL;
    ldb_status ls = ldb_env_new_logger(impl->env, log_name, &logger);
    if (ldb_ok(ls)) {
      o->info_log = logger;
      impl->owns_info_log = 1;
    }
    ldb_status_destroy(&ls);
    free(old_log);
    free(log_name);
  }
  if (o->block_cache == NULL) {
    o->block_cache = ldb_cache_new_lru((size_t)8 << 20);
    impl->owns_cache = 1;
  }

  impl->table_cache = ldb_table_cache_new(
      dbname, &impl->options, table_cache_size(&impl->options));
  impl->db_lock = NULL;
  ldb_atomic_store(&impl->shutting_down, 0);
  impl->mem = NULL;
  impl->imm = NULL;
  ldb_atomic_store(&impl->has_imm, 0);
  impl->logfile = NULL;
  impl->logfile_number = 0;
  impl->log = NULL;
  impl->seed = 0;
  ldb_write_batch_init(&impl->tmp_batch);
  impl->writers = NULL;
  impl->background_compaction_scheduled = 0;
  impl->manual_compaction = NULL;
  ldb_snapshot_list_init(&impl->snapshots);
  uint64_set_init(&impl->pending_outputs);
  impl->bg_error = ldb_status_ok();
  memset(impl->stats, 0, sizeof(impl->stats));

  ldb_mutex_init(&impl->mutex);
  ldb_cond_init(&impl->background_work_finished_signal, &impl->mutex);

  ldb_version_set_init(&impl->versions, dbname, &impl->options,
                       impl->table_cache, &impl->internal_comparator,
                       &impl->mutex);
  return impl;
}

void ldb_db_impl_destroy(ldb_db_impl* impl) {
  // Wait for background work to finish.
  ldb_mutex_lock(&impl->mutex);
  ldb_atomic_store(&impl->shutting_down, 1);
  while (impl->background_compaction_scheduled) {
    ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
  }
  ldb_mutex_unlock(&impl->mutex);

  if (impl->db_lock != NULL) {
    ldb_status_release(ldb_env_unlock_file(impl->env, impl->db_lock));
  }

  ldb_version_set_destroy(&impl->versions);
  if (impl->mem != NULL) ldb_memtable_unref(impl->mem);
  if (impl->imm != NULL) ldb_memtable_unref(impl->imm);
  ldb_write_batch_destroy(&impl->tmp_batch);
  if (impl->log) ldb_log_writer_destroy(impl->log);
  if (impl->logfile) impl->logfile->m->destroy(impl->logfile);
  ldb_table_cache_destroy(impl->table_cache);

  if (impl->owns_info_log && impl->options.info_log) {
    ldb_logger_destroy(impl->options.info_log);
  }
  if (impl->owns_cache && impl->options.block_cache) {
    impl->options.block_cache->destroy(impl->options.block_cache);
  }
  ldb_status_destroy(&impl->bg_error);
  uint64_set_destroy(&impl->pending_outputs);
  ldb_mutex_destroy(&impl->mutex);
  ldb_cond_destroy(&impl->background_work_finished_signal);
  free(impl->dbname);
  free(impl);
}

// ------------------------------------------------------------------ NewDB
static ldb_status db_new_db(ldb_db_impl* impl) {
  ldb_version_edit new_db;
  ldb_version_edit_init(&new_db);
  // leveldb records the USER comparator name in the descriptor.
  ldb_version_edit_set_comparator(
      &new_db, impl->internal_comparator.user_comparator->name(
                   impl->internal_comparator.user_comparator));
  ldb_version_edit_set_log_number(&new_db, 0);
  ldb_version_edit_set_next_file(&new_db, 2);
  ldb_version_edit_set_last_sequence(&new_db, 0);

  char* manifest = ldb_descriptor_file_name(impl->dbname, 1);
  ldb_writable_file* file = NULL;
  ldb_status s = ldb_env_new_writable_file(impl->env, manifest, &file);
  if (!ldb_ok(s)) {
    free(manifest);
    ldb_version_edit_destroy(&new_db);
    return s;
  }
  {
    ldb_log_writer log;
    log.dest = file;
    log.block_offset = 0;
    ldb_buffer record;
    ldb_buffer_init(&record);
    ldb_version_edit_encode(&new_db, &record);
    ldb_slice rs = ldb_buffer_slice(&record);
    s = ldb_log_writer_add_record(&log, &rs);
    ldb_buffer_destroy(&record);
    if (ldb_ok(s)) {
      ldb_status ss = file->m->sync(file);
      ldb_status_set(&s, ss);
    }
    if (ldb_ok(s)) {
      ldb_status cs = file->m->close(file);
      ldb_status_set(&s, cs);
    }
  }
  file->m->destroy(file);
  if (ldb_ok(s)) {
    // Make "CURRENT" file that points to the new manifest file.
    ldb_status cs = ldb_set_current_file(impl->env, impl->dbname, 1);
    ldb_status_set(&s, cs);
  } else {
    ldb_status_release(ldb_env_remove_file(impl->env, manifest));
  }
  free(manifest);
  ldb_version_edit_destroy(&new_db);
  return s;
}

static void maybe_ignore_error(ldb_db_impl* impl, ldb_status* s) {
  if (ldb_ok(*s) || impl->options.paranoid_checks) {
    // No change needed
  } else {
    ldb_log(impl->options.info_log, "Ignoring error %s", "");
    ldb_status_destroy(s);
    *s = ldb_status_ok();
  }
}

// ------------------------------------------------------------------ RemoveObsoleteFiles
static void assert_impl_held(ldb_db_impl* impl) {
  (void)impl;  // single-writer discipline enforced by ldb_mutex usage
}

static void remove_obsolete_files(ldb_db_impl* impl) {
  assert_impl_held(impl);

  if (!ldb_ok(impl->bg_error)) {
    // After a background error, we don't know whether a new version may
    // or may not have been committed, so we cannot safely garbage collect.
    return;
  }

  // Make a set of all of the live files
  uint64_set_t live;
  uint64_set_init(&live);
  for (size_t i = 0; i < impl->pending_outputs.count; i++) {
    uint64_set_insert(&live, impl->pending_outputs.items[i]);
  }
  ldb_version_set_add_live_files(&impl->versions, &live);

  ldb_strings filenames;
  ldb_strings_init(&filenames);
  ldb_status_release(
      ldb_env_get_children(impl->env, impl->dbname, &filenames));
  for (size_t i = 0; i < filenames.count; i++) {
    uint64_t number;
    int type;
    if (ldb_parse_file_name(filenames.items[i], &number, &type)) {
      int keep = 1;
      switch (type) {
        case LDB_K_LOG_FILE:
          keep = ((number >= impl->versions.log_number) ||
                  (number == impl->versions.prev_log_number));
          break;
        case LDB_K_DESCRIPTOR_FILE:
          // Keep my manifest file, and any newer incarnations'
          keep = (number >= impl->versions.manifest_file_number);
          break;
        case LDB_K_TABLE_FILE:
          keep = uint64_set_contains(&live, number);
          break;
        case LDB_K_TEMP_FILE:
          keep = uint64_set_contains(&live, number);
          break;
        case LDB_K_CURRENT_FILE:
        case LDB_K_DB_LOCK_FILE:
        case LDB_K_INFO_LOG_FILE:
          keep = 1;
          break;
      }
      if (!keep) {
        char* path =
            (char*)malloc(strlen(impl->dbname) + 1 + strlen(filenames.items[i]) + 1);
        sprintf(path, "%s/%s", impl->dbname, filenames.items[i]);
        ldb_status_release(ldb_env_remove_file(impl->env, path));
        free(path);
        if (type == LDB_K_TABLE_FILE) {
          ldb_table_cache_evict(impl->table_cache, number);
        }
        ldb_log(impl->options.info_log, "Delete type=%d #%llu\n", type,
                (unsigned long long)number);
      }
    }
  }
  ldb_strings_destroy(&filenames);
  uint64_set_destroy(&live);
}

// ------------------------------------------------------------------ Recover
static ldb_status recover_log_file(ldb_db_impl* impl, uint64_t log_number,
                                   int last_log, int* save_manifest,
                                   ldb_version_edit* edit,
                                   uint64_t* max_sequence);

static ldb_status db_recover(ldb_db_impl* impl, ldb_version_edit* edit,
                             int* save_manifest) {
  // Ignore error from CreateDir since the creation of the DB is committed
  // only when the descriptor is created.
  ldb_status_release(ldb_env_create_dir(impl->env, impl->dbname));
  assert(impl->db_lock == NULL);
  char* lock_name = ldb_lock_file_name(impl->dbname);
  ldb_status s = ldb_env_lock_file(impl->env, lock_name, &impl->db_lock);
  free(lock_name);
  if (!ldb_ok(s)) {
    return s;
  }

  char* current = ldb_current_file_name(impl->dbname);
  int exists = ldb_env_file_exists(impl->env, current);
  free(current);
  if (!exists) {
    if (impl->options.create_if_missing) {
      ldb_log(impl->options.info_log, "Creating DB %s since it was missing.",
              impl->dbname);
      s = db_new_db(impl);
      if (!ldb_ok(s)) {
        return s;
      }
    } else {
      return ldb_status_invalid_argument(impl->dbname,
                                         "does not exist (create_if_missing is false)");
    }
  } else {
    if (impl->options.error_if_exists) {
      return ldb_status_invalid_argument(impl->dbname,
                                         "exists (error_if_exists is true)");
    }
  }

  s = ldb_version_set_recover(&impl->versions, save_manifest);
  if (!ldb_ok(s)) {
    return s;
  }
  uint64_t max_sequence = 0;

  // Recover from all newer log files than the ones named in the descriptor.
  const uint64_t min_log = impl->versions.log_number;
  const uint64_t prev_log = impl->versions.prev_log_number;
  ldb_strings filenames;
  ldb_strings_init(&filenames);
  s = ldb_env_get_children(impl->env, impl->dbname, &filenames);
  if (!ldb_ok(s)) {
    ldb_strings_destroy(&filenames);
    return s;
  }
  uint64_set_t expected;
  uint64_set_init(&expected);
  ldb_version_set_add_live_files(&impl->versions, &expected);
  uint64_t* logs = NULL;
  size_t logs_count = 0, logs_cap = 0;
  for (size_t i = 0; i < filenames.count; i++) {
    uint64_t number;
    int type;
    if (ldb_parse_file_name(filenames.items[i], &number, &type)) {
      uint64_set_erase(&expected, number);
      if (type == LDB_K_LOG_FILE &&
          ((number >= min_log) || (number == prev_log))) {
        if (logs_count == logs_cap) {
          logs_cap = logs_cap ? logs_cap * 2 : 4;
          logs = (uint64_t*)realloc(logs, sizeof(uint64_t) * logs_cap);
          assert(logs);
        }
        logs[logs_count++] = number;
      }
    }
  }
  if (uint64_set_size(&expected) > 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d missing files; e.g.",
             (int)uint64_set_size(&expected));
    char* missing = ldb_table_file_name(impl->dbname, uint64_set_first(&expected));
    ldb_status_destroy(&s);
    s = ldb_status_corruption(buf, missing);
    free(missing);
    free(logs);
    uint64_set_destroy(&expected);
    ldb_strings_destroy(&filenames);
    return s;
  }
  uint64_set_destroy(&expected);
  ldb_strings_destroy(&filenames);

  // Recover in the order in which the logs were generated
  for (size_t i = 0; i + 1 < logs_count; i++) {
    for (size_t j = i + 1; j < logs_count; j++) {
      if (logs[j] < logs[i]) {
        uint64_t t = logs[i];
        logs[i] = logs[j];
        logs[j] = t;
      }
    }
  }
  for (size_t i = 0; i < logs_count; i++) {
    s = recover_log_file(impl, logs[i], (int)(i == logs_count - 1),
                         save_manifest, edit, &max_sequence);
    if (!ldb_ok(s)) {
      break;
    }
    // The previous incarnation may not have written any MANIFEST records
    // after allocating this log number.
    ldb_version_set_mark_file_number_used(&impl->versions, logs[i]);
  }
  free(logs);

  if (ldb_ok(s) && impl->versions.last_sequence < max_sequence) {
    impl->versions.last_sequence = max_sequence;
  }

  return s;
}

static void log_reporter_corruption(void* arg, size_t bytes, ldb_status s) {
  // arg is (paranoid ? &status : NULL)-style: we pass a pointer to a struct.
  typedef struct lr {
    int paranoid;
    ldb_status* status;
  } lr;
  lr* r = (lr*)arg;
  ldb_log(NULL, "(log) dropping %d bytes; corruption", (int)bytes);
  if (r->paranoid && r->status != NULL && ldb_ok(*r->status)) {
    *r->status = s;
  } else {
    ldb_status_destroy(&s);
  }
}

static ldb_status recover_log_file(ldb_db_impl* impl, uint64_t log_number,
                                   int last_log, int* save_manifest,
                                   ldb_version_edit* edit,
                                   uint64_t* max_sequence) {
  ldb_mutex_lock(&impl->mutex);

  // Open the log file
  char* fname = ldb_log_file_name(impl->dbname, log_number);
  ldb_seq_file* file = NULL;
  ldb_status status = ldb_env_new_sequential_file(impl->env, fname, &file);
  if (!ldb_ok(status)) {
    maybe_ignore_error(impl, &status);
    free(fname);
    ldb_mutex_unlock(&impl->mutex);
    return status;
  }

  typedef struct lr {
    int paranoid;
    ldb_status* status;
  } lr;
  lr reporter;
  reporter.paranoid = impl->options.paranoid_checks;
  reporter.status = &status;
  // We intentionally make log::Reader do checksumming even if
  // paranoid_checks==false so that corruptions cause entire commits to be
  // skipped instead of propagating bad information.
  ldb_log_reader* reader =
      ldb_log_reader_new(file, &reporter, log_reporter_corruption,
                         1 /*checksum*/, 0 /*initial_offset*/);
  ldb_log(impl->options.info_log, "Recovering log #%llu",
          (unsigned long long)log_number);

  // Read all the records and add to a memtable
  ldb_buffer scratch;
  ldb_buffer_init(&scratch);
  ldb_slice record;
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  int compactions = 0;
  ldb_memtable* mem = NULL;
  const ldb_ikc* icmp = &impl->internal_comparator;
  while (ldb_log_reader_read_record(reader, &record, &scratch) &&
         ldb_ok(status)) {
    if (record.size < 12) {
      reporter.paranoid = impl->options.paranoid_checks;
      ldb_status_destroy(&status);
      status = ldb_status_ok();
      continue;  // "log record too small" — dropped
    }
    ldb_write_batch_set_contents(&batch, &record);

    if (mem == NULL) {
      mem = ldb_memtable_new(icmp);
      ldb_memtable_ref(mem);
    }
    uint64_t last_seq = 0;
    ldb_status st = ldb_write_batch_insert_into(&batch, mem, &last_seq);
    if (!ldb_ok(st)) {
      maybe_ignore_error(impl, &st);
      if (!ldb_ok(st)) {
        ldb_status_set(&status, st);
        break;
      }
    }
    last_seq = ldb_write_batch_sequence(&batch) +
               ldb_write_batch_count(&batch) - 1;
    if (last_seq > *max_sequence) {
      *max_sequence = last_seq;
    }

    if (ldb_memtable_approximate_memory_usage(mem) >
        impl->options.write_buffer_size) {
      compactions++;
      *save_manifest = 1;
      ldb_status st2 = ldb_write_level0_table(impl, mem, edit, NULL);
      ldb_status_set(&status, st2);
      ldb_memtable_unref(mem);
      mem = NULL;
      if (!ldb_ok(status)) {
        // Reflect errors immediately so that conditions like full
        // file-systems cause the DB open to fail.
        break;
      }
    }
  }
  ldb_log_reader_destroy(reader);
  ldb_write_batch_destroy(&batch);
  ldb_buffer_destroy(&scratch);

  file->m->destroy(file);
  free(fname);
  (void)last_log;  // reuse_logs not implemented (always treated as false)

  if (mem != NULL) {
    // mem did not get reused; compact it.
    if (ldb_ok(status)) {
      *save_manifest = 1;
      ldb_status st = ldb_write_level0_table(impl, mem, edit, NULL);
      ldb_status_set(&status, st);
    }
    ldb_memtable_unref(mem);
  }

  ldb_mutex_unlock(&impl->mutex);
  return status;
}

// ------------------------------------------------------------------ WriteLevel0Table
ldb_status ldb_write_level0_table(ldb_db_impl* impl, ldb_memtable* mem,
                                  ldb_version_edit* edit, ldb_version* base) {
  uint64_t start_micros = ldb_env_now_micros(impl->env);
  ldb_file_meta meta;
  ldb_file_meta_init(&meta);
  meta.number = ldb_version_set_new_file_number(&impl->versions);
  uint64_set_insert(&impl->pending_outputs, meta.number);
  ldb_iterator* iter = ldb_memtable_new_iterator(mem);
  ldb_log(impl->options.info_log, "Level-0 table #%llu: started",
          (unsigned long long)meta.number);

  ldb_status s;
  {
    ldb_mutex_unlock(&impl->mutex);
    s = ldb_build_table(impl->dbname, impl->env, &impl->options,
                        impl->table_cache, iter, &meta);
    ldb_mutex_lock(&impl->mutex);
  }

  ldb_log(impl->options.info_log, "Level-0 table #%llu: %lld bytes",
          (unsigned long long)meta.number, (unsigned long long)meta.file_size);
  ldb_iterator_destroy(iter);
  uint64_set_erase(&impl->pending_outputs, meta.number);

  int level = 0;
  if (ldb_ok(s) && meta.file_size > 0) {
    ldb_slice sbuf = ldb_buffer_slice(&meta.smallest);
    ldb_slice lbuf = ldb_buffer_slice(&meta.largest);
    ldb_slice min_user_key = ldb_extract_user_key(&sbuf);
    ldb_slice max_user_key = ldb_extract_user_key(&lbuf);
    if (base != NULL) {
      level = ldb_version_pick_level_for_memtable_output(
          base, &min_user_key, &max_user_key);
    }
    ldb_version_edit_add_file(edit, level, meta.number, meta.file_size,
                              &meta.smallest, &meta.largest);
  }

  impl->stats[level].micros +=
      (int64_t)(ldb_env_now_micros(impl->env) - start_micros);
  impl->stats[level].bytes_written += (int64_t)meta.file_size;
  ldb_file_meta_destroy(&meta);
  return s;
}

// ------------------------------------------------------------------ CompactMemTable
static void compact_mem_table(ldb_db_impl* impl) {
  assert(impl->imm != NULL);

  // Save the contents of the memtable as a new Table
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  ldb_version* base = ldb_version_set_current(&impl->versions);
  ldb_version_ref(base);
  ldb_status s = ldb_write_level0_table(impl, impl->imm, &edit, base);
  ldb_version_unref(base);

  if (ldb_ok(s) && ldb_atomic_load(&impl->shutting_down)) {
    ldb_status_destroy(&s);
    s = ldb_status_io_error("Deleting DB during memtable compaction", NULL);
  }

  // Replace immutable memtable with the generated Table
  if (ldb_ok(s)) {
    ldb_version_edit_set_prev_log_number(&edit, 0);
    ldb_version_edit_set_log_number(&edit, impl->logfile_number);
    ldb_status la = ldb_version_set_log_and_apply(&impl->versions, &edit);
    ldb_status_set(&s, la);
  }

  if (ldb_ok(s)) {
    // Commit to the new state
    ldb_memtable_unref(impl->imm);
    impl->imm = NULL;
    ldb_atomic_store(&impl->has_imm, 0);
    remove_obsolete_files(impl);
  } else {
    record_background_error(impl, s);
  }
  ldb_status_destroy(&s);
  ldb_version_edit_destroy(&edit);
}

// ------------------------------------------------------------------ compaction driving
static void record_background_error(ldb_db_impl* impl, ldb_status s) {
  if (ldb_ok(impl->bg_error)) {
    impl->bg_error = ldb_status_copy(s);
    ldb_cond_signal_all(&impl->background_work_finished_signal);
  }
}

static void background_call(ldb_db_impl* impl);

static void bg_work_entry(void* arg) { background_call((ldb_db_impl*)arg); }

static void maybe_schedule_compaction(ldb_db_impl* impl) {
  if (impl->background_compaction_scheduled) {
    // Already scheduled
  } else if (ldb_atomic_load(&impl->shutting_down)) {
    // DB is being deleted; no more background compactions
  } else if (!ldb_ok(impl->bg_error)) {
    // Already got an error; no more changes
  } else if (impl->imm == NULL && impl->manual_compaction == NULL &&
             !ldb_version_set_needs_compaction(&impl->versions)) {
    // No work to be done
  } else {
    impl->background_compaction_scheduled = 1;
    ldb_env_schedule(impl->env, bg_work_entry, impl);
  }
}

static void background_call(ldb_db_impl* impl) {
  ldb_mutex_lock(&impl->mutex);
  assert(impl->background_compaction_scheduled);
  if (ldb_atomic_load(&impl->shutting_down)) {
    // No more background work when shutting down.
  } else if (!ldb_ok(impl->bg_error)) {
    // No more background work after a background error.
  } else {
    background_compaction(impl);
  }

  impl->background_compaction_scheduled = 0;

  // Previous compaction may have produced too many files in a level,
  // so reschedule another compaction if needed.
  maybe_schedule_compaction(impl);
  ldb_cond_signal_all(&impl->background_work_finished_signal);
  ldb_mutex_unlock(&impl->mutex);
}

static void background_compaction(ldb_db_impl* impl) {
  if (impl->imm != NULL) {
    compact_mem_table(impl);
    return;
  }

  ldb_compaction* c;
  int is_manual = (impl->manual_compaction != NULL);
  ldb_buffer manual_end;
  ldb_buffer_init(&manual_end);
  if (is_manual) {
    ldb_manual_compaction* m = impl->manual_compaction;
    ldb_slice begin_s, end_s;
    if (m->has_begin) begin_s = ldb_bslice(&m->begin);
    if (m->has_end) end_s = ldb_bslice(&m->end);
    c = ldb_version_set_compact_range(
        &impl->versions, m->level,
        m->has_begin ? &begin_s : NULL,
        m->has_end ? &end_s : NULL);
    m->done = (c == NULL);
    if (c != NULL) {
      ldb_buffer_copy(&manual_end,
                      &c->inputs[0][c->inputs_count[0] - 1]->largest);
    }
  } else {
    c = ldb_version_set_pick_compaction(&impl->versions);
  }

  ldb_status status = ldb_status_ok();
  if (c == NULL) {
    // Nothing to do
  } else if (!is_manual && ldb_compaction_is_trivial_move(c)) {
    // Move file to next level
    assert(c->inputs_count[0] == 1);
    ldb_file_meta* f = c->inputs[0][0];
    ldb_version_edit_remove_file(&c->edit, c->level, f->number);
    ldb_version_edit_add_file(&c->edit, c->level + 1, f->number, f->file_size,
                              &f->smallest, &f->largest);
    status = ldb_version_set_log_and_apply(&impl->versions, &c->edit);
    if (!ldb_ok(status)) {
      record_background_error(impl, status);
    }
    ldb_log(impl->options.info_log, "Moved #%lld to level-%d %lld bytes",
            (unsigned long long)f->number, c->level + 1,
            (unsigned long long)f->file_size);
  } else {
    ldb_compaction_state* compact =
        (ldb_compaction_state*)calloc(1, sizeof(ldb_compaction_state));
    compact->compaction = c;
    status = do_compaction_work(impl, compact);
    if (!ldb_ok(status)) {
      record_background_error(impl, status);
    }
    cleanup_compaction(impl, compact);
    ldb_version_unref(c->input_version);
    c->input_version = NULL;
    remove_obsolete_files(impl);
  }
  ldb_compaction_destroy(c);

  if (is_manual) {
    ldb_manual_compaction* m = impl->manual_compaction;
    if (!ldb_ok(status)) {
      m->done = 1;
    }
    if (!m->done) {
      // We only compacted part of the requested range. Update *m to the
      // range that is left to be compacted.
      ldb_buffer_copy(&m->tmp_storage, &manual_end);
      ldb_buffer_copy(&m->begin, &m->tmp_storage);
    }
    impl->manual_compaction = NULL;
  }
  ldb_status_destroy(&status);
  ldb_buffer_destroy(&manual_end);
}

static ldb_status open_compaction_output_file(ldb_db_impl* impl,
                                              ldb_compaction_state* compact) {
  assert(compact->builder == NULL);
  uint64_t file_number;
  ldb_mutex_lock(&impl->mutex);
  file_number = ldb_version_set_new_file_number(&impl->versions);
  uint64_set_insert(&impl->pending_outputs, file_number);
  if (compact->outputs_count == compact->outputs_cap) {
    compact->outputs_cap =
        compact->outputs_cap ? compact->outputs_cap * 2 : 4;
    compact->outputs = (ldb_compaction_output*)realloc(
        compact->outputs,
        sizeof(ldb_compaction_output) * compact->outputs_cap);
    assert(compact->outputs);
  }
  ldb_compaction_output* out = &compact->outputs[compact->outputs_count++];
  out->number = file_number;
  out->file_size = 0;
  ldb_buffer_init(&out->smallest);
  ldb_buffer_init(&out->largest);
  ldb_mutex_unlock(&impl->mutex);

  // Make the output file
  char* fname = ldb_table_file_name(impl->dbname, file_number);
  ldb_status s = ldb_env_new_writable_file(impl->env, fname,
                                           &compact->outfile);
  free(fname);
  if (ldb_ok(s)) {
    compact->builder = ldb_table_builder_new(&impl->options, compact->outfile);
  }
  return s;
}

static ldb_status finish_compaction_output_file(ldb_db_impl* impl,
                                                ldb_compaction_state* compact,
                                                ldb_iterator* input) {
  assert(compact->outfile != NULL);
  assert(compact->builder != NULL);

  const uint64_t output_number =
      compact->outputs[compact->outputs_count - 1].number;
  assert(output_number != 0);

  // Check for iterator errors
  ldb_status s = ldb_iter_status(input);
  const uint64_t current_entries = ldb_table_builder_num_entries(compact->builder);
  if (ldb_ok(s)) {
    ldb_status fs = ldb_table_builder_finish(compact->builder);
    ldb_status_set(&s, fs);
  } else {
    ldb_table_builder_abandon(compact->builder);
  }
  const uint64_t current_bytes = ldb_table_builder_file_size(compact->builder);
  compact->outputs[compact->outputs_count - 1].file_size = current_bytes;
  compact->total_bytes += current_bytes;
  ldb_table_builder_destroy(compact->builder);
  compact->builder = NULL;

  // Finish and check for file errors
  if (ldb_ok(s)) {
    ldb_status ss = compact->outfile->m->sync(compact->outfile);
    ldb_status_set(&s, ss);
  }
  if (ldb_ok(s)) {
    ldb_status cs = compact->outfile->m->close(compact->outfile);
    ldb_status_set(&s, cs);
  }
  compact->outfile->m->destroy(compact->outfile);
  compact->outfile = NULL;

  if (ldb_ok(s) && current_entries > 0) {
    // Verify that the table is usable
    ldb_read_options ro;
    ldb_read_options_init(&ro);
    ldb_iterator* iter = ldb_table_cache_new_iterator(
        impl->table_cache, &ro, output_number, current_bytes);
    ldb_status is = ldb_iter_status(iter);
    ldb_status_set(&s, is);
    ldb_iterator_destroy(iter);
    if (ldb_ok(s)) {
      ldb_log(impl->options.info_log, "Generated table #%llu@%d: %lld keys",
              (unsigned long long)output_number, compact->compaction->level,
              (unsigned long long)current_entries);
    }
  }
  return s;
}

static void cleanup_compaction(ldb_db_impl* impl,
                               ldb_compaction_state* compact) {
  if (compact->builder != NULL) {
    // May happen if we get a shutdown call in the middle of compaction
    ldb_table_builder_abandon(compact->builder);
    ldb_table_builder_destroy(compact->builder);
    compact->builder = NULL;
  }
  if (compact->outfile != NULL) {
    compact->outfile->m->destroy(compact->outfile);
    compact->outfile = NULL;
  }
  for (size_t i = 0; i < compact->outputs_count; i++) {
    uint64_set_erase(&impl->pending_outputs, compact->outputs[i].number);
    ldb_buffer_destroy(&compact->outputs[i].smallest);
    ldb_buffer_destroy(&compact->outputs[i].largest);
  }
  free(compact->outputs);
  free(compact);
}

static ldb_status install_compaction_results(ldb_db_impl* impl,
                                             ldb_compaction_state* compact) {
  ldb_log(impl->options.info_log, "Compacted %d@%d + %d@%d files => %lld bytes",
          (int)compact->compaction->inputs_count[0], compact->compaction->level,
          (int)compact->compaction->inputs_count[1],
          compact->compaction->level + 1, (long long)compact->total_bytes);

  // Add compaction outputs
  ldb_compaction* c = compact->compaction;
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < c->inputs_count[which]; i++) {
      ldb_version_edit_remove_file(&c->edit, c->level + which,
                                   c->inputs[which][i]->number);
    }
  }
  const int level = c->level;
  for (size_t i = 0; i < compact->outputs_count; i++) {
    ldb_compaction_output* out = &compact->outputs[i];
    ldb_version_edit_add_file(&c->edit, level + 1, out->number, out->file_size,
                              &out->smallest, &out->largest);
  }
  return ldb_version_set_log_and_apply(&impl->versions, &c->edit);
}

static ldb_status do_compaction_work(ldb_db_impl* impl,
                                     ldb_compaction_state* compact) {
  const uint64_t start_micros = ldb_env_now_micros(impl->env);
  int64_t imm_micros = 0;  // Micros spent doing imm_ compactions

  assert(ldb_version_set_num_level_files(&impl->versions,
                                         compact->compaction->level) > 0);
  assert(compact->builder == NULL);
  assert(compact->outfile == NULL);
  if (ldb_snapshot_list_empty(&impl->snapshots)) {
    compact->smallest_snapshot = impl->versions.last_sequence;
  } else {
    compact->smallest_snapshot =
        ldb_snapshot_list_oldest(&impl->snapshots)->sequence;
  }

  ldb_iterator* input =
      ldb_version_set_make_input_iterator(&impl->versions, compact->compaction);

  // Release mutex while we're actually doing the compaction work
  ldb_mutex_unlock(&impl->mutex);

  ldb_iter_seek_to_first(input);
  ldb_status status = ldb_status_ok();
  ldb_parsed_internal_key ikey;
  ldb_buffer current_user_key;
  ldb_buffer_init(&current_user_key);
  int has_current_user_key = 0;
  uint64_t last_sequence_for_key = LDB_K_MAX_SEQUENCE_NUMBER;
  while (ldb_iter_valid(input) && !ldb_atomic_load(&impl->shutting_down)) {
    // Prioritize immutable compaction work
    if (ldb_atomic_load(&impl->has_imm)) {
      const uint64_t imm_start = ldb_env_now_micros(impl->env);
      ldb_mutex_lock(&impl->mutex);
      if (impl->imm != NULL) {
        compact_mem_table(impl);
        // Wake up MakeRoomForWrite() if necessary.
        ldb_cond_signal_all(&impl->background_work_finished_signal);
      }
      ldb_mutex_unlock(&impl->mutex);
      imm_micros += (int64_t)(ldb_env_now_micros(impl->env) - imm_start);
    }

    ldb_slice key = ldb_iter_key(input);
    if (ldb_compaction_should_stop_before(compact->compaction, &key) &&
        compact->builder != NULL) {
      ldb_status fs = finish_compaction_output_file(impl, compact, input);
      ldb_status_set(&status, fs);
      if (!ldb_ok(status)) {
        break;
      }
    }

    // Handle key/value, add to state, etc.
    int drop = 0;
    const ldb_comparator* ucmp = impl->internal_comparator.user_comparator;
    if (!ldb_parse_internal_key(&key, &ikey)) {
      // Do not hide error keys
      current_user_key.size = 0;
      has_current_user_key = 0;
      last_sequence_for_key = LDB_K_MAX_SEQUENCE_NUMBER;
    } else {
      ldb_slice uk = ikey.user_key;
      ldb_slice cuk = ldb_buffer_slice(&current_user_key);
      if (!has_current_user_key || ucmp->compare(ucmp, &uk, &cuk) != 0) {
        // First occurrence of this user key
        ldb_buffer_clear(&current_user_key);
        ldb_buffer_append_slice(&current_user_key, &uk);
        has_current_user_key = 1;
        last_sequence_for_key = LDB_K_MAX_SEQUENCE_NUMBER;
      }

      if (last_sequence_for_key <= compact->smallest_snapshot) {
        // Hidden by a newer entry for same user key
        drop = 1;  // (A)
      } else if (ikey.type == LDB_TYPE_DELETION &&
                 ikey.sequence <= compact->smallest_snapshot &&
                 ldb_compaction_is_base_level_for_key(compact->compaction,
                                                      &uk)) {
        // This deletion marker is obsolete and can be dropped.
        drop = 1;
      }

      last_sequence_for_key = ikey.sequence;
    }

    if (!drop) {
      // Open output file if necessary
      if (compact->builder == NULL) {
        ldb_status of = open_compaction_output_file(impl, compact);
        ldb_status_set(&status, of);
        if (!ldb_ok(status)) {
          break;
        }
      }
      if (ldb_table_builder_num_entries(compact->builder) == 0) {
        ldb_buffer_clear(&compact->outputs[compact->outputs_count - 1].smallest);
        ldb_buffer_append_slice(
            &compact->outputs[compact->outputs_count - 1].smallest, &key);
      }
      ldb_buffer_clear(&compact->outputs[compact->outputs_count - 1].largest);
      ldb_buffer_append_slice(
          &compact->outputs[compact->outputs_count - 1].largest, &key);
      ldb_slice value = ldb_iter_value(input);
      ldb_table_builder_add(compact->builder, &key, &value);

      // Close output file if it is big enough
      if (ldb_table_builder_file_size(compact->builder) >=
          compact->compaction->max_output_file_size) {
        ldb_status fs = finish_compaction_output_file(impl, compact, input);
        ldb_status_set(&status, fs);
        if (!ldb_ok(status)) {
          break;
        }
      }
    }

    ldb_iter_next(input);
  }
  ldb_buffer_destroy(&current_user_key);

  if (ldb_ok(status) && ldb_atomic_load(&impl->shutting_down)) {
    status = ldb_status_io_error("Deleting DB during compaction", NULL);
  }
  if (ldb_ok(status) && compact->builder != NULL) {
    ldb_status fs = finish_compaction_output_file(impl, compact, input);
    ldb_status_set(&status, fs);
  }
  if (ldb_ok(status)) {
    ldb_status is = ldb_iter_status(input);
    ldb_status_set(&status, is);
  }
  ldb_iterator_destroy(input);
  input = NULL;

  int64_t bytes_read = 0;
  for (int which = 0; which < 2; which++) {
    for (size_t i = 0; i < compact->compaction->inputs_count[which]; i++) {
      bytes_read += (int64_t)compact->compaction->inputs[which][i]->file_size;
    }
  }
  int64_t bytes_written = 0;
  for (size_t i = 0; i < compact->outputs_count; i++) {
    bytes_written += (int64_t)compact->outputs[i].file_size;
  }

  ldb_mutex_lock(&impl->mutex);
  impl->stats[compact->compaction->level + 1].micros +=
      (int64_t)(ldb_env_now_micros(impl->env) - start_micros) - imm_micros;
  impl->stats[compact->compaction->level + 1].bytes_read += bytes_read;
  impl->stats[compact->compaction->level + 1].bytes_written += bytes_written;

  if (ldb_ok(status)) {
    ldb_status ir = install_compaction_results(impl, compact);
    ldb_status_set(&status, ir);
  }
  if (!ldb_ok(status)) {
    record_background_error(impl, status);
  }
  ldb_log(impl->options.info_log, "compacted to: %s", "");
  return status;
}

// ------------------------------------------------------------------ CompactRange
void ldb_db_impl_compact_range(ldb_db_impl* impl, const ldb_slice* begin,
                               const ldb_slice* end) {
  int max_level_with_files = 1;
  {
    ldb_mutex_lock(&impl->mutex);
    ldb_version* base = ldb_version_set_current(&impl->versions);
    for (int level = 1; level < LDB_K_NUM_LEVELS; level++) {
      if (ldb_version_overlap_in_level(base, level, begin, end)) {
        max_level_with_files = level;
      }
    }
    ldb_mutex_unlock(&impl->mutex);
  }
  // TODO: Skip if no overlap
  ldb_status_release(ldb_db_impl_test_compact_mem_table(impl));
  for (int level = 0; level < max_level_with_files; level++) {
    ldb_db_impl_test_compact_range(impl, level, begin, end);
  }
}

void ldb_db_impl_test_compact_range(ldb_db_impl* impl, int level,
                                    const ldb_slice* begin,
                                    const ldb_slice* end) {
  assert(level >= 0);
  assert(level + 1 < LDB_K_NUM_LEVELS);

  ldb_manual_compaction manual;
  memset(&manual, 0, sizeof(manual));
  ldb_buffer_init(&manual.begin);
  ldb_buffer_init(&manual.end);
  ldb_buffer_init(&manual.tmp_storage);
  manual.level = level;
  manual.done = 0;
  if (begin != NULL) {
    manual.has_begin = 1;
    ldb_append_internal_key(&manual.begin, begin, LDB_K_MAX_SEQUENCE_NUMBER,
                            LDB_VALUE_TYPE_FOR_SEEK);
  }
  if (end != NULL) {
    manual.has_end = 1;
    ldb_append_internal_key(&manual.end, end, 0, LDB_TYPE_DELETION);
  }

  ldb_mutex_lock(&impl->mutex);
  while (!manual.done && !ldb_atomic_load(&impl->shutting_down) &&
         ldb_ok(impl->bg_error)) {
    if (impl->manual_compaction == NULL) {  // Idle
      impl->manual_compaction = &manual;
      maybe_schedule_compaction(impl);
    } else {  // Running either my compaction or another compaction.
      ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
    }
  }
  // Finish current background compaction in the case where
  // `background_work_finished_signal_` was signalled due to an error.
  while (impl->background_compaction_scheduled) {
    ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
  }
  if (impl->manual_compaction == &manual) {
    // Cancel my manual compaction since we aborted early for some reason.
    impl->manual_compaction = NULL;
  }
  ldb_mutex_unlock(&impl->mutex);

  ldb_buffer_destroy(&manual.begin);
  ldb_buffer_destroy(&manual.end);
  ldb_buffer_destroy(&manual.tmp_storage);
}

ldb_status ldb_db_impl_test_compact_mem_table(ldb_db_impl* impl) {
  // NULL batch means just wait for earlier writes to be done
  ldb_write_options wo;
  ldb_write_options_init(&wo);
  ldb_status s = ldb_db_impl_write(impl, &wo, NULL);
  if (ldb_ok(s)) {
    // Wait until the compaction completes
    ldb_mutex_lock(&impl->mutex);
    while (impl->imm != NULL && ldb_ok(impl->bg_error) &&
           !ldb_atomic_load(&impl->shutting_down)) {
      ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
    }
    if (impl->imm != NULL) {
      ldb_status_destroy(&s);
      s = ldb_status_copy(impl->bg_error);
    }
    ldb_mutex_unlock(&impl->mutex);
  }
  return s;
}

// ------------------------------------------------------------------ iterators
typedef struct iter_state {
  ldb_mutex* mu;
  ldb_version* version;
  ldb_memtable* mem;
  ldb_memtable* imm;
} iter_state;

static void cleanup_iterator_state(void* arg1, void* arg2) {
  (void)arg2;
  iter_state* state = (iter_state*)arg1;
  ldb_mutex_lock(state->mu);
  ldb_memtable_unref(state->mem);
  if (state->imm != NULL) ldb_memtable_unref(state->imm);
  ldb_version_unref(state->version);
  ldb_mutex_unlock(state->mu);
  free(state);
}

static ldb_iterator* new_internal_iterator(ldb_db_impl* impl,
                                           const ldb_read_options* options,
                                           uint64_t* latest_snapshot,
                                           uint32_t* seed) {
  ldb_mutex_lock(&impl->mutex);
  *latest_snapshot = impl->versions.last_sequence;

  // Collect together all needed child iterators
  ldb_iterator** list = NULL;
  size_t list_count = 0, list_cap = 0;
  if (list_count == list_cap) {
    list_cap = list_cap ? list_cap * 2 : 8;
    list = (ldb_iterator**)realloc(list, sizeof(ldb_iterator*) * list_cap);
  }
  list[list_count++] = ldb_memtable_new_iterator(impl->mem);
  ldb_memtable_ref(impl->mem);
  if (impl->imm != NULL) {
    if (list_count == list_cap) {
      list_cap = list_cap ? list_cap * 2 : 8;
      list = (ldb_iterator**)realloc(list, sizeof(ldb_iterator*) * list_cap);
    }
    list[list_count++] = ldb_memtable_new_iterator(impl->imm);
    ldb_memtable_ref(impl->imm);
  }
  ldb_version_add_iterators(impl->versions.current_, options, &list,
                            &list_count, &list_cap);
  ldb_iterator* internal_iter =
      ldb_new_merging_iterator(&impl->internal_comparator, list, list_count);
  free(list);
  ldb_version_ref(impl->versions.current_);

  iter_state* cleanup = (iter_state*)malloc(sizeof(iter_state));
  cleanup->mu = &impl->mutex;
  cleanup->mem = impl->mem;
  cleanup->imm = impl->imm;
  cleanup->version = impl->versions.current_;
  ldb_iterator_register_cleanup(internal_iter, cleanup_iterator_state,
                                cleanup, NULL);

  *seed = ++impl->seed;
  ldb_mutex_unlock(&impl->mutex);
  return internal_iter;
}

ldb_iterator* ldb_db_impl_new_iterator(ldb_db_impl* impl,
                                       const ldb_read_options* options) {
  uint64_t latest_snapshot;
  uint32_t seed;
  ldb_read_options default_options;
  if (options == NULL) {
    ldb_read_options_init(&default_options);
    options = &default_options;
  }
  ldb_iterator* iter =
      new_internal_iterator(impl, options, &latest_snapshot, &seed);
  uint64_t seq = latest_snapshot;
  if (options->snapshot != NULL) {
    seq = ((const ldb_snapshot_impl*)options->snapshot)->sequence;
  }
  return ldb_new_db_iterator(impl, impl->internal_comparator.user_comparator,
                             iter, seq, seed);
}

ldb_iterator* ldb_db_impl_test_new_internal_iterator(ldb_db_impl* impl) {
  uint64_t ignored;
  uint32_t ignored_seed;
  ldb_read_options ro;
  ldb_read_options_init(&ro);
  return new_internal_iterator(impl, &ro, &ignored, &ignored_seed);
}

int64_t ldb_db_impl_test_max_next_level_overlapping_bytes(ldb_db_impl* impl) {
  ldb_mutex_lock(&impl->mutex);
  int64_t r =
      (int64_t)ldb_version_set_max_next_level_overlapping_bytes(&impl->versions);
  ldb_mutex_unlock(&impl->mutex);
  return r;
}

// ------------------------------------------------------------------ Get
ldb_status ldb_db_impl_get(ldb_db_impl* impl, const ldb_read_options* options,
                           const ldb_slice* key, ldb_buffer* value) {
  ldb_status s = ldb_status_ok();
  ldb_mutex_lock(&impl->mutex);
  uint64_t snapshot;
  if (options->snapshot != NULL) {
    snapshot = ((const ldb_snapshot_impl*)options->snapshot)->sequence;
  } else {
    snapshot = impl->versions.last_sequence;
  }

  ldb_memtable* mem = impl->mem;
  ldb_memtable* imm = impl->imm;
  ldb_version* current = impl->versions.current_;
  ldb_memtable_ref(mem);
  if (imm != NULL) ldb_memtable_ref(imm);
  ldb_version_ref(current);

  int have_stat_update = 0;
  ldb_file_meta* seek_file = NULL;
  int seek_file_level = -1;

  // Unlock while reading from files and memtables
  {
    ldb_mutex_unlock(&impl->mutex);
    // First look in the memtable, then in the immutable memtable (if any).
    ldb_lookup_key lkey;
    ldb_lookup_key_init(&lkey, key, snapshot);
    if (ldb_memtable_get(mem, &lkey, value, &s)) {
      // Done
    } else if (imm != NULL && ldb_memtable_get(imm, &lkey, value, &s)) {
      // Done
    } else {
      ldb_status vs = ldb_version_get(current, options, &lkey, value,
                                      &seek_file, &seek_file_level);
      ldb_status_set(&s, vs);
      have_stat_update = 1;
    }
    ldb_lookup_key_destroy(&lkey);
    ldb_mutex_lock(&impl->mutex);
  }

  if (have_stat_update &&
      ldb_version_update_stats(current, seek_file, seek_file_level)) {
    maybe_schedule_compaction(impl);
  }
  ldb_memtable_unref(mem);
  if (imm != NULL) ldb_memtable_unref(imm);
  ldb_version_unref(current);
  ldb_mutex_unlock(&impl->mutex);
  return s;
}

void ldb_db_impl_record_read_sample(ldb_db_impl* impl, const ldb_slice* key) {
  ldb_mutex_lock(&impl->mutex);
  if (ldb_version_record_read_sample(impl->versions.current_, key)) {
    maybe_schedule_compaction(impl);
  }
  ldb_mutex_unlock(&impl->mutex);
}

// ------------------------------------------------------------------ snapshots
const ldb_snapshot_impl* ldb_db_impl_get_snapshot(ldb_db_impl* impl) {
  ldb_mutex_lock(&impl->mutex);
  const ldb_snapshot_impl* s =
      ldb_snapshot_list_new(&impl->snapshots, impl->versions.last_sequence);
  ldb_mutex_unlock(&impl->mutex);
  return s;
}

void ldb_db_impl_release_snapshot(ldb_db_impl* impl,
                                  const ldb_snapshot_impl* snapshot) {
  ldb_mutex_lock(&impl->mutex);
  ldb_snapshot_list_delete(&impl->snapshots, (ldb_snapshot_impl*)snapshot);
  ldb_mutex_unlock(&impl->mutex);
}

uint64_t ldb_db_impl_test_last_sequence(ldb_db_impl* impl) {
  ldb_mutex_lock(&impl->mutex);
  uint64_t seq = impl->versions.last_sequence;
  ldb_mutex_unlock(&impl->mutex);
  return seq;
}

// ------------------------------------------------------------------ Write
static ldb_write_batch* build_batch_group(ldb_db_impl* impl,
                                          ldb_writer** last_writer) {
  assert(impl->writers != NULL);
  ldb_writer* first = impl->writers;
  ldb_write_batch* result = first->batch;
  assert(result != NULL);

  size_t size = ldb_write_batch_byte_size(first->batch);

  // Allow the group to grow up to a maximum size, but if the
  // original write is small, limit the growth so we do not slow
  // down the small write too much.
  size_t max_size = 1 << 20;
  if (size <= (128 << 10)) {
    max_size = size + (128 << 10);
  }

  *last_writer = first;
  ldb_writer* w = first->next;
  for (; w != NULL; w = w->next) {
    if (w->sync && !first->sync) {
      // Do not include a sync write into a batch handled by a non-sync write.
      break;
    }

    if (w->batch != NULL) {
      size += ldb_write_batch_byte_size(w->batch);
      if (size > max_size) {
        // Do not make batch too big
        break;
      }

      // Append to *result
      if (result == first->batch) {
        // Switch to temporary batch instead of disturbing caller's batch
        result = &impl->tmp_batch;
        assert(ldb_write_batch_count(result) == 0);
        ldb_write_batch_append(result, first->batch);
      }
      ldb_write_batch_append(result, w->batch);
    }
    *last_writer = w;
  }
  return result;
}

static ldb_status make_room_for_write(ldb_db_impl* impl, int force) {
  assert(impl->writers != NULL);
  int allow_delay = !force;
  ldb_status s = ldb_status_ok();
  while (1) {
    if (!ldb_ok(impl->bg_error)) {
      // Yield previous error
      ldb_status_destroy(&s);
      s = ldb_status_copy(impl->bg_error);
      break;
    } else if (allow_delay &&
               ldb_version_set_num_level_files(&impl->versions, 0) >=
                   LDB_K_L0_SLOWDOWN_WRITES_TRIGGER) {
      // We are getting close to hitting a hard limit on the number of L0
      // files. Rather than delaying a single write by several seconds when
      // we hit the hard limit, start delaying each individual write by 1ms
      // to reduce latency variance.
      ldb_mutex_unlock(&impl->mutex);
      ldb_env_sleep_for_microseconds(impl->env, 1000);
      allow_delay = 0;  // Do not delay a single write more than once
      ldb_mutex_lock(&impl->mutex);
    } else if (!force &&
               (ldb_memtable_approximate_memory_usage(impl->mem) <=
                impl->options.write_buffer_size)) {
      // There is room in current memtable
      break;
    } else if (impl->imm != NULL) {
      // We have filled up the current memtable, but the previous one is
      // still being compacted, so we wait.
      ldb_log(impl->options.info_log, "Current memtable full; waiting...");
      ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
    } else if (ldb_version_set_num_level_files(&impl->versions, 0) >=
               LDB_K_L0_STOP_WRITES_TRIGGER) {
      // There are too many level-0 files.
      ldb_log(impl->options.info_log, "Too many L0 files; waiting...");
      ldb_cond_wait(&impl->background_work_finished_signal, &impl->mutex);
    } else {
      // Attempt to switch to a new memtable and trigger compaction of old
      assert(impl->versions.prev_log_number == 0);
      uint64_t new_log_number =
          ldb_version_set_new_file_number(&impl->versions);
      char* log_name = ldb_log_file_name(impl->dbname, new_log_number);
      ldb_writable_file* lfile = NULL;
      s = ldb_env_new_writable_file(impl->env, log_name, &lfile);
      free(log_name);
      if (!ldb_ok(s)) {
        // Avoid chewing through file number space in a tight loop.
        ldb_version_set_reuse_file_number(&impl->versions, new_log_number);
        break;
      }

      ldb_log_writer_destroy(impl->log);

      ldb_status cs = impl->logfile->m->close(impl->logfile);
      if (!ldb_ok(cs)) {
        // We may have lost some data written to the previous log file.
        // Switch to the new log file anyway, but record as a background
        // error so we do not attempt any more writes.
        record_background_error(impl, cs);
      }
      ldb_status_destroy(&cs);
      impl->logfile->m->destroy(impl->logfile);

      impl->logfile = lfile;
      impl->logfile_number = new_log_number;
      impl->log = ldb_log_writer_new(lfile);
      impl->imm = impl->mem;
      ldb_atomic_store(&impl->has_imm, 1);
      impl->mem = ldb_memtable_new(&impl->internal_comparator);
      ldb_memtable_ref(impl->mem);
      force = 0;  // Do not force another compaction if have room
      maybe_schedule_compaction(impl);
    }
  }
  return s;
}

ldb_status ldb_db_impl_write(ldb_db_impl* impl,
                             const ldb_write_options* options,
                             ldb_write_batch* updates) {
  ldb_writer w;
  w.batch = updates;
  w.sync = options ? options->sync : 0;
  w.done = 0;
  w.status = ldb_status_ok();
  w.next = NULL;
  ldb_cond_init(&w.cv, &impl->mutex);

  ldb_mutex_lock(&impl->mutex);
  // Append to writers_ queue
  if (impl->writers == NULL) {
    impl->writers = &w;
  } else {
    ldb_writer* tail = impl->writers;
    while (tail->next != NULL) tail = tail->next;
    tail->next = &w;
  }
  while (!w.done && &w != impl->writers) {
    ldb_cond_wait(&w.cv, &impl->mutex);
  }
  if (w.done) {
    ldb_status r = w.status;
    ldb_mutex_unlock(&impl->mutex);
    ldb_cond_destroy(&w.cv);
    return r;
  }

  // May temporarily unlock and wait.
  ldb_status status = make_room_for_write(impl, updates == NULL);
  uint64_t last_sequence = impl->versions.last_sequence;
  ldb_writer* last_writer = &w;
  if (ldb_ok(status) && updates != NULL) {  // NULL batch is for compactions
    ldb_write_batch* write_batch = build_batch_group(impl, &last_writer);
    ldb_write_batch_set_sequence(write_batch, last_sequence + 1);
    last_sequence += ldb_write_batch_count(write_batch);

    // Add to log and apply to memtable. We can release the lock during this
    // phase since &w is currently responsible for logging and protects
    // against concurrent loggers and concurrent writes into mem_.
    {
      ldb_mutex_unlock(&impl->mutex);
      ldb_slice contents = ldb_write_batch_contents(write_batch);
      status = ldb_log_writer_add_record(impl->log, &contents);
      int sync_error = 0;
      if (ldb_ok(status) && w.sync) {
        ldb_status ss = impl->logfile->m->sync(impl->logfile);
        ldb_status_set(&status, ss);
        if (!ldb_ok(status)) {
          sync_error = 1;
        }
      }
      if (ldb_ok(status)) {
        ldb_status ii = ldb_write_batch_insert_into(write_batch, impl->mem, NULL);
        ldb_status_set(&status, ii);
      }
      ldb_mutex_lock(&impl->mutex);
      if (sync_error) {
        // The state of the log file is indeterminate: the log record we
        // just added may or may not show up when the DB is re-opened.
        // So we force the DB into a mode where all future writes fail.
        record_background_error(impl, status);
      }
    }
    if (write_batch == &impl->tmp_batch) {
      ldb_write_batch_clear(&impl->tmp_batch);
    }

    impl->versions.last_sequence = last_sequence;
  }

  while (1) {
    ldb_writer* ready = impl->writers;
    impl->writers = ready->next;
    if (ready != &w) {
      ready->status = ldb_status_copy(status);
      ready->done = 1;
      ldb_cond_signal(&ready->cv);
    }
    if (ready == last_writer) break;
  }
  // Notify new head of write queue
  if (impl->writers != NULL) {
    ldb_cond_signal(&impl->writers->cv);
  }

  ldb_mutex_unlock(&impl->mutex);
  ldb_cond_destroy(&w.cv);
  return status;
}

// ------------------------------------------------------------------ properties
int ldb_db_impl_get_property(ldb_db_impl* impl, const ldb_slice* property,
                             ldb_buffer* value) {
  ldb_buffer_clear(value);

  ldb_mutex_lock(&impl->mutex);
  ldb_slice in = *property;
  ldb_slice prefix = ldb_slice_str("leveldb.");
  if (!ldb_slice_starts_with(&in, &prefix)) {
    ldb_mutex_unlock(&impl->mutex);
    return 0;
  }
  in.data += prefix.size;
  in.size -= prefix.size;

  ldb_slice num_prefix = ldb_slice_str("num-files-at-level");
  if (ldb_slice_starts_with(&in, &num_prefix)) {
    in.data += num_prefix.size;
    in.size -= num_prefix.size;
    uint64_t level;
    if (!ldb_consume_decimal_number(&in, &level) || in.size != 0 ||
        level >= LDB_K_NUM_LEVELS) {
      ldb_mutex_unlock(&impl->mutex);
      return 0;
    }
    ldb_buffer_appendf(value, "%d",
                       ldb_version_set_num_level_files(&impl->versions,
                                                       (int)level));
    ldb_mutex_unlock(&impl->mutex);
    return 1;
  }
  ldb_slice stats = ldb_slice_str("stats");
  if (ldb_slice_equals(&in, &stats)) {
    ldb_buffer_append_str(value,
                          "                               Compactions\n"
                          "Level  Files Size(MB) Time(sec) Read(MB) Write(MB)\n"
                          "--------------------------------------------------\n");
    for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
      int files = ldb_version_set_num_level_files(&impl->versions, level);
      if (impl->stats[level].micros > 0 || files > 0) {
        ldb_buffer_appendf(value,
                           "%3d %8d %8.0f %9.0f %8.0f %9.0f\n",
                           level, files,
                           (double)ldb_version_set_num_level_bytes(
                               &impl->versions, level) / 1048576.0,
                           (double)impl->stats[level].micros / 1e6,
                           (double)impl->stats[level].bytes_read / 1048576.0,
                           (double)impl->stats[level].bytes_written /
                               1048576.0);
      }
    }
    ldb_mutex_unlock(&impl->mutex);
    return 1;
  }
  ldb_slice sstables = ldb_slice_str("sstables");
  if (ldb_slice_equals(&in, &sstables)) {
    char* dbg = ldb_version_debug_string(impl->versions.current_);
    ldb_buffer_append_str(value, dbg);
    free(dbg);
    ldb_mutex_unlock(&impl->mutex);
    return 1;
  }
  ldb_slice mem_usage = ldb_slice_str("approximate-memory-usage");
  if (ldb_slice_equals(&in, &mem_usage)) {
    size_t total_usage =
        impl->options.block_cache->total_charge(impl->options.block_cache);
    if (impl->mem) {
      total_usage += ldb_memtable_approximate_memory_usage(impl->mem);
    }
    if (impl->imm) {
      total_usage += ldb_memtable_approximate_memory_usage(impl->imm);
    }
    ldb_buffer_appendf(value, "%llu", (unsigned long long)total_usage);
    ldb_mutex_unlock(&impl->mutex);
    return 1;
  }
  ldb_mutex_unlock(&impl->mutex);
  return 0;
}

void ldb_db_impl_get_approximate_sizes(ldb_db_impl* impl,
                                       const ldb_slice* start,
                                       const ldb_slice* limit, int n,
                                       uint64_t* sizes) {
  ldb_mutex_lock(&impl->mutex);
  ldb_version* v = impl->versions.current_;
  v->refs++;
  for (int i = 0; i < n; i++) {
    // Convert user_key into a corresponding internal key.
    ldb_buffer k1, k2;
    ldb_buffer_init(&k1);
    ldb_buffer_init(&k2);
    ldb_append_internal_key(&k1, &start[i], LDB_K_MAX_SEQUENCE_NUMBER,
                            LDB_VALUE_TYPE_FOR_SEEK);
    ldb_append_internal_key(&k2, &limit[i], LDB_K_MAX_SEQUENCE_NUMBER,
                            LDB_VALUE_TYPE_FOR_SEEK);
    ldb_slice k1s = ldb_bslice(&k1);
    ldb_slice k2s = ldb_bslice(&k2);
    uint64_t s =
        ldb_version_set_approximate_offset_of(&impl->versions, v, &k1s);
    uint64_t l =
        ldb_version_set_approximate_offset_of(&impl->versions, v, &k2s);
    sizes[i] = (l >= s) ? (l - s) : 0;
    ldb_buffer_destroy(&k1);
    ldb_buffer_destroy(&k2);
  }
  v->refs--;
  ldb_mutex_unlock(&impl->mutex);
}

// ------------------------------------------------------------------ Open / DestroyDB
ldb_status ldb_db_open(const ldb_options* options, const char* dbname,
                       ldb_db_impl** dbptr) {
  *dbptr = NULL;

  ldb_db_impl* impl = ldb_db_impl_new(options, dbname);
  ldb_mutex_lock(&impl->mutex);
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  // Recover handles create_if_missing, error_if_exists
  int save_manifest = 0;
  ldb_status s = db_recover(impl, &edit, &save_manifest);
  if (ldb_ok(s) && impl->mem == NULL) {
    // Create new log and a corresponding memtable.
    uint64_t new_log_number = ldb_version_set_new_file_number(&impl->versions);
    char* log_name = ldb_log_file_name(dbname, new_log_number);
    ldb_writable_file* lfile = NULL;
    s = ldb_env_new_writable_file(impl->env, log_name, &lfile);
    free(log_name);
    if (ldb_ok(s)) {
      ldb_version_edit_set_log_number(&edit, new_log_number);
      impl->logfile = lfile;
      impl->logfile_number = new_log_number;
      impl->log = ldb_log_writer_new(lfile);
      impl->mem = ldb_memtable_new(&impl->internal_comparator);
      ldb_memtable_ref(impl->mem);
    }
  }
  if (ldb_ok(s) && save_manifest) {
    ldb_version_edit_set_prev_log_number(&edit, 0);  // No older logs needed
    ldb_version_edit_set_log_number(&edit, impl->logfile_number);
    ldb_status la = ldb_version_set_log_and_apply(&impl->versions, &edit);
    ldb_status_set(&s, la);
  }
  if (ldb_ok(s)) {
    remove_obsolete_files(impl);
    maybe_schedule_compaction(impl);
  }
  ldb_version_edit_destroy(&edit);
  ldb_mutex_unlock(&impl->mutex);
  if (ldb_ok(s)) {
    assert(impl->mem != NULL);
    *dbptr = impl;
  } else {
    ldb_db_impl_destroy(impl);
  }
  return s;
}

ldb_status ldb_destroy_db(const ldb_options* options, const char* dbname) {
  ldb_env* env = options->env ? options->env : ldb_env_default();
  ldb_strings filenames;
  ldb_strings_init(&filenames);
  ldb_status result = ldb_env_get_children(env, dbname, &filenames);
  if (!ldb_ok(result)) {
    // Ignore error in case directory does not exist
    ldb_status_destroy(&result);
    ldb_strings_destroy(&filenames);
    return ldb_status_ok();
  }

  ldb_file_lock* lock = NULL;
  char* lockname = ldb_lock_file_name(dbname);
  ldb_status_set(&result, ldb_env_lock_file(env, lockname, &lock));
  if (ldb_ok(result)) {
    for (size_t i = 0; i < filenames.count; i++) {
      uint64_t number;
      int type;
      if (ldb_parse_file_name(filenames.items[i], &number, &type) &&
          type != LDB_K_DB_LOCK_FILE) {  // Lock file deleted at end
        char* path = (char*)malloc(strlen(dbname) + 1 +
                                   strlen(filenames.items[i]) + 1);
        sprintf(path, "%s/%s", dbname, filenames.items[i]);
        ldb_status del = ldb_env_remove_file(env, path);
        if (ldb_ok(result) && !ldb_ok(del)) {
          ldb_status_set(&result, del);
        } else {
          ldb_status_destroy(&del);
        }
        free(path);
      }
    }
    // Ignore errors from here on: the state is already gone.
    ldb_status_release(ldb_env_unlock_file(env, lock));
    ldb_status_release(ldb_env_remove_file(env, lockname));
    ldb_status_release(ldb_env_delete_dir(env, dbname));
  }
  free(lockname);
  ldb_strings_destroy(&filenames);
  return result;
}
