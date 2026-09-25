// test_log.c - port of leveldb log_test (core scenarios), run against the
// in-memory env.
#include "harness.h"

static const char* std_string(size_t n);

typedef struct log_test_env {
  ldb_env* env;
  char dir[256];
  char wname[300];
  char rname[300];
  ldb_writable_file* writer_dest;
  ldb_log_writer* writer;
  ldb_seq_file* reader_src;
  ldb_log_reader* reader;
  ldb_buffer report;
} log_test_env;

static void lt_open_writer(log_test_env* t) {
  t->env = ldb_memenv_new();
  t->writer_dest = NULL;
  t->writer = NULL;
  t->reader = NULL;
  t->reader_src = NULL;
  ldb_buffer_init(&t->report);
  snprintf(t->wname, sizeof(t->wname), "/memenv/test.log");
  // memenv paths are matched exactly
  CHECK_STATUS_OK(ldb_env_new_writable_file(t->env, t->wname, &t->writer_dest));
  t->writer = ldb_log_writer_new(t->writer_dest);
}

static void lt_close_writer(log_test_env* t) {
  if (t->writer) ldb_log_writer_destroy(t->writer);
  t->writer = NULL;
  if (t->writer_dest) {
    t->writer_dest->m->close(t->writer_dest);
    t->writer_dest->m->destroy(t->writer_dest);
    t->writer_dest = NULL;
  }
}

static void lt_open_reader(log_test_env* t) {
  CHECK_STATUS_OK(ldb_env_new_sequential_file(t->env, t->wname, &t->reader_src));
  t->reader = ldb_log_reader_new(t->reader_src, NULL, NULL, 1, 0);
}

static void lt_close_reader(log_test_env* t) {
  if (t->reader) ldb_log_reader_destroy(t->reader);
  t->reader = NULL;
  if (t->reader_src) {
    t->reader_src->m->destroy(t->reader_src);
    t->reader_src = NULL;
  }
}

static void lt_teardown(log_test_env* t) {
  lt_close_writer(t);
  lt_close_reader(t);
  ldb_buffer_destroy(&t->report);
  ldb_memenv_destroy(t->env);
  t->env = NULL;
}

static void lt_write(log_test_env* t, const char* msg) {
  ldb_slice s = ldb_slice_str(msg);
  CHECK_STATUS_OK(ldb_log_writer_add_record(t->writer, &s));
}

static ldb_slice lt_read(log_test_env* t) {
  ldb_slice record;
  static ldb_buffer scratch;
  ldb_buffer_clear(&scratch);
  if (ldb_log_reader_read_record(t->reader, &record, &scratch)) {
    return record;
  }
  return ldb_slice_make("EOF", 3);
}

static void lt_check_eq(log_test_env* t, const char* expected) {
  ldb_slice r = lt_read(t);
  ldb_slice e = ldb_slice_str(expected);
  if (!ldb_slice_equals(&r, &e)) {
    ldb_test_fail(__FILE__, __LINE__, "expected '%s', got '%.*s'", expected,
                  (int)r.size, r.data ? r.data : "");
  }
  return;
}

TEST(log, Empty) {
  log_test_env t;
  lt_open_writer(&t);
  lt_open_reader(&t);
  ldb_slice r = lt_read(&t);
  CHECK_STR_EQ("EOF", r.data);
  lt_teardown(&t);
}

TEST(log, ReadWrite) {
  log_test_env t;
  lt_open_writer(&t);
  lt_write(&t, "foo");
  lt_write(&t, "bar");
  lt_write(&t, "");
  lt_open_reader(&t);
  lt_check_eq(&t, "foo");
  lt_check_eq(&t, "bar");
  lt_check_eq(&t, "");
  lt_check_eq(&t, "EOF");
  lt_teardown(&t);
}

TEST(log, ManyRecords) {
  log_test_env t;
  lt_open_writer(&t);
  for (int i = 0; i < 1000; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "record %d", i);
    lt_write(&t, buf);
  }
  lt_open_reader(&t);
  for (int i = 0; i < 1000; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "record %d", i);
    lt_check_eq(&t, buf);
  }
  lt_check_eq(&t, "EOF");
  lt_teardown(&t);
}

TEST(log, ManyBlocks) {
  log_test_env t;
  lt_open_writer(&t);
  char big[30000];
  memset(big, 'a', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  for (int i = 0; i < 10; i++) {
    lt_write(&t, big);
  }
  lt_open_reader(&t);
  for (int i = 0; i < 10; i++) {
    lt_check_eq(&t, big);
  }
  lt_check_eq(&t, "EOF");
  lt_teardown(&t);
}

TEST(log, Fragmentation) {
  log_test_env t;
  lt_open_writer(&t);
  // Record larger than a block (32768): force FIRST/MIDDLE/LAST records
  char big[80000];
  memset(big, 'x', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  lt_write(&t, big);
  lt_open_reader(&t);
  lt_check_eq(&t, big);
  lt_check_eq(&t, "EOF");
  lt_teardown(&t);
}

TEST(log, BlockBoundary) {
  log_test_env t;
  lt_open_writer(&t);
  // Write a record leaving exactly the header at a block boundary
  ldb_slice a = ldb_slice_str("one");
  CHECK_STATUS_OK(ldb_log_writer_add_record(t.writer, &a));
  // Fill block up to LDB_LOG_BLOCK_SIZE - header - small record
  size_t target = LDB_LOG_BLOCK_SIZE - 7 - 3 /* 'one' */ - 7;
  char* filler = (char*)malloc(target + 1);
  memset(filler, 'b', target);
  filler[target] = '\0';
  ldb_slice f = ldb_slice_make(filler, target);
  CHECK_STATUS_OK(ldb_log_writer_add_record(t.writer, &f));
  ldb_slice b = ldb_slice_str("two");
  CHECK_STATUS_OK(ldb_log_writer_add_record(t.writer, &b));
  free(filler);
  lt_open_reader(&t);
  lt_check_eq(&t, "one");
  lt_check_eq(&t, std_string(target));
  lt_check_eq(&t, "two");
  lt_check_eq(&t, "EOF");
  lt_teardown(&t);
}

// Helper producing a reusable string of 'n' 'b' chars (test-only).
static const char* std_string(size_t n) {
  static char* buf = NULL;
  static size_t buf_len = 0;
  if (buf_len < n + 1) {
    buf = (char*)realloc(buf, n + 1);
    buf_len = n + 1;
  }
  memset(buf, 'b', n);
  buf[n] = '\0';
  return buf;
}

// ------------------------------------------------- initial offset reading
// (port of leveldb log_test WriteInitialOffsetLog / CheckInitialOffsetRecord)
static const size_t lt_initial_offset_sizes[] = {
    10000,                        // Two sizable records in first block
    10000,
    2 * LDB_LOG_BLOCK_SIZE - 1000,  // Span three blocks
    1,
    13716,                          // Consume all but two bytes of block 3.
    LDB_LOG_BLOCK_SIZE - 7,         // Consume the entirety of block 4.
};

static const uint64_t lt_initial_offset_last_offsets[] = {
    0,
    7 + 10000,
    2 * (7 + 10000),
    2 * (7 + 10000) + (2 * LDB_LOG_BLOCK_SIZE - 1000) + 3 * 7,
    2 * (7 + 10000) + (2 * LDB_LOG_BLOCK_SIZE - 1000) + 3 * 7 + 7 + 1,
    3 * LDB_LOG_BLOCK_SIZE,
};

#define LT_NUM_INITIAL_OFFSET_RECORDS \
  (sizeof(lt_initial_offset_sizes) / sizeof(lt_initial_offset_sizes[0]))

static void lt_write_initial_offset_log(log_test_env* t) {
  for (size_t i = 0; i < LT_NUM_INITIAL_OFFSET_RECORDS; i++) {
    size_t n = lt_initial_offset_sizes[i];
    char* buf = (char*)malloc(n);
    memset(buf, 'a' + (int)i, n);
    ldb_slice s = ldb_slice_make(buf, n);
    CHECK_STATUS_OK(ldb_log_writer_add_record(t->writer, &s));
    free(buf);
  }
}

static void lt_check_initial_offset_record(uint64_t initial_offset,
                                           int expected_record_offset) {
  log_test_env t;
  lt_open_writer(&t);
  lt_write_initial_offset_log(&t);
  CHECK_STATUS_OK(ldb_env_new_sequential_file(t.env, t.wname, &t.reader_src));
  t.reader = ldb_log_reader_new(t.reader_src, NULL, NULL, 1, initial_offset);

  CHECK(expected_record_offset >= 0 &&
        (size_t)expected_record_offset < LT_NUM_INITIAL_OFFSET_RECORDS);
  // scratch must outlive the checks below: fragmented records alias it.
  ldb_buffer scratch;
  ldb_buffer_init(&scratch);
  for (int i = expected_record_offset; i < (int)LT_NUM_INITIAL_OFFSET_RECORDS;
       i++) {
    ldb_slice record;
    if (!ldb_log_reader_read_record(t.reader, &record, &scratch)) {
      ldb_buffer_destroy(&scratch);
      ldb_test_fail(__FILE__, __LINE__, "offset %llu: record %d: EOF",
                    (unsigned long long)initial_offset, i);
    }
    if ((size_t)record.size != lt_initial_offset_sizes[i]) {
      ldb_buffer_destroy(&scratch);
      ldb_test_fail(__FILE__, __LINE__, "offset %llu: record %d: size %zu != %zu",
                    (unsigned long long)initial_offset, i, record.size,
                    lt_initial_offset_sizes[i]);
    }
    if (ldb_log_reader_last_record_offset(t.reader) !=
        lt_initial_offset_last_offsets[i]) {
      ldb_buffer_destroy(&scratch);
      ldb_test_fail(__FILE__, __LINE__,
                    "offset %llu: record %d: last_record_offset %llu != %llu",
                    (unsigned long long)initial_offset, i,
                    (unsigned long long)ldb_log_reader_last_record_offset(t.reader),
                    (unsigned long long)lt_initial_offset_last_offsets[i]);
    }
    if (record.data[0] != 'a' + i) {
      ldb_buffer_destroy(&scratch);
      ldb_test_fail(__FILE__, __LINE__,
                    "offset %llu: record %d: first byte '%c' != '%c'",
                    (unsigned long long)initial_offset, i, record.data[0],
                    (char)('a' + i));
    }
  }
  ldb_buffer_destroy(&scratch);
  lt_teardown(&t);
}

static void lt_check_offset_past_end(uint64_t offset_past_end) {
  log_test_env t;
  lt_open_writer(&t);
  lt_write_initial_offset_log(&t);
  // Ask the env for the file size (covers fragment headers and end-of-block
  // zero padding exactly).
  uint64_t written = 0;
  CHECK_STATUS_OK(ldb_env_get_file_size(t.env, t.wname, &written));
  CHECK_STATUS_OK(ldb_env_new_sequential_file(t.env, t.wname, &t.reader_src));
  t.reader = ldb_log_reader_new(t.reader_src, NULL, NULL, 1,
                                written + offset_past_end);
  ldb_slice record;
  ldb_buffer scratch;
  ldb_buffer_init(&scratch);
  CHECK_EQ(0, ldb_log_reader_read_record(t.reader, &record, &scratch));
  ldb_buffer_destroy(&scratch);
  lt_teardown(&t);
}

TEST(log, ReadStart) { lt_check_initial_offset_record(0, 0); }

TEST(log, ReadSecondOneOff) { lt_check_initial_offset_record(1, 1); }

TEST(log, ReadSecondTenThousand) { lt_check_initial_offset_record(10000, 1); }

TEST(log, ReadSecondStart) { lt_check_initial_offset_record(10007, 1); }

TEST(log, ReadThirdOneOff) { lt_check_initial_offset_record(10008, 2); }

TEST(log, ReadThirdStart) { lt_check_initial_offset_record(20014, 2); }

TEST(log, ReadFourthOneOff) { lt_check_initial_offset_record(20015, 3); }

TEST(log, ReadFourthFirstBlockTrailer) {
  lt_check_initial_offset_record(LDB_LOG_BLOCK_SIZE - 4, 3);
}

TEST(log, ReadFourthMiddleBlock) {
  lt_check_initial_offset_record(LDB_LOG_BLOCK_SIZE + 1, 3);
}

TEST(log, ReadFourthLastBlock) {
  lt_check_initial_offset_record(2 * LDB_LOG_BLOCK_SIZE + 1, 3);
}

TEST(log, ReadFourthStart) {
  lt_check_initial_offset_record(
      2 * (7 + 1000) + (2 * LDB_LOG_BLOCK_SIZE - 1000) + 3 * 7, 3);
}

TEST(log, ReadInitialOffsetIntoBlockPadding) {
  lt_check_initial_offset_record(3 * LDB_LOG_BLOCK_SIZE - 3, 5);
}

TEST(log, ReadEnd) { lt_check_offset_past_end(0); }

TEST(log, ReadPastEnd) { lt_check_offset_past_end(5); }
