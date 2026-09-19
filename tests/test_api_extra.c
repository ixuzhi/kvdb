// Public C API regressions; each test uses a separate real-filesystem DB.
#include "port.h"
#include "harness.h"
#include "leveldb/c.h"

typedef struct api_fixture {
  char path[400];
  leveldb_t* db;
  leveldb_options_t* options;
  leveldb_readoptions_t* ro;
  leveldb_writeoptions_t* wo;
} api_fixture;

static void api_init(api_fixture* f, const char* name) {
  memset(f, 0, sizeof(*f));
  ldb_test_make_db_path(name, f->path, sizeof(f->path));
  ldb_test_destroy_dir(f->path); // Also remove leftovers from a timed-out run.
  f->options = leveldb_options_create();
  leveldb_options_set_create_if_missing(f->options, 1);
  leveldb_options_set_compression(f->options, leveldb_no_compression);
  f->ro = leveldb_readoptions_create();
  leveldb_readoptions_set_verify_checksums(f->ro, 1);
  f->wo = leveldb_writeoptions_create();
}

static void api_open(api_fixture* f) {
  char* err = NULL;
  f->db = leveldb_open(f->options, f->path, &err);
  int ok = f->db != NULL && err == NULL;
  leveldb_free(err);
  CHECK(ok);
}

static void api_reopen(api_fixture* f) {
  leveldb_close(f->db);
  f->db = NULL;
  leveldb_options_set_create_if_missing(f->options, 0);
  api_open(f);
}

static void api_done(api_fixture* f) {
  if (f->db) leveldb_close(f->db);
  leveldb_readoptions_destroy(f->ro);
  leveldb_writeoptions_destroy(f->wo);
  leveldb_options_destroy(f->options);
  ldb_test_destroy_dir(f->path);
}

static void api_put(api_fixture* f, const char* k, size_t kn,
                    const char* v, size_t vn) {
  char* err = NULL;
  leveldb_put(f->db, f->wo, k, kn, v, vn, &err);
  int ok = err == NULL;
  leveldb_free(err);
  CHECK(ok);
}

static void api_delete(api_fixture* f, const char* k, size_t kn) {
  char* err = NULL;
  leveldb_delete(f->db, f->wo, k, kn, &err);
  int ok = err == NULL;
  leveldb_free(err);
  CHECK(ok);
}

// NULL expected means absent; "", 0 means a present, empty value.
static int api_matches(api_fixture* f, const char* k, size_t kn,
                       const char* expected, size_t n) {
  size_t len = 98765;
  char* err = NULL;
  char* got = leveldb_get(f->db, f->ro, k, kn, &len, &err);
  int ok = err == NULL && len == n;
  if (expected) ok = ok && got != NULL && (n == 0 || memcmp(got, expected, n) == 0);
  else ok = ok && got == NULL;
  leveldb_free(got);
  leveldb_free(err);
  return ok;
}

static void api_compact(api_fixture* f) {
  leveldb_compact_range(f->db, NULL, 0, NULL, 0);
}

TEST(api_extra, MissingKeyZeroLength) {
  api_fixture f;
  api_init(&f, "api_extra_missing");
  api_open(&f);
  int ok = api_matches(&f, "never-written", 13, NULL, 0);
  api_put(&f, "gone", 4, "value", 5);
  api_delete(&f, "gone", 4);
  ok &= api_matches(&f, "gone", 4, NULL, 0);
  api_compact(&f);
  api_reopen(&f);
  ok &= api_matches(&f, "gone", 4, NULL, 0);
  ok &= api_matches(&f, "never-written", 13, NULL, 0);
  api_done(&f);
  CHECK(ok);
}

TEST(api_extra, MissingKeyPreservesExistingError) {
  api_fixture f;
  api_init(&f, "api_extra_existing_error");
  // Obtain an actual library-allocated error (important for Windows CRTs).
  leveldb_options_set_create_if_missing(f.options, 0);
  char* err = NULL;
  leveldb_t* absent = leveldb_open(f.options, f.path, &err);
  CHECK(absent == NULL && err != NULL);
  size_t saved_len = strlen(err);
  char* saved_text = (char*)malloc(saved_len + 1);
  CHECK(saved_text != NULL);
  memcpy(saved_text, err, saved_len + 1);
  char* original = err;
  leveldb_options_set_create_if_missing(f.options, 1);
  api_open(&f);
  size_t len = 12345;
  char* got = leveldb_get(f.db, f.ro, "absent", 6, &len, &err);
  int ok = got == NULL && len == 0 && err == original;
  ok = ok && err != NULL && strcmp(err, saved_text) == 0;
  leveldb_free(got);
  leveldb_free(err); // Do not free original separately: failure may replace it.
  free(saved_text);
  api_done(&f);
  CHECK(ok);
}

TEST(api_extra, BinaryAndEmptyKeysValues) {
  api_fixture f;
  api_init(&f, "api_extra_binary_empty");
  api_open(&f);
  const char k1[] = {'a', 0, 'b', (char)0xff};
  const char k2[] = {'a', 0, 'c', (char)0xff};
  const char value[] = {0, (char)0xff, 'x', 0, 'y'};
  api_put(&f, "", 0, value, sizeof(value));
  api_put(&f, k1, sizeof(k1), value, sizeof(value));
  api_put(&f, k2, sizeof(k2), "", 0);
  api_put(&f, "a", 1, "prefix", 6);
  for (int pass = 0; pass < 2; ++pass) {
    CHECK(api_matches(&f, "", 0, value, sizeof(value)));
    CHECK(api_matches(&f, k1, sizeof(k1), value, sizeof(value)));
    CHECK(api_matches(&f, k2, sizeof(k2), "", 0));
    CHECK(api_matches(&f, "a", 1, "prefix", 6));
    if (pass == 0) { api_compact(&f); api_reopen(&f); }
  }
  api_put(&f, "", 0, "", 0);
  CHECK(api_matches(&f, "", 0, "", 0));
  api_delete(&f, k1, sizeof(k1));
  CHECK(api_matches(&f, k1, sizeof(k1), NULL, 0));
  CHECK(api_matches(&f, k2, sizeof(k2), "", 0));
  api_done(&f);
}

TEST(api_extra, BatchOrderingAppendAndClear) {
  api_fixture f;
  api_init(&f, "api_extra_batch");
  api_open(&f);
  leveldb_writebatch_t* a = leveldb_writebatch_create();
  leveldb_writebatch_t* b = leveldb_writebatch_create();
  const char key[] = {'k', 0, 'b'};
  leveldb_writebatch_put(a, key, sizeof(key), "old", 3);
  leveldb_writebatch_delete(a, key, sizeof(key));
  leveldb_writebatch_put(b, key, sizeof(key), "new", 3);
  leveldb_writebatch_put(b, "", 0, "", 0);
  leveldb_writebatch_append(a, b);
  leveldb_writebatch_clear(b);
  leveldb_writebatch_put(b, "unwritten", 9, "x", 1);
  char* err = NULL;
  leveldb_write(f.db, f.wo, a, &err);
  int ok = err == NULL;
  leveldb_free(err);
  leveldb_writebatch_destroy(a);
  leveldb_writebatch_destroy(b);
  api_reopen(&f);
  ok &= api_matches(&f, key, sizeof(key), "new", 3);
  ok &= api_matches(&f, "", 0, "", 0);
  ok &= api_matches(&f, "unwritten", 9, NULL, 0);
  api_done(&f);
  CHECK(ok);
}

TEST(api_extra, SnapshotSurvivesCompactionThenReopen) {
  api_fixture f;
  api_init(&f, "api_extra_snapshot");
  api_open(&f);
  api_put(&f, "a", 1, "old", 3);
  api_put(&f, "b", 1, "kept", 4);
  const leveldb_snapshot_t* snap = leveldb_create_snapshot(f.db);
  CHECK(snap != NULL);
  api_put(&f, "a", 1, "new", 3);
  api_delete(&f, "b", 1);
  api_put(&f, "c", 1, "later", 5);
  api_compact(&f);
  leveldb_readoptions_set_snapshot(f.ro, snap);
  int ok = api_matches(&f, "a", 1, "old", 3);
  ok &= api_matches(&f, "b", 1, "kept", 4);
  ok &= api_matches(&f, "c", 1, NULL, 0);
  leveldb_readoptions_set_snapshot(f.ro, NULL);
  leveldb_release_snapshot(f.db, snap); // Snapshots never cross close/reopen.
  api_compact(&f);
  api_reopen(&f);
  ok &= api_matches(&f, "a", 1, "new", 3);
  ok &= api_matches(&f, "b", 1, NULL, 0);
  ok &= api_matches(&f, "c", 1, "later", 5);
  api_done(&f);
  CHECK(ok);
}

TEST(api_extra, BloomCompactReopenReads) {
  api_fixture f;
  api_init(&f, "api_extra_bloom");
  leveldb_filterpolicy_t* bloom = leveldb_filterpolicy_create_bloom(10);
  leveldb_options_set_filter_policy(f.options, bloom);
  leveldb_options_set_block_size(f.options, 512);
  api_open(&f);
  char value[160];
  for (int i = 0; i < 512; ++i) {
    char key[4] = {(char)(i >> 8), (char)i, 0, (char)0xff};
    memset(value, i & 255, sizeof(value));
    api_put(&f, key, sizeof(key), value, sizeof(value));
  }
  // Reading the memtable alone never exercises the C Bloom bridge.
  api_compact(&f);
  int files = 0;
  for (int level = 0; level < 7; ++level) {
    char property[64];
    snprintf(property, sizeof(property), "leveldb.num-files-at-level%d", level);
    char* count = leveldb_property_value(f.db, property);
    if (count) files += atoi(count);
    leveldb_free(count);
  }
  CHECK(files > 0);
  api_reopen(&f);
  for (int i = 0; i < 512; ++i) {
    char key[4] = {(char)(i >> 8), (char)i, 0, (char)0xff};
    memset(value, i & 255, sizeof(value));
    CHECK(api_matches(&f, key, sizeof(key), value, sizeof(value)));
  }
  CHECK(api_matches(&f, "missing", 7, NULL, 0));
  api_done(&f);
  leveldb_filterpolicy_destroy(bloom);
}

typedef struct api_cmp_state {
  const char* name;
  int destroyed;
} api_cmp_state;

static void api_cmp_destroy(void* raw) {
  ((api_cmp_state*)raw)->destroyed++;
}
static const char* api_cmp_name(void* raw) {
  return ((api_cmp_state*)raw)->name;
}
static int api_cmp_reverse(void* raw, const char* a, size_t an,
                           const char* b, size_t bn) {
  (void)raw;
  int r = memcmp(a, b, an < bn ? an : bn);
  if (r) return r < 0 ? 1 : -1;
  return an < bn ? 1 : an > bn ? -1 : 0;
}
static leveldb_comparator_t* api_cmp_new(api_cmp_state* state) {
  return leveldb_comparator_create(state, api_cmp_destroy,
                                   api_cmp_reverse, api_cmp_name);
}

TEST(api_extra, CustomComparatorOrderAfterReopen) {
  api_fixture f;
  api_init(&f, "api_extra_comparator_order");
  api_cmp_state state = {"api_extra.Reverse.v1", 0};
  leveldb_comparator_t* cmp = api_cmp_new(&state);
  leveldb_options_set_comparator(f.options, cmp);
  api_open(&f);
  api_put(&f, "a", 1, "one", 3);
  api_put(&f, "b", 1, "two", 3);
  api_put(&f, "c", 1, "three", 5);
  api_compact(&f);
  api_reopen(&f);
  leveldb_iterator_t* it = leveldb_create_iterator(f.db, f.ro);
  leveldb_iter_seek_to_first(it);
  int ok = 1;
  for (char expected = 'c'; expected >= 'a'; --expected) {
    if (!leveldb_iter_valid(it)) { ok = 0; break; }
    size_t len = 0;
    const char* key = leveldb_iter_key(it, &len);
    ok &= len == 1 && key[0] == expected;
    leveldb_iter_next(it);
  }
  ok &= !leveldb_iter_valid(it);
  leveldb_iter_seek(it, "bb", 2);
  if (!leveldb_iter_valid(it)) ok = 0;
  else {
    size_t len = 0;
    const char* key = leveldb_iter_key(it, &len);
    ok &= len == 1 && key[0] == 'b';
  }
  char* err = NULL;
  leveldb_iter_get_error(it, &err);
  ok &= err == NULL;
  leveldb_free(err);
  leveldb_iter_destroy(it);
  ok &= api_matches(&f, "b", 1, "two", 3);
  api_done(&f);
  ok &= state.destroyed == 0;
  leveldb_comparator_destroy(cmp);
  CHECK(ok);
  CHECK_EQ(1, state.destroyed);
}

TEST(api_extra, ComparatorNameMismatchRejectsOpen) {
  api_fixture f;
  api_init(&f, "api_extra_comparator_mismatch");
  api_cmp_state a = {"api_extra.Reverse.v1", 0};
  api_cmp_state b = {"api_extra.Reverse.v2", 0};
  leveldb_comparator_t* ca = api_cmp_new(&a);
  leveldb_comparator_t* cb = api_cmp_new(&b);
  leveldb_options_set_comparator(f.options, ca);
  api_open(&f);
  api_put(&f, "k", 1, "persistent", 10);
  leveldb_close(f.db);
  f.db = NULL;
  leveldb_options_set_create_if_missing(f.options, 0);
  leveldb_options_set_comparator(f.options, cb);
  char* err = NULL;
  leveldb_t* wrong = leveldb_open(f.options, f.path, &err);
  int ok = wrong == NULL && err != NULL;
  ok = ok && strstr(err, "comparator") != NULL;
  if (wrong) leveldb_close(wrong);
  leveldb_free(err);
  leveldb_options_set_comparator(f.options, ca);
  api_open(&f); // Rejection must not damage the manifest or retain the lock.
  ok &= api_matches(&f, "k", 1, "persistent", 10);
  api_done(&f);
  leveldb_comparator_destroy(ca);
  leveldb_comparator_destroy(cb);
  CHECK(ok);
  CHECK_EQ(1, a.destroyed);
  CHECK_EQ(1, b.destroyed);
}

// DestroyDB must be able to remove the database while this very process is
// still alive, so every handle the previous open took has to be back. Windows
// refuses to delete or rename a file it still has open, which turns a leaked
// info-log handle into an IO error on the *next* open.
TEST(api_extra, DestroyDbThenReopenInSameProcess) {
  api_fixture f;
  api_init(&f, "api_extra_destroy_reopen");
  api_open(&f);
  api_put(&f, "keep", 4, "value", 5);
  for (int round = 0; round < 3; round++) {
    leveldb_close(f.db);
    f.db = NULL;
    char* err = NULL;
    leveldb_destroy_db(f.options, f.path, &err);
    int ok = err == NULL;
    leveldb_free(err);
    CHECK(ok);
    leveldb_options_set_create_if_missing(f.options, 1);
    api_open(&f);
    size_t len = 1;
    err = NULL;
    char* got = leveldb_get(f.db, f.ro, "keep", 4, &len, &err);
    // The database is brand new again: nothing of the previous round survives.
    ok = err == NULL && got == NULL;
    leveldb_free(got);
    leveldb_free(err);
    CHECK(ok);
    api_put(&f, "keep", 4, "value", 5);
    api_compact(&f);
    CHECK(api_matches(&f, "keep", 4, "value", 5));
  }
  api_done(&f);
}

// port.h deliberately has detached threads only. The completion handshake
// acts as a lifetime fence: after the final unlock a worker never touches
// shared state again. No harness assertion/longjmp is allowed in a worker,
// or in the parent between launch and completion. A deadlock blocks on a
// condition variable, not a spin loop; the external runner supplies timeout.
typedef struct api_threads {
  api_fixture* f;
  ldb_mutex mu;
  ldb_cond cv;
  int start, done, failures, read_done, write_done;
  int arrived, generation;
  int read_case;
} api_threads;

typedef struct api_worker {
  api_threads* shared;
  int id;
} api_worker;

static void api_threads_init(api_threads* t, api_fixture* f) {
  memset(t, 0, sizeof(*t));
  t->f = f;
  ldb_mutex_init(&t->mu);
  ldb_cond_init(&t->cv, &t->mu);
}
static void api_worker_finish(api_threads* t, int failures) {
  ldb_mutex_lock(&t->mu);
  t->failures += failures;
  ++t->done;
  ldb_cond_signal_all(&t->cv);
  ldb_mutex_unlock(&t->mu); // Last access to caller-owned state.
}
static int api_threads_wait(api_threads* t, int count) {
  ldb_mutex_lock(&t->mu);
  t->start = 1;
  ldb_cond_signal_all(&t->cv);
  while (t->done != count) ldb_cond_wait(&t->cv, &t->mu);
  int failures = t->failures;
  ldb_mutex_unlock(&t->mu);
  ldb_cond_destroy(&t->cv);
  ldb_mutex_destroy(&t->mu);
  return failures;
}

static void api_get_then_wait(void* raw) {
  api_threads* t = (api_threads*)raw;
  const char* keys[] = {"hit", "absent", "deleted"};
  int failure = !api_matches(t->f, keys[t->read_case],
                             strlen(keys[t->read_case]),
                             t->read_case == 0 ? "value" : NULL,
                             t->read_case == 0 ? 5 : 0);
  ldb_mutex_lock(&t->mu);
  t->read_done = 1;
  ldb_cond_signal_all(&t->cv);
  // Keep the Get thread alive: an accidentally retained recursive DB mutex
  // must not be hidden by thread exit (or OS abandoned-lock behavior).
  while (!t->write_done) ldb_cond_wait(&t->cv, &t->mu);
  ldb_mutex_unlock(&t->mu);
  api_worker_finish(t, failure);
}
static void api_put_after_get(void* raw) {
  api_threads* t = (api_threads*)raw;
  ldb_mutex_lock(&t->mu);
  while (!t->read_done) ldb_cond_wait(&t->cv, &t->mu);
  ldb_mutex_unlock(&t->mu);
  char* err = NULL;
  leveldb_put(t->f->db, t->f->wo, "after", 5, "written", 7, &err);
  int failure = err != NULL;
  leveldb_free(err);
  ldb_mutex_lock(&t->mu);
  t->write_done = 1;
  ldb_cond_signal_all(&t->cv);
  ldb_mutex_unlock(&t->mu);
  api_worker_finish(t, failure);
}

TEST(api_extra, GetThenOtherThreadPut) {
  api_fixture f;
  api_init(&f, "api_extra_get_unlock");
  api_open(&f);
  api_put(&f, "hit", 3, "value", 5);
  api_put(&f, "deleted", 7, "old", 3);
  api_delete(&f, "deleted", 7);
  int failures = 0;
  for (int read_case = 0; read_case < 3; ++read_case) {
    api_threads t;
    api_threads_init(&t, &f);
    t.read_case = read_case;
    ldb_start_thread(api_get_then_wait, &t);
    ldb_start_thread(api_put_after_get, &t);
    failures += api_threads_wait(&t, 2);
  }
  int ok = api_matches(&f, "after", 5, "written", 7);
  api_done(&f);
  CHECK_EQ(0, failures);
  CHECK(ok);
}

static void api_round_barrier(api_threads* t) {
  ldb_mutex_lock(&t->mu);
  int generation = t->generation;
  if (++t->arrived == 4) {
    t->arrived = 0;
    ++t->generation;
    ldb_cond_signal_all(&t->cv);
  } else {
    while (generation == t->generation) ldb_cond_wait(&t->cv, &t->mu);
  }
  ldb_mutex_unlock(&t->mu);
}
static void api_concurrent_worker(void* raw) {
  api_worker* w = (api_worker*)raw;
  api_threads* t = w->shared;
  int id = w->id;
  int failures = 0;
  leveldb_readoptions_t* ro = leveldb_readoptions_create();
  leveldb_writeoptions_t* wo = leveldb_writeoptions_create();
  ldb_mutex_lock(&t->mu);
  while (!t->start) ldb_cond_wait(&t->cv, &t->mu);
  ldb_mutex_unlock(&t->mu);
  for (int round = 1; round <= 120; ++round) {
    api_round_barrier(t);
    char key = (char)('a' + id % 2);
    char* err = NULL;
    if (id < 2) {
      char value[128];
      memset(value, round, sizeof(value));
      leveldb_put(t->f->db, wo, &key, 1, value, sizeof(value), &err);
      failures += err != NULL;
    } else {
      size_t n = 0;
      char* got = leveldb_get(t->f->db, ro, &key, 1, &n, &err);
      int ok = err == NULL && got != NULL && n == 128;
      if (ok) {
        unsigned char v = (unsigned char)got[0];
        // The barrier ensures each read sees this round or the previous one.
        ok = v == round || v == round - 1;
        for (size_t j = 1; j < n; ++j) ok &= got[j] == got[0];
      }
      failures += !ok;
      leveldb_free(got);
    }
    leveldb_free(err);
    // Do not early-return on error: all workers must reach every barrier.
    api_round_barrier(t);
  }
  leveldb_readoptions_destroy(ro);
  leveldb_writeoptions_destroy(wo);
  api_worker_finish(t, failures);
}

TEST(api_extra, ConcurrentReadersAndWriters) {
  api_fixture f;
  api_init(&f, "api_extra_concurrent");
  api_open(&f);
  char value[128] = {0};
  api_put(&f, "a", 1, value, sizeof(value));
  api_put(&f, "b", 1, value, sizeof(value));
  api_threads t;
  api_worker workers[4];
  api_threads_init(&t, &f);
  for (int i = 0; i < 4; ++i) {
    workers[i].shared = &t;
    workers[i].id = i;
    ldb_start_thread(api_concurrent_worker, &workers[i]);
  }
  int failures = api_threads_wait(&t, 4);
  memset(value, 120, sizeof(value));
  int ok = api_matches(&f, "a", 1, value, sizeof(value));
  ok &= api_matches(&f, "b", 1, value, sizeof(value));
  api_reopen(&f);
  ok &= api_matches(&f, "a", 1, value, sizeof(value));
  ok &= api_matches(&f, "b", 1, value, sizeof(value));
  api_done(&f);
  CHECK_EQ(0, failures);
  CHECK(ok);
}
