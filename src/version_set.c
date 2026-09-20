// version_set.c - version management, lookup, and compaction picking
// (mirrors leveldb/db/version_set.cc)
#include "kvdb.h"

#include <assert.h>

// =================================================================== helpers
uint64_t ldb_max_grandparent_overlap_bytes(const ldb_options* options) {
  return 10 * options->max_file_size;
}

uint64_t ldb_expanded_compaction_byte_size_limit(const ldb_options* options) {
  return 25 * options->max_file_size;
}

uint64_t ldb_max_bytes_for_level(const ldb_options* options, int level) {
  // Result for both level-0 and level-1
  double result = 10.0 * 1048576.0;
  while (level > 1) {
    result *= 10;
    level--;
  }
  return (uint64_t)result;
}

uint64_t ldb_max_file_size_for_level(const ldb_options* options, int level) {
  (void)level;
  return options->max_file_size;
}

uint64_t ldb_total_file_size(ldb_file_meta** files, size_t n) {
  uint64_t sum = 0;
  for (size_t i = 0; i < n; i++) sum += files[i]->file_size;
  return sum;
}

// Binary search: return first index whose file largest >= key.
static size_t find_file(const ldb_ikc* icmp, ldb_file_meta** files, size_t n,
                        const ldb_slice* key) {
  size_t left = 0;
  size_t right = n;
  while (left < right) {
    size_t mid = (left + right) / 2;
    ldb_slice largest = ldb_buffer_slice(&files[mid]->largest);
    if (ldb_ikc_compare(icmp, &largest, key) < 0) {
      left = mid + 1;
    } else {
      right = mid;
    }
  }
  return right;
}

// =================================================================== version
ldb_version* ldb_version_new(ldb_version_set* vset) {
  ldb_version* v = (ldb_version*)calloc(1, sizeof(ldb_version));
  v->vset = vset;
  v->refs = 0;
  v->compaction_level = -1;
  v->compaction_score = -1;
  v->file_to_compact = NULL;
  v->file_to_compact_level = -1;
  v->next = v->prev = NULL;
  return v;
}

void ldb_version_destroy(ldb_version* v) {
  assert(v->refs == 0);
  // Remove from linked list
  v->prev->next = v->next;
  v->next->prev = v->prev;
  // Drop references to files
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    for (size_t i = 0; i < v->nfiles[level]; i++) {
      ldb_file_meta* f = v->files[level][i];
      assert(f->refs > 0);
      f->refs--;
      if (f->refs <= 0) {
        ldb_file_meta_destroy(f);
        free(f);
      }
    }
    free(v->files[level]);
    v->files[level] = NULL;
    v->nfiles[level] = 0;
  }
  free(v);
}

void ldb_version_ref(ldb_version* v) { v->refs++; }

void ldb_version_unref(ldb_version* v) {
  assert(v != &v->vset->dummy_versions);
  assert(v->refs >= 1);
  v->refs--;
  if (v->refs == 0) {
    ldb_version_destroy(v);
  }
}

static int after_file(const ldb_comparator* ucmp, const ldb_slice* user_key,
                      const ldb_file_meta* f) {
  if (user_key == NULL) return 0;
  ldb_slice largest = ldb_buffer_slice((ldb_buffer*)&f->largest);
  ldb_slice fu = ldb_extract_user_key(&largest);
  return ldb_cmp(ucmp, user_key, &fu) > 0;
}

static int before_file(const ldb_comparator* ucmp, const ldb_slice* user_key,
                       const ldb_file_meta* f) {
  if (user_key == NULL) return 0;
  ldb_slice smallest = ldb_buffer_slice((ldb_buffer*)&f->smallest);
  ldb_slice fu = ldb_extract_user_key(&smallest);
  return ldb_cmp(ucmp, user_key, &fu) < 0;
}

int ldb_version_some_file_overlaps_range(const ldb_version* v, int level,
                                         int disjoint_sorted_files,
                                         const ldb_slice* smallest_user_key,
                                         const ldb_slice* largest_user_key) {
  const ldb_comparator* ucmp = v->vset->icmp.user_comparator;
  ldb_file_meta** files = v->files[level];
  size_t n = v->nfiles[level];
  if (!disjoint_sorted_files) {
    // Need to check against all files
    for (size_t i = 0; i < n; i++) {
      const ldb_file_meta* f = files[i];
      if (after_file(ucmp, smallest_user_key, f) ||
          before_file(ucmp, largest_user_key, f)) {
        // No overlap
      } else {
        return 1;  // Overlap
      }
    }
    return 0;
  }

  // Binary search over file list
  size_t index = 0;
  if (smallest_user_key != NULL) {
    // Find the earliest possible internal key for smallest_user_key
    char buf[4096];
    char* heap = NULL;
    char* storage = buf;
    if (smallest_user_key->size + 8 > sizeof(buf)) {
      heap = (char*)malloc(smallest_user_key->size + 8);
      storage = heap;
    }
    /* The caller may pass a zero-length user key whose data is NULL
     * (leveldb_compact_range(db, NULL, 0, NULL, 0) does exactly that), and
     * memcpy's nonnull contract is violated even with n == 0. */
    if (smallest_user_key->size > 0) {
      memcpy(storage, smallest_user_key->data, smallest_user_key->size);
    }
    ldb_encode_fixed64(storage + smallest_user_key->size,
                       ldb_pack_sequence_and_type(LDB_K_MAX_SEQUENCE_NUMBER,
                                                  LDB_VALUE_TYPE_FOR_SEEK));
    ldb_slice small_key = ldb_slice_make(storage, smallest_user_key->size + 8);
    index = find_file(&v->vset->icmp, files, n, &small_key);
    free(heap);
  }

  if (index >= n) {
    // beginning of range is after all files, so no overlap.
    return 0;
  }

  return !before_file(ucmp, largest_user_key, files[index]);
}

int ldb_version_overlap_in_level(const ldb_version* v, int level,
                                 const ldb_slice* smallest_user_key,
                                 const ldb_slice* largest_user_key) {
  return ldb_version_some_file_overlaps_range(v, level, level > 0,
                                              smallest_user_key,
                                              largest_user_key);
}

int ldb_version_pick_level_for_memtable_output(const ldb_version* v,
                                               const ldb_slice* smallest,
                                               const ldb_slice* largest) {
  int level = 0;
  if (!ldb_version_overlap_in_level(v, 0, smallest, largest)) {
    // Push to next level if there is no overlap in next level,
    // and the #bytes overlapping in the level after that are limited.
    ldb_buffer start_buf, limit_buf;
    ldb_buffer_init(&start_buf);
    ldb_buffer_init(&limit_buf);
    ldb_append_internal_key(&start_buf, smallest, LDB_K_MAX_SEQUENCE_NUMBER,
                            LDB_VALUE_TYPE_FOR_SEEK);
    ldb_append_internal_key(&limit_buf, largest, 0, LDB_TYPE_DELETION);
    ldb_slice start = ldb_buffer_slice(&start_buf);
    ldb_slice limit = ldb_buffer_slice(&limit_buf);
    ldb_file_meta** overlaps = NULL;
    size_t overlaps_count = 0, overlaps_cap = 0;
    while (level < LDB_K_MAX_MEM_COMPACT_LEVEL) {
      if (ldb_version_overlap_in_level(v, level + 1, smallest, largest)) {
        break;
      }
      if (level + 2 < LDB_K_NUM_LEVELS) {
        // Check that file does not overlap too many grandparent bytes.
        ldb_version_get_overlapping_inputs(v, level + 2, &start, &limit,
                                           &overlaps, &overlaps_count,
                                           &overlaps_cap);
        uint64_t sum = ldb_total_file_size(overlaps, overlaps_count);
        if (sum > ldb_max_grandparent_overlap_bytes(v->vset->options)) {
          break;
        }
      }
      level++;
    }
    free(overlaps);
    ldb_buffer_destroy(&start_buf);
    ldb_buffer_destroy(&limit_buf);
  }
  return level;
}

// Store in (*inputs) all files in "level" that overlap [begin,end].
// begin/end are internal keys or NULL.
void ldb_version_get_overlapping_inputs(const ldb_version* v, int level,
                                        const ldb_slice* begin,
                                        const ldb_slice* end,
                                        ldb_file_meta*** inputs,
                                        size_t* count, size_t* cap) {
  assert(level >= 0);
  assert(level < LDB_K_NUM_LEVELS);
  *count = 0;
  ldb_slice user_begin, user_end;
  if (begin != NULL) user_begin = ldb_extract_user_key(begin);
  if (end != NULL) user_end = ldb_extract_user_key(end);
  const ldb_comparator* user_cmp = v->vset->icmp.user_comparator;
  size_t i = 0;
  while (i < v->nfiles[level]) {
    ldb_file_meta* f = v->files[level][i++];
    ldb_slice fs_buf = ldb_buffer_slice(&f->smallest);
    ldb_slice fl_buf = ldb_buffer_slice(&f->largest);
    ldb_slice file_start = ldb_extract_user_key(&fs_buf);
    ldb_slice file_limit = ldb_extract_user_key(&fl_buf);
    if (begin != NULL && ldb_cmp(user_cmp, &file_limit, &user_begin) < 0) {
      // "f" is completely before specified range; skip it
    } else if (end != NULL && ldb_cmp(user_cmp, &file_start, &user_end) > 0) {
      // "f" is completely after specified range; skip it
    } else {
      if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *inputs = (ldb_file_meta**)realloc(*inputs,
                                           sizeof(ldb_file_meta*) * *cap);
        assert(*inputs);
      }
      (*inputs)[(*count)++] = f;
      if (level == 0) {
        // Level-0 files may overlap each other. So check if the newly
        // added file has expanded the range. If so, restart search.
        if (begin != NULL && ldb_cmp(user_cmp, &file_start, &user_begin) < 0) {
          user_begin = file_start;
          *count = 0;
          i = 0;
        } else if (end != NULL &&
                   ldb_cmp(user_cmp, &file_limit, &user_end) > 0) {
          user_end = file_limit;
          *count = 0;
          i = 0;
        }
      }
    }
  }
}

// ------------------------------------------------------------------ LevelFileNumIterator
typedef struct level_file_num_iter {
  ldb_iterator base;
  const ldb_ikc* icmp;
  ldb_file_meta** flist;
  size_t nfiles;
  size_t index;  // == nfiles marks invalid
  char value_buf[16];
} level_file_num_iter;

static void lfn_destroy(ldb_iterator* it) { free(it); }
static int lfn_valid(const ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  return l->index < l->nfiles;
}
static void lfn_seek(ldb_iterator* it, const ldb_slice* target) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  l->index = find_file(l->icmp, l->flist, l->nfiles, target);
}
static void lfn_seek_to_first(ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  l->index = 0;
}
static void lfn_seek_to_last(ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  l->index = (l->nfiles == 0) ? 0 : l->nfiles - 1;
}
static void lfn_next(ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  assert(lfn_valid(it));
  l->index++;
}
static void lfn_prev(ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  assert(lfn_valid(it));
  if (l->index == 0) {
    l->index = l->nfiles;  // Marks as invalid
  } else {
    l->index--;
  }
}
static ldb_slice lfn_key(const ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  assert(lfn_valid(it));
  return ldb_buffer_slice(&l->flist[l->index]->largest);
}
static ldb_slice lfn_value(const ldb_iterator* it) {
  level_file_num_iter* l = (level_file_num_iter*)it;
  assert(lfn_valid(it));
  ldb_encode_fixed64(l->value_buf, l->flist[l->index]->number);
  ldb_encode_fixed64(l->value_buf + 8, l->flist[l->index]->file_size);
  return ldb_slice_make(l->value_buf, sizeof(l->value_buf));
}
static ldb_status lfn_status(const ldb_iterator* it) {
  (void)it;
  return ldb_status_ok();
}

static const ldb_iterator_methods g_lfn_methods = {
    lfn_destroy, lfn_valid,  lfn_seek_to_first, lfn_seek_to_last, lfn_seek,
    lfn_next,    lfn_prev,   lfn_key,           lfn_value,        lfn_status};

static ldb_iterator* lfn_new(const ldb_ikc* icmp, ldb_file_meta** flist,
                             size_t nfiles) {
  level_file_num_iter* l =
      (level_file_num_iter*)malloc(sizeof(level_file_num_iter));
  l->base.m = &g_lfn_methods;
  l->base.impl = l;
  l->base.cleanups = NULL;
  l->icmp = icmp;
  l->flist = flist;
  l->nfiles = nfiles;
  l->index = nfiles;  // Marks as invalid
  return &l->base;
}

// Callback from table_cache_get for two-level iteration over a level.
static ldb_iterator* get_file_iterator(void* arg,
                                       const ldb_read_options* options,
                                       const ldb_slice* file_value) {
  ldb_table_cache* cache = (ldb_table_cache*)arg;
  if (file_value->size != 16) {
    return ldb_new_error_iterator(
        ldb_status_corruption("FileReader invoked with unexpected value", NULL));
  }
  return ldb_table_cache_new_iterator(cache, options,
                                      ldb_decode_fixed64(file_value->data),
                                      ldb_decode_fixed64(file_value->data + 8));
}

void ldb_version_add_iterators(const ldb_version* v,
                               const ldb_read_options* options,
                               ldb_iterator*** list, size_t* count,
                               size_t* cap) {
  // Merge all level zero files together since they may overlap
  for (size_t i = 0; i < v->nfiles[0]; i++) {
    if (*count == *cap) {
      *cap = *cap ? *cap * 2 : 8;
      *list = (ldb_iterator**)realloc(*list, sizeof(ldb_iterator*) * *cap);
      assert(*list);
    }
    (*list)[(*count)++] = ldb_table_cache_new_iterator(
        v->vset->table_cache, options, v->files[0][i]->number,
        v->files[0][i]->file_size);
  }

  // For levels > 0, use a concatenating iterator that sequentially walks
  // through the non-overlapping files in the level, opening them lazily.
  for (int level = 1; level < LDB_K_NUM_LEVELS; level++) {
    if (v->nfiles[level] > 0) {
      ldb_iterator* index_iter =
          lfn_new(&v->vset->icmp, v->files[level], v->nfiles[level]);
      if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 8;
        *list = (ldb_iterator**)realloc(*list, sizeof(ldb_iterator*) * *cap);
        assert(*list);
      }
      (*list)[(*count)++] = ldb_new_two_level_iterator(
          index_iter, get_file_iterator, v->vset->table_cache, options);
    }
  }
}

// ------------------------------------------------------------------ Get
enum {
  SAVER_NOT_FOUND = 0,
  SAVER_FOUND = 1,
  SAVER_DELETED = 2,
  SAVER_CORRUPT = 3,
};

typedef struct get_state {
  int saver_state;
  const ldb_comparator* ucmp;
  ldb_slice user_key;
  ldb_buffer* value;

  ldb_file_meta** seek_file_slot;
  int* seek_file_level_slot;
  ldb_file_meta* last_file_read;
  int last_file_read_level;
  ldb_version_set* vset;
  ldb_status s;
  int found;
  const ldb_read_options* options;
  ldb_slice ikey;
} get_state;

static void save_value(void* arg, const ldb_slice* ikey, const ldb_slice* v) {
  get_state* st = (get_state*)arg;
  ldb_parsed_internal_key parsed_key;
  if (!ldb_parse_internal_key(ikey, &parsed_key)) {
    st->saver_state = SAVER_CORRUPT;
    st->user_key = parsed_key.user_key;  // best effort
  } else {
    if (st->ucmp->compare(st->ucmp, &parsed_key.user_key, &st->user_key) == 0) {
      st->saver_state =
          (parsed_key.type == LDB_TYPE_VALUE) ? SAVER_FOUND : SAVER_DELETED;
      if (st->saver_state == SAVER_FOUND) {
        ldb_buffer_clear(st->value);
        ldb_buffer_append_slice(st->value, v);
      }
    }
  }
}

static int get_match(void* arg, int level, ldb_file_meta* f) {
  get_state* st = (get_state*)arg;

  if (*st->seek_file_slot == NULL && st->last_file_read != NULL) {
    // We have had more than one seek for this read. Charge the 1st file.
    *st->seek_file_slot = st->last_file_read;
    *st->seek_file_level_slot = st->last_file_read_level;
  }

  st->last_file_read = f;
  st->last_file_read_level = level;

  st->s = ldb_table_cache_get(st->vset->table_cache, st->options, f->number,
                              f->file_size, &st->ikey, st, save_value, NULL);
  if (!ldb_ok(st->s)) {
    st->found = 1;
    return 0;
  }
  switch (st->saver_state) {
    case SAVER_NOT_FOUND:
      return 1;  // Keep searching in other files
    case SAVER_FOUND:
      st->found = 1;
      return 0;
    case SAVER_DELETED:
      return 0;
    case SAVER_CORRUPT:
      ldb_status_destroy(&st->s);
      st->s = ldb_status_corruption("corrupted key for ", NULL);
      st->found = 1;
      return 0;
  }
  return 0;
}

static void for_each_overlapping(const ldb_version* v, ldb_slice user_key,
                                 ldb_slice internal_key, void* arg,
                                 int (*func)(void*, int, ldb_file_meta*)) {
  const ldb_comparator* ucmp = v->vset->icmp.user_comparator;

  // Search level-0 in order from newest to oldest.
  ldb_file_meta** tmp = NULL;
  size_t tmp_count = 0, tmp_cap = 0;
  for (size_t i = 0; i < v->nfiles[0]; i++) {
    ldb_file_meta* f = v->files[0][i];
    ldb_slice smallest = ldb_buffer_slice(&f->smallest);
    ldb_slice largest = ldb_buffer_slice(&f->largest);
    ldb_slice fs = ldb_extract_user_key(&smallest);
    ldb_slice fl = ldb_extract_user_key(&largest);
    if (ucmp->compare(ucmp, &user_key, &fs) >= 0 &&
        ucmp->compare(ucmp, &user_key, &fl) <= 0) {
      if (tmp_count == tmp_cap) {
        tmp_cap = tmp_cap ? tmp_cap * 2 : 8;
        tmp = (ldb_file_meta**)realloc(tmp, sizeof(ldb_file_meta*) * tmp_cap);
        assert(tmp);
      }
      tmp[tmp_count++] = f;
    }
  }
  if (tmp_count > 0) {
    // sort newest first
    for (size_t i = 0; i + 1 < tmp_count; i++) {
      for (size_t j = i + 1; j < tmp_count; j++) {
        if (tmp[j]->number > tmp[i]->number) {
          ldb_file_meta* t = tmp[i];
          tmp[i] = tmp[j];
          tmp[j] = t;
        }
      }
    }
    for (size_t i = 0; i < tmp_count; i++) {
      if (!func(arg, 0, tmp[i])) {
        free(tmp);
        return;
      }
    }
  }
  free(tmp);

  // Search other levels.
  for (int level = 1; level < LDB_K_NUM_LEVELS; level++) {
    size_t num_files = v->nfiles[level];
    if (num_files == 0) continue;

    // Binary search to find earliest index whose largest key >= internal_key.
    size_t index =
        find_file(&v->vset->icmp, v->files[level], num_files, &internal_key);
    if (index < num_files) {
      ldb_file_meta* f = v->files[level][index];
      ldb_slice smallest = ldb_buffer_slice(&f->smallest);
      ldb_slice fs = ldb_extract_user_key(&smallest);
      if (ucmp->compare(ucmp, &user_key, &fs) < 0) {
        // All of "f" is past any data for user_key
      } else {
        if (!func(arg, level, f)) {
          return;
        }
      }
    }
  }
}

ldb_status ldb_version_get(const ldb_version* v, const ldb_read_options* options,
                           const ldb_lookup_key* k, ldb_buffer* value,
                           ldb_file_meta** seek_file, int* seek_file_level) {
  *seek_file = NULL;
  *seek_file_level = -1;

  get_state st;
  memset(&st, 0, sizeof(st));
  st.saver_state = SAVER_NOT_FOUND;
  st.ucmp = v->vset->icmp.user_comparator;
  st.user_key = k->user_key;
  st.value = value;
  st.seek_file_slot = seek_file;
  st.seek_file_level_slot = seek_file_level;
  st.last_file_read = NULL;
  st.last_file_read_level = -1;
  st.options = options;
  st.ikey = k->internal_key;
  st.vset = v->vset;
  st.found = 0;
  st.s = ldb_status_ok();

  for_each_overlapping(v, k->user_key, k->internal_key, &st, get_match);

  if (st.found) {
    return st.s;
  }
  ldb_status_destroy(&st.s);
  return ldb_status_notfound("", NULL);
}

int ldb_version_update_stats(ldb_version* v, ldb_file_meta* seek_file,
                             int seek_file_level) {
  ldb_file_meta* f = seek_file;
  if (f != NULL) {
    f->allowed_seeks--;
    if (f->allowed_seeks <= 0 && v->file_to_compact == NULL) {
      v->file_to_compact = f;
      v->file_to_compact_level = seek_file_level;
      return 1;
    }
  }
  return 0;
}

int ldb_version_record_read_sample(ldb_version* v, const ldb_slice* ikey) {
  ldb_parsed_internal_key parsed;
  if (!ldb_parse_internal_key(ikey, &parsed)) {
    return 0;
  }

  int matches = 0;
  ldb_file_meta* first_match = NULL;
  int first_match_level = -1;

  // A simplified ForEachOverlapping that counts matches.
  const ldb_comparator* ucmp = v->vset->icmp.user_comparator;
  int stopped = 0;
  // level 0, newest first
  {
    ldb_file_meta** tmp = NULL;
    size_t tc = 0, tcap = 0;
    for (size_t i = 0; i < v->nfiles[0]; i++) {
      ldb_file_meta* f = v->files[0][i];
      ldb_slice smallest = ldb_buffer_slice(&f->smallest);
      ldb_slice largest = ldb_buffer_slice(&f->largest);
      ldb_slice fs = ldb_extract_user_key(&smallest);
      ldb_slice fl = ldb_extract_user_key(&largest);
      if (ucmp->compare(ucmp, &parsed.user_key, &fs) >= 0 &&
          ucmp->compare(ucmp, &parsed.user_key, &fl) <= 0) {
        if (tc == tcap) {
          tcap = tcap ? tcap * 2 : 8;
          tmp = (ldb_file_meta**)realloc(tmp, sizeof(ldb_file_meta*) * tcap);
        }
        tmp[tc++] = f;
      }
    }
    for (size_t i = 0; i + 1 < tc; i++) {
      for (size_t j = i + 1; j < tc; j++) {
        if (tmp[j]->number > tmp[i]->number) {
          ldb_file_meta* t = tmp[i];
          tmp[i] = tmp[j];
          tmp[j] = t;
        }
      }
    }
    for (size_t i = 0; i < tc && !stopped; i++) {
      matches++;
      if (matches == 1) {
        first_match = tmp[i];
        first_match_level = 0;
      } else {
        stopped = 1;
      }
    }
    free(tmp);
  }
  if (!stopped) {
    for (int level = 1; level < LDB_K_NUM_LEVELS && !stopped; level++) {
      size_t num_files = v->nfiles[level];
      if (num_files == 0) continue;
      size_t index =
          find_file(&v->vset->icmp, v->files[level], num_files, ikey);
      if (index < num_files) {
        ldb_file_meta* f = v->files[level][index];
        ldb_slice smallest = ldb_buffer_slice(&f->smallest);
        ldb_slice fs = ldb_extract_user_key(&smallest);
        if (ucmp->compare(ucmp, &parsed.user_key, &fs) < 0) {
          // past
        } else {
          matches++;
          if (matches == 1) {
            first_match = f;
            first_match_level = level;
          } else {
            stopped = 1;
          }
        }
      }
    }
  }

  if (matches >= 2) {
    return ldb_version_update_stats(v, first_match, first_match_level);
  }
  return 0;
}

void ldb_version_add_live_files(const ldb_version* v, uint64_set_t* set) {
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    for (size_t i = 0; i < v->nfiles[level]; i++) {
      uint64_set_insert(set, v->files[level][i]->number);
    }
  }
}

uint64_t ldb_version_max_next_level_overlapping_bytes(const ldb_version* v) {
  uint64_t result = 0;
  ldb_file_meta** overlaps = NULL;
  size_t oc = 0, ocap = 0;
  for (int level = 1; level < LDB_K_NUM_LEVELS - 1; level++) {
    for (size_t i = 0; i < v->nfiles[level]; i++) {
      ldb_file_meta* f = v->files[level][i];
      ldb_slice smallest = ldb_buffer_slice(&f->smallest);
      ldb_slice largest = ldb_buffer_slice(&f->largest);
      ldb_version_get_overlapping_inputs(v, level + 1, &smallest, &largest,
                                         &overlaps, &oc, &ocap);
      uint64_t sum = ldb_total_file_size(overlaps, oc);
      if (sum > result) result = sum;
    }
  }
  free(overlaps);
  return result;
}

char* ldb_version_debug_string(const ldb_version* v) {
  ldb_buffer r;
  ldb_buffer_init(&r);
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    ldb_buffer_appendf(&r, "--- level %d ---\n", level);
    for (size_t i = 0; i < v->nfiles[level]; i++) {
      ldb_file_meta* f = v->files[level][i];
      ldb_buffer_append(&r, " ", 1);
      ldb_append_number_to(&r, f->number);
      ldb_buffer_append(&r, ":", 1);
      ldb_append_number_to(&r, f->file_size);
      ldb_buffer_append(&r, "[", 1);
      { ldb_slice sbuf = ldb_buffer_slice(&f->smallest); ldb_buffer_append_slice(&r, &sbuf); }
      ldb_buffer_append(&r, " .. ", 4);
      { ldb_slice lbuf = ldb_buffer_slice(&f->largest); ldb_buffer_append_slice(&r, &lbuf); }
      ldb_buffer_append(&r, "]\n", 2);
    }
  }
  char* out = (char*)malloc(r.size + 1);
  if (r.size > 0) {  // an empty summary buffer still has a NULL data pointer
    memcpy(out, r.data, r.size);
  }
  out[r.size] = '\0';
  ldb_buffer_destroy(&r);
  return out;
}

// =================================================================== builder for version edits
typedef struct vs_builder {
  ldb_version_set* vset;
  ldb_version* base;
  // per level: deleted files set and added files list
  uint64_set_t deleted_files[LDB_K_NUM_LEVELS];
  ldb_file_meta** added_files[LDB_K_NUM_LEVELS];
  size_t added_count[LDB_K_NUM_LEVELS];
  size_t added_cap[LDB_K_NUM_LEVELS];
} vs_builder;

static void vbuilder_init(vs_builder* b, ldb_version_set* vset,
                          ldb_version* base) {
  b->vset = vset;
  b->base = base;
  ldb_version_ref(base);
  for (int i = 0; i < LDB_K_NUM_LEVELS; i++) {
    uint64_set_init(&b->deleted_files[i]);
    b->added_files[i] = NULL;
    b->added_count[i] = 0;
    b->added_cap[i] = 0;
  }
}

static void vbuilder_destroy(vs_builder* b) {
  for (int i = 0; i < LDB_K_NUM_LEVELS; i++) {
    for (size_t j = 0; j < b->added_count[i]; j++) {
      ldb_file_meta* f = b->added_files[i][j];
      f->refs--;
      if (f->refs <= 0) {
        ldb_file_meta_destroy(f);
        free(f);
      }
    }
    free(b->added_files[i]);
    uint64_set_destroy(&b->deleted_files[i]);
  }
  ldb_version_unref(b->base);
}

static void vbuilder_apply(vs_builder* b, const ldb_version_edit* edit) {
  // Update compaction pointers
  for (size_t i = 0; i < edit->compact_pointers_count; i++) {
    int level = edit->compact_pointers[i].level;
    ldb_buffer* cp = &b->vset->compact_pointer[level];
    ldb_buffer_clear(cp);
    { ldb_slice kbuf = ldb_buffer_slice(&edit->compact_pointers[i].key); ldb_buffer_append_slice(cp, &kbuf); }
  }

  // Delete files
  for (size_t i = 0; i < edit->deleted_count; i++) {
    int level = edit->deleted_levels[i];
    uint64_set_insert(&b->deleted_files[level], edit->deleted_numbers[i]);
  }

  // Add new files
  for (size_t i = 0; i < edit->new_files_count; i++) {
    int level = edit->new_files[i].level;
    ldb_file_meta* f = (ldb_file_meta*)malloc(sizeof(ldb_file_meta));
    *f = edit->new_files[i].meta;
    // deep-copy buffers (they currently alias edit storage)
    ldb_buffer_init(&f->smallest);
    { ldb_slice sbuf = ldb_buffer_slice(&edit->new_files[i].meta.smallest); ldb_buffer_append_slice(&f->smallest, &sbuf); }
    ldb_buffer_init(&f->largest);
    { ldb_slice lbuf = ldb_buffer_slice(&edit->new_files[i].meta.largest); ldb_buffer_append_slice(&f->largest, &lbuf); }
    f->refs = 1;
    f->allowed_seeks = (int)((f->file_size / 16384U));
    if (f->allowed_seeks < 100) f->allowed_seeks = 100;

    uint64_set_erase(&b->deleted_files[level], f->number);
    if (b->added_count[level] == b->added_cap[level]) {
      b->added_cap[level] = b->added_cap[level] ? b->added_cap[level] * 2 : 4;
      b->added_files[level] = (ldb_file_meta**)realloc(
          b->added_files[level], sizeof(ldb_file_meta*) * b->added_cap[level]);
      assert(b->added_files[level]);
    }
    b->added_files[level][b->added_count[level]++] = f;
  }
}

// Sort comparator: by smallest key, then file number.
static int vbuilder_file_cmp(const ldb_ikc* icmp, const ldb_file_meta* f1,
                             const ldb_file_meta* f2) {
  ldb_slice s1 = ldb_buffer_slice((ldb_buffer*)&f1->smallest);
  ldb_slice s2 = ldb_buffer_slice((ldb_buffer*)&f2->smallest);
  int r = ldb_ikc_compare(icmp, &s1, &s2);
  if (r != 0) return r;
  if (f1->number < f2->number) return -1;
  if (f1->number > f2->number) return 1;
  return 0;
}

static void vbuilder_maybe_add_file(vs_builder* b, ldb_version* v, int level,
                                    ldb_file_meta* f) {
  if (uint64_set_contains(&b->deleted_files[level], f->number)) {
    // File is deleted: do nothing
  } else {
    ldb_file_meta*** files = &v->files[level];
    size_t* count = &v->nfiles[level];
    if (level > 0 && *count > 0) {
      // Must not overlap
      ldb_slice prev_largest =
          ldb_buffer_slice(&(*files)[*count - 1]->largest);
      ldb_slice smallest = ldb_buffer_slice(&f->smallest);
      assert(ldb_ikc_compare(&b->vset->icmp, &prev_largest, &smallest) < 0);
    }
    if (*count == v->files_cap[level]) {
      v->files_cap[level] = v->files_cap[level] ? v->files_cap[level] * 2 : 4;
      *files = (ldb_file_meta**)realloc(*files, sizeof(ldb_file_meta*) *
                                                    v->files_cap[level]);
      assert(*files);
    }
    f->refs++;
    (*files)[(*count)++] = f;
  }
}

static void vbuilder_save_to(vs_builder* b, ldb_version* v) {
  const ldb_ikc* icmp = &b->vset->icmp;
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    // Merge the set of added files with the set of pre-existing files.
    // Drop any deleted files. Store the result in *v.
    ldb_file_meta** base_files = b->base->files[level];
    size_t base_count = b->base->nfiles[level];
    size_t base_iter = 0;
    ldb_file_meta** added_files = b->added_files[level];
    size_t added_count = b->added_count[level];

    // Sort added files by smallest key (they may have been applied in any
    // order across multiple edits).
    for (size_t i = 0; i + 1 < added_count; i++) {
      for (size_t j = i + 1; j < added_count; j++) {
        if (vbuilder_file_cmp(icmp, added_files[j], added_files[i]) < 0) {
          ldb_file_meta* t = added_files[i];
          added_files[i] = added_files[j];
          added_files[j] = t;
        }
      }
    }

    for (size_t ai = 0; ai < added_count; ai++) {
      ldb_file_meta* added_file = added_files[ai];
      // Add all smaller files listed in base_
      while (base_iter < base_count &&
             vbuilder_file_cmp(icmp, base_files[base_iter], added_file) < 0) {
        vbuilder_maybe_add_file(b, v, level, base_files[base_iter]);
        base_iter++;
      }
      vbuilder_maybe_add_file(b, v, level, added_file);
    }

    // Add remaining base files
    while (base_iter < base_count) {
      vbuilder_maybe_add_file(b, v, level, base_files[base_iter]);
      base_iter++;
    }
  }
}

// =================================================================== version set
static void ldb_version_set_append_version(ldb_version_set* vs,
                                           ldb_version* v);

void ldb_version_set_init(ldb_version_set* vs, const char* dbname,
                          const ldb_options* options,
                          ldb_table_cache* table_cache, const ldb_ikc* icmp,
                          ldb_mutex* db_mutex) {
  vs->env = options->env;
  vs->dbname = strdup(dbname);
  vs->options = options;
  vs->table_cache = table_cache;
  vs->icmp = *icmp;
  vs->next_file_number = 2;
  vs->manifest_file_number = 0;  // Filled by Recover()
  vs->last_sequence = 0;
  vs->log_number = 0;
  vs->prev_log_number = 0;
  vs->descriptor_file = NULL;
  vs->descriptor_log = NULL;
  vs->db_mutex = db_mutex;
  memset(&vs->dummy_versions, 0, sizeof(vs->dummy_versions));
  vs->dummy_versions.vset = vs;
  vs->dummy_versions.next = &vs->dummy_versions;
  vs->dummy_versions.prev = &vs->dummy_versions;
  vs->current_ = NULL;
  for (int i = 0; i < LDB_K_NUM_LEVELS; i++) {
    ldb_buffer_init(&vs->compact_pointer[i]);
  }
  // Append an empty version.
  ldb_version* v = ldb_version_new(vs);
  ldb_version_set_append_version(vs, v);
}

static void ldb_version_set_append_version(ldb_version_set* vs,
                                           ldb_version* v) {
  // Make "v" current
  assert(v->refs == 0);
  if (vs->current_ != NULL) {
    ldb_version_unref(vs->current_);
  }
  vs->current_ = v;
  v->refs = 1;

  // Append to linked list
  v->prev = vs->dummy_versions.prev;
  v->next = &vs->dummy_versions;
  v->prev->next = v;
  v->next->prev = v;
}

void ldb_version_set_destroy(ldb_version_set* vs) {
  ldb_version_unref(vs->current_);
  assert(vs->dummy_versions.next == &vs->dummy_versions);
  if (vs->descriptor_log) ldb_log_writer_destroy(vs->descriptor_log);
  if (vs->descriptor_file) vs->descriptor_file->m->destroy(vs->descriptor_file);
  free(vs->dbname);
  for (int i = 0; i < LDB_K_NUM_LEVELS; i++) {
    ldb_buffer_destroy(&vs->compact_pointer[i]);
  }
}

ldb_version* ldb_version_set_current(ldb_version_set* vs) {
  return vs->current_;
}

uint64_t ldb_version_set_new_file_number(ldb_version_set* vs) {
  uint64_t n = vs->next_file_number++;
  return n;
}

void ldb_version_set_reuse_file_number(ldb_version_set* vs, uint64_t n) {
  if (vs->next_file_number == n + 1) {
    vs->next_file_number = n;
  }
}

void ldb_version_set_mark_file_number_used(ldb_version_set* vs, uint64_t n) {
  if (vs->next_file_number <= n) {
    vs->next_file_number = n + 1;
  }
}

int ldb_version_set_num_level_files(const ldb_version_set* vs, int level) {
  assert(level >= 0 && level < LDB_K_NUM_LEVELS);
  return (int)vs->current_->nfiles[level];
}

uint64_t ldb_version_set_num_level_bytes(const ldb_version_set* vs,
                                         int level) {
  assert(level >= 0 && level < LDB_K_NUM_LEVELS);
  return ldb_total_file_size(vs->current_->files[level],
                             vs->current_->nfiles[level]);
}

char* ldb_version_set_level_summary(const ldb_version_set* vs) {
  char* buf = (char*)malloc(128);
  snprintf(buf, 128, "files[ %d %d %d %d %d %d %d ]",
           (int)vs->current_->nfiles[0], (int)vs->current_->nfiles[1],
           (int)vs->current_->nfiles[2], (int)vs->current_->nfiles[3],
           (int)vs->current_->nfiles[4], (int)vs->current_->nfiles[5],
           (int)vs->current_->nfiles[6]);
  return buf;
}

void ldb_version_set_add_live_files(ldb_version_set* vs, uint64_set_t* live) {
  for (ldb_version* v = vs->dummy_versions.next; v != &vs->dummy_versions;
       v = v->next) {
    ldb_version_add_live_files(v, live);
  }
}

uint64_t ldb_version_set_max_next_level_overlapping_bytes(
    ldb_version_set* vs) {
  return ldb_version_max_next_level_overlapping_bytes(vs->current_);
}

// =================================================================== Finalize
void ldb_version_set_finalize(ldb_version_set* vs, ldb_version* v) {
  int best_level = -1;
  double best_score = -1;
  for (int level = 0; level < LDB_K_NUM_LEVELS - 1; level++) {
    double score;
    if (level == 0) {
      score = (double)v->nfiles[0] / (double)LDB_K_L0_COMPACTION_TRIGGER;
    } else {
      const uint64_t level_bytes =
          ldb_total_file_size(v->files[level], v->nfiles[level]);
      score = (double)level_bytes /
              (double)ldb_max_bytes_for_level(vs->options, level);
    }
    if (score > best_score) {
      best_level = level;
      best_score = score;
    }
  }
  v->compaction_level = best_level;
  v->compaction_score = best_score;
}

// =================================================================== snapshot of version
ldb_status ldb_version_set_write_snapshot(ldb_version_set* vs,
                                          ldb_log_writer* log) {
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  ldb_version_edit_set_comparator(
      &edit, vs->icmp.user_comparator->name(vs->icmp.user_comparator));

  // Save compaction pointers
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    if (vs->compact_pointer[level].size > 0) {
      ldb_version_edit_set_compact_pointer(&edit, level,
                                           &vs->compact_pointer[level]);
    }
  }

  // Save files
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    for (size_t i = 0; i < vs->current_->nfiles[level]; i++) {
      ldb_file_meta* f = vs->current_->files[level][i];
      ldb_version_edit_add_file(&edit, level, f->number, f->file_size,
                                &f->smallest, &f->largest);
    }
  }

  ldb_buffer record;
  ldb_buffer_init(&record);
  ldb_status s = ldb_version_edit_encode(&edit, &record);
  if (ldb_ok(s)) {
    ldb_slice rs = ldb_buffer_slice(&record);
    s = ldb_log_writer_add_record(log, &rs);
  }
  ldb_buffer_destroy(&record);
  ldb_version_edit_destroy(&edit);
  return s;
}


static void ve_report_corruption(void* arg, size_t bytes, ldb_status s);

// =================================================================== LogAndApply
ldb_status ldb_version_set_log_and_apply(ldb_version_set* vs,
                                         ldb_version_edit* edit) {
  if (edit->has_log_number) {
    assert(edit->log_number >= vs->log_number);
    assert(edit->log_number < vs->next_file_number);
  } else {
    ldb_version_edit_set_log_number(edit, vs->log_number);
  }

  if (!edit->has_prev_log_number) {
    ldb_version_edit_set_prev_log_number(edit, vs->prev_log_number);
  }

  ldb_version_edit_set_next_file(edit, vs->next_file_number);
  ldb_version_edit_set_last_sequence(edit, vs->last_sequence);

  ldb_version* v = ldb_version_new(vs);
  vs_builder builder;
  vbuilder_init(&builder, vs, vs->current_);
  vbuilder_apply(&builder, edit);
  vbuilder_save_to(&builder, v);
  vbuilder_destroy(&builder);
  ldb_version_set_finalize(vs, v);

  // Initialize new descriptor log file if necessary by creating a
  // temporary file that contains a snapshot of the current version.
  char* new_manifest_file = NULL;
  ldb_status s = ldb_status_ok();
  if (vs->descriptor_log == NULL) {
    assert(vs->descriptor_file == NULL);
    new_manifest_file =
        ldb_descriptor_file_name(vs->dbname, vs->manifest_file_number);
    s = ldb_env_new_writable_file(vs->env, new_manifest_file,
                                  &vs->descriptor_file);
    if (ldb_ok(s)) {
      vs->descriptor_log = ldb_log_writer_new(vs->descriptor_file);
      s = ldb_version_set_write_snapshot(vs, vs->descriptor_log);
    }
  }

  // Unlock during expensive MANIFEST log write
  {
    ldb_mutex_unlock(vs->db_mutex);

    // Write new record to MANIFEST log
    if (ldb_ok(s)) {
      ldb_buffer record;
      ldb_buffer_init(&record);
      s = ldb_version_edit_encode(edit, &record);
      if (ldb_ok(s)) {
        ldb_slice rs = ldb_buffer_slice(&record);
        s = ldb_log_writer_add_record(vs->descriptor_log, &rs);
      }
      if (ldb_ok(s)) {
        ldb_status ss = vs->descriptor_file->m->sync(vs->descriptor_file);
        ldb_status_set(&s, ss);
      }
      ldb_buffer_destroy(&record);
    }

    // If we just created a new descriptor file, install it by writing a
    // new CURRENT file that points to it.
    if (ldb_ok(s) && new_manifest_file != NULL) {
      ldb_status cs =
          ldb_set_current_file(vs->env, vs->dbname, vs->manifest_file_number);
      ldb_status_set(&s, cs);
    }

    ldb_mutex_lock(vs->db_mutex);
  }

  // Install the new version
  if (ldb_ok(s)) {
    ldb_version_set_append_version(vs, v);
    vs->log_number = edit->log_number;
    vs->prev_log_number = edit->prev_log_number;
  } else {
    ldb_version_unref(v);
    if (new_manifest_file != NULL) {
      ldb_log_writer_destroy(vs->descriptor_log);
      vs->descriptor_log = NULL;
      vs->descriptor_file->m->destroy(vs->descriptor_file);
      vs->descriptor_file = NULL;
      ldb_status_release(ldb_env_remove_file(vs->env, new_manifest_file));
    }
  }

  free(new_manifest_file);
  return s;
}

// =================================================================== Recover
ldb_status ldb_version_set_recover(ldb_version_set* vs, int* save_manifest) {
  // Read "CURRENT" file, which contains a pointer to the current manifest
  char* current_name = ldb_current_file_name(vs->dbname);
  ldb_buffer current;
  ldb_buffer_init(&current);
  ldb_status s = ldb_read_file_to_string(vs->env, current_name, &current);
  free(current_name);
  if (!ldb_ok(s)) {
    ldb_buffer_destroy(&current);
    return s;
  }
  if (current.size == 0 || current.data[current.size - 1] != '\n') {
    ldb_buffer_destroy(&current);
    return ldb_status_corruption("CURRENT file does not end with newline", NULL);
  }
  current.size--;  // strip '\n'

  // dscname = dbname + "/" + current
  size_t dscname_len = strlen(vs->dbname) + 1 + current.size + 1;
  char* dscname = (char*)malloc(dscname_len);
  snprintf(dscname, dscname_len, "%s/%.*s", vs->dbname, (int)current.size,
           current.data);
  current_name = NULL;

  ldb_seq_file* file = NULL;
  s = ldb_env_new_sequential_file(vs->env, dscname, &file);
  if (!ldb_ok(s)) {
    if (s.code == LDB_IO_ERROR) {
      // leveldb distinguishes NotFound; approximate with corruption text.
    }
    free(dscname);
    ldb_buffer_destroy(&current);
    return s;
  }

  int have_log_number = 0;
  int have_prev_log_number = 0;
  int have_next_file = 0;
  int have_last_sequence = 0;
  uint64_t next_file = 0;
  uint64_t last_sequence = 0;
  uint64_t log_number = 0;
  uint64_t prev_log_number = 0;
  vs_builder builder;
  vbuilder_init(&builder, vs, vs->current_);
  int read_records = 0;

  ldb_log_reader* reader = ldb_log_reader_new(file, &s, ve_report_corruption,
                                              1 /*checksum*/, 0);
  ldb_slice record;
  ldb_buffer scratch;
  ldb_buffer_init(&scratch);
  while (ldb_log_reader_read_record(reader, &record, &scratch) && ldb_ok(s)) {
    read_records++;
    ldb_version_edit edit;
    ldb_version_edit_init(&edit);
    s = ldb_version_edit_decode(&edit, &record);
    if (ldb_ok(s)) {
      if (edit.has_comparator) {
        ldb_slice cmp_name = ldb_buffer_slice(&edit.comparator);
        const char* expected =
            vs->icmp.user_comparator->name(vs->icmp.user_comparator);
        ldb_slice expected_slice = ldb_slice_str(expected);
        if (!ldb_slice_equals(&cmp_name, &expected_slice)) {
          char buf[300];
          ldb_slice c = ldb_buffer_slice(&edit.comparator);
          snprintf(buf, sizeof(buf), "%.*s does not match existing comparator",
                   (int)c.size, c.data);
          ldb_status_destroy(&s);
          s = ldb_status_invalid_argument(buf, expected);
        }
      }
    }
    if (ldb_ok(s)) {
      vbuilder_apply(&builder, &edit);
    }
    if (edit.has_log_number) {
      log_number = edit.log_number;
      have_log_number = 1;
    }
    if (edit.has_prev_log_number) {
      prev_log_number = edit.prev_log_number;
      have_prev_log_number = 1;
    }
    if (edit.has_next_file_number) {
      next_file = edit.next_file_number;
      have_next_file = 1;
    }
    if (edit.has_last_sequence) {
      last_sequence = edit.last_sequence;
      have_last_sequence = 1;
    }
    ldb_version_edit_destroy(&edit);
  }
  ldb_buffer_destroy(&scratch);
  ldb_log_reader_destroy(reader);
  file->m->destroy(file);
  file = NULL;

  if (ldb_ok(s)) {
    if (!have_next_file) {
      s = ldb_status_corruption("no meta-nextfile entry in descriptor", NULL);
    } else if (!have_log_number) {
      s = ldb_status_corruption("no meta-lognumber entry in descriptor", NULL);
    } else if (!have_last_sequence) {
      s = ldb_status_corruption("no last-sequence-number entry in descriptor",
                                NULL);
    }
    if (!have_prev_log_number) {
      prev_log_number = 0;
    }
    ldb_version_set_mark_file_number_used(vs, prev_log_number);
    ldb_version_set_mark_file_number_used(vs, log_number);
  }

  if (ldb_ok(s)) {
    ldb_version* v = ldb_version_new(vs);
    vbuilder_save_to(&builder, v);
    vbuilder_destroy(&builder);
    // Install recovered version
    ldb_version_set_finalize(vs, v);
    ldb_version_set_append_version(vs, v);
    vs->manifest_file_number = next_file;
    vs->next_file_number = next_file + 1;
    vs->last_sequence = last_sequence;
    vs->log_number = log_number;
    vs->prev_log_number = prev_log_number;
    *save_manifest = 1;
  } else {
    vbuilder_destroy(&builder);
  }

  free(dscname);
  ldb_buffer_destroy(&current);
  return s;
}

// =================================================================== ApproximateOffsetOf
uint64_t ldb_version_set_approximate_offset_of(ldb_version_set* vs,
                                               ldb_version* v,
                                               const ldb_slice* ikey) {
  uint64_t result = 0;
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    for (size_t i = 0; i < v->nfiles[level]; i++) {
      ldb_file_meta* f = v->files[level][i];
      ldb_slice largest = ldb_buffer_slice(&f->largest);
      ldb_slice smallest = ldb_buffer_slice(&f->smallest);
      if (ldb_ikc_compare(&vs->icmp, &largest, ikey) <= 0) {
        // Entire file is before "ikey", so just add the file size
        result += f->file_size;
      } else if (ldb_ikc_compare(&vs->icmp, &smallest, ikey) > 0) {
        // Entire file is after "ikey", so ignore
        if (level > 0) {
          // Files other than level 0 are sorted by meta->smallest, so
          // no further files in this level will contain data for "ikey".
          break;
        }
      } else {
        // "ikey" falls in the range for this table. Add the approximate
        // offset of "ikey" within the table.
        ldb_read_options ro;
        ldb_read_options_init(&ro);
        ldb_table* tableptr = NULL;
        ldb_iterator* iter = ldb_table_cache_new_iterator(
            vs->table_cache, &ro, f->number, f->file_size);
        // Fetch the table object to get its offset function.
        (void)ldb_table_cache_get(vs->table_cache, &ro, f->number,
                                  f->file_size, NULL, NULL, NULL, &tableptr);
        if (tableptr != NULL) {
          result += ldb_table_approximate_offset_of(tableptr, ikey);
        }
        ldb_iterator_destroy(iter);
      }
    }
  }
  return result;
}

// =================================================================== Compaction
ldb_compaction* ldb_compaction_new(const ldb_options* options, int level) {
  ldb_compaction* c = (ldb_compaction*)calloc(1, sizeof(ldb_compaction));
  c->level = level;
  c->max_output_file_size = ldb_max_file_size_for_level(options, level);
  c->input_version = NULL;
  c->grandparent_index = 0;
  c->seen_key = 0;
  c->overlapped_bytes = 0;
  ldb_version_edit_init(&c->edit);
  return c;
}

void ldb_compaction_destroy(ldb_compaction* c) {
  if (c == NULL) return;
  if (c->input_version != NULL) {
    ldb_version_unref(c->input_version);
  }
  for (int i = 0; i < 2; i++) free(c->inputs[i]);
  free(c->grandparents);
  ldb_version_edit_destroy(&c->edit);
  free(c);
}

int ldb_compaction_is_trivial_move(const ldb_compaction* c) {
  const ldb_version_set* vset = c->input_version->vset;
  // Avoid a move if there is lots of overlapping grandparent data.
  return (c->inputs_count[0] == 1 && c->inputs_count[1] == 0 &&
          ldb_total_file_size(c->grandparents, c->grandparents_count) <=
              ldb_max_grandparent_overlap_bytes(vset->options));
}

int ldb_compaction_is_base_level_for_key(ldb_compaction* c,
                                         const ldb_slice* user_key) {
  const ldb_comparator* user_cmp =
      c->input_version->vset->icmp.user_comparator;
  for (int lvl = c->level + 2; lvl < LDB_K_NUM_LEVELS; lvl++) {
    ldb_file_meta** files = c->input_version->files[lvl];
    size_t n = c->input_version->nfiles[lvl];
    while (c->level_ptrs[lvl] < n) {
      ldb_file_meta* f = files[c->level_ptrs[lvl]];
      ldb_slice largest = ldb_buffer_slice(&f->largest);
      ldb_slice fl = ldb_extract_user_key(&largest);
      if (user_cmp->compare(user_cmp, user_key, &fl) <= 0) {
        // We've advanced far enough
        ldb_slice smallest = ldb_buffer_slice(&f->smallest);
        ldb_slice fs = ldb_extract_user_key(&smallest);
        if (user_cmp->compare(user_cmp, user_key, &fs) >= 0) {
          // Key falls in this file's range, so definitely not base level
          return 0;
        }
        break;
      }
      c->level_ptrs[lvl]++;
    }
  }
  return 1;
}

int ldb_compaction_should_stop_before(ldb_compaction* c,
                                      const ldb_slice* internal_key) {
  const ldb_version_set* vset = c->input_version->vset;
  const ldb_ikc* icmp = &vset->icmp;
  while (c->grandparent_index < c->grandparents_count) {
    ldb_slice gp_largest =
        ldb_bslice(&c->grandparents[c->grandparent_index]->largest);
    if (ldb_ikc_compare(icmp, internal_key, &gp_largest) > 0) {
      if (c->seen_key) {
        c->overlapped_bytes +=
            c->grandparents[c->grandparent_index]->file_size;
      }
      c->grandparent_index++;
    } else {
      break;
    }
  }
  c->seen_key = 1;

  if (c->overlapped_bytes > ldb_max_grandparent_overlap_bytes(vset->options)) {
    // Too much overlap for current output; start new output
    c->overlapped_bytes = 0;
    return 1;
  }
  return 0;
}

static void push_file(ldb_file_meta*** arr, size_t* count, size_t* cap,
                      ldb_file_meta* f) {
  if (*count == *cap) {
    *cap = *cap ? *cap * 2 : 4;
    *arr = (ldb_file_meta**)realloc(*arr, sizeof(ldb_file_meta*) * *cap);
    assert(*arr);
  }
  (*arr)[(*count)++] = f;
}

static void get_range(ldb_version_set* vs, ldb_file_meta** inputs, size_t n,
                      ldb_buffer* smallest, ldb_buffer* largest) {
  assert(n > 0);
  for (size_t i = 0; i < n; i++) {
    ldb_file_meta* f = inputs[i];
    if (i == 0) {
      ldb_buffer_copy(smallest, &f->smallest);
      ldb_buffer_copy(largest, &f->largest);
    } else {
      ldb_slice f_small = ldb_bslice(&f->smallest);
      ldb_slice f_large = ldb_bslice(&f->largest);
      ldb_slice cur_small = ldb_bslice(smallest);
      ldb_slice cur_large = ldb_bslice(largest);
      if (ldb_ikc_compare(&vs->icmp, &f_small, &cur_small) < 0) {
        ldb_buffer_copy(smallest, &f->smallest);
      }
      if (ldb_ikc_compare(&vs->icmp, &f_large, &cur_large) > 0) {
        ldb_buffer_copy(largest, &f->largest);
      }
    }
  }
}

static void get_range2(ldb_version_set* vs, ldb_file_meta** inputs1,
                       size_t n1, ldb_file_meta** inputs2, size_t n2,
                       ldb_buffer* smallest, ldb_buffer* largest) {
  size_t n = n1 + n2;
  ldb_file_meta** all = (ldb_file_meta**)malloc(sizeof(ldb_file_meta*) * n);
  // A caller with an empty level passes NULL with count 0 (the official C++
  // version merges std::vectors and never hands out a null base). Passing NULL
  // to memcpy is UB even for size 0, and glibc declares the arguments nonnull:
  // UBSan aborts here on Linux, and an inlined memcpy may assume it too.
  if (n1 > 0) memcpy(all, inputs1, sizeof(ldb_file_meta*) * n1);
  if (n2 > 0) memcpy(all + n1, inputs2, sizeof(ldb_file_meta*) * n2);
  get_range(vs, all, n, smallest, largest);
  free(all);
}

// Finds minimum file b2=(l2, u2) in level_files for which l2 > largest_key
// and user_key(l2) = user_key(largest_key).
static ldb_file_meta* find_smallest_boundary_file(
    const ldb_ikc* icmp, ldb_file_meta** level_files, size_t n,
    const ldb_buffer* largest_key) {
  const ldb_comparator* user_cmp = icmp->user_comparator;
  ldb_file_meta* smallest_boundary_file = NULL;
  ldb_slice lk = ldb_bslice(largest_key);
  ldb_slice lk_user = ldb_extract_user_key(&lk);
  for (size_t i = 0; i < n; ++i) {
    ldb_file_meta* f = level_files[i];
    ldb_slice smallest = ldb_bslice(&f->smallest);
    ldb_slice fs_user = ldb_extract_user_key(&smallest);
    if (ldb_ikc_compare(icmp, &smallest, &lk) > 0 &&
        user_cmp->compare(user_cmp, &fs_user, &lk_user) == 0) {
      if (smallest_boundary_file == NULL) {
        smallest_boundary_file = f;
      } else {
        ldb_slice best = ldb_bslice(&smallest_boundary_file->smallest);
        if (ldb_ikc_compare(icmp, &smallest, &best) < 0) {
          smallest_boundary_file = f;
        }
      }
    }
  }
  return smallest_boundary_file;
}

// Extracts the largest file b1 from compaction_files and then searches for a
// b2 in level_files for which user_key(u1) = user_key(l2). If it finds such
// a file b2 (known as a boundary file) it adds it to compaction_files and
// then searches again using this new upper bound.
static void add_boundary_inputs(const ldb_ikc* icmp,
                                ldb_file_meta** level_files, size_t n,
                                ldb_file_meta*** compaction_files,
                                size_t* cf_count, size_t* cf_cap) {
  if (n == 0 || *cf_count == 0) {
    return;
  }
  // Find largest key in compaction_files
  ldb_buffer largest_key;
  ldb_buffer_init(&largest_key);
  ldb_buffer_copy(&largest_key, &(*compaction_files)[0]->largest);
  for (size_t i = 1; i < *cf_count; ++i) {
    ldb_slice cur = ldb_bslice(&(*compaction_files)[i]->largest);
    ldb_slice lk = ldb_bslice(&largest_key);
    if (ldb_ikc_compare(icmp, &cur, &lk) > 0) {
      ldb_buffer_copy(&largest_key, &(*compaction_files)[i]->largest);
    }
  }

  int continue_searching = 1;
  while (continue_searching) {
    ldb_file_meta* smallest_boundary_file = find_smallest_boundary_file(
        icmp, level_files, n, &largest_key);
    if (smallest_boundary_file != NULL) {
      push_file(compaction_files, cf_count, cf_cap, smallest_boundary_file);
      ldb_buffer_copy(&largest_key, &smallest_boundary_file->largest);
    } else {
      continue_searching = 0;
    }
  }
  ldb_buffer_destroy(&largest_key);
}

static void vs_setup_other_inputs(ldb_version_set* vs, ldb_compaction* c) {
  const int level = c->level;
  ldb_buffer smallest, largest;
  ldb_buffer_init(&smallest);
  ldb_buffer_init(&largest);

  add_boundary_inputs(&vs->icmp, vs->current_->files[level],
                      vs->current_->nfiles[level], &c->inputs[0],
                      &c->inputs_count[0], &c->inputs_cap[0]);
  get_range(vs, c->inputs[0], c->inputs_count[0], &smallest, &largest);

  ldb_slice smallest_s = ldb_bslice(&smallest);
  ldb_slice largest_s = ldb_bslice(&largest);
  ldb_version_get_overlapping_inputs(vs->current_, level + 1, &smallest_s,
                                     &largest_s, &c->inputs[1],
                                     &c->inputs_count[1], &c->inputs_cap[1]);
  add_boundary_inputs(&vs->icmp, vs->current_->files[level + 1],
                      vs->current_->nfiles[level + 1], &c->inputs[1],
                      &c->inputs_count[1], &c->inputs_cap[1]);

  // Get entire range covered by compaction
  ldb_buffer all_start, all_limit;
  ldb_buffer_init(&all_start);
  ldb_buffer_init(&all_limit);
  get_range2(vs, c->inputs[0], c->inputs_count[0], c->inputs[1],
             c->inputs_count[1], &all_start, &all_limit);

  // See if we can grow the number of inputs in "level" without
  // changing the number of "level+1" files we pick up.
  if (c->inputs_count[1] > 0) {
    ldb_file_meta** expanded0 = NULL;
    size_t e0_count = 0, e0_cap = 0;
    ldb_slice all_start_s = ldb_bslice(&all_start);
    ldb_slice all_limit_s = ldb_bslice(&all_limit);
    ldb_version_get_overlapping_inputs(vs->current_, level, &all_start_s,
                                       &all_limit_s, &expanded0, &e0_count,
                                       &e0_cap);
    add_boundary_inputs(&vs->icmp, vs->current_->files[level],
                        vs->current_->nfiles[level], &expanded0, &e0_count,
                        &e0_cap);
    uint64_t inputs0_size =
        ldb_total_file_size(c->inputs[0], c->inputs_count[0]);
    uint64_t inputs1_size =
        ldb_total_file_size(c->inputs[1], c->inputs_count[1]);
    uint64_t expanded0_size = ldb_total_file_size(expanded0, e0_count);
    if (e0_count > c->inputs_count[0] &&
        inputs1_size + expanded0_size <
            ldb_expanded_compaction_byte_size_limit(vs->options)) {
      ldb_buffer new_start, new_limit;
      ldb_buffer_init(&new_start);
      ldb_buffer_init(&new_limit);
      get_range(vs, expanded0, e0_count, &new_start, &new_limit);
      ldb_file_meta** expanded1 = NULL;
      size_t e1_count = 0, e1_cap = 0;
      ldb_slice new_start_s = ldb_bslice(&new_start);
      ldb_slice new_limit_s = ldb_bslice(&new_limit);
      ldb_version_get_overlapping_inputs(vs->current_, level + 1, &new_start_s,
                                         &new_limit_s, &expanded1, &e1_count,
                                         &e1_cap);
      add_boundary_inputs(&vs->icmp, vs->current_->files[level + 1],
                          vs->current_->nfiles[level + 1], &expanded1,
                          &e1_count, &e1_cap);
      if (e1_count == c->inputs_count[1]) {
        ldb_log(vs->options->info_log,
                "Expanding@%d %d+%d (%llu+%llu bytes) to %d+%d "
                "(%llu+%llu bytes)\n",
                level, (int)c->inputs_count[0], (int)c->inputs_count[1],
                (unsigned long long)inputs0_size,
                (unsigned long long)inputs1_size, (int)e0_count, (int)e1_count,
                (unsigned long long)expanded0_size,
                (unsigned long long)inputs1_size);
        ldb_buffer_copy(&smallest, &new_start);
        ldb_buffer_copy(&largest, &new_limit);
        free(c->inputs[0]);
        c->inputs[0] = expanded0;
        c->inputs_count[0] = e0_count;
        c->inputs_cap[0] = e0_cap;
        expanded0 = NULL;
        free(c->inputs[1]);
        c->inputs[1] = expanded1;
        c->inputs_count[1] = e1_count;
        c->inputs_cap[1] = e1_cap;
        expanded1 = NULL;
        get_range2(vs, c->inputs[0], c->inputs_count[0], c->inputs[1],
                   c->inputs_count[1], &all_start, &all_limit);
      }
      free(expanded1);
      ldb_buffer_destroy(&new_start);
      ldb_buffer_destroy(&new_limit);
    }
    free(expanded0);
  }

  // Compute the set of grandparent files that overlap this compaction
  // (parent == level+1; grandparent == level+2)
  if (level + 2 < LDB_K_NUM_LEVELS) {
    ldb_slice all_start_g = ldb_bslice(&all_start);
    ldb_slice all_limit_g = ldb_bslice(&all_limit);
    ldb_version_get_overlapping_inputs(vs->current_, level + 2, &all_start_g,
                                       &all_limit_g, &c->grandparents,
                                       &c->grandparents_count,
                                       &c->grandparents_cap);
  }

  // Update the place where we will do the next compaction for this level.
  ldb_buffer_copy(&vs->compact_pointer[level], &largest);
  ldb_version_edit_set_compact_pointer(&c->edit, level, &largest);

  ldb_buffer_destroy(&smallest);
  ldb_buffer_destroy(&largest);
  ldb_buffer_destroy(&all_start);
  ldb_buffer_destroy(&all_limit);
}

ldb_compaction* ldb_version_set_pick_compaction(ldb_version_set* vs) {
  ldb_compaction* c;
  int level;

  const int size_compaction = (vs->current_->compaction_score >= 1);
  const int seek_compaction = (vs->current_->file_to_compact != NULL);
  if (size_compaction) {
    level = vs->current_->compaction_level;
    assert(level >= 0);
    assert(level + 1 < LDB_K_NUM_LEVELS);
    c = ldb_compaction_new(vs->options, level);

    // Pick the first file that comes after compact_pointer_[level]
    for (size_t i = 0; i < vs->current_->nfiles[level]; i++) {
      ldb_file_meta* f = vs->current_->files[level][i];
      ldb_slice largest = ldb_bslice(&f->largest);
      ldb_slice cp = ldb_bslice(&vs->compact_pointer[level]);
      if (vs->compact_pointer[level].size == 0 ||
          ldb_ikc_compare(&vs->icmp, &largest, &cp) > 0) {
        push_file(&c->inputs[0], &c->inputs_count[0], &c->inputs_cap[0], f);
        break;
      }
    }
    if (c->inputs_count[0] == 0) {
      // Wrap-around to the beginning of the key space
      push_file(&c->inputs[0], &c->inputs_count[0], &c->inputs_cap[0],
                vs->current_->files[level][0]);
    }
  } else if (seek_compaction) {
    level = vs->current_->file_to_compact_level;
    c = ldb_compaction_new(vs->options, level);
    push_file(&c->inputs[0], &c->inputs_count[0], &c->inputs_cap[0],
              vs->current_->file_to_compact);
  } else {
    return NULL;
  }

  c->input_version = vs->current_;
  c->input_version->refs++;

  // Files in level 0 may overlap each other, so pick up all overlapping ones
  if (level == 0) {
    ldb_buffer smallest, largest;
    ldb_buffer_init(&smallest);
    ldb_buffer_init(&largest);
    get_range(vs, c->inputs[0], c->inputs_count[0], &smallest, &largest);
    ldb_slice smallest_s = ldb_bslice(&smallest);
    ldb_slice largest_s = ldb_bslice(&largest);
    ldb_version_get_overlapping_inputs(vs->current_, 0, &smallest_s,
                                       &largest_s, &c->inputs[0],
                                       &c->inputs_count[0],
                                       &c->inputs_cap[0]);
    assert(c->inputs_count[0] > 0);
    ldb_buffer_destroy(&smallest);
    ldb_buffer_destroy(&largest);
  }

  vs_setup_other_inputs(vs, c);

  return c;
}

ldb_compaction* ldb_version_set_compact_range(ldb_version_set* vs, int level,
                                              const ldb_slice* begin,
                                              const ldb_slice* end) {
  ldb_file_meta** inputs = NULL;
  size_t inputs_count = 0, inputs_cap = 0;
  ldb_version_get_overlapping_inputs(vs->current_, level, begin, end, &inputs,
                                     &inputs_count, &inputs_cap);
  if (inputs_count == 0) {
    free(inputs);
    return NULL;
  }

  // Avoid compacting too much in one shot in case the range is large.
  // But we cannot do this for level-0 since level-0 files can overlap
  // and we must not pick one file and drop another older file if the
  // two files overlap.
  if (level > 0) {
    const uint64_t limit = ldb_max_file_size_for_level(vs->options, level);
    uint64_t total = 0;
    for (size_t i = 0; i < inputs_count; i++) {
      uint64_t s = inputs[i]->file_size;
      total += s;
      if (total >= limit) {
        inputs_count = i + 1;
        break;
      }
    }
  }

  ldb_compaction* c = ldb_compaction_new(vs->options, level);
  c->input_version = vs->current_;
  c->input_version->refs++;
  for (size_t i = 0; i < inputs_count; i++) {
    push_file(&c->inputs[0], &c->inputs_count[0], &c->inputs_cap[0],
              inputs[i]);
  }
  free(inputs);
  vs_setup_other_inputs(vs, c);
  return c;
}

// =================================================================== MakeInputIterator
ldb_iterator* ldb_version_set_make_input_iterator(ldb_version_set* vs,
                                                  ldb_compaction* c) {
  ldb_read_options options;
  ldb_read_options_init(&options);
  options.verify_checksums = vs->options->paranoid_checks;
  options.fill_cache = 0;

  // Level-0 files have to be merged together. For other levels,
  // we will make a concatenating iterator per level.
  size_t space = (c->level == 0) ? (c->inputs_count[0] + 1) : 2;
  ldb_iterator** list = (ldb_iterator**)malloc(sizeof(ldb_iterator*) * space);
  size_t num = 0;
  for (int which = 0; which < 2; which++) {
    if (c->inputs_count[which] > 0) {
      if (c->level + which == 0) {
        for (size_t i = 0; i < c->inputs_count[which]; i++) {
          list[num++] = ldb_table_cache_new_iterator(
              vs->table_cache, &options, c->inputs[which][i]->number,
              c->inputs[which][i]->file_size);
        }
      } else {
        // Create concatenating iterator for the files from this level
        ldb_iterator* index_iter =
            lfn_new(&vs->icmp, c->inputs[which], c->inputs_count[which]);
        list[num++] = ldb_new_two_level_iterator(
            index_iter, get_file_iterator, vs->table_cache, &options);
      }
    }
  }
  ldb_iterator* result = ldb_new_merging_iterator(&vs->icmp, list, num);
  free(list);
  return result;
}

// =================================================================== snapshot list
void ldb_snapshot_list_init(ldb_snapshot_list* l) {
  l->head.prev = &l->head;
  l->head.next = &l->head;
}

ldb_snapshot_impl* ldb_snapshot_list_new(ldb_snapshot_list* l, uint64_t seq) {
  ldb_snapshot_impl* s =
      (ldb_snapshot_impl*)malloc(sizeof(ldb_snapshot_impl));
  s->sequence = seq;
  s->next = &l->head;
  s->prev = l->head.prev;
  s->prev->next = s;
  s->next->prev = s;
  return s;
}

void ldb_snapshot_list_delete(ldb_snapshot_list* l, ldb_snapshot_impl* s) {
  (void)l;
  s->next->prev = s->prev;
  s->prev->next = s->next;
  free(s);
}

ldb_snapshot_impl* ldb_snapshot_list_oldest(const ldb_snapshot_list* l) {
  ldb_snapshot_impl* s = l->head.next;
  if (s == &l->head) return NULL;
  return s;
}

int ldb_snapshot_list_empty(const ldb_snapshot_list* l) {
  return l->head.next == &l->head;
}

// Corruption reporter for manifest recovery: saves first error into the
// caller-provided ldb_status*.
void ve_report_corruption(void* arg, size_t bytes, ldb_status s) {
  (void)bytes;
  ldb_status* status = (ldb_status*)arg;
  if (ldb_ok(*status)) {
    *status = s;
  } else {
    ldb_status_destroy(&s);
  }
}

int ldb_version_set_needs_compaction(const ldb_version_set* vs) {
  // Score-based compaction or seek-based compaction pending?
  return (vs->current_->compaction_score >= 1) ||
         (vs->current_->file_to_compact != NULL);
}

uint64_t ldb_snapshot_list_smallest(const ldb_snapshot_list* l) {
  // All snapshots are kept in ascending sequence order in the list.
  ldb_snapshot_impl* s = ldb_snapshot_list_oldest(l);
  return s ? s->sequence : LDB_K_MAX_SEQUENCE_NUMBER;
}
