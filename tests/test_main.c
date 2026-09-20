// test_main.c - test registry and runner (mirrors leveldb testharness main)
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"

static ldb_test_entry* g_tests = NULL;
static ldb_test_entry* g_tail = NULL;
static int g_count = 0;
static jmp_buf g_test_jmp;
static const char* g_current_test = NULL;

int ldb_test_register(const char* name, ldb_test_fn fn) {
  ldb_test_entry* e = (ldb_test_entry*)malloc(sizeof(ldb_test_entry));
  e->name = name;
  e->fn = fn;
  e->next = NULL;
  if (g_tail == NULL) {
    g_tests = e;
  } else {
    g_tail->next = e;
  }
  g_tail = e;
  g_count++;
  return 0;
}

void ldb_test_fail(const char* file, int line, const char* fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  fprintf(stderr, "    FAIL %s:%d: ", file, line);
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
  longjmp(g_test_jmp, 1);
}

// Test directory helpers
void ldb_test_make_db_path(const char* name, char* out, size_t out_size) {
  ldb_buffer path;
  ldb_buffer_init(&path);
  ldb_status_release(ldb_env_get_test_directory(ldb_env_default(), &path));
  // Ensure the test root exists (needed for the real filesystem env).
  char* root = (char*)malloc(path.size + 1);
  memcpy(root, path.data ? path.data : "", path.size);
  root[path.size] = '\0';
  ldb_status_release(ldb_env_create_dir(ldb_env_default(), root));
  free(root);
  snprintf(out, out_size, "%.*s/%s", (int)path.size,
           path.data ? path.data : "", name);
  ldb_buffer_destroy(&path);
}

void ldb_test_destroy_dir(const char* path) {
  ldb_env* env = ldb_env_default();
  ldb_strings files;
  ldb_strings_init(&files);
  ldb_status s = ldb_env_get_children(env, path, &files);
  if (ldb_ok(s)) {
    for (size_t i = 0; i < files.count; i++) {
      char* full = (char*)malloc(strlen(path) + strlen(files.items[i]) + 2);
      sprintf(full, "%s/%s", path, files.items[i]);
      ldb_status_release(ldb_env_remove_file(env, full));
      free(full);
    }
  }
  ldb_status_destroy(&s);
  ldb_strings_destroy(&files);
  ldb_status_release(ldb_env_delete_dir(env, path));
}

static int run_one(ldb_test_entry* e) {
  g_current_test = e->name;
  if (setjmp(g_test_jmp) == 0) {
    e->fn();
    return 0;
  }
  return 1;
}

int ldb_test_run_all(void) {
  int failed = 0;
  int ran = 0;
  for (ldb_test_entry* e = g_tests; e != NULL; e = e->next) {
    printf("[ RUN      ] %s\n", e->name);
    fflush(stdout);
    if (run_one(e) == 0) {
      printf("[       OK ] %s\n", e->name);
    } else {
      printf("[   FAILED ] %s\n", e->name);
      failed++;
    }
    ran++;
    fflush(stdout);
  }
  printf("%d tests, %d failed\n", ran, failed);
  return (failed || ran == 0) ? 1 : 0;
}

int main(int argc, char** argv) {
  if (argc > 1) {
    // Run only matching tests (substring match)
    return ldb_test_run_matching(argv[1]);
  }
  return ldb_test_run_all();
}

int ldb_test_run_matching(const char* pattern) {
  int failed = 0, ran = 0;
  for (ldb_test_entry* e = g_tests; e != NULL; e = e->next) {
    if (!strstr(e->name, pattern)) continue;
    printf("[ RUN      ] %s\n", e->name);
    fflush(stdout);
    if (run_one(e) == 0) {
      printf("[       OK ] %s\n", e->name);
    } else {
      printf("[   FAILED ] %s\n", e->name);
      failed++;
    }
    ran++;
    fflush(stdout);
  }
  printf("%d tests, %d failed\n", ran, failed);
  return (failed || ran == 0) ? 1 : 0;
}
