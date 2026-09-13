// skiplist.c - skiplist for the memtable (mirrors leveldb/db/skiplist.h)
// Keys are length-prefixed internal-key entries allocated in an arena.
#include "kvdb.h"

#include <assert.h>

#define LDB_SKIPLIST_MAX_HEIGHT 12
#define LDB_SKIPLIST_BRANCHING 4
#define LDB_SKIPLIST_SEED 0xdeadbeefu


struct ldb_skiplist {
  void* cmp_arg;
  ldb_skiplist_cmp cmp;
  ldb_arena* arena;
  ldb_skiplist_node* head;
  int max_height;  // height of the tallest node
  uint32_t rnd_seed;
};

static inline int skiplist_get_max_height(const ldb_skiplist* l) {
  // max_height is only mutated by the single writer thread.
  return l->max_height;
}

static ldb_skiplist_node* node_new(ldb_skiplist* l, const char* key,
                                   int height) {
  size_t bytes = sizeof(ldb_skiplist_node) +
                 sizeof(ldb_skiplist_node*) * (size_t)(height - 1);
  ldb_skiplist_node* n = (ldb_skiplist_node*)ldb_arena_allocate(l->arena, bytes);
  n->key = key;
  for (int i = 0; i < height; i++) n->next[i] = NULL;
  return n;
}

static inline const char* node_key(const ldb_skiplist_node* n) {
  return n->key;
}

static inline ldb_skiplist_node* node_next(const ldb_skiplist_node* n, int level) {
  assert(n != NULL);
  return n->next[level];
}

// Returns 1 if node key is strictly after "key" (n->key < key).
static int key_is_after_node(ldb_skiplist* l, const ldb_skiplist_node* n,
                             const char* key) {
  // NULL n is considered infinite
  if (n == NULL) return 0;
  return l->cmp(l->cmp_arg, node_key(n), key) < 0;
}

// Returns the earliest node with key >= target; fills prev[level] if not NULL.
static ldb_skiplist_node* find_greater_or_equal(ldb_skiplist* l,
                                                const char* key,
                                                ldb_skiplist_node** prev) {
  ldb_skiplist_node* x = l->head;
  int level = skiplist_get_max_height(l) - 1;
  while (1) {
    ldb_skiplist_node* next = node_next(x, level);
    if (key_is_after_node(l, next, key)) {
      x = next;
    } else {
      if (prev != NULL) prev[level] = x;
      if (level == 0) {
        return next;
      }
      level--;
    }
  }
}

static ldb_skiplist_node* find_less_than(ldb_skiplist* l, const char* key) {
  ldb_skiplist_node* x = l->head;
  int level = skiplist_get_max_height(l) - 1;
  while (1) {
    assert(x == l->head || l->cmp(l->cmp_arg, node_key(x), key) < 0);
    ldb_skiplist_node* next = node_next(x, level);
    if (next == NULL || l->cmp(l->cmp_arg, node_key(next), key) >= 0) {
      if (level == 0) {
        return x;
      } else {
        level--;
      }
    } else {
      x = next;
    }
  }
}

static ldb_skiplist_node* find_last(ldb_skiplist* l) {
  ldb_skiplist_node* x = l->head;
  int level = skiplist_get_max_height(l) - 1;
  while (1) {
    ldb_skiplist_node* next = node_next(x, level);
    if (next == NULL) {
      if (level == 0) {
        return x;
      }
      level--;
    } else {
      x = next;
    }
  }
}

ldb_skiplist* ldb_skiplist_new(void* cmp_arg, ldb_skiplist_cmp cmp,
                               ldb_arena* arena) {
  ldb_skiplist* l = (ldb_skiplist*)malloc(sizeof(ldb_skiplist));
  l->cmp_arg = cmp_arg;
  l->cmp = cmp;
  l->arena = arena;
  l->max_height = 1;
  l->rnd_seed = LDB_SKIPLIST_SEED;
  l->head = node_new(l, NULL, LDB_SKIPLIST_MAX_HEIGHT);
  return l;
}

static int random_height(ldb_skiplist* l) {
  int height = 1;
  // Increase height with probability 1 in kBranching (matches leveldb's
  // Random::OneIn using the MINSTD generator).
  const uint64_t M = 2147483647u;  // 2^31-1
  while (height < LDB_SKIPLIST_MAX_HEIGHT) {
    uint64_t product = l->rnd_seed * 16807ull;
    l->rnd_seed = (uint32_t)((product >> 31) + (product & M));
    if (l->rnd_seed > M) l->rnd_seed -= (uint32_t)M;
    if ((int)(l->rnd_seed % LDB_SKIPLIST_BRANCHING) != 0) break;
    height++;
  }
  assert(height > 0);
  assert(height <= LDB_SKIPLIST_MAX_HEIGHT);
  return height;
}

// leveldb's Random uses seed = (seed * A) % M with correction; our simplified
// LCG above is sufficient for node heights but keep it consistent.
void ldb_skiplist_insert(ldb_skiplist* l, const char* key) {
  ldb_skiplist_node* prev[LDB_SKIPLIST_MAX_HEIGHT];
  ldb_skiplist_node* x = find_greater_or_equal(l, key, prev);

  // Our data structure does not allow duplicate insertion
  assert(x == NULL || l->cmp(l->cmp_arg, node_key(x), key) != 0);

  int height = random_height(l);
  if (height > skiplist_get_max_height(l)) {
    for (int i = skiplist_get_max_height(l); i < height; i++) {
      prev[i] = l->head;
    }
    l->max_height = height;
  }

  x = node_new(l, key, height);
  for (int i = 0; i < height; i++) {
    x->next[i] = prev[i]->next[i];
    prev[i]->next[i] = x;
  }
}

int ldb_skiplist_seek(const ldb_skiplist* l, const char* target,
                      const char** out) {
  // "l" is const but we need internal access; the find helpers don't mutate.
  ldb_skiplist* ml = (ldb_skiplist*)l;
  ldb_skiplist_node* n = find_greater_or_equal(ml, target, NULL);
  if (n == NULL) return 0;
  *out = (const char*)n;
  return 1;
}

int ldb_skiplist_seek_to_first(const ldb_skiplist* l, const char** out) {
  ldb_skiplist_node* n = l->head->next[0];
  if (n == NULL) return 0;
  *out = (const char*)n;
  return 1;
}

int ldb_skiplist_seek_to_last(const ldb_skiplist* l, const char** out) {
  ldb_skiplist* ml = (ldb_skiplist*)l;
  ldb_skiplist_node* n = find_last(ml);
  if (n == l->head) return 0;
  *out = (const char*)n;
  return 1;
}

int ldb_skiplist_valid(const ldb_skiplist* l, const char* pos) {
  (void)l;
  return pos != NULL;
}

const char* ldb_skiplist_next(const ldb_skiplist* l, const char* pos) {
  (void)l;
  const ldb_skiplist_node* n = (const ldb_skiplist_node*)pos;
  return (const char*)n->next[0];
}

const char* ldb_skiplist_prev(const ldb_skiplist* l, const char* pos) {
  ldb_skiplist* ml = (ldb_skiplist*)l;
  const ldb_skiplist_node* n = (const ldb_skiplist_node*)pos;
  assert(pos != NULL);
  ldb_skiplist_node* p = find_less_than(ml, node_key(n));
  if (p == ml->head) return NULL;
  return (const char*)p;
}

uint64_t ldb_skiplist_approximate_memory(const ldb_skiplist* l) {
  return ldb_arena_memory_usage(l->arena);
}
