// log.c - write-ahead log writer and reader
// (mirrors leveldb/db/log_writer.cc and log_reader.cc)
#include "kvdb.h"

#include <assert.h>

// =================================================================== writer
ldb_log_writer* ldb_log_writer_new(ldb_writable_file* dest) {
  return ldb_log_writer_new_at(dest, 0);
}

ldb_log_writer* ldb_log_writer_new_at(ldb_writable_file* dest,
                                      uint64_t dest_length) {
  ldb_log_writer* w = (ldb_log_writer*)malloc(sizeof(ldb_log_writer));
  w->dest = dest;
  w->block_offset = (size_t)(dest_length % LDB_LOG_BLOCK_SIZE);
  return w;
}

void ldb_log_writer_destroy(ldb_log_writer* w) { free(w); }

static ldb_status emit_physical_record(ldb_log_writer* w, int type,
                                       const char* ptr, size_t length) {
  assert(length <= 0xffff);
  assert(w->block_offset + LDB_LOG_HEADER_SIZE + length <= LDB_LOG_BLOCK_SIZE);
  char buf[LDB_LOG_HEADER_SIZE];
  buf[4] = (char)(length & 0xff);
  buf[5] = (char)((length >> 8) & 0xff);
  buf[6] = (char)type;

  // Compute the crc of the record type and the payload.
  uint32_t crc = ldb_crc32c_extend(
      ldb_crc32c_value((const char*)&type, 1), ptr, length);
  crc = ldb_crc32c_mask(crc);
  ldb_encode_fixed32(buf, crc);

  ldb_status s = w->dest->m->append(w->dest, &(ldb_slice){buf, LDB_LOG_HEADER_SIZE});
  if (ldb_ok(s)) {
    ldb_slice payload = ldb_slice_make(ptr, length);
    s = w->dest->m->append(w->dest, &payload);
    if (ldb_ok(s)) {
      s = w->dest->m->flush(w->dest);
    }
    w->block_offset += LDB_LOG_HEADER_SIZE + length;
  }
  return s;
}

ldb_status ldb_log_writer_add_record(ldb_log_writer* w, const ldb_slice* data) {
  const char* ptr = data->data;
  size_t left = data->size;
  ldb_status s = ldb_status_ok();
  int begin = 1;
  if (ptr == NULL && left == 0) {
    ptr = "";  // allow empty records
  }
  do {
    const size_t leftover = LDB_LOG_BLOCK_SIZE - w->block_offset;
    if (leftover < LDB_LOG_HEADER_SIZE) {
      // Fill up the rest of the block with padding.
      if (leftover > 0) {
        static const char padding[LDB_LOG_HEADER_SIZE - 1] = {0};
        ldb_slice pad = ldb_slice_make(padding, leftover);
        assert(leftover < (int)sizeof(padding));
        s = w->dest->m->append(w->dest, &pad);
      }
      w->block_offset = 0;
    }

    // Invariant: we never leave < kHeaderSize bytes in a block.
    const size_t avail =
        LDB_LOG_BLOCK_SIZE - w->block_offset - LDB_LOG_HEADER_SIZE;
    const size_t fragment_length = (left < avail) ? left : avail;

    int type;
    const int end = (left == fragment_length);
    if (begin && end) {
      type = LDB_LOG_FULL_TYPE;
    } else if (begin) {
      type = LDB_LOG_FIRST_TYPE;
    } else if (end) {
      type = LDB_LOG_LAST_TYPE;
    } else {
      type = LDB_LOG_MIDDLE_TYPE;
    }

    s = emit_physical_record(w, type, ptr, fragment_length);
    ptr += fragment_length;
    left -= fragment_length;
    begin = 0;
  } while (ldb_ok(s) && left > 0);
  return s;
}

// =================================================================== reader
enum {
  LOG_EOF = 5,             // EOF indicator (internal)
  LOG_BAD_RECORD = 6,      // record with bad length / crc (internal)
};

typedef struct ldb_log_reader {
  ldb_seq_file* file;
  void* reporter;
  ldb_log_corruption_cb corruption_cb;
  int checksum;
  uint64_t initial_offset;

  ldb_buffer buffer;         // scratch for the current block
  int eof;
  int resyncing;  // set when initial_offset>0; silently skip MIDDLE/LAST
  uint64_t last_record_offset;
  uint64_t end_of_buffer_offset;
  size_t pos;  // parse position within buffer (no memmove consumption)
} ldb_log_reader;

ldb_log_reader* ldb_log_reader_new(ldb_seq_file* file, void* reporter,
                                   ldb_log_corruption_cb corruption_cb,
                                   int checksum, uint64_t initial_offset) {
  ldb_log_reader* r = (ldb_log_reader*)malloc(sizeof(ldb_log_reader));
  r->file = file;
  r->reporter = reporter;
  r->corruption_cb = corruption_cb;
  r->checksum = checksum;
  r->initial_offset = initial_offset;
  r->eof = 0;
  r->resyncing = (initial_offset > 0);
  r->last_record_offset = 0;
  r->end_of_buffer_offset = 0;
  r->pos = 0;
  ldb_buffer_init(&r->buffer);
  ldb_buffer_resize(&r->buffer, LDB_LOG_BLOCK_SIZE);
  r->buffer.size = 0;
  return r;
}

void ldb_log_reader_destroy(ldb_log_reader* r) {
  ldb_buffer_destroy(&r->buffer);
  free(r);
}

static void report_drop(ldb_log_reader* r, uint64_t bytes,
                        const ldb_status* reason) {
  // Like leveldb: only report corruption at or after initial_offset.
  if (r->corruption_cb != NULL && bytes > 0 &&
      r->end_of_buffer_offset - (r->buffer.size - r->pos) - bytes >=
          r->initial_offset) {
    r->corruption_cb(r->reporter, bytes, ldb_status_copy(*reason));
  }
}

static void report_corruption(ldb_log_reader* r, uint64_t bytes,
                              const char* reason) {
  ldb_status s = ldb_status_corruption(reason, NULL);
  report_drop(r, bytes, &s);
  ldb_status_destroy(&s);
}

static int skip_to_initial_block(ldb_log_reader* r) {
  const uint64_t target = r->initial_offset;
  const uint64_t block_start_location =
      (target / LDB_LOG_BLOCK_SIZE) * LDB_LOG_BLOCK_SIZE;
  const uint64_t offset_in_block = target - block_start_location;
  uint64_t block_start_location_ret = block_start_location;

  if (offset_in_block > LDB_LOG_BLOCK_SIZE - 6) {
    block_start_location_ret += LDB_LOG_BLOCK_SIZE;
  }
  r->end_of_buffer_offset = block_start_location_ret;

  // Seek the underlying file to the computed block start; the sequential
  // reads below must start there for the offset bookkeeping to line up.
  if (block_start_location_ret > 0) {
    ldb_status s = r->file->m->skip(r->file, block_start_location_ret);
    if (!ldb_ok(s)) {
      report_drop(r, block_start_location_ret, &s);
      ldb_status_destroy(&s);
      return 0;
    }
  }
  return 1;
}

// Reads the next physical record; returns its type, LOG_EOF, or LOG_BAD_RECORD.
// NOTE: like leveldb, the buffer is consumed by ADVANCING a position (no
// memmove), so returned slices point into stable buffer storage until the
// next block read.
static int read_physical_record(ldb_log_reader* r, ldb_slice* result,
                                size_t* drop_size_out) {
  *drop_size_out = 0;
  while (1) {
    size_t remaining = r->buffer.size - r->pos;
    if (remaining < LDB_LOG_HEADER_SIZE) {
      if (!r->eof) {
        // Last read was a full read, so this is a trailer to skip
        ldb_buffer_clear(&r->buffer);
        ldb_buffer_resize(&r->buffer, LDB_LOG_BLOCK_SIZE);
        ldb_slice fragment;
        ldb_status s = r->file->m->read(r->file, LDB_LOG_BLOCK_SIZE, &fragment,
                                        r->buffer.data);
        r->buffer.size = fragment.size;
        r->pos = 0;
        r->end_of_buffer_offset += fragment.size;
        if (!ldb_ok(s)) {
          ldb_buffer_clear(&r->buffer);
          report_drop(r, LDB_LOG_BLOCK_SIZE, &s);
          ldb_status_destroy(&s);
          r->eof = 1;
          return LOG_EOF;
        } else if (r->buffer.size < LDB_LOG_BLOCK_SIZE) {
          r->eof = 1;
        }
        continue;
      } else {
        // If the file ended without a full header, we're done.
        ldb_buffer_clear(&r->buffer);
        r->pos = 0;
        return LOG_EOF;
      }
    }

    // Parse the header
    const char* header = r->buffer.data + r->pos;
    const uint32_t a = (uint32_t)(unsigned char)header[4];
    const uint32_t b = (uint32_t)(unsigned char)header[5];
    const unsigned int type = (unsigned char)header[6];
    const uint32_t length = a | (b << 8);
    if (LDB_LOG_HEADER_SIZE + length > remaining) {
      size_t drop_size = remaining;
      ldb_buffer_clear(&r->buffer);
      r->pos = 0;
      if (!r->eof) {
        report_corruption(r, drop_size, "bad record length");
        *drop_size_out = drop_size;
        return LOG_BAD_RECORD;
      }
      // If the end of the file has been reached without reading |length|
      // bytes of payload, assume the writer died in the middle of writing
      // the record.  Don't report a corruption.
      return LOG_EOF;
    }

    // Skip zero length records without checking any CRCs
    if (type == LDB_LOG_ZERO_TYPE && length == 0) {
      // Such records are produced by mmap-based writing code that
      // preallocates file regions; drop the rest of the block.
      ldb_buffer_clear(&r->buffer);
      r->pos = 0;
      return LOG_BAD_RECORD;
    }

    // Check crc
    if (r->checksum) {
      uint32_t expected_crc =
          ldb_crc32c_unmask(ldb_decode_fixed32(header));
      uint32_t actual_crc = ldb_crc32c_value(header + 6, 1 + length);
      if (actual_crc != expected_crc) {
        size_t drop_size = remaining;
        ldb_buffer_clear(&r->buffer);
        r->pos = 0;
        report_corruption(r, drop_size, "checksum mismatch");
        *drop_size_out = drop_size;
        return LOG_BAD_RECORD;
      }
    }

    r->pos += LDB_LOG_HEADER_SIZE + length;

    // Skip physical record that started before initial_offset_
    if (r->end_of_buffer_offset - (r->buffer.size - r->pos) -
            LDB_LOG_HEADER_SIZE -
            length <
        r->initial_offset) {
      result->size = 0;
      return LOG_BAD_RECORD;
    }

    *result = ldb_slice_make(header + LDB_LOG_HEADER_SIZE, length);
    return (int)type;
  }
}

int ldb_log_reader_read_record(ldb_log_reader* r, ldb_slice* record,
                               ldb_buffer* scratch) {
  if (r->last_record_offset < r->initial_offset) {
    if (!skip_to_initial_block(r)) {
      return 0;
    }
  }

  ldb_buffer_clear(scratch);
  *record = ldb_slice_make(NULL, 0);
  int in_fragmented_record = 0;
  // File offset of the logical record currently being assembled.
  uint64_t prospective_record_offset = 0;

  while (1) {
    // Empty by default: read_physical_record leaves it untouched on the
    // bad-length / CRC / EOF paths (official default-constructs its Slice).
    ldb_slice fragment = ldb_slice_make(NULL, 0);
    size_t drop_size;
    int type = read_physical_record(r, &fragment, &drop_size);
    // Offset of the physical record just returned. r->pos has already
    // advanced past its header+payload, so the remaining byte count
    // (buffer.size - pos) recovers the record's start position.
    uint64_t physical_record_offset =
        r->end_of_buffer_offset - (r->buffer.size - r->pos) -
        LDB_LOG_HEADER_SIZE - fragment.size;

    if (r->resyncing) {
      if (type == LDB_LOG_MIDDLE_TYPE) {
        continue;
      } else if (type == LDB_LOG_LAST_TYPE) {
        r->resyncing = 0;
        continue;
      } else {
        r->resyncing = 0;
      }
    }

    switch (type) {
      case LDB_LOG_FULL_TYPE:
        if (in_fragmented_record) {
          // Handle bug in earlier versions of log::Writer where
          // it could emit an empty kFirstType record at the tail end
          // of a block followed by a kFullType or kFirstType record
          // at the beginning of the next block.
          if (scratch->size != 0) {
            report_corruption(r, scratch->size,
                              "partial record without end(1)");
          }
        }
        prospective_record_offset = physical_record_offset;
        ldb_buffer_clear(scratch);
        *record = fragment;
        r->last_record_offset = prospective_record_offset;
        return 1;

      case LDB_LOG_FIRST_TYPE:
        if (in_fragmented_record) {
          if (scratch->size != 0) {
            report_corruption(r, scratch->size,
                              "partial record without end(2)");
          }
        }
        prospective_record_offset = physical_record_offset;
        ldb_buffer_clear(scratch);
        ldb_buffer_append_slice(scratch, &fragment);
        in_fragmented_record = 1;
        break;

      case LDB_LOG_MIDDLE_TYPE:
        if (!in_fragmented_record) {
          report_corruption(r, fragment.size,
                            "missing start of fragmented record(1)");
        } else {
          ldb_buffer_append_slice(scratch, &fragment);
        }
        break;

      case LDB_LOG_LAST_TYPE:
        if (!in_fragmented_record) {
          report_corruption(r, fragment.size,
                            "missing start of fragmented record(2)");
        } else {
          ldb_buffer_append_slice(scratch, &fragment);
          *record = ldb_slice_make(scratch->data, scratch->size);
          r->last_record_offset = prospective_record_offset;
          return 1;
        }
        break;

      case LOG_BAD_RECORD:
        if (in_fragmented_record) {
          report_corruption(r, scratch->size, "error in middle of record");
          in_fragmented_record = 0;
          ldb_buffer_clear(scratch);
        }
        (void)drop_size;
        break;

      case LOG_EOF:
        if (in_fragmented_record) {
          // The writer died immediately after writing a physical record
          // but before completing the next; ignore the logical record
          // without reporting corruption (matches leveldb).
          ldb_buffer_clear(scratch);
        }
        return 0;

      default:
        report_corruption(r, (fragment.size + (in_fragmented_record ? scratch->size : 0)),
                          "unknown record type");
        in_fragmented_record = 0;
        ldb_buffer_clear(scratch);
        break;
    }
  }
}

uint64_t ldb_log_reader_last_record_offset(const ldb_log_reader* r) {
  return r->last_record_offset;
}
