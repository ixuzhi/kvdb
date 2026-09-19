// block.c - block handle/footer formats, block builder, and block reader
// (mirrors leveldb/table/format.cc, block_builder.cc, block.cc)
#include "kvdb.h"

#include <assert.h>

// =================================================================== block handle / footer
void ldb_block_handle_init(ldb_block_handle* h) {
  h->offset = ~(uint64_t)0;
  h->size = ~(uint64_t)0;
}

void ldb_block_handle_encode(const ldb_block_handle* h, ldb_buffer* dst) {
  assert(h->offset != ~(uint64_t)0);
  assert(h->size != ~(uint64_t)0);
  ldb_put_varint64(dst, h->offset);
  ldb_put_varint64(dst, h->size);
}

ldb_status ldb_block_handle_decode(ldb_block_handle* h, ldb_slice* input) {
  const char* p = input->data;
  const char* limit = input->data + input->size;
  uint64_t offset, size;
  if (ldb_get_varint64(&p, limit, &offset) &&
      ldb_get_varint64(&p, limit, &size)) {
    h->offset = offset;
    h->size = size;
    // Advance the input past the consumed handle (mirrors leveldb).
    input->data = p;
    input->size = (size_t)(limit - p);
    return ldb_status_ok();
  }
  return ldb_status_corruption("bad block handle", NULL);
}

void ldb_footer_encode(const ldb_footer* f, ldb_buffer* dst) {
  const size_t original_size = dst->size;
  ldb_block_handle_encode(&f->metaindex_handle, dst);
  ldb_block_handle_encode(&f->index_handle, dst);
  // Pad to 2 * BlockHandle::kMaxEncodedLength
  ldb_buffer_resize(dst, original_size + 2 * 10 + 2 * 10);
  ldb_put_fixed32(dst, (uint32_t)(LDB_TABLE_MAGIC_NUMBER & 0xffffffffu));
  ldb_put_fixed32(dst, (uint32_t)(LDB_TABLE_MAGIC_NUMBER >> 32));
  assert(dst->size == original_size + LDB_FOOTER_ENCODED_LENGTH);
}

ldb_status ldb_footer_decode(ldb_footer* f, ldb_slice* input) {
  if (input->size < LDB_FOOTER_ENCODED_LENGTH) {
    return ldb_status_corruption("not an sstable (footer too short)", NULL);
  }
  const char* magic_ptr = input->data + LDB_FOOTER_ENCODED_LENGTH - 8;
  const uint32_t magic_lo = ldb_decode_fixed32(magic_ptr);
  const uint32_t magic_hi = ldb_decode_fixed32(magic_ptr + 4);
  const uint64_t magic = ((uint64_t)magic_hi << 32) | magic_lo;
  if (magic != LDB_TABLE_MAGIC_NUMBER) {
    return ldb_status_corruption("not an sstable (bad magic number)", NULL);
  }
  const char* p = input->data;
  const char* limit = input->data + input->size;
  ldb_slice islice = ldb_slice_make(p, (size_t)(magic_ptr + 8 - p));
  ldb_slice s = islice;
  ldb_status result = ldb_block_handle_decode(&f->metaindex_handle, &s);
  if (ldb_ok(result)) {
    result = ldb_block_handle_decode(&f->index_handle, &s);
  }
  (void)limit;
  return result;
}

void ldb_block_contents_destroy(ldb_block_contents* c) {
  free(c->alloc);
  c->alloc = NULL;
}

ldb_status ldb_read_block(ldb_rand_file* file, const ldb_read_options* options,
                          const ldb_block_handle* handle,
                          ldb_block_contents* result) {
  result->data = ldb_slice_make(NULL, 0);
  result->alloc = NULL;
  result->cachable = 0;

  size_t n = (size_t)handle->size;
  char* buf = (char*)malloc(n + LDB_BLOCK_TRAILER_SIZE);
  if (!buf) return ldb_status_io_error("out of memory", NULL);
  ldb_slice contents;
  ldb_status s = file->m->read(file, handle->offset, n + LDB_BLOCK_TRAILER_SIZE,
                            &contents, buf);
  if (!ldb_ok(s)) {
    free(buf);
    return s;
  }
  if (contents.size != n + LDB_BLOCK_TRAILER_SIZE) {
    free(buf);
    return ldb_status_corruption("truncated block read", NULL);
  }

  const char* data = contents.data;
  if (options->verify_checksums) {
    const uint32_t crc = ldb_crc32c_unmask(
        ldb_decode_fixed32(data + n + 1));
    const uint32_t actual = ldb_crc32c_value(data, n + 1);
    if (actual != crc) {
      free(buf);
      return ldb_status_corruption("block checksum mismatch", NULL);
    }
  }

  switch (data[n]) {
    case LDB_NO_COMPRESSION: {
      result->data = ldb_slice_make(data, n);
      result->alloc = buf;
      result->cachable = 1;
      break;
    }
    case LDB_SNAPPY_COMPRESSION: {
      size_t ulength = 0;
      if (!ldb_snappy_get_uncompressed_length(data, n, &ulength)) {
        free(buf);
        return ldb_status_corruption(
            "corrupted snappy compressed block length", NULL);
      }
      char* ubuf = (char*)malloc(ulength);
      if (!ubuf) {
        free(buf);
        return ldb_status_io_error("out of memory", NULL);
      }
      if (!ldb_snappy_uncompress(data, n, ubuf, ulength)) {
        free(buf);
        free(ubuf);
        return ldb_status_corruption("corrupted snappy compressed block contents",
                                     NULL);
      }
      free(buf);
      result->data = ldb_slice_make(ubuf, ulength);
      result->alloc = ubuf;
      result->cachable = 1;
      break;
    }
    default:
      free(buf);
      return ldb_status_corruption("bad block type", NULL);
  }

  return ldb_status_ok();
}

// =================================================================== block builder
void ldb_block_builder_init(ldb_block_builder* b, const ldb_options* options) {
  assert(options->block_restart_interval >= 1);
  memset(b, 0, sizeof(*b));
  b->options = options;
  ldb_buffer_init(&b->buffer);
  ldb_buffer_init(&b->last_key);
  b->restarts_cap = 8;
  b->restarts = (uint32_t*)malloc(sizeof(uint32_t) * b->restarts_cap);
  assert(b->restarts);
  b->restarts_count = 1;
  b->restarts[0] = 0;  // First restart point is at offset 0
  b->counter = 0;
  b->finished = 0;
}

void ldb_block_builder_destroy(ldb_block_builder* b) {
  ldb_buffer_destroy(&b->buffer);
  ldb_buffer_destroy(&b->last_key);
  free(b->restarts);
}

void ldb_block_builder_reset(ldb_block_builder* b) {
  ldb_buffer_clear(&b->buffer);
  ldb_buffer_clear(&b->last_key);
  b->restarts_count = 1;
  b->restarts[0] = 0;
  b->counter = 0;
  b->finished = 0;
}

void ldb_block_builder_add(ldb_block_builder* b, const ldb_slice* key,
                           const ldb_slice* value) {
  const ldb_comparator* comparator = b->options->comparator;
  ldb_slice last_key_piece = ldb_buffer_slice(&b->last_key);
  assert(!b->finished);
  assert(b->counter <= b->options->block_restart_interval);
  assert(ldb_buffer_empty(&b->buffer) ||
         comparator->compare(comparator, key, &last_key_piece) > 0);
  size_t shared = 0;
  if (b->counter < b->options->block_restart_interval) {
    // See how much sharing to do with previous string
    size_t min_length =
        last_key_piece.size < key->size ? last_key_piece.size : key->size;
    while (shared < min_length && last_key_piece.data[shared] == key->data[shared]) {
      shared++;
    }
  } else {
    // Restart compression
    if (b->restarts_count == b->restarts_cap) {
      b->restarts_cap *= 2;
      b->restarts = (uint32_t*)realloc(
          b->restarts, sizeof(uint32_t) * b->restarts_cap);
      assert(b->restarts);
    }
    b->restarts[b->restarts_count++] = (uint32_t)b->buffer.size;
    b->counter = 0;
  }
  const size_t non_shared = key->size - shared;

  // Add "<shared><non_shared><value_size>" to buffer
  ldb_put_varint32(&b->buffer, (uint32_t)shared);
  ldb_put_varint32(&b->buffer, (uint32_t)non_shared);
  ldb_put_varint32(&b->buffer, (uint32_t)value->size);

  // Add string delta to buffer followed by value
  ldb_buffer_append(&b->buffer, key->data + shared, non_shared);
  ldb_buffer_append(&b->buffer, value->data, value->size);

  // Update state
  ldb_buffer_resize(&b->last_key, shared);
  ldb_buffer_append(&b->last_key, key->data + shared, non_shared);
  b->counter++;
}

ldb_slice ldb_block_builder_finish(ldb_block_builder* b) {
  // Append restart array
  for (size_t i = 0; i < b->restarts_count; i++) {
    ldb_put_fixed32(&b->buffer, b->restarts[i]);
  }
  ldb_put_fixed32(&b->buffer, (uint32_t)b->restarts_count);
  b->finished = 1;
  return ldb_buffer_slice(&b->buffer);
}

size_t ldb_block_builder_current_size_estimate(const ldb_block_builder* b) {
  return b->buffer.size + b->restarts_count * sizeof(uint32_t) +
         sizeof(uint32_t);
}

int ldb_block_builder_empty(const ldb_block_builder* b) {
  return b->buffer.size == 0;
}

// =================================================================== block reader
struct ldb_block {
  const char* data;
  size_t size;  // 0 => error marker
  char* owned;  // malloc'ed data if heap_allocated
  size_t restart_offset;  // offset in data of restart array
};

ldb_block* ldb_block_new(ldb_block_contents* contents) {
  ldb_block* b = (ldb_block*)malloc(sizeof(ldb_block));
  b->data = contents->data.data;
  b->size = contents->data.size;
  b->owned = contents->alloc;
  contents->alloc = NULL;  // ownership moved
  if (b->size < sizeof(uint32_t)) {
    b->size = 0;  // Error marker
  } else {
    uint32_t num_restarts = ldb_decode_fixed32(b->data + b->size - sizeof(uint32_t));
    size_t max_restarts_allowed = (b->size - sizeof(uint32_t)) / sizeof(uint32_t);
    if (num_restarts > max_restarts_allowed) {
      b->size = 0;
    } else {
      b->restart_offset =
          b->size - (1 + num_restarts) * sizeof(uint32_t);
    }
  }
  return b;
}

void ldb_block_destroy(ldb_block* b) {
  free(b->owned);
  free(b);
}

size_t ldb_block_size(const ldb_block* b) { return b->size; }

// ------------------------------------------------------------------ block iterator
typedef struct block_iter {
  ldb_iterator base;
  const ldb_comparator* comparator;
  const char* data;
  uint32_t restarts;      // offset of restart array
  uint32_t num_restarts;  // number of uint32 entries in restart array
  uint32_t current;       // offset of current entry; >= restarts if !valid
  uint32_t restart_index;
  ldb_buffer key;
  ldb_slice value;
  int has_error;
} block_iter;

static inline int bi_compare(const block_iter* bi, const ldb_slice* a,
                             const ldb_slice* b) {
  return ldb_cmp(bi->comparator, a, b);
}

static inline uint32_t bi_next_entry_offset(const block_iter* bi) {
  return (uint32_t)((bi->value.data + bi->value.size) - bi->data);
}

static uint32_t bi_get_restart_point(const block_iter* bi, uint32_t index) {
  assert(index < bi->num_restarts);
  return ldb_decode_fixed32(bi->data + bi->restarts + index * sizeof(uint32_t));
}

static void bi_seek_to_restart_point(block_iter* bi, uint32_t index) {
  ldb_buffer_clear(&bi->key);
  bi->restart_index = index;
  // ParseNextKey() starts at the end of value_, so set value_ accordingly
  uint32_t offset = bi_get_restart_point(bi, index);
  bi->value = ldb_slice_make(bi->data + offset, 0);
}

// Decode the entry starting at p; returns pointer to key delta or NULL.
static const char* decode_entry(const char* p, const char* limit,
                                uint32_t* shared, uint32_t* non_shared,
                                uint32_t* value_length) {
  if (limit - p < 3) return NULL;
  *shared = (uint8_t)p[0];
  *non_shared = (uint8_t)p[1];
  *value_length = (uint8_t)p[2];
  if ((*shared | *non_shared | *value_length) < 128) {
    // Fast path: all three values are encoded in one byte each
    p += 3;
  } else {
    if (!ldb_get_varint32(&p, limit, shared)) return NULL;
    if (!ldb_get_varint32(&p, limit, non_shared)) return NULL;
    if (!ldb_get_varint32(&p, limit, value_length)) return NULL;
  }
  if ((uint32_t)(limit - p) < (*non_shared + *value_length)) {
    return NULL;
  }
  return p;
}

static void bi_corruption_error(block_iter* bi) {
  bi->current = bi->restarts;
  bi->restart_index = bi->num_restarts;
  // Store error in key buffer? we need a status; use key clearing + flag.
  // We follow leveldb: status_ = Corruption("bad entry in block")
  bi->value = ldb_slice_make(NULL, 0);
  ldb_buffer_clear(&bi->key);
  bi->has_error = 1;
}

static int bi_parse_next_key(block_iter* bi) {
  bi->current = bi_next_entry_offset(bi);
  const char* p = bi->data + bi->current;
  const char* limit = bi->data + bi->restarts;  // Restarts come right after data
  if (p >= limit) {
    // No more entries to return. Mark as invalid.
    bi->current = bi->restarts;
    bi->restart_index = bi->num_restarts;
    return 0;
  }
  // Decode next entry
  uint32_t shared, non_shared, value_length;
  p = decode_entry(p, limit, &shared, &non_shared, &value_length);
  if (p == NULL || bi->key.size < shared) {
    bi_corruption_error(bi);
    return 0;
  } else {
    ldb_buffer_resize(&bi->key, shared);
    ldb_buffer_append(&bi->key, p, non_shared);
    bi->value = ldb_slice_make(p + non_shared, value_length);
    while (bi->restart_index + 1 < bi->num_restarts &&
           bi_get_restart_point(bi, bi->restart_index + 1) < bi->current) {
      bi->restart_index++;
    }
    return 1;
  }
}

static void bi_destroy(ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  ldb_buffer_destroy(&bi->key);
  free(bi);
}

static int bi_valid(const ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  return bi->current < bi->restarts;
}

static void bi_next(ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  assert(bi_valid(it));
  bi_parse_next_key(bi);
}

static void bi_prev(ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  assert(bi_valid(it));
  // Scan backwards to a restart point before current_
  const uint32_t original = bi->current;
  while (bi_get_restart_point(bi, bi->restart_index) >= original) {
    if (bi->restart_index == 0) {
      // No more entries
      bi->current = bi->restarts;
      bi->restart_index = bi->num_restarts;
      return;
    }
    bi->restart_index--;
  }
  bi_seek_to_restart_point(bi, bi->restart_index);
  do {
    // Loop until end of current entry hits the start of original entry
  } while (bi_parse_next_key(bi) && bi_next_entry_offset(bi) < original);
}

static void bi_seek(ldb_iterator* it, const ldb_slice* target) {
  block_iter* bi = (block_iter*)it;
  // Binary search in restart array to find the last restart point
  // with a key < target
  uint32_t left = 0;
  uint32_t right = bi->num_restarts - 1;
  int current_key_compare = 0;

  if (bi_valid(it)) {
    // If we're already scanning, use the current position as a starting
    // point.
    ldb_slice ck = ldb_buffer_slice(&bi->key);
    current_key_compare = bi_compare(bi, &ck, target);
    if (current_key_compare < 0) {
      left = bi->restart_index;
    } else if (current_key_compare > 0) {
      right = bi->restart_index;
    } else {
      // We're seeking to the key we're already at.
      return;
    }
  }

  while (left < right) {
    uint32_t mid = (left + right + 1) / 2;
    uint32_t region_offset = bi_get_restart_point(bi, mid);
    uint32_t shared, non_shared, value_length;
    const char* key_ptr = decode_entry(bi->data + region_offset,
                                       bi->data + bi->restarts, &shared,
                                       &non_shared, &value_length);
    if (key_ptr == NULL || shared != 0) {
      bi_corruption_error(bi);
      return;
    }
    ldb_slice mid_key = ldb_slice_make(key_ptr, non_shared);
    if (bi_compare(bi, &mid_key, target) < 0) {
      // Key at "mid" is smaller than "target". Therefore all blocks before
      // "mid" are uninteresting.
      left = mid;
    } else {
      // Key at "mid" is >= "target". Therefore all blocks at or after "mid"
      // are uninteresting.
      right = mid - 1;
    }
  }

  assert(current_key_compare == 0 || bi_valid(it));
  int skip_seek = (left == bi->restart_index) && (current_key_compare < 0);
  if (!skip_seek) {
    bi_seek_to_restart_point(bi, left);
  }
  // Linear search (within restart block) for first key >= target
  while (1) {
    if (!bi_parse_next_key(bi)) {
      return;
    }
    ldb_slice ck = ldb_buffer_slice(&bi->key);
    if (bi_compare(bi, &ck, target) >= 0) {
      return;
    }
  }
}

static void bi_seek_to_first(ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  bi_seek_to_restart_point(bi, 0);
  bi_parse_next_key(bi);
}

static void bi_seek_to_last(ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  bi_seek_to_restart_point(bi, bi->num_restarts - 1);
  while (bi_parse_next_key(bi) && bi_next_entry_offset(bi) < bi->restarts) {
    // Keep skipping
  }
}

static ldb_slice bi_key(const ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  return ldb_buffer_slice(&bi->key);
}

static ldb_slice bi_value(const ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  return bi->value;
}

static ldb_status bi_status(const ldb_iterator* it) {
  block_iter* bi = (block_iter*)it;
  if (bi->has_error) {
    return ldb_status_corruption("bad entry in block", NULL);
  }
  return ldb_status_ok();
}

static const ldb_iterator_methods g_block_iter_methods = {
    bi_destroy,     bi_valid,      bi_seek_to_first, bi_seek_to_last,
    bi_seek,        bi_next,       bi_prev,          bi_key,
    bi_value,       bi_status};

ldb_iterator* ldb_block_new_iterator(const ldb_block* b,
                                     const ldb_comparator* comparator) {
  if (b->size < sizeof(uint32_t)) {
    return ldb_new_error_iterator(
        ldb_status_corruption("bad block contents", NULL));
  }
  uint32_t num_restarts =
      ldb_decode_fixed32(b->data + b->size - sizeof(uint32_t));
  if (num_restarts == 0) {
    return ldb_new_empty_iterator();
  }
  block_iter* bi = (block_iter*)calloc(1, sizeof(block_iter));
  bi->base.m = &g_block_iter_methods;
  bi->base.impl = bi;
  bi->base.cleanups = NULL;
  bi->comparator = comparator;
  bi->data = b->data;
  bi->restarts = (uint32_t)b->restart_offset;
  bi->num_restarts = num_restarts;
  bi->current = bi->restarts;
  bi->restart_index = num_restarts;
  ldb_buffer_init(&bi->key);
  return &bi->base;
}
