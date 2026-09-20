// table.c - sstable builder and reader (mirrors leveldb/table_builder.cc
// and table/table.cc)
#include "kvdb.h"

#include <assert.h>

// =================================================================== builder
struct ldb_table_builder {
  const ldb_options* options;
  ldb_options* index_block_opts;  // options copy with restart interval 1
  ldb_buffer last_key;
  ldb_writable_file* file;
  uint64_t offset;
  ldb_status status;
  ldb_block_builder data_block;
  ldb_block_builder index_block;
  int64_t num_entries;
  int closed;
  ldb_filter_block_builder* filter_block;  // NULL unless policy set
  int pending_index_entry;
  ldb_block_handle pending_handle;
  ldb_buffer compressed_output;
};

static void tb_write_raw_block(ldb_table_builder* b,
                               const ldb_slice* block_contents, int type,
                               ldb_block_handle* handle);

static int tb_ok(const ldb_table_builder* b) { return ldb_ok(b->status); }

ldb_table_builder* ldb_table_builder_new(const ldb_options* options,
                                         ldb_writable_file* file) {
  ldb_table_builder* b =
      (ldb_table_builder*)calloc(1, sizeof(ldb_table_builder));
  b->options = options;
  ldb_buffer_init(&b->last_key);
  ldb_buffer_init(&b->compressed_output);
  b->file = file;
  b->offset = 0;
  b->status = ldb_status_ok();
  ldb_block_builder_init(&b->data_block, options);
  // index_block_options(opt) with restart interval 1
  ldb_options* iopts = (ldb_options*)calloc(1, sizeof(ldb_options));
  *iopts = *options;
  iopts->block_restart_interval = 1;
  b->index_block_opts = iopts;
  ldb_block_builder_init(&b->index_block, iopts);
  b->num_entries = 0;
  b->closed = 0;
  if (options->filter_policy != NULL) {
    b->filter_block =
        (ldb_filter_block_builder*)malloc(sizeof(ldb_filter_block_builder));
    ldb_filter_block_builder_init(b->filter_block, options->filter_policy);
    ldb_filter_block_builder_start_block(b->filter_block, 0);
  }
  b->pending_index_entry = 0;
  ldb_block_handle_init(&b->pending_handle);
  return b;
}

void ldb_table_builder_destroy(ldb_table_builder* b) {
  ldb_block_builder_destroy(&b->data_block);
  ldb_block_builder_destroy(&b->index_block);
  free(b->index_block_opts);
  if (b->filter_block) {
    ldb_filter_block_builder_destroy(b->filter_block);
    free(b->filter_block);
  }
  ldb_buffer_destroy(&b->last_key);
  ldb_buffer_destroy(&b->compressed_output);
  ldb_status_destroy(&b->status);
  free(b);
}

static void tb_write_block(ldb_table_builder* b, ldb_block_builder* block,
                           ldb_block_handle* handle) {
  ldb_slice raw = ldb_block_builder_finish(block);

  ldb_slice block_contents;
  int type = b->options->compression;
  switch (type) {
    case LDB_NO_COMPRESSION:
      block_contents = raw;
      break;
    case LDB_SNAPPY_COMPRESSION: {
      ldb_buffer* compressed = &b->compressed_output;
      if (ldb_snappy_compress(raw.data, raw.size, compressed) &&
          compressed->size < raw.size - (raw.size / 8u)) {
        block_contents = ldb_buffer_slice(compressed);
      } else {
        // Snappy not supported, or compressed less than 12.5%, so just
        // store uncompressed form
        block_contents = raw;
        type = LDB_NO_COMPRESSION;
      }
      break;
    }
    default:
      block_contents = raw;
      type = LDB_NO_COMPRESSION;
      break;
  }
  tb_write_raw_block(b, &block_contents, type, handle);
  ldb_buffer_clear(&b->compressed_output);
  ldb_block_builder_reset(block);
}

static void tb_write_raw_block(ldb_table_builder* b,
                               const ldb_slice* block_contents, int type,
                               ldb_block_handle* handle) {
  handle->offset = b->offset;
  handle->size = block_contents->size;
  b->status = b->file->m->append(b->file, block_contents);
  if (ldb_ok(b->status)) {
    char trailer[LDB_BLOCK_TRAILER_SIZE];
    trailer[0] = (char)type;
    uint32_t crc = ldb_crc32c_value(block_contents->data, block_contents->size);
    crc = ldb_crc32c_extend(crc, trailer, 1);  // Extend crc to cover block type
    ldb_encode_fixed32(trailer + 1, ldb_crc32c_mask(crc));
    ldb_slice tslice = ldb_slice_make(trailer, LDB_BLOCK_TRAILER_SIZE);
    b->status = b->file->m->append(b->file, &tslice);
    if (ldb_ok(b->status)) {
      b->offset += block_contents->size + LDB_BLOCK_TRAILER_SIZE;
    }
  }
}

void ldb_table_builder_add(ldb_table_builder* b, const ldb_slice* key,
                           const ldb_slice* value) {
  assert(!b->closed);
  if (!tb_ok(b)) return;
  if (b->num_entries > 0) {
    ldb_slice lk = ldb_buffer_slice(&b->last_key);
    const ldb_comparator* cmp = b->options->comparator;
    assert(cmp->compare(cmp, key, &lk) > 0);
  }

  if (b->pending_index_entry) {
    assert(ldb_block_builder_empty(&b->data_block));
    const ldb_comparator* cmp = b->options->comparator;
    cmp->find_shortest_separator(cmp, &b->last_key, key);
    ldb_buffer handle_encoding;
    ldb_buffer_init(&handle_encoding);
    ldb_block_handle_encode(&b->pending_handle, &handle_encoding);
    ldb_slice he = ldb_buffer_slice(&handle_encoding);
    ldb_slice last_slice = ldb_buffer_slice(&b->last_key);
    ldb_block_builder_add(&b->index_block, &last_slice, &he);
    ldb_buffer_destroy(&handle_encoding);
    b->pending_index_entry = 0;
  }

  if (b->filter_block != NULL) {
    ldb_filter_block_builder_add_key(b->filter_block, key);
  }

  ldb_buffer_clear(&b->last_key);
  ldb_buffer_append_slice(&b->last_key, key);
  b->num_entries++;
  ldb_block_builder_add(&b->data_block, key, value);

  const size_t estimated_block_size =
      ldb_block_builder_current_size_estimate(&b->data_block);
  if (estimated_block_size >= b->options->block_size) {
    ldb_table_builder_flush(b);
  }
}

void ldb_table_builder_flush(ldb_table_builder* b) {
  assert(!b->closed);
  if (!tb_ok(b)) return;
  if (ldb_block_builder_empty(&b->data_block)) return;
  assert(!b->pending_index_entry);
  tb_write_block(b, &b->data_block, &b->pending_handle);
  if (tb_ok(b)) {
    b->pending_index_entry = 1;
    ldb_status fs = b->file->m->flush(b->file);
    ldb_status_set(&b->status, fs);
  }
  if (b->filter_block != NULL) {
    ldb_filter_block_builder_start_block(b->filter_block, b->offset);
  }
}

ldb_status ldb_table_builder_finish(ldb_table_builder* b) {
  ldb_table_builder_flush(b);
  assert(!b->closed);
  b->closed = 1;

  ldb_block_handle filter_block_handle;
  ldb_block_handle metaindex_block_handle;
  ldb_block_handle index_block_handle;
  ldb_block_handle_init(&filter_block_handle);
  ldb_block_handle_init(&metaindex_block_handle);
  ldb_block_handle_init(&index_block_handle);

  // Write filter block
  if (tb_ok(b) && b->filter_block != NULL) {
    ldb_slice fdata = ldb_filter_block_builder_finish(b->filter_block);
    tb_write_raw_block(b, &fdata, LDB_NO_COMPRESSION, &filter_block_handle);
  }

  // Write metaindex block
  if (tb_ok(b)) {
    ldb_block_builder meta_index_block;
    ldb_block_builder_init(&meta_index_block, b->options);
    if (b->filter_block != NULL) {
      // Add mapping from "filter.Name" to location of filter data
      ldb_buffer key;
      ldb_buffer_init(&key);
      ldb_buffer_append_str(&key, "filter.");
      ldb_buffer_append_str(&key, b->options->filter_policy->name(
                                      b->options->filter_policy));
      ldb_buffer handle_encoding;
      ldb_buffer_init(&handle_encoding);
      ldb_block_handle_encode(&filter_block_handle, &handle_encoding);
      ldb_slice k = ldb_buffer_slice(&key);
      ldb_slice he = ldb_buffer_slice(&handle_encoding);
      ldb_block_builder_add(&meta_index_block, &k, &he);
      ldb_buffer_destroy(&key);
      ldb_buffer_destroy(&handle_encoding);
    }
    tb_write_block(b, &meta_index_block, &metaindex_block_handle);
    ldb_block_builder_destroy(&meta_index_block);
  }

  // Write index block
  if (tb_ok(b)) {
    if (b->pending_index_entry) {
      const ldb_comparator* cmp = b->options->comparator;
      cmp->find_short_successor(cmp, &b->last_key);
      ldb_buffer handle_encoding;
      ldb_buffer_init(&handle_encoding);
      ldb_block_handle_encode(&b->pending_handle, &handle_encoding);
      ldb_slice he = ldb_buffer_slice(&handle_encoding);
      ldb_slice last_slice2 = ldb_buffer_slice(&b->last_key);
      ldb_block_builder_add(&b->index_block, &last_slice2, &he);
      ldb_buffer_destroy(&handle_encoding);
      b->pending_index_entry = 0;
    }
    tb_write_block(b, &b->index_block, &index_block_handle);
  }

  // Write footer
  if (tb_ok(b)) {
    ldb_footer footer;
    footer.metaindex_handle = metaindex_block_handle;
    footer.index_handle = index_block_handle;
    ldb_buffer footer_encoding;
    ldb_buffer_init(&footer_encoding);
    ldb_footer_encode(&footer, &footer_encoding);
    ldb_slice fe = ldb_buffer_slice(&footer_encoding);
    b->status = b->file->m->append(b->file, &fe);
    if (ldb_ok(b->status)) {
      b->offset += footer_encoding.size;
    }
    ldb_buffer_destroy(&footer_encoding);
  }
  return ldb_status_copy(b->status);
}

void ldb_table_builder_abandon(ldb_table_builder* b) {
  assert(!b->closed);
  b->closed = 1;
}

uint64_t ldb_table_builder_num_entries(const ldb_table_builder* b) {
  return (uint64_t)b->num_entries;
}

uint64_t ldb_table_builder_file_size(const ldb_table_builder* b) {
  return b->offset;
}

ldb_status ldb_table_builder_status(const ldb_table_builder* b) {
  return ldb_status_copy(b->status);
}

// =================================================================== reader
static void ldb_delete_cached_block(const ldb_slice* key, void* value) {
  (void)key;
  ldb_block_destroy((ldb_block*)value);
}

static void ldb_delete_block_cleanup(void* arg1, void* arg2) {
  (void)arg2;
  ldb_block_destroy((ldb_block*)arg1);
}

static void ldb_release_block_cleanup(void* arg1, void* arg2) {
  ldb_cache* cache = (ldb_cache*)arg1;
  ldb_cache_handle* handle = (ldb_cache_handle*)arg2;
  cache->release(cache, handle);
}

struct ldb_table {
  const ldb_options* options;
  ldb_status status;
  ldb_rand_file* file;
  uint64_t cache_id;
  ldb_filter_block_reader* filter;
  char* filter_data;  // malloc'ed filter block contents (owned)

  ldb_block_handle metaindex_handle;
  ldb_block* index_block;
};

static void ldb_table_read_filter(ldb_table* t, const ldb_slice* handle_value);
static void ldb_table_read_meta(ldb_table* t, const ldb_footer* footer);

ldb_status ldb_table_open(const ldb_options* options, ldb_rand_file* file,
                          uint64_t size, ldb_table** table) {
  *table = NULL;
  if (size < LDB_FOOTER_ENCODED_LENGTH) {
    return ldb_status_corruption("file is too short to be an sstable", NULL);
  }

  char* footer_space = (char*)malloc(LDB_FOOTER_ENCODED_LENGTH);
  ldb_slice footer_input;
  ldb_status s = file->m->read(file, size - LDB_FOOTER_ENCODED_LENGTH,
                            LDB_FOOTER_ENCODED_LENGTH, &footer_input,
                            footer_space);
  if (!ldb_ok(s)) {
    free(footer_space);
    return s;
  }

  ldb_footer footer;
  s = ldb_footer_decode(&footer, &footer_input);
  if (!ldb_ok(s)) {
    free(footer_space);
    return s;
  }

  // Read the index block
  ldb_block_contents index_block_contents;
  ldb_read_options opt;
  ldb_read_options_init(&opt);
  if (options->paranoid_checks) {
    opt.verify_checksums = 1;
  }
  s = ldb_read_block(file, &opt, &footer.index_handle, &index_block_contents);

  if (ldb_ok(s)) {
    // We've successfully read the footer and the index block: we're ready
    // to serve requests.
    ldb_block* index_block = ldb_block_new(&index_block_contents);
    ldb_table* t = (ldb_table*)calloc(1, sizeof(ldb_table));
    t->options = options;
    t->file = file;
    t->metaindex_handle = footer.metaindex_handle;
    t->index_block = index_block;
    t->cache_id =
        (options->block_cache ? options->block_cache->new_id(options->block_cache)
                              : 0);
    t->filter_data = NULL;
    t->filter = NULL;
    *table = t;
    ldb_table_read_meta(t, &footer);
  }
  free(footer_space);
  return s;
}

static void ldb_table_read_meta(ldb_table* t, const ldb_footer* footer) {
  if (t->options->filter_policy == NULL) {
    return;  // Do not need any metadata
  }

  ldb_read_options opt;
  ldb_read_options_init(&opt);
  if (t->options->paranoid_checks) {
    opt.verify_checksums = 1;
  }
  ldb_block_contents contents;
  ldb_status rs = ldb_read_block(t->file, &opt, &footer->metaindex_handle,
                                 &contents);
  if (!ldb_ok(rs)) {
    ldb_status_destroy(&rs);
    return;  // meta info not needed for operation
  }
  ldb_block* meta = ldb_block_new(&contents);

  ldb_iterator* iter = ldb_block_new_iterator(meta, ldb_bytewise_comparator());
  ldb_buffer key;
  ldb_buffer_init(&key);
  ldb_buffer_append_str(&key, "filter.");
  ldb_buffer_append_str(&key, t->options->filter_policy->name(
                                  t->options->filter_policy));
  ldb_slice k = ldb_buffer_slice(&key);
  ldb_iter_seek(iter, &k);
  if (ldb_iter_valid(iter)) {
    ldb_slice iter_key = ldb_iter_key(iter);
    if (ldb_slice_equals(&iter_key, &k)) {
      ldb_slice v = ldb_iter_value(iter);
      ldb_table_read_filter(t, &v);
    }
  }
  ldb_buffer_destroy(&key);
  ldb_iterator_destroy(iter);
  ldb_block_destroy(meta);
}

static void ldb_table_read_filter(ldb_table* t, const ldb_slice* filter_handle_value) {
  ldb_slice v = *filter_handle_value;
  ldb_block_handle filter_handle;
  ldb_status s = ldb_block_handle_decode(&filter_handle, &v);
  if (!ldb_ok(s)) {
    ldb_status_destroy(&s);
    return;
  }

  ldb_read_options opt;
  ldb_read_options_init(&opt);
  if (t->options->paranoid_checks) {
    opt.verify_checksums = 1;
  }
  ldb_block_contents block;
  ldb_status bs = ldb_read_block(t->file, &opt, &filter_handle, &block);
  if (!ldb_ok(bs)) {
    ldb_status_destroy(&bs);
    return;
  }
  if (block.alloc) {
    t->filter_data = block.data.data;  // Will need to delete later
    block.alloc = NULL;
  }
  t->filter = (ldb_filter_block_reader*)malloc(sizeof(ldb_filter_block_reader));
  if (!ldb_filter_block_reader_init(t->filter, t->options->filter_policy,
                                    &block.data)) {
    free(t->filter);
    t->filter = NULL;
  }
  ldb_block_contents_destroy(&block);
}

void ldb_table_destroy(ldb_table* t) {
  if (t->filter) free(t->filter);
  free(t->filter_data);
  if (t->index_block) ldb_block_destroy(t->index_block);
  ldb_status_destroy(&t->status);
  free(t);
}

uint64_t ldb_table_cache_id(const ldb_table* t) { return t->cache_id; }
ldb_rand_file* ldb_table_file(const ldb_table* t) { return t->file; }

// Convert an index iterator value (i.e., an encoded BlockHandle) into an
// iterator over the contents of the corresponding block.
ldb_iterator* ldb_table_block_reader(void* arg, const ldb_read_options* options,
                                     const ldb_slice* index_value) {
  ldb_table* table = (ldb_table*)arg;
  ldb_cache* block_cache = table->options->block_cache;
  ldb_block* block = NULL;
  ldb_cache_handle* cache_handle = NULL;

  ldb_block_handle handle;
  ldb_slice input = *index_value;
  ldb_status s = ldb_block_handle_decode(&handle, &input);
  // We intentionally allow extra stuff in index_value so that we can add
  // more features in the future.

  if (ldb_ok(s)) {
    ldb_block_contents contents;
    if (block_cache != NULL) {
      char cache_key_buffer[16];
      ldb_encode_fixed64(cache_key_buffer, table->cache_id);
      ldb_encode_fixed64(cache_key_buffer + 8, handle.offset);
      ldb_slice key = ldb_slice_make(cache_key_buffer, sizeof(cache_key_buffer));
      cache_handle = block_cache->lookup(block_cache, &key);
      if (cache_handle != NULL) {
        block = (ldb_block*)block_cache->value(block_cache, cache_handle);
      } else {
        s = ldb_read_block(table->file, options, &handle, &contents);
        if (ldb_ok(s)) {
          block = ldb_block_new(&contents);
          if (contents.cachable && options->fill_cache) {
            cache_handle = block_cache->insert(
                block_cache, &key, block, ldb_block_size(block),
                ldb_delete_cached_block);
          }
        }
      }
    } else {
      s = ldb_read_block(table->file, options, &handle, &contents);
      if (ldb_ok(s)) {
        block = ldb_block_new(&contents);
      }
    }
  }

  ldb_iterator* iter;
  if (block != NULL) {
    iter = ldb_block_new_iterator(block, table->options->comparator);
    if (cache_handle == NULL) {
      ldb_iterator_register_cleanup(iter, ldb_delete_block_cleanup, block, NULL);
    } else {
      ldb_iterator_register_cleanup(iter, ldb_release_block_cleanup,
                                    block_cache, cache_handle);
    }
  } else {
    iter = ldb_new_error_iterator(s);
  }
  return iter;
}

ldb_iterator* ldb_table_new_iterator(const ldb_table* t,
                                     const ldb_read_options* options) {
  ldb_iterator* index_iter =
      ldb_block_new_iterator(t->index_block, t->options->comparator);
  return ldb_new_two_level_iterator(index_iter, ldb_table_block_reader,
                                    (void*)t, options);
}

ldb_status ldb_table_internal_get(const ldb_table* t,
                                  const ldb_read_options* options,
                                  const ldb_slice* k, void* arg,
                                  void (*handle_result)(void*, const ldb_slice*,
                                                        const ldb_slice*)) {
  ldb_status s = ldb_status_ok();
  ldb_iterator* iiter = ldb_block_new_iterator(t->index_block, t->options->comparator);
  ldb_iter_seek(iiter, k);
  if (ldb_iter_valid(iiter)) {
    ldb_slice handle_value = ldb_iter_value(iiter);
    ldb_filter_block_reader* filter = t->filter;
    ldb_block_handle handle;
    ldb_slice hv = handle_value;
    ldb_status ds = ldb_status_ok();
    int have_handle = 0;
    if (filter != NULL) {
      ds = ldb_block_handle_decode(&handle, &hv);
      have_handle = ldb_ok(ds);
    }
    if (have_handle &&
        !ldb_filter_block_reader_key_may_match(filter, handle.offset, k)) {
      // Not found
    } else {
      ldb_iterator* block_iter = ldb_table_block_reader((void*)t, options, &handle_value);
      ldb_iter_seek(block_iter, k);
      if (ldb_iter_valid(block_iter)) {
        ldb_slice bk = ldb_iter_key(block_iter);
        ldb_slice bv = ldb_iter_value(block_iter);
        (*handle_result)(arg, &bk, &bv);
      }
      s = ldb_iter_status(block_iter);
      ldb_iterator_destroy(block_iter);
    }
    ldb_status_destroy(&ds);
  }
  if (ldb_ok(s)) {
    ldb_status is = ldb_iter_status(iiter);
    s = is;
  }
  ldb_iterator_destroy(iiter);
  return s;
}

uint64_t ldb_table_approximate_offset_of(const ldb_table* t,
                                         const ldb_slice* key) {
  ldb_iterator* index_iter =
      ldb_block_new_iterator(t->index_block, t->options->comparator);
  ldb_iter_seek(index_iter, key);
  uint64_t result;
  if (ldb_iter_valid(index_iter)) {
    ldb_block_handle handle;
    ldb_slice input = ldb_iter_value(index_iter);
    ldb_status s = ldb_block_handle_decode(&handle, &input);
    if (ldb_ok(s)) {
      result = handle.offset;
    } else {
      result = t->metaindex_handle.offset;
      ldb_status_destroy(&s);
    }
  } else {
    result = t->metaindex_handle.offset;
  }
  ldb_iterator_destroy(index_iter);
  return result;
}
