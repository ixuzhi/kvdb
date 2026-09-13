// db_iter.c - user-facing iterator combining internal entries per user key
// (mirrors leveldb/db/db_iter.cc)
#include "kvdb.h"

#include <assert.h>

#define LDB_READ_BYTES_PERIOD 1048576

typedef struct db_iter {
  ldb_iterator base;
  ldb_db_impl* db;
  const ldb_comparator* user_comparator;
  ldb_iterator* iter;  // owned internal iterator
  uint64_t sequence;
  ldb_status status;
  ldb_buffer saved_key;    // == current key when direction==kReverse
  ldb_buffer saved_value;  // == current raw value when direction==kReverse
  int direction;           // 0 = forward, 1 = reverse
  int valid;
  ldb_random rnd;
  size_t bytes_until_read_sampling;
} db_iter;

static size_t random_compaction_period(db_iter* di) {
  return ldb_random_uniform(&di->rnd, 2 * LDB_READ_BYTES_PERIOD);
}

static int di_parse_key(db_iter* di, ldb_parsed_internal_key* ikey) {
  ldb_slice k = ldb_iter_key(di->iter);

  ldb_slice v = ldb_iter_value(di->iter);
  size_t bytes_read = k.size + v.size;
  while (di->bytes_until_read_sampling < bytes_read) {
    di->bytes_until_read_sampling += random_compaction_period(di);
    ldb_db_impl_record_read_sample(di->db, &k);
  }
  assert(di->bytes_until_read_sampling >= bytes_read);
  di->bytes_until_read_sampling -= bytes_read;

  if (!ldb_parse_internal_key(&k, ikey)) {
    ldb_status_destroy(&di->status);
    di->status = ldb_status_corruption("corrupted internal key in DBIter", NULL);
    return 0;
  }
  return 1;
}

static void di_save_key(db_iter* di, const ldb_slice* k) {
  ldb_buffer_clear(&di->saved_key);
  ldb_buffer_append_slice(&di->saved_key, k);
}

static void di_clear_saved_value(db_iter* di) {
  ldb_buffer_clear(&di->saved_value);
}

static void di_find_next_user_entry(db_iter* di, int skipping,
                                    ldb_buffer* skip);

static void di_destroy(ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  ldb_iterator_destroy(di->iter);
  ldb_buffer_destroy(&di->saved_key);
  ldb_buffer_destroy(&di->saved_value);
  ldb_status_destroy(&di->status);
  free(di);
}

static int di_valid(const ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  return di->valid;
}

static void di_next(ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  assert(di->valid);

  if (di->direction == 1) {  // Switch directions?
    di->direction = 0;
    // iter_ is pointing just before the entries for this->key(),
    // so advance into the range of entries for this->key() and then
    // use the normal skipping code below.
    if (!ldb_iter_valid(di->iter)) {
      ldb_iter_seek_to_first(di->iter);
    } else {
      ldb_iter_next(di->iter);
    }
    if (!ldb_iter_valid(di->iter)) {
      di->valid = 0;
      ldb_buffer_clear(&di->saved_key);
      return;
    }
    // saved_key_ already contains the key to skip past.
  } else {
    // Store in saved_key_ the current key so we skip it below.
    ldb_slice cur = ldb_iter_key(di->iter);
    ldb_slice uk = ldb_extract_user_key(&cur);
    di_save_key(di, &uk);

    // iter_ is pointing to current key. We can now safely move to the next
    // to avoid checking current key.
    ldb_iter_next(di->iter);
    if (!ldb_iter_valid(di->iter)) {
      di->valid = 0;
      ldb_buffer_clear(&di->saved_key);
      return;
    }
  }

  di_find_next_user_entry(di, 1 /*skipping*/, &di->saved_key);
}

static void di_find_next_user_entry(db_iter* di, int skipping,
                                    ldb_buffer* skip) {
  // Loop until we hit an acceptable entry to yield
  assert(ldb_iter_valid(di->iter));
  assert(di->direction == 0);
  do {
    ldb_parsed_internal_key ikey;
    if (di_parse_key(di, &ikey) && ikey.sequence <= di->sequence) {
      switch (ikey.type) {
        case LDB_TYPE_DELETION:
          // Arrange to skip all upcoming entries for this key since
          // they are hidden by this deletion.
          di_save_key(di, &ikey.user_key);
          skipping = 1;
          break;
        case LDB_TYPE_VALUE:
          if (skipping &&
              di->user_comparator->compare(di->user_comparator,
                                           &ikey.user_key,
                                           &(ldb_slice){skip->data, skip->size}) <= 0) {
            // Entry hidden
          } else {
            di->valid = 1;
            ldb_buffer_clear(&di->saved_key);
            return;
          }
          break;
      }
    }
    ldb_iter_next(di->iter);
  } while (ldb_iter_valid(di->iter));
  ldb_buffer_clear(&di->saved_key);
  di->valid = 0;
}

static void di_find_prev_user_entry(db_iter* di);

static void di_prev(ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  assert(di->valid);

  if (di->direction == 0) {  // Switch directions?
    // iter_ is pointing at the current entry. Scan backwards until the key
    // changes so we can use the normal reverse scanning code.
    assert(ldb_iter_valid(di->iter));  // Otherwise valid_ would have been false
    ldb_slice cur = ldb_iter_key(di->iter);
    ldb_slice uk = ldb_extract_user_key(&cur);
    di_save_key(di, &uk);
    while (1) {
      ldb_iter_prev(di->iter);
      if (!ldb_iter_valid(di->iter)) {
        di->valid = 0;
        ldb_buffer_clear(&di->saved_key);
        di_clear_saved_value(di);
        return;
      }
      ldb_slice ik = ldb_iter_key(di->iter);
      ldb_slice iku = ldb_extract_user_key(&ik);
      ldb_slice sk = ldb_bslice(&di->saved_key);
      if (di->user_comparator->compare(di->user_comparator, &iku,
                                       &sk) < 0) {
        break;
      }
    }
    di->direction = 1;
  }

  di_find_prev_user_entry(di);
}

static void di_find_prev_user_entry(db_iter* di) {
  assert(di->direction == 1);

  int value_type = LDB_TYPE_DELETION;
  if (ldb_iter_valid(di->iter)) {
    do {
      ldb_parsed_internal_key ikey;
      if (di_parse_key(di, &ikey) && ikey.sequence <= di->sequence) {
        ldb_slice sk = ldb_buffer_slice(&di->saved_key);
        if ((value_type != LDB_TYPE_DELETION) &&
            di->user_comparator->compare(di->user_comparator,
                                         &ikey.user_key, &sk) < 0) {
          // We encountered a non-deleted value in entries for previous
          // keys.
          break;
        }
        value_type = ikey.type;
        if (value_type == LDB_TYPE_DELETION) {
          ldb_buffer_clear(&di->saved_key);
          di_clear_saved_value(di);
        } else {
          ldb_slice raw_value = ldb_iter_value(di->iter);
          di_clear_saved_value(di);
          ldb_buffer_append_slice(&di->saved_value, &raw_value);
          di_save_key(di, &ikey.user_key);
        }
      }
      ldb_iter_prev(di->iter);
    } while (ldb_iter_valid(di->iter));
  }

  if (value_type == LDB_TYPE_DELETION) {
    // End
    di->valid = 0;
    ldb_buffer_clear(&di->saved_key);
    di_clear_saved_value(di);
    di->direction = 0;
  } else {
    di->valid = 1;
  }
}

static void di_seek(ldb_iterator* it, const ldb_slice* target) {
  db_iter* di = (db_iter*)it;
  di->direction = 0;
  di_clear_saved_value(di);
  ldb_buffer_clear(&di->saved_key);
  ldb_append_internal_key(&di->saved_key, target, di->sequence,
                          LDB_VALUE_TYPE_FOR_SEEK);
  ldb_slice seek_key = ldb_buffer_slice(&di->saved_key);
  ldb_iter_seek(di->iter, &seek_key);
  if (ldb_iter_valid(di->iter)) {
    di_find_next_user_entry(di, 0 /*not skipping*/, &di->saved_key);
  } else {
    di->valid = 0;
  }
}

static void di_seek_to_first(ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  di->direction = 0;
  di_clear_saved_value(di);
  ldb_iter_seek_to_first(di->iter);
  if (ldb_iter_valid(di->iter)) {
    di_find_next_user_entry(di, 0 /*not skipping*/, &di->saved_key);
  } else {
    di->valid = 0;
  }
}

static void di_seek_to_last(ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  di->direction = 1;
  di_clear_saved_value(di);
  ldb_iter_seek_to_last(di->iter);
  di_find_prev_user_entry(di);
}

static ldb_slice di_key(const ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  assert(di->valid);
  if (di->direction == 0) {
    ldb_slice k = ldb_iter_key(di->iter);
    return ldb_extract_user_key(&k);
  } else {
    return ldb_buffer_slice(&di->saved_key);
  }
}

static ldb_slice di_value(const ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  assert(di->valid);
  if (di->direction == 0) {
    return ldb_iter_value(di->iter);
  } else {
    return ldb_buffer_slice(&di->saved_value);
  }
}

static ldb_status di_status(const ldb_iterator* it) {
  db_iter* di = (db_iter*)it;
  if (ldb_ok(di->status)) {
    return ldb_iter_status(di->iter);
  }
  return ldb_status_copy(di->status);
}

static const ldb_iterator_methods g_db_iter_methods = {
    di_destroy, di_valid,  di_seek_to_first, di_seek_to_last, di_seek,
    di_next,    di_prev,   di_key,           di_value,        di_status};

ldb_iterator* ldb_new_db_iterator(ldb_db_impl* db,
                                  const ldb_comparator* user_comparator,
                                  ldb_iterator* internal_iter,
                                  uint64_t sequence, uint32_t seed) {
  db_iter* di = (db_iter*)malloc(sizeof(db_iter));
  di->base.m = &g_db_iter_methods;
  di->base.impl = di;
  di->base.cleanups = NULL;
  di->db = db;
  di->user_comparator = user_comparator;
  di->iter = internal_iter;
  di->sequence = sequence;
  di->status = ldb_status_ok();
  ldb_buffer_init(&di->saved_key);
  ldb_buffer_init(&di->saved_value);
  di->direction = 0;
  di->valid = 0;
  ldb_random_init(&di->rnd, seed);
  di->bytes_until_read_sampling = random_compaction_period(di);
  return &di->base;
}
