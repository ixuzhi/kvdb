// iterator.c - iterator base helpers, empty/error iterators, and the
// merging iterator (mirrors leveldb/util/cache? no - leveldb/db,
// table/iterator.cc, table/merger.cc).
#include "kvdb.h"

#include <assert.h>

ldb_iterator* ldb_iterator_alloc(const struct ldb_iterator_methods* m,
                                 void* impl) {
  ldb_iterator* it = (ldb_iterator*)malloc(sizeof(ldb_iterator));
  it->m = m;
  it->impl = impl;
  it->cleanups = NULL;
  return it;
}

void ldb_iterator_register_cleanup(ldb_iterator* it, void (*fn)(void*, void*),
                                   void* arg1, void* arg2) {
  struct ldb_cleanup_node* node =
      (struct ldb_cleanup_node*)malloc(sizeof(*node));
  node->fn = fn;
  node->arg1 = arg1;
  node->arg2 = arg2;
  node->next = it->cleanups;
  it->cleanups = node;
}

// Calls run() even if iterator is invalid; runs cleanup before moving on.
void ldb_iter_seek_run_cleanup(ldb_iterator* it, const ldb_slice* target) {
  it->m->seek(it, target);
}

void ldb_iterator_destroy(ldb_iterator* it) {
  // Run cleanup handlers in reverse registration order.
  struct ldb_cleanup_node* node = it->cleanups;
  while (node != NULL) {
    struct ldb_cleanup_node* next = node->next;
    node->fn(node->arg1, node->arg2);
    free(node);
    node = next;
  }
  it->cleanups = NULL;
  it->m->destroy(it);
}

// ------------------------------------------------------------------ empty iterator
static void empty_destroy(ldb_iterator* it) { free(it); }
static int empty_valid(const ldb_iterator* it) {
  (void)it;
  return 0;
}
static void empty_nop(ldb_iterator* it) { assert(!empty_valid(it)); }
static void empty_seek(ldb_iterator* it, const ldb_slice* t) { (void)it; (void)t; }
static ldb_slice empty_slice(const ldb_iterator* it) {
  (void)it;
  assert(0);
  return ldb_slice_make("", 0);
}
static ldb_status empty_status(const ldb_iterator* it) {
  (void)it;
  return ldb_status_ok();
}

static const ldb_iterator_methods g_empty_methods = {
    empty_destroy, empty_valid, empty_nop,     empty_nop, empty_seek,
    empty_nop,     empty_nop,   empty_slice,  empty_slice, empty_status};

ldb_iterator* ldb_new_empty_iterator(void) {
  return ldb_iterator_alloc(&g_empty_methods, NULL);
}

// ------------------------------------------------------------------ error iterator
typedef struct error_iter {
  ldb_iterator base;
  ldb_status s;
} error_iter;

static void error_destroy(ldb_iterator* it) {
  error_iter* e = (error_iter*)it;
  ldb_status_destroy(&e->s);
  free(e);
}
static int error_valid(const ldb_iterator* it) {
  (void)it;
  return 0;
}
static void error_nop(ldb_iterator* it) { assert(!error_valid(it)); }
static void error_seek(ldb_iterator* it, const ldb_slice* t) { (void)it; (void)t; }
static ldb_slice error_slice(const ldb_iterator* it) {
  (void)it;
  assert(0);
  return ldb_slice_make("", 0);
}
static ldb_status error_status(const ldb_iterator* it) {
  error_iter* e = (error_iter*)it;
  return ldb_status_copy(e->s);
}

static const ldb_iterator_methods g_error_methods = {
    error_destroy, error_valid, error_nop,     error_nop, error_seek,
    error_nop,     error_nop,   error_slice,  error_slice, error_status};

ldb_iterator* ldb_new_error_iterator(ldb_status s) {
  error_iter* e = (error_iter*)malloc(sizeof(error_iter));
  e->base.m = &g_error_methods;
  e->base.impl = e;
  e->base.cleanups = NULL;
  e->s = s;
  return &e->base;
}

// ------------------------------------------------------------------ merging iterator
typedef struct merging_iter {
  ldb_iterator base;
  ldb_iterator** children;
  size_t n;
  int direction;  // 0 forward, 1 reverse
  ssize_t current;  // index of current child or -1
  const ldb_ikc* comparator;
  ldb_status status;
} merging_iter;

static void mg_destroy(ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  for (size_t i = 0; i < mg->n; i++) {
    ldb_iterator_destroy(mg->children[i]);
  }
  free(mg->children);
  ldb_status_destroy(&mg->status);
  free(mg);
}

static int mg_valid(const ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  return mg->current >= 0;
}

static void mg_find_smallest(merging_iter* mg) {
  ssize_t smallest = -1;
  for (size_t i = 0; i < mg->n; i++) {
    ldb_iterator* child = mg->children[i];
    if (ldb_iter_valid(child)) {
      if (smallest < 0) {
        smallest = (ssize_t)i;
      } else {
        ldb_slice a = ldb_iter_key(mg->children[smallest]);
        ldb_slice b = ldb_iter_key(child);
        if (ldb_ikc_compare(mg->comparator, &b, &a) < 0) {
          smallest = (ssize_t)i;
        }
      }
    }
  }
  mg->current = smallest;
}

static void mg_find_largest(merging_iter* mg) {
  ssize_t largest = -1;
  for (ssize_t i = (ssize_t)mg->n - 1; i >= 0; i--) {
    ldb_iterator* child = mg->children[i];
    if (ldb_iter_valid(child)) {
      if (largest < 0) {
        largest = i;
      } else {
        ldb_slice a = ldb_iter_key(mg->children[largest]);
        ldb_slice b = ldb_iter_key(child);
        if (ldb_ikc_compare(mg->comparator, &b, &a) > 0) {
          largest = i;
        }
      }
    }
  }
  mg->current = largest;
}

static void mg_seek_to_first(ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  for (size_t i = 0; i < mg->n; i++) {
    ldb_iter_seek_to_first(mg->children[i]);
  }
  mg->direction = 0;
  mg_find_smallest(mg);
}

static void mg_seek_to_last(ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  for (size_t i = 0; i < mg->n; i++) {
    ldb_iter_seek_to_last(mg->children[i]);
  }
  mg->direction = 1;
  mg_find_largest(mg);
}

static void mg_seek(ldb_iterator* it, const ldb_slice* target) {
  merging_iter* mg = (merging_iter*)it;
  for (size_t i = 0; i < mg->n; i++) {
    ldb_iter_seek(mg->children[i], target);
  }
  mg->direction = 0;
  mg_find_smallest(mg);
}

static void mg_next(ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  assert(mg_valid(it));
  if (mg->current < 0) {
    // Orphaned child; fall back to SeekToFirst
    ldb_iter_seek_to_first(it);
    return;
  }
  // Switch directions if needed: reposition all other children just past
  // the current key.
  if (mg->direction != 0) {
    for (size_t i = 0; i < mg->n; i++) {
      if ((ssize_t)i != mg->current) {
        ldb_slice k = ldb_iter_key(mg->children[mg->current]);
        ldb_iter_seek(mg->children[i], &k);
        if (ldb_iter_valid(mg->children[i])) {
          ldb_slice ck = ldb_iter_key(mg->children[i]);
          ldb_slice kk = ldb_iter_key(mg->children[mg->current]);
          if (ldb_ikc_compare(mg->comparator, &ck, &kk) == 0) {
            ldb_iter_next(mg->children[i]);
          }
        }
      }
    }
    mg->direction = 0;
  }
  ldb_iter_next(mg->children[mg->current]);
  mg_find_smallest(mg);
}

static void mg_prev(ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  assert(mg_valid(it));
  if (mg->current < 0) {
    // Orphaned child; fall back to SeekToLast
    ldb_iter_seek_to_last(it);
    return;
  }
  // Switch directions if needed: reposition all other children at the last
  // entry strictly before the current key.
  if (mg->direction != 1) {
    for (size_t i = 0; i < mg->n; i++) {
      if ((ssize_t)i != mg->current) {
        ldb_slice k = ldb_iter_key(mg->children[mg->current]);
        ldb_iter_seek(mg->children[i], &k);
        if (ldb_iter_valid(mg->children[i])) {
          ldb_iter_prev(mg->children[i]);
        } else {
          ldb_iter_seek_to_last(mg->children[i]);
        }
      }
    }
    mg->direction = 1;
  }
  ldb_iter_prev(mg->children[mg->current]);
  mg_find_largest(mg);
}

static ldb_slice mg_key(const ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  assert(mg->current >= 0);
  return ldb_iter_key(mg->children[mg->current]);
}

static ldb_slice mg_value(const ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  assert(mg->current >= 0);
  return ldb_iter_value(mg->children[mg->current]);
}

static ldb_status mg_status(const ldb_iterator* it) {
  merging_iter* mg = (merging_iter*)it;
  if (ldb_ok(mg->status)) {
    for (size_t i = 0; i < mg->n; i++) {
      ldb_status s = ldb_iter_status(mg->children[i]);
      if (!ldb_ok(s)) {
        return s;  // propagate child error (ownership moves to caller)
      }
      ldb_status_destroy(&s);
    }
    return ldb_status_ok();
  }
  return ldb_status_copy(mg->status);
}

static const ldb_iterator_methods g_merging_methods = {
    mg_destroy,      mg_valid,         mg_seek_to_first, mg_seek_to_last,
    mg_seek,         mg_next,          mg_prev,          mg_key,
    mg_value,        mg_status};

ldb_iterator* ldb_new_merging_iterator(const ldb_ikc* comparator,
                                       ldb_iterator** children, size_t n) {
  merging_iter* mg = (merging_iter*)malloc(sizeof(merging_iter));
  mg->base.m = &g_merging_methods;
  mg->base.impl = mg;
  mg->base.cleanups = NULL;
  mg->children = (ldb_iterator**)malloc(sizeof(ldb_iterator*) * (n ? n : 1));
  memcpy(mg->children, children, sizeof(ldb_iterator*) * n);
  mg->n = n;
  mg->direction = 0;
  mg->current = -1;
  mg->comparator = comparator;
  mg->status = ldb_status_ok();
  return &mg->base;
}
