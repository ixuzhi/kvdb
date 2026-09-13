// filter_block.c - filter block builder/reader for sstables
// (mirrors leveldb/table/filter_block.cc)
#include "kvdb.h"

#include <assert.h>

#define LDB_FILTER_BASE_LG 11
#define LDB_FILTER_BASE (1 << LDB_FILTER_BASE_LG)

// =================================================================== builder
void ldb_filter_block_builder_init(ldb_filter_block_builder* b,
                                   const ldb_filterpolicy* policy) {
  memset(b, 0, sizeof(*b));
  b->policy = policy;
  b->base_lg = LDB_FILTER_BASE_LG;
  ldb_buffer_init(&b->result);
  ldb_buffer_init(&b->keys_data);  // flattened key contents
}

static void fbb_push_offset(ldb_filter_block_builder* b, uint32_t offset) {
  if (b->filter_offsets_count == b->filter_offsets_cap) {
    b->filter_offsets_cap =
        b->filter_offsets_cap ? b->filter_offsets_cap * 2 : 8;
    b->filter_offsets = (uint32_t*)realloc(
        b->filter_offsets, sizeof(uint32_t) * b->filter_offsets_cap);
    assert(b->filter_offsets);
  }
  b->filter_offsets[b->filter_offsets_count++] = offset;
}

static void fbb_generate_filter(ldb_filter_block_builder* b) {
  const size_t num_keys = b->start_count;
  if (num_keys == 0) {
    // Fast path if there are no keys for this filter
    fbb_push_offset(b, (uint32_t)b->result.size);
    return;
  }

  // Make list of keys from flattened key structure
  ldb_slice* tmp_keys = (ldb_slice*)malloc(sizeof(ldb_slice) * num_keys);
  assert(tmp_keys);
  for (size_t i = 0; i < num_keys; i++) {
    tmp_keys[i] = ldb_slice_make(b->keys_data.data + b->start[i],
                                 (i + 1 < num_keys ? b->start[i + 1]
                                                   : b->keys_data.size) -
                                     b->start[i]);
  }

  // Generate filter for current set of keys and append to result_.
  fbb_push_offset(b, (uint32_t)b->result.size);
  b->policy->create_filter(b->policy, tmp_keys, (int)num_keys, &b->result);

  free(tmp_keys);
  ldb_buffer_clear(&b->keys_data);
  b->start_count = 0;
}

void ldb_filter_block_builder_start_block(ldb_filter_block_builder* b,
                                          uint64_t block_offset) {
  uint64_t filter_index = (block_offset / LDB_FILTER_BASE);
  assert(filter_index >= b->filter_offsets_count);
  // Generate filters for the wasted space between blocks
  while (filter_index > b->filter_offsets_count) {
    fbb_generate_filter(b);
  }
}

void ldb_filter_block_builder_add_key(ldb_filter_block_builder* b,
                                      const ldb_slice* key) {
  if (b->start_count == b->start_cap) {
    b->start_cap = b->start_cap ? b->start_cap * 2 : 16;
    b->start = (size_t*)realloc(b->start, sizeof(size_t) * b->start_cap);
    assert(b->start);
  }
  b->start[b->start_count++] = b->keys_data.size;
  ldb_buffer_append_slice(&b->keys_data, key);
}

ldb_slice ldb_filter_block_builder_finish(ldb_filter_block_builder* b) {
  if (b->start_count > 0) {
    fbb_generate_filter(b);
  }
  // Append array of per-filter offsets
  const uint32_t array_offset = (uint32_t)b->result.size;
  for (size_t i = 0; i < b->filter_offsets_count; i++) {
    ldb_put_fixed32(&b->result, b->filter_offsets[i]);
  }
  ldb_put_fixed32(&b->result, array_offset);
  // Save encoding parameter as the very last byte
  ldb_buffer_append(&b->result, &b->base_lg, 1);
  return ldb_buffer_slice(&b->result);
}

void ldb_filter_block_builder_destroy(ldb_filter_block_builder* b) {
  ldb_buffer_destroy(&b->result);
  ldb_buffer_destroy(&b->keys_data);
  free(b->start);
  free(b->filter_offsets);
}

// =================================================================== reader
int ldb_filter_block_reader_init(ldb_filter_block_reader* r,
                                 const ldb_filterpolicy* policy,
                                 const ldb_slice* contents) {
  memset(r, 0, sizeof(*r));
  r->policy = policy;
  size_t size = contents->size;
  const char* data = contents->data;
  if (size < 5) {
    return 0;  // invalid; all KeyMayMatch calls return true
  }
  const uint32_t last_word = ldb_decode_fixed32(data + size - 5);
  if (last_word > size - 5) {
    return 0;
  }
  r->data = data;
  const size_t offset_lg = (unsigned char)data[size - 1];
  if (offset_lg < 1 || offset_lg > 30) {
    return 0;
  }
  r->base_lg = offset_lg;
  r->offset_of_offsets = last_word;
  r->num = (size - 5 - last_word) / 4;
  return 1;
}

// Return the filter at "index" or NULL if index out of bounds.
static ldb_slice get_filter(const ldb_filter_block_reader* r, size_t index) {
  if (index >= r->num) {
    return ldb_slice_make(NULL, 0);
  }
  const char* offsets = r->data + r->offset_of_offsets;
  uint32_t start = ldb_decode_fixed32(offsets + index * 4);
  uint32_t limit;
  if (index + 1 < r->num) {
    limit = ldb_decode_fixed32(offsets + (index + 1) * 4);
  } else {
    // The last filter is followed by the offset array
    limit = (uint32_t)r->offset_of_offsets;
  }
  if (limit < start) {
    return ldb_slice_make(NULL, 0);
  }
  return ldb_slice_make(r->data + start, limit - start);
}

int ldb_filter_block_reader_key_may_match(const ldb_filter_block_reader* r,
                                          uint64_t block_offset,
                                          const ldb_slice* key) {
  if (r->data == NULL) {
    return 1;  // Invalid filter: match all
  }
  const uint64_t index = block_offset >> r->base_lg;
  if (index < r->num) {
    ldb_slice filter = get_filter(r, (size_t)index);
    return r->policy->key_may_match(r->policy, key, &filter);
  }
  return 1;  // Filters data never given: match all
}
