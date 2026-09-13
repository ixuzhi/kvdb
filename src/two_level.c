// two_level.c - two-level iterator (index block -> data blocks)
// (mirrors leveldb/table/two_level_iterator.cc)
#include "kvdb.h"

#include <assert.h>

typedef struct two_level_iter {
  ldb_iterator base;
  ldb_iterator* index_iter;  // owned
  ldb_iterator* data_iter;   // owned; may be NULL
  ldb_block_reader_fn block_function;
  void* arg;
  ldb_read_options options;
  ldb_buffer data_block_handle;
  ldb_status status;  // saved error from replaced data iterators
} two_level_iter;

static void tl_save_error(two_level_iter* tl, ldb_status s) {
  if (ldb_ok(tl->status) && !ldb_ok(s)) {
    tl->status = s;
  } else {
    ldb_status_destroy(&s);
  }
}

static void tl_set_data_iterator(two_level_iter* tl, ldb_iterator* data_iter) {
  if (tl->data_iter != NULL) {
    tl_save_error(tl, ldb_iter_status(tl->data_iter));
    ldb_iterator_destroy(tl->data_iter);
  }
  tl->data_iter = data_iter;
}

static void tl_init_data_block(two_level_iter* tl) {
  if (!ldb_iter_valid(tl->index_iter)) {
    tl_set_data_iterator(tl, NULL);
  } else {
    ldb_slice handle = ldb_iter_value(tl->index_iter);
    ldb_slice saved = ldb_buffer_slice(&tl->data_block_handle);
    if (tl->data_iter != NULL && ldb_slice_equals(&handle, &saved)) {
      // data_iter_ is already constructed with this iterator, so
      // no need to change anything
    } else {
      ldb_iterator* iter = tl->block_function(tl->arg, &tl->options, &handle);
      ldb_buffer_clear(&tl->data_block_handle);
      ldb_buffer_append(&tl->data_block_handle, handle.data, handle.size);
      tl_set_data_iterator(tl, iter);
    }
  }
}

static void tl_skip_empty_forward(two_level_iter* tl) {
  while (tl->data_iter == NULL || !ldb_iter_valid(tl->data_iter)) {
    // Move to next block
    if (!ldb_iter_valid(tl->index_iter)) {
      tl_set_data_iterator(tl, NULL);
      return;
    }
    ldb_iter_next(tl->index_iter);
    tl_init_data_block(tl);
    if (tl->data_iter != NULL) ldb_iter_seek_to_first(tl->data_iter);
  }
}

static void tl_skip_empty_backward(two_level_iter* tl) {
  while (tl->data_iter == NULL || !ldb_iter_valid(tl->data_iter)) {
    // Move to previous block
    if (!ldb_iter_valid(tl->index_iter)) {
      tl_set_data_iterator(tl, NULL);
      return;
    }
    ldb_iter_prev(tl->index_iter);
    tl_init_data_block(tl);
    if (tl->data_iter != NULL) ldb_iter_seek_to_last(tl->data_iter);
  }
}

static void tl_destroy(ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  if (tl->index_iter) ldb_iterator_destroy(tl->index_iter);
  if (tl->data_iter) ldb_iterator_destroy(tl->data_iter);
  ldb_buffer_destroy(&tl->data_block_handle);
  ldb_status_destroy(&tl->status);
  free(tl);
}

static int tl_valid(const ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  return tl->data_iter != NULL && ldb_iter_valid(tl->data_iter);
}

static void tl_seek(ldb_iterator* it, const ldb_slice* target) {
  two_level_iter* tl = (two_level_iter*)it;
  ldb_iter_seek(tl->index_iter, target);
  tl_init_data_block(tl);
  if (tl->data_iter != NULL) ldb_iter_seek(tl->data_iter, target);
  tl_skip_empty_forward(tl);
}

static void tl_seek_to_first(ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  ldb_iter_seek_to_first(tl->index_iter);
  tl_init_data_block(tl);
  if (tl->data_iter != NULL) ldb_iter_seek_to_first(tl->data_iter);
  tl_skip_empty_forward(tl);
}

static void tl_seek_to_last(ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  ldb_iter_seek_to_last(tl->index_iter);
  tl_init_data_block(tl);
  if (tl->data_iter != NULL) ldb_iter_seek_to_last(tl->data_iter);
  tl_skip_empty_backward(tl);
}

static void tl_next(ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  assert(tl_valid(it));
  ldb_iter_next(tl->data_iter);
  tl_skip_empty_forward(tl);
}

static void tl_prev(ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  assert(tl_valid(it));
  ldb_iter_prev(tl->data_iter);
  tl_skip_empty_backward(tl);
}

static ldb_slice tl_key(const ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  return ldb_iter_key(tl->data_iter);
}

static ldb_slice tl_value(const ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  return ldb_iter_value(tl->data_iter);
}

static ldb_status tl_status(const ldb_iterator* it) {
  two_level_iter* tl = (two_level_iter*)it;
  ldb_status is = ldb_iter_status(tl->index_iter);
  if (!ldb_ok(is)) {
    return is;
  }
  if (tl->data_iter != NULL) {
    ldb_status ds = ldb_iter_status(tl->data_iter);
    if (!ldb_ok(ds)) {
      return ds;
    }
    ldb_status_destroy(&ds);
  }
  return ldb_status_copy(tl->status);
}

static const ldb_iterator_methods g_two_level_methods = {
    tl_destroy, tl_valid,  tl_seek_to_first, tl_seek_to_last, tl_seek,
    tl_next,    tl_prev,   tl_key,           tl_value,        tl_status};

ldb_iterator* ldb_new_two_level_iterator(ldb_iterator* index_iter,
                                         ldb_block_reader_fn block_reader,
                                         void* arg,
                                         const ldb_read_options* options) {
  two_level_iter* tl = (two_level_iter*)malloc(sizeof(two_level_iter));
  tl->base.m = &g_two_level_methods;
  tl->base.impl = tl;
  tl->base.cleanups = NULL;
  tl->index_iter = index_iter;
  tl->data_iter = NULL;
  tl->block_function = block_reader;
  tl->arg = arg;
  tl->options = *options;
  ldb_buffer_init(&tl->data_block_handle);
  tl->status = ldb_status_ok();
  return &tl->base;
}
