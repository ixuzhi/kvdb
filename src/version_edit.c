// version_edit.c - VersionEdit encode/decode and FileMetaData
// (mirrors leveldb/db/version_edit.cc)
#include "kvdb.h"

#include <assert.h>

// Tag numbers for serialized VersionEdit. These numbers are exposed to
// other tools and must not change.
enum {
  VE_TAG_COMPARATOR = 1,
  VE_TAG_LOG_NUMBER = 2,
  VE_TAG_NEXT_FILE_NUMBER = 3,
  VE_TAG_LAST_SEQUENCE = 4,
  VE_TAG_COMPACT_POINTER = 5,
  VE_TAG_DELETED_FILE = 6,
  VE_TAG_NEW_FILE = 7,
  VE_TAG_PREV_LOG_NUMBER = 9
};

// =================================================================== file meta
void ldb_file_meta_init(ldb_file_meta* f) {
  f->number = 0;
  f->file_size = 0;
  ldb_buffer_init(&f->smallest);
  ldb_buffer_init(&f->largest);
  f->refs = 0;
  f->allowed_seeks = 0;
  f->seq_of_largest = 0;
}

void ldb_file_meta_destroy(ldb_file_meta* f) {
  ldb_buffer_destroy(&f->smallest);
  ldb_buffer_destroy(&f->largest);
}

uint64_t ldb_file_meta_largest_seqno(const ldb_file_meta* f) {
  ldb_slice largest = ldb_buffer_slice((ldb_buffer*)&f->largest);
  ldb_parsed_internal_key p;
  if (largest.size >= 8 && ldb_parse_internal_key(&largest, &p)) {
    return p.sequence;
  }
  return 0;
}

// =================================================================== version edit
void ldb_version_edit_init(ldb_version_edit* e) { memset(e, 0, sizeof(*e)); }

void ldb_version_edit_clear(ldb_version_edit* e) {
  for (size_t i = 0; i < e->compact_pointers_count; i++) {
    ldb_buffer_destroy(&e->compact_pointers[i].key);
  }
  free(e->compact_pointers);
  free(e->deleted_levels);
  free(e->deleted_numbers);
  for (size_t i = 0; i < e->new_files_count; i++) {
    ldb_file_meta_destroy(&e->new_files[i].meta);
  }
  free(e->new_files);
  ldb_buffer_destroy(&e->comparator);
  ldb_version_edit_init(e);
}

void ldb_version_edit_destroy(ldb_version_edit* e) {
  ldb_version_edit_clear(e);
}

void ldb_version_edit_set_comparator(ldb_version_edit* e, const char* name) {
  if (e->comparator.data == NULL) {
    ldb_buffer_init(&e->comparator);
  }
  ldb_buffer_clear(&e->comparator);
  ldb_buffer_append_str(&e->comparator, name);
  e->has_comparator = 1;
}

void ldb_version_edit_set_log_number(ldb_version_edit* e, uint64_t n) {
  e->log_number = n;
  e->has_log_number = 1;
}

void ldb_version_edit_set_prev_log_number(ldb_version_edit* e, uint64_t n) {
  e->prev_log_number = n;
  e->has_prev_log_number = 1;
}

void ldb_version_edit_set_next_file(ldb_version_edit* e, uint64_t n) {
  e->next_file_number = n;
  e->has_next_file_number = 1;
}

void ldb_version_edit_set_last_sequence(ldb_version_edit* e, uint64_t seq) {
  e->last_sequence = seq;
  e->has_last_sequence = 1;
}

void ldb_version_edit_set_compact_pointer(ldb_version_edit* e, int level,
                                          const ldb_buffer* internal_key) {
  if (e->compact_pointers_count == e->compact_pointers_cap) {
    e->compact_pointers_cap = e->compact_pointers_cap
                                  ? e->compact_pointers_cap * 2
                                  : 4;
    e->compact_pointers = (ldb_compact_pointer_entry*)realloc(
        e->compact_pointers,
        sizeof(ldb_compact_pointer_entry) * e->compact_pointers_cap);
    assert(e->compact_pointers);
  }
  ldb_compact_pointer_entry* entry = &e->compact_pointers[e->compact_pointers_count++];
  entry->level = level;
  ldb_buffer_init(&entry->key);
  ldb_buffer_copy(&entry->key, internal_key);
}

void ldb_version_edit_add_file(ldb_version_edit* e, int level, uint64_t number,
                               uint64_t file_size, const ldb_buffer* smallest,
                               const ldb_buffer* largest) {
  if (e->new_files_count == e->new_files_cap) {
    e->new_files_cap = e->new_files_cap ? e->new_files_cap * 2 : 4;
    e->new_files = (ldb_new_file_entry*)realloc(
        e->new_files, sizeof(ldb_new_file_entry) * e->new_files_cap);
    assert(e->new_files);
  }
  ldb_new_file_entry* entry = &e->new_files[e->new_files_count++];
  entry->level = level;
  ldb_file_meta_init(&entry->meta);
  entry->meta.number = number;
  entry->meta.file_size = file_size;
  ldb_buffer_copy(&entry->meta.smallest, smallest);
  ldb_buffer_copy(&entry->meta.largest, largest);
}

void ldb_version_edit_remove_file(ldb_version_edit* e, int level,
                                  uint64_t number) {
  if (e->deleted_count == e->deleted_cap) {
    e->deleted_cap = e->deleted_cap ? e->deleted_cap * 2 : 4;
    e->deleted_levels =
        (int*)realloc(e->deleted_levels, sizeof(int) * e->deleted_cap);
    e->deleted_numbers = (uint64_t*)realloc(e->deleted_numbers,
                                            sizeof(uint64_t) * e->deleted_cap);
    assert(e->deleted_levels && e->deleted_numbers);
  }
  e->deleted_levels[e->deleted_count] = level;
  e->deleted_numbers[e->deleted_count] = number;
  e->deleted_count++;
}

ldb_status ldb_version_edit_encode(const ldb_version_edit* e, ldb_buffer* dst) {
  if (e->has_comparator) {
    ldb_put_varint32(dst, VE_TAG_COMPARATOR);
    ldb_slice c = ldb_buffer_slice((ldb_buffer*)&e->comparator);
    ldb_put_length_prefixed_slice(dst, &c);
  }
  if (e->has_log_number) {
    ldb_put_varint32(dst, VE_TAG_LOG_NUMBER);
    ldb_put_varint64(dst, e->log_number);
  }
  if (e->has_prev_log_number) {
    ldb_put_varint32(dst, VE_TAG_PREV_LOG_NUMBER);
    ldb_put_varint64(dst, e->prev_log_number);
  }
  if (e->has_next_file_number) {
    ldb_put_varint32(dst, VE_TAG_NEXT_FILE_NUMBER);
    ldb_put_varint64(dst, e->next_file_number);
  }
  if (e->has_last_sequence) {
    ldb_put_varint32(dst, VE_TAG_LAST_SEQUENCE);
    ldb_put_varint64(dst, e->last_sequence);
  }

  for (size_t i = 0; i < e->compact_pointers_count; i++) {
    ldb_put_varint32(dst, VE_TAG_COMPACT_POINTER);
    ldb_put_varint32(dst, (uint32_t)e->compact_pointers[i].level);
    ldb_slice k = ldb_buffer_slice(&e->compact_pointers[i].key);
    ldb_put_length_prefixed_slice(dst, &k);
  }

  for (size_t i = 0; i < e->deleted_count; i++) {
    ldb_put_varint32(dst, VE_TAG_DELETED_FILE);
    ldb_put_varint32(dst, (uint32_t)e->deleted_levels[i]);
    ldb_put_varint64(dst, e->deleted_numbers[i]);
  }

  for (size_t i = 0; i < e->new_files_count; i++) {
    const ldb_new_file_entry* f = &e->new_files[i];
    ldb_put_varint32(dst, VE_TAG_NEW_FILE);
    ldb_put_varint32(dst, (uint32_t)f->level);
    ldb_put_varint64(dst, f->meta.number);
    ldb_put_varint64(dst, f->meta.file_size);
    ldb_slice s = ldb_buffer_slice(&f->meta.smallest);
    ldb_put_length_prefixed_slice(dst, &s);
    ldb_slice l = ldb_buffer_slice(&f->meta.largest);
    ldb_put_length_prefixed_slice(dst, &l);
  }
  return ldb_status_ok();
}

static int ve_get_level(const char** p, const char* limit, uint32_t* level) {
  if (!ldb_get_varint32(p, limit, level)) return 0;
  if (*level >= LDB_K_NUM_LEVELS) return 0;
  return 1;
}

ldb_status ldb_version_edit_decode(ldb_version_edit* e, const ldb_slice* src) {
  const char* p = src->data;
  const char* limit = src->data + src->size;
  while (p < limit) {
    uint32_t tag;
    if (!ldb_get_varint32(&p, limit, &tag)) {
      return ldb_status_corruption("VersionEdit", "bad tag");
    }
    switch (tag) {
      case VE_TAG_COMPARATOR: {
        ldb_slice str;
        if (!ldb_get_length_prefixed_slice(&p, limit, &str)) {
          return ldb_status_corruption("new file entry", "comparator name");
        }
        if (e->comparator.data == NULL) ldb_buffer_init(&e->comparator);
        ldb_buffer_clear(&e->comparator);
        ldb_buffer_append_slice(&e->comparator, &str);
        e->has_comparator = 1;
        break;
      }
      case VE_TAG_LOG_NUMBER:
        if (!ldb_get_varint64(&p, limit, &e->log_number)) {
          return ldb_status_corruption("log number", NULL);
        }
        e->has_log_number = 1;
        break;
      case VE_TAG_PREV_LOG_NUMBER:
        if (!ldb_get_varint64(&p, limit, &e->prev_log_number)) {
          return ldb_status_corruption("previous log number", NULL);
        }
        e->has_prev_log_number = 1;
        break;
      case VE_TAG_NEXT_FILE_NUMBER:
        if (!ldb_get_varint64(&p, limit, &e->next_file_number)) {
          return ldb_status_corruption("next file number", NULL);
        }
        e->has_next_file_number = 1;
        break;
      case VE_TAG_LAST_SEQUENCE:
        if (!ldb_get_varint64(&p, limit, &e->last_sequence)) {
          return ldb_status_corruption("last sequence number", NULL);
        }
        e->has_last_sequence = 1;
        break;
      case VE_TAG_COMPACT_POINTER: {
        uint32_t level;
        if (!ve_get_level(&p, limit, &level)) {
          return ldb_status_corruption("compact pointer level", NULL);
        }
        ldb_slice key;
        if (!ldb_get_length_prefixed_slice(&p, limit, &key)) {
          return ldb_status_corruption("compact pointer", NULL);
        }
        ldb_buffer kbuf;
        ldb_buffer_init(&kbuf);
        ldb_buffer_append_slice(&kbuf, &key);
        ldb_version_edit_set_compact_pointer(e, (int)level, &kbuf);
        ldb_buffer_destroy(&kbuf);
        break;
      }
      case VE_TAG_DELETED_FILE: {
        uint32_t level;
        if (!ve_get_level(&p, limit, &level)) {
          return ldb_status_corruption("deleted file-level", NULL);
        }
        uint64_t number;
        if (!ldb_get_varint64(&p, limit, &number)) {
          return ldb_status_corruption("deleted file-", NULL);
        }
        ldb_version_edit_remove_file(e, (int)level, number);
        break;
      }
      case VE_TAG_NEW_FILE: {
        uint32_t level;
        if (!ve_get_level(&p, limit, &level)) {
          return ldb_status_corruption("new file-level", NULL);
        }
        uint64_t number, file_size;
        if (!ldb_get_varint64(&p, limit, &number) ||
            !ldb_get_varint64(&p, limit, &file_size)) {
          return ldb_status_corruption("new file entry", NULL);
        }
        ldb_slice smallest, largest;
        if (!ldb_get_length_prefixed_slice(&p, limit, &smallest) ||
            !ldb_get_length_prefixed_slice(&p, limit, &largest)) {
          return ldb_status_corruption("new file entry", "smallest, largest");
        }
        ldb_buffer sbuf, lbuf;
        ldb_buffer_init(&sbuf);
        ldb_buffer_init(&lbuf);
        ldb_buffer_append_slice(&sbuf, &smallest);
        ldb_buffer_append_slice(&lbuf, &largest);
        ldb_version_edit_add_file(e, (int)level, number, file_size, &sbuf,
                                  &lbuf);
        ldb_buffer_destroy(&sbuf);
        ldb_buffer_destroy(&lbuf);
        break;
      }
      default:
        {
          char buf[64];
          snprintf(buf, sizeof(buf), "unknown tag %u", (unsigned)tag);
          return ldb_status_corruption("VersionEdit", buf);
        }
    }
  }
  return ldb_status_ok();
}
