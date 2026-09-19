// Isolated memenv fixtures, not process-crash or real-disk durability tests.
// References: leveldb/db/log_test.cc (trailing truncation, ChecksumMismatch,
// ReadSecondStart/OneOff, ReadEnd/PastEnd, OpenForAppend/ShortTrailer),
// db/recovery_test.cc (MultipleLogFiles), db/db_test.cc (Snapshot,
// HiddenValuesAreRemoved, DeletionMarkers1). All mutations are bounded and
// applied only to a closed, test-owned WAL; DB recovery uses valid WALs.
#include "harness.h"

static void re_ok(ldb_status s) {
  CHECK_STATUS_OK(s);
  ldb_status_destroy(&s);
}

static void re_bytes(ldb_slice actual, const char* expected, size_t size) {
  CHECK_EQ(size, actual.size);
  CHECK(size == 0 || memcmp(actual.data, expected, size) == 0);
}

typedef struct re_log {
  ldb_env* env;
  ldb_writable_file* dest;
  ldb_log_writer* writer;
  ldb_seq_file* source;
  ldb_log_reader* reader;
  ldb_buffer scratch;
  ldb_buffer errors;
  size_t dropped;
  size_t reports;
} re_log;

static const char* re_log_path = "/extra/log";

static void re_log_init(re_log* t) {
  memset(t, 0, sizeof(*t));
  t->env = ldb_memenv_new();
  ldb_buffer_init(&t->scratch);
  ldb_buffer_init(&t->errors);
  re_ok(ldb_env_new_writable_file(t->env, re_log_path, &t->dest));
  t->writer = ldb_log_writer_new(t->dest);
}

static void re_log_close_writer(re_log* t) {
  if (t->writer) ldb_log_writer_destroy(t->writer);
  t->writer = NULL;
  if (t->dest) {
    re_ok(t->dest->m->close(t->dest));
    t->dest->m->destroy(t->dest);
    t->dest = NULL;
  }
}

static void re_log_add(re_log* t, const char* data, size_t size) {
  ldb_slice record = ldb_slice_make(data, size);
  re_ok(ldb_log_writer_add_record(t->writer, &record));
}

static void re_report(void* arg, size_t bytes, ldb_status status) {
  re_log* t = (re_log*)arg;
  t->dropped += bytes;
  t->reports++;
  CHECK_EQ(LDB_CORRUPTION, status.code);
  if (status.msg) ldb_buffer_append_str(&t->errors, status.msg);
  ldb_status_destroy(&status);  // reader transfers a copy to the reporter
}

static void re_log_read_from(re_log* t, uint64_t offset) {
  re_log_close_writer(t);
  re_ok(ldb_env_new_sequential_file(t->env, re_log_path, &t->source));
  t->reader = ldb_log_reader_new(t->source, t, re_report, 1, offset);
}

static void re_log_expect(re_log* t, const char* data, size_t size) {
  ldb_slice record;
  CHECK(ldb_log_reader_read_record(t->reader, &record, &t->scratch));
  re_bytes(record, data, size);
}

static void re_log_eof(re_log* t) {
  ldb_slice record;
  CHECK(!ldb_log_reader_read_record(t->reader, &record, &t->scratch));
  CHECK(!ldb_log_reader_read_record(t->reader, &record, &t->scratch));
}

static void re_log_destroy(re_log* t) {
  re_log_close_writer(t);
  if (t->reader) ldb_log_reader_destroy(t->reader);
  if (t->source) t->source->m->destroy(t->source);
  ldb_buffer_destroy(&t->scratch);
  ldb_buffer_destroy(&t->errors);
  ldb_memenv_destroy(t->env);
}

static void re_truncated_tail(size_t retained) {
  re_log t;
  re_log_init(&t);
  re_log_add(&t, "good", 4);
  re_log_add(&t, "tail", 4);
  re_log_close_writer(&t);
  ldb_buffer bytes;
  ldb_buffer_init(&bytes);
  re_ok(ldb_read_file_to_string(t.env, re_log_path, &bytes));
  CHECK_EQ(22, bytes.size);
  CHECK_LT(retained, 11);
  ldb_buffer_resize(&bytes, 11 + retained);
  re_ok(ldb_write_string_to_file_sync(t.env, &bytes, re_log_path));
  ldb_buffer_destroy(&bytes);
  re_log_read_from(&t, 0);
  re_log_expect(&t, "good", 4);
  re_log_eof(&t);
  CHECK_EQ(0, t.dropped);
  CHECK_EQ(0, t.reports);
  re_log_destroy(&t);
}

TEST(recovery_extra, LogTruncatedTailHeader) {
  // Every partial physical header, including no trailing bytes at all.
  for (size_t n = 0; n < LDB_LOG_HEADER_SIZE; n++) re_truncated_tail(n);
}

TEST(recovery_extra, LogTruncatedTailPayload) {
  // Complete header, zero through three bytes of the four-byte payload.
  for (size_t n = LDB_LOG_HEADER_SIZE; n < 11; n++) re_truncated_tail(n);
}

TEST(recovery_extra, LogChecksumDamageSkipsBlock) {
  re_log t;
  re_log_init(&t);
  const size_t payload_size = LDB_LOG_BLOCK_SIZE - LDB_LOG_HEADER_SIZE;
  char* payload = (char*)malloc(payload_size);
  CHECK(payload != NULL);
  memset(payload, 'x', payload_size);
  re_log_add(&t, payload, payload_size);
  free(payload);
  re_log_add(&t, "survivor", 8);  // next physical block, not same-block data
  re_log_close_writer(&t);
  ldb_buffer bytes;
  ldb_buffer_init(&bytes);
  re_ok(ldb_read_file_to_string(t.env, re_log_path, &bytes));
  CHECK_EQ(LDB_LOG_BLOCK_SIZE + 15, bytes.size);
  bytes.data[LDB_LOG_HEADER_SIZE] ^= 1;  // change one payload byte only
  re_ok(ldb_write_string_to_file_sync(t.env, &bytes, re_log_path));
  ldb_buffer_destroy(&bytes);
  re_log_read_from(&t, 0);
  re_log_expect(&t, "survivor", 8);
  re_log_eof(&t);
  CHECK_EQ(LDB_LOG_BLOCK_SIZE, t.dropped);
  CHECK_EQ(1, t.reports);
  ldb_buffer_append(&t.errors, "\0", 1);
  CHECK_STR_CONTAINS(t.errors.data, "checksum mismatch");
  re_log_destroy(&t);
}

TEST(recovery_extra, LogNonzeroOffsetAtHeaderAndPayload) {
  // Exact header and offsets inside a header/payload, all within block zero.
  const uint64_t offsets[] = {1, 8, 10, 11, 18, 20};
  const int expected[] = {1, 1, 1, 2, 2, 2};
  const char* records[] = {"one", "two", "three"};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    re_log t;
    re_log_init(&t);
    for (int j = 0; j < 3; j++)
      re_log_add(&t, records[j], strlen(records[j]));
    re_log_read_from(&t, offsets[i]);
    for (int j = expected[i]; j < 3; j++)
      re_log_expect(&t, records[j], strlen(records[j]));
    re_log_eof(&t);
    CHECK_EQ(0, t.reports);
    re_log_destroy(&t);
  }
}

TEST(recovery_extra, LogOffsetAtAndPastEof) {
  const uint64_t offsets[] = {10, 15};
  for (size_t i = 0; i < sizeof(offsets) / sizeof(offsets[0]); i++) {
    re_log t;
    re_log_init(&t);
    re_log_add(&t, "one", 3);
    re_log_read_from(&t, offsets[i]);
    re_log_eof(&t);
    CHECK_EQ(0, t.reports);
    re_log_destroy(&t);
  }
}

TEST(recovery_extra, LogAppendRespectsShortBlockTrailer) {
  re_log t;
  re_log_init(&t);
  size_t payload_size = LDB_LOG_BLOCK_SIZE - LDB_LOG_HEADER_SIZE - 3;
  char* payload = (char*)malloc(payload_size);
  CHECK(payload != NULL);
  memset(payload, 'p', payload_size);
  re_log_add(&t, payload, payload_size);
  re_log_close_writer(&t);
  uint64_t length;
  re_ok(ldb_env_get_file_size(t.env, re_log_path, &length));
  CHECK_EQ(LDB_LOG_BLOCK_SIZE - 3, length);
  re_ok(ldb_env_new_appendable_file(t.env, re_log_path, &t.dest));
  t.writer = ldb_log_writer_new_at(t.dest, length);
  re_log_add(&t, "appended", 8);
  re_log_close_writer(&t);
  ldb_buffer bytes;
  ldb_buffer_init(&bytes);
  re_ok(ldb_read_file_to_string(t.env, re_log_path, &bytes));
  CHECK_EQ(LDB_LOG_BLOCK_SIZE + 15, bytes.size);
  re_bytes(ldb_slice_make(bytes.data + length, 3), "\0\0\0", 3);
  CHECK_EQ(LDB_LOG_FULL_TYPE, bytes.data[LDB_LOG_BLOCK_SIZE + 6]);
  ldb_buffer_destroy(&bytes);
  re_log_read_from(&t, 0);
  re_log_expect(&t, payload, payload_size);
  re_log_expect(&t, "appended", 8);
  re_log_eof(&t);
  CHECK_EQ(0, t.reports);
  free(payload);
  re_log_destroy(&t);
}

typedef struct re_db {
  ldb_options options;
  ldb_db_impl* db;
} re_db;

static const char* re_db_path = "/extra/db";

static void re_db_reopen(re_db* t) {
  if (t->db) ldb_db_impl_destroy(t->db);
  t->db = NULL;
  re_ok(ldb_db_open(&t->options, re_db_path, &t->db));
}

static void re_db_init(re_db* t) {
  ldb_options_init(&t->options);
  t->options.env = ldb_memenv_new();
  t->options.create_if_missing = 1;
  t->options.paranoid_checks = 1;
  t->options.reuse_logs = 0;
  t->db = NULL;
  re_db_reopen(t);
}

static void re_db_destroy(re_db* t) {
  ldb_db_impl_destroy(t->db);
  ldb_memenv_destroy(t->options.env);
}

static void re_db_write(re_db* t, ldb_write_batch* batch) {
  ldb_write_options options;
  ldb_write_options_init(&options);
  re_ok(ldb_db_impl_write(t->db, &options, batch));
}

static void re_db_set(re_db* t, const char* key, const char* value) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_slice k = ldb_slice_str(key);
  if (value) {
    ldb_slice v = ldb_slice_str(value);
    ldb_write_batch_put(&batch, &k, &v);
  } else {
    ldb_write_batch_delete(&batch, &k);
  }
  re_db_write(t, &batch);
  ldb_write_batch_destroy(&batch);
}

static void re_db_expect(re_db* t, const ldb_snapshot_impl* snapshot,
                         const char* key, const char* expected) {
  ldb_read_options options;
  ldb_read_options_init(&options);
  options.verify_checksums = 1;
  options.snapshot = snapshot;
  ldb_slice k = ldb_slice_str(key);
  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_status s = ldb_db_impl_get(t->db, &options, &k, &value);
  if (expected) {
    CHECK_STATUS_OK(s);
    CHECK_BUF_EQ(expected, value);
  } else {
    CHECK_EQ(LDB_NOTFOUND, s.code);
  }
  ldb_status_destroy(&s);
  ldb_buffer_destroy(&value);
}

TEST(recovery_extra, BatchSequenceAcrossRepeatedRecovery) {
  re_db t;
  re_db_init(&t);
  re_db_set(&t, "gone", "old");
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_slice key = ldb_slice_str("key"), gone = ldb_slice_str("gone");
  ldb_slice first = ldb_slice_str("first"), last = ldb_slice_str("last");
  ldb_write_batch_put(&batch, &key, &first);
  ldb_write_batch_delete(&batch, &gone);
  ldb_write_batch_put(&batch, &key, &last);
  re_db_write(&t, &batch);
  ldb_write_batch_destroy(&batch);
  CHECK_EQ(4, ldb_db_impl_test_last_sequence(t.db));
  for (int i = 0; i < 3; i++) {
    re_db_reopen(&t);
    CHECK_EQ(4, ldb_db_impl_test_last_sequence(t.db));
    re_db_expect(&t, NULL, "gone", NULL);
    re_db_expect(&t, NULL, "key", "last");
  }
  re_db_set(&t, "key", "new");
  CHECK_EQ(5, ldb_db_impl_test_last_sequence(t.db));
  re_db_reopen(&t);
  CHECK_EQ(5, ldb_db_impl_test_last_sequence(t.db));
  re_db_expect(&t, NULL, "key", "new");
  re_db_destroy(&t);
}

// Equivalent to RecoveryTest::MakeLogFile: valid WriteBatch in a valid WAL,
// only while DB is closed. No manifest editing or real files involved.
static void re_make_log(re_db* t, uint64_t number, uint64_t sequence,
                        const char* key, const char* value) {
  char* path = ldb_log_file_name(re_db_path, number);
  ldb_writable_file* file = NULL;
  re_ok(ldb_env_new_writable_file(t->options.env, path, &file));
  ldb_log_writer* writer = ldb_log_writer_new(file);
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_slice k = ldb_slice_str(key), v = ldb_slice_str(value);
  ldb_write_batch_put(&batch, &k, &v);
  ldb_write_batch_set_sequence(&batch, sequence);
  ldb_slice record = ldb_write_batch_contents(&batch);
  re_ok(ldb_log_writer_add_record(writer, &record));
  ldb_write_batch_destroy(&batch);
  ldb_log_writer_destroy(writer);
  re_ok(file->m->close(file));
  file->m->destroy(file);
  free(path);
}

TEST(recovery_extra, MultipleValidLogsRecoverMaximumSequence) {
  re_db t;
  re_db_init(&t);
  re_db_set(&t, "key", "original");
  uint64_t old_log = t.db->logfile_number;
  ldb_db_impl_destroy(t.db);
  t.db = NULL;
  // Create in reverse order to ensure recovery sorts by file number.
  re_make_log(&t, old_log + 3, 1002, "key", "latest");
  re_make_log(&t, old_log + 2, 1001, "other", "retained");
  re_make_log(&t, old_log + 1, 1000, "key", "middle");
  for (int i = 0; i < 2; i++) {
    re_db_reopen(&t);
    CHECK_EQ(1002, ldb_db_impl_test_last_sequence(t.db));
    re_db_expect(&t, NULL, "key", "latest");
    re_db_expect(&t, NULL, "other", "retained");
    CHECK_GT(t.db->logfile_number, old_log + 3);
  }
  re_db_set(&t, "key", "after-recovery");
  CHECK_EQ(1003, ldb_db_impl_test_last_sequence(t.db));
  re_db_reopen(&t);
  re_db_expect(&t, NULL, "key", "after-recovery");
  CHECK_EQ(1003, ldb_db_impl_test_last_sequence(t.db));
  re_db_destroy(&t);
}

static void re_compact(re_db* t) {
  re_ok(ldb_db_impl_test_compact_mem_table(t->db));
  // Synchronous test hooks, no sleeps or assumptions about background timing.
  for (int level = 0; level < LDB_K_NUM_LEVELS - 1; level++)
    ldb_db_impl_test_compact_range(t->db, level, NULL, NULL);
  CHECK_STATUS_OK(t->db->bg_error);
}

TEST(recovery_extra, MultipleSnapshotsSurviveCompaction) {
  re_db t;
  re_db_init(&t);
  re_db_set(&t, "key", "v1");
  const ldb_snapshot_impl* s1 = ldb_db_impl_get_snapshot(t.db);
  re_compact(&t);
  re_db_set(&t, "key", "v2");
  const ldb_snapshot_impl* s2 = ldb_db_impl_get_snapshot(t.db);
  re_compact(&t);
  re_db_set(&t, "key", "v3");
  re_compact(&t);
  re_db_expect(&t, s1, "key", "v1");
  re_db_expect(&t, s2, "key", "v2");
  re_db_expect(&t, NULL, "key", "v3");
  ldb_db_impl_release_snapshot(t.db, s1);
  re_compact(&t);
  re_db_expect(&t, s2, "key", "v2");
  re_db_expect(&t, NULL, "key", "v3");
  ldb_db_impl_release_snapshot(t.db, s2);
  re_compact(&t);
  re_db_reopen(&t);  // snapshots were released before DB destruction
  re_db_expect(&t, NULL, "key", "v3");
  CHECK_EQ(3, ldb_db_impl_test_last_sequence(t.db));
  re_db_destroy(&t);
}

TEST(recovery_extra, SnapshotDeletionAndReinsertAcrossCompaction) {
  re_db t;
  re_db_init(&t);
  re_db_set(&t, "key", "original");
  const ldb_snapshot_impl* before = ldb_db_impl_get_snapshot(t.db);
  re_compact(&t);
  re_db_set(&t, "key", NULL);
  const ldb_snapshot_impl* deleted = ldb_db_impl_get_snapshot(t.db);
  re_compact(&t);
  re_db_expect(&t, before, "key", "original");
  re_db_expect(&t, deleted, "key", NULL);
  re_db_expect(&t, NULL, "key", NULL);
  re_db_set(&t, "key", "reborn");
  re_compact(&t);
  re_db_expect(&t, before, "key", "original");
  re_db_expect(&t, deleted, "key", NULL);
  re_db_expect(&t, NULL, "key", "reborn");
  ldb_db_impl_release_snapshot(t.db, before);
  re_compact(&t);
  re_db_expect(&t, deleted, "key", NULL);
  ldb_db_impl_release_snapshot(t.db, deleted);
  re_compact(&t);
  re_db_reopen(&t);
  re_db_expect(&t, NULL, "key", "reborn");
  CHECK_EQ(3, ldb_db_impl_test_last_sequence(t.db));
  re_db_destroy(&t);
}
