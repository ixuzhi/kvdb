// harness.h - minimal test harness mirroring leveldb/util/testharness.h
#ifndef KVDB_TESTS_HARNESS_H_
#define KVDB_TESTS_HARNESS_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kvdb.h"

// Registration
typedef void (*ldb_test_fn)(void);

typedef struct ldb_test_entry {
  const char* name;
  ldb_test_fn fn;
  struct ldb_test_entry* next;
} ldb_test_entry;

int ldb_test_register(const char* name, ldb_test_fn fn);
int ldb_test_run_all(void);

#define TEST(group, name)                                            \
  static void ldb_test_##group##_##name(void);                       \
  __attribute__((constructor)) static void ldb_test_regfn_##group##_##name(void) { \
    ldb_test_register(#group "." #name, ldb_test_##group##_##name);  \
  }                                                                  \
  static void ldb_test_##group##_##name(void)

// Assertions (mirror leveldb's CHECK_* / ASSERT_* semantics: abort test)
void ldb_test_fail(const char* file, int line, const char* fmt, ...);

#define CHECK(cond)                                                  \
  do {                                                               \
    if (!(cond)) {                                                   \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s", #cond);  \
    }                                                                \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    long long va_ = (long long)(a), vb_ = (long long)(b);                  \
    if (va_ != vb_) {                                                      \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s == %s (%lld vs %lld)", \
                    #a, #b, va_, vb_);                                     \
    }                                                                      \
  } while (0)

#define CHECK_NE(a, b)                                                     \
  do {                                                                     \
    if ((long long)(a) == (long long)(b)) {                                \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s != %s", #a, #b); \
    }                                                                      \
  } while (0)

#define CHECK_LT(a, b)                                                     \
  do {                                                                     \
    if (!((long long)(a) < (long long)(b))) {                              \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s < %s", #a, #b);  \
    }                                                                      \
  } while (0)

#define CHECK_GT(a, b)                                                     \
  do {                                                                     \
    if (!((long long)(a) > (long long)(b))) {                              \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s > %s", #a, #b);  \
    }                                                                      \
  } while (0)

#define CHECK_LE(a, b)                                                     \
  do {                                                                     \
    if (!((long long)(a) <= (long long)(b))) {                             \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s <= %s", #a, #b); \
    }                                                                      \
  } while (0)

#define CHECK_GE(a, b)                                                     \
  do {                                                                     \
    if (!((long long)(a) >= (long long)(b))) {                             \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s >= %s", #a, #b); \
    }                                                                      \
  } while (0)

#define CHECK_STR_EQ(a, b)                                            \
  do {                                                                \
    const char* sa_ = (a);                                            \
    const char* sb_ = (b);                                            \
    if (strcmp(sa_ ? sa_ : "", sb_ ? sb_ : "") != 0) {                \
      ldb_test_fail(__FILE__, __LINE__, "check failed: %s == %s (\"%s\" vs \"%s\")", \
                    #a, #b, sa_ ? sa_ : "(null)", sb_ ? sb_ : "(null)"); \
    }                                                                 \
  } while (0)

#define CHECK_STR_CONTAINS(hay, needle)                                 \
  do {                                                                  \
    const char* sh_ = (hay);                                            \
    const char* sn_ = (needle);                                         \
    if (sh_ == NULL || strstr(sh_, sn_) == NULL) {                      \
      ldb_test_fail(__FILE__, __LINE__, "check failed: \"%s\" contains \"%s\"", \
                    sh_ ? sh_ : "(null)", sn_);                         \
    }                                                                   \
  } while (0)

// Status macros: they take a COPY for reporting so the caller keeps
// ownership of the passed status and must destroy it themselves.
#define CHECK_BUF_EQ(expect, bufp)                                    \
  do {                                                                \
    const char* e_ = (expect);                                        \
    const ldb_buffer* b_ = &(bufp);                                     \
    size_t el_ = strlen(e_);                                          \
    if (b_->size != el_ || (el_ > 0 && memcmp(b_->data, e_, el_) != 0)) { \
      ldb_test_fail(__FILE__, __LINE__, "buffer mismatch: expected \"%s\", got %zu bytes", \
                    e_, b_->size);                                    \
    }                                                                 \
  } while (0)

#define CHECK_STATUS_OK(s)                                             \
  do {                                                                 \
    ldb_status st_ = ldb_status_copy(s);                               \
    if (!ldb_ok(st_)) {                                                \
      char* msg_ = ldb_status_to_string(st_);                          \
      ldb_test_fail(__FILE__, __LINE__, "status not ok: %s", msg_);    \
      free(msg_);                                                      \
    }                                                                  \
    ldb_status_destroy(&st_);                                          \
  } while (0)

#define CHECK_STATUS_ERR(s)                                            \
  do {                                                                 \
    ldb_status st_ = ldb_status_copy(s);                               \
    if (ldb_ok(st_)) {                                                 \
      ldb_test_fail(__FILE__, __LINE__, "expected error, got ok");     \
    }                                                                  \
    ldb_status_destroy(&st_);                                          \
  } while (0)

// Helpers shared by tests
void ldb_test_make_db_path(const char* name, char* out, size_t out_size);
void ldb_test_destroy_dir(const char* path);

#endif  // KVDB_TESTS_HARNESS_H_
