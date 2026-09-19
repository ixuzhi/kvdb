// Deterministic format fixtures. References: leveldb/db/version_edit_test.cc
// (EncodeDecode), table/table_test.cc (Empty/SimpleMulti/SimpleSpecialKey),
// table/format.cc and util/coding_test.cc. No filesystem or random inputs.
#include "harness.h"

// Consume owned statuses, including errors returned by fixture decoders.
static void fe_ok(ldb_status s) {
  CHECK_STATUS_OK(s);
  ldb_status_destroy(&s);
}

static void fe_bytes(ldb_slice actual, const void* expected, size_t size) {
  CHECK_EQ(size, actual.size);
  CHECK(size == 0 || memcmp(actual.data, expected, size) == 0);
}

TEST(format_extra, FixedLittleEndianUnaligned) {
  char bytes[14];
  static const unsigned char expected[] = {
      0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab, 0x89,
      0x67, 0x45, 0x23, 0x01};
  memset(bytes, 0x55, sizeof(bytes));
  ldb_encode_fixed32(bytes + 1, UINT32_C(0x12345678));
  ldb_encode_fixed64(bytes + 5, UINT64_C(0x0123456789abcdef));
  fe_bytes(ldb_slice_make(bytes + 1, 12), expected, sizeof(expected));
  CHECK_EQ(0x55, bytes[0]);
  CHECK_EQ(0x55, bytes[13]);
  CHECK(ldb_decode_fixed32(bytes + 1) == UINT32_C(0x12345678));
  CHECK(ldb_decode_fixed64(bytes + 5) == UINT64_C(0x0123456789abcdef));
}

TEST(format_extra, VarintBoundariesAndPrefixes) {
  static const uint64_t values[] = {
      0, 1, 127, 128, 16383, 16384, UINT32_MAX,
      UINT64_C(1) << 32, UINT64_C(1) << 50, UINT64_MAX};
  static const unsigned char golden[] = {0x80, 0x01};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    char data[10];
    char* end = ldb_encode_varint64(data, values[i]);
    const char* p = data;
    uint64_t decoded = 0;
    CHECK(ldb_get_varint64(&p, end, &decoded));
    CHECK(decoded == values[i]);
    CHECK(p == end);
    if (values[i] == 128)
      fe_bytes(ldb_slice_make(data, (size_t)(end - data)), golden, 2);
    // Each proper prefix of a canonical varint is incomplete.
    for (const char* limit = data; limit < end; limit++) {
      p = data;
      CHECK(!ldb_get_varint64(&p, limit, &decoded));
    }
    if (values[i] <= UINT32_MAX) {
      uint32_t small = 0;
      end = ldb_encode_varint32(data, (uint32_t)values[i]);
      p = data;
      CHECK(ldb_get_varint32(&p, end, &small));
      CHECK(small == values[i]);
      CHECK(p == end);
      for (const char* limit = data; limit < end; limit++) {
        p = data;
        CHECK(!ldb_get_varint32(&p, limit, &small));
      }
    }
  }
}

TEST(format_extra, LengthPrefixedBinaryAndEmpty) {
  static const char binary[] = {'a', '\0', (char)0xff};
  static const unsigned char expected[] = {3, 'a', 0, 255, 0};
  ldb_buffer b;
  ldb_buffer_init(&b);
  ldb_slice key = ldb_slice_make(binary, sizeof(binary));
  ldb_slice empty = ldb_slice_str("");
  ldb_put_length_prefixed_slice(&b, &key);
  ldb_put_length_prefixed_slice(&b, &empty);
  fe_bytes(ldb_bslice(&b), expected, sizeof(expected));
  const char* p = b.data;
  ldb_slice decoded;
  CHECK(ldb_get_length_prefixed_slice(&p, b.data + b.size, &decoded));
  fe_bytes(decoded, binary, sizeof(binary));
  CHECK(ldb_get_length_prefixed_slice(&p, b.data + b.size, &decoded));
  CHECK_EQ(0, decoded.size);
  CHECK(p == b.data + b.size);
  for (size_t n = 0; n < 4; n++) {
    p = b.data;
    CHECK(!ldb_get_length_prefixed_slice(&p, b.data + n, &decoded));
  }
  ldb_buffer_destroy(&b);
}

TEST(format_extra, InternalKeyTrailerAndOrdering) {
  ldb_buffer old, recent;
  ldb_buffer_init(&old);
  ldb_buffer_init(&recent);
  ldb_slice user = ldb_slice_make("a\0z", 3);
  ldb_append_internal_key(&old, &user, 7, LDB_TYPE_DELETION);
  ldb_append_internal_key(&recent, &user, LDB_K_MAX_SEQUENCE_NUMBER,
                          LDB_TYPE_VALUE);
  static const unsigned char trailer[] = {0, 7, 0, 0, 0, 0, 0, 0};
  fe_bytes(ldb_slice_make(old.data + user.size, 8), trailer, 8);
  ldb_slice a = ldb_bslice(&old), b = ldb_bslice(&recent);
  ldb_parsed_internal_key parsed;
  CHECK(ldb_parse_internal_key(&b, &parsed));
  CHECK(parsed.sequence == LDB_K_MAX_SEQUENCE_NUMBER);
  CHECK_EQ(LDB_TYPE_VALUE, parsed.type);
  CHECK(ldb_slice_equals(&user, &parsed.user_key));
  ldb_ikc cmp;
  ldb_ikc_init(&cmp, ldb_bytewise_comparator());
  CHECK_LT(ldb_ikc_compare(&cmp, &b, &a), 0);
  ldb_slice short_key = ldb_slice_str("short");
  CHECK(!ldb_parse_internal_key(&short_key, &parsed));
  ldb_buffer_destroy(&old);
  ldb_buffer_destroy(&recent);
}

TEST(format_extra, VersionEditEmptyAndClear) {
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  ldb_buffer encoded, key;
  ldb_buffer_init(&encoded);
  ldb_buffer_init(&key);
  fe_ok(ldb_version_edit_encode(&edit, &encoded));
  CHECK_EQ(0, encoded.size);
  ldb_slice empty = ldb_slice_str("");
  fe_ok(ldb_version_edit_decode(&edit, &empty));
  ldb_version_edit_set_comparator(&edit, "test");
  ldb_version_edit_set_log_number(&edit, 4);
  ldb_version_edit_set_prev_log_number(&edit, 3);
  ldb_version_edit_set_next_file(&edit, 8);
  ldb_version_edit_set_last_sequence(&edit, 1000);
  ldb_slice user = ldb_slice_str("k");
  ldb_append_internal_key(&key, &user, 1000, LDB_TYPE_VALUE);
  ldb_version_edit_set_compact_pointer(&edit, 1, &key);
  ldb_version_edit_remove_file(&edit, 2, 9);
  ldb_version_edit_add_file(&edit, 1, 10, 200, &key, &key);
  ldb_version_edit_clear(&edit);
  CHECK(!edit.has_comparator && !edit.has_log_number &&
        !edit.has_prev_log_number && !edit.has_next_file_number &&
        !edit.has_last_sequence);
  CHECK_EQ(0, edit.compact_pointers_count);
  CHECK_EQ(0, edit.deleted_count);
  CHECK_EQ(0, edit.new_files_count);
  fe_ok(ldb_version_edit_encode(&edit, &encoded));
  CHECK_EQ(0, encoded.size);
  ldb_version_edit_destroy(&edit);
  ldb_buffer_destroy(&key);
  ldb_buffer_destroy(&encoded);
}

TEST(format_extra, VersionEditScalarGolden) {
  // Tag values are part of the LevelDB manifest format, not C enum ordinals.
  static const unsigned char expected[] = {
      1, 1, 'x', 2, 0x80, 1, 9, 0, 3, 0xac, 2, 4, 0xe8, 7};
  ldb_version_edit edit, parsed;
  ldb_version_edit_init(&edit);
  ldb_version_edit_init(&parsed);
  ldb_version_edit_set_comparator(&edit, "x");
  ldb_version_edit_set_log_number(&edit, 128);
  ldb_version_edit_set_prev_log_number(&edit, 0);
  ldb_version_edit_set_next_file(&edit, 300);
  ldb_version_edit_set_last_sequence(&edit, 1000);
  ldb_buffer b;
  ldb_buffer_init(&b);
  fe_ok(ldb_version_edit_encode(&edit, &b));
  fe_bytes(ldb_bslice(&b), expected, sizeof(expected));
  ldb_slice input = ldb_bslice(&b);
  fe_ok(ldb_version_edit_decode(&parsed, &input));
  CHECK(parsed.has_comparator && parsed.has_log_number &&
        parsed.has_prev_log_number && parsed.has_next_file_number &&
        parsed.has_last_sequence);
  CHECK_BUF_EQ("x", parsed.comparator);
  CHECK_EQ(128, parsed.log_number);
  CHECK_EQ(0, parsed.prev_log_number);
  CHECK_EQ(300, parsed.next_file_number);
  CHECK_EQ(1000, parsed.last_sequence);
  ldb_version_edit_destroy(&edit);
  ldb_version_edit_destroy(&parsed);
  ldb_buffer_destroy(&b);
}

TEST(format_extra, VersionEditLargeMetadataRoundtrip) {
  const uint64_t big = UINT64_C(1) << 50;
  ldb_version_edit edit, parsed;
  ldb_version_edit_init(&edit);
  ldb_version_edit_init(&parsed);
  ldb_buffer lo, hi, encoded, again;
  ldb_buffer_init(&lo);
  ldb_buffer_init(&hi);
  ldb_buffer_init(&encoded);
  ldb_buffer_init(&again);
  ldb_slice a = ldb_slice_make("a\0", 2), z = ldb_slice_str("z");
  ldb_append_internal_key(&lo, &a, big + 7, LDB_TYPE_VALUE);
  ldb_append_internal_key(&hi, &z, big + 900, LDB_TYPE_DELETION);
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    ldb_version_edit_add_file(&edit, level, big + 100 + level,
                              big + 200 + level, &lo, &hi);
    ldb_version_edit_remove_file(&edit, level, big + 300 + level);
    ldb_version_edit_set_compact_pointer(&edit, level, &hi);
  }
  ldb_version_edit_set_last_sequence(&edit, big + 10000);
  fe_ok(ldb_version_edit_encode(&edit, &encoded));
  ldb_slice input = ldb_bslice(&encoded);
  fe_ok(ldb_version_edit_decode(&parsed, &input));
  CHECK_EQ(LDB_K_NUM_LEVELS, parsed.new_files_count);
  CHECK_EQ(LDB_K_NUM_LEVELS, parsed.deleted_count);
  CHECK_EQ(LDB_K_NUM_LEVELS, parsed.compact_pointers_count);
  CHECK(parsed.has_last_sequence && parsed.last_sequence == big + 10000);
  for (int level = 0; level < LDB_K_NUM_LEVELS; level++) {
    const ldb_new_file_entry* f = &parsed.new_files[level];
    CHECK_EQ(level, f->level);
    CHECK(f->meta.number == big + 100 + level);
    CHECK(f->meta.file_size == big + 200 + level);
    fe_bytes(ldb_bslice(&f->meta.smallest), lo.data, lo.size);
    fe_bytes(ldb_bslice(&f->meta.largest), hi.data, hi.size);
    CHECK_EQ(level, parsed.deleted_levels[level]);
    CHECK(parsed.deleted_numbers[level] == big + 300 + level);
    CHECK_EQ(level, parsed.compact_pointers[level].level);
    fe_bytes(ldb_bslice(&parsed.compact_pointers[level].key), hi.data, hi.size);
  }
  fe_ok(ldb_version_edit_encode(&parsed, &again));
  fe_bytes(ldb_bslice(&again), encoded.data, encoded.size);
  ldb_version_edit_destroy(&edit);
  ldb_version_edit_destroy(&parsed);
  ldb_buffer_destroy(&lo);
  ldb_buffer_destroy(&hi);
  ldb_buffer_destroy(&encoded);
  ldb_buffer_destroy(&again);
}

static void fe_bad_edit(const char* data, size_t size) {
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  ldb_slice input = ldb_slice_make(data, size);
  ldb_status s = ldb_version_edit_decode(&edit, &input);
  CHECK_EQ(LDB_CORRUPTION, s.code);
  ldb_status_destroy(&s);
  ldb_version_edit_destroy(&edit);
}

TEST(format_extra, VersionEditInvalidTagAndLevel) {
  const char unknown[] = {8};  // reserved/unknown tag
  const char incomplete_tag[] = {(char)0x80};
  fe_bad_edit(unknown, sizeof(unknown));
  fe_bad_edit(incomplete_tag, sizeof(incomplete_tag));
  const char tags[] = {5, 6, 7};
  for (size_t i = 0; i < sizeof(tags); i++) {
    const char bad_level[] = {tags[i], LDB_K_NUM_LEVELS};
    fe_bad_edit(bad_level, sizeof(bad_level));
  }
}

TEST(format_extra, VersionEditTruncatedFields) {
  // Small, explicitly bounded fixtures: comparator, scalar and new-file entry.
  static const char comparator[] = {1, 3, 'a', 'b', 'c'};
  static const char scalar[] = {4, (char)0xe8, 7};
  ldb_buffer entry, key;
  ldb_buffer_init(&entry);
  ldb_buffer_init(&key);
  ldb_slice user = ldb_slice_str("k");
  ldb_append_internal_key(&key, &user, 42, LDB_TYPE_VALUE);
  ldb_version_edit edit;
  ldb_version_edit_init(&edit);
  ldb_version_edit_add_file(&edit, 2, 128, 300, &key, &key);
  fe_ok(ldb_version_edit_encode(&edit, &entry));
  for (size_t n = 1; n < sizeof(comparator); n++) fe_bad_edit(comparator, n);
  for (size_t n = 1; n < sizeof(scalar); n++) fe_bad_edit(scalar, n);
  for (size_t n = 1; n < entry.size; n++) fe_bad_edit(entry.data, n);
  ldb_version_edit_destroy(&edit);
  ldb_buffer_destroy(&entry);
  ldb_buffer_destroy(&key);
}

TEST(format_extra, BlockHandleGoldenRoundtripAndTruncation) {
  const uint64_t values[] = {0, 127, 128, UINT64_C(1) << 50, UINT64_MAX - 1};
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    ldb_block_handle handle = {values[i], 300}, parsed;
    ldb_buffer b;
    ldb_buffer_init(&b);
    ldb_block_handle_encode(&handle, &b);
    size_t encoded_size = b.size;
    if (values[i] == 128) {
      static const unsigned char expected[] = {0x80, 1, 0xac, 2};
      fe_bytes(ldb_bslice(&b), expected, sizeof(expected));
    }
    for (size_t n = 0; n < encoded_size; n++) {
      ldb_slice prefix = ldb_slice_make(b.data, n);
      ldb_status s = ldb_block_handle_decode(&parsed, &prefix);
      CHECK_EQ(LDB_CORRUPTION, s.code);
      ldb_status_destroy(&s);
    }
    ldb_buffer_append_str(&b, "tail");
    ldb_slice input = ldb_bslice(&b);
    fe_ok(ldb_block_handle_decode(&parsed, &input));
    CHECK(parsed.offset == handle.offset);
    CHECK(parsed.size == handle.size);
    fe_bytes(input, "tail", 4);
    ldb_buffer_destroy(&b);
  }
}

TEST(format_extra, FooterAppendRoundtripAndInvalid) {
  ldb_footer footer = {{128, 300}, {UINT64_C(1) << 40, 4096}}, parsed;
  ldb_buffer b;
  ldb_buffer_init(&b);
  ldb_buffer_append_str(&b, "prefix");
  ldb_footer_encode(&footer, &b);
  CHECK_EQ(6 + LDB_FOOTER_ENCODED_LENGTH, b.size);
  fe_bytes(ldb_slice_make(b.data, 6), "prefix", 6);
  static const unsigned char magic[] = {0x57, 0xfb, 0x80, 0x8b,
                                         0x24, 0x75, 0x47, 0xdb};
  fe_bytes(ldb_slice_make(b.data + b.size - 8, 8), magic, 8);
  ldb_slice input = ldb_slice_make(b.data + 6, LDB_FOOTER_ENCODED_LENGTH);
  fe_ok(ldb_footer_decode(&parsed, &input));
  CHECK(parsed.metaindex_handle.offset == footer.metaindex_handle.offset);
  CHECK(parsed.metaindex_handle.size == footer.metaindex_handle.size);
  CHECK(parsed.index_handle.offset == footer.index_handle.offset);
  CHECK(parsed.index_handle.size == footer.index_handle.size);
  for (size_t n = 0; n < LDB_FOOTER_ENCODED_LENGTH; n++) {
    input = ldb_slice_make(b.data + 6, n);
    ldb_status s = ldb_footer_decode(&parsed, &input);
    CHECK_EQ(LDB_CORRUPTION, s.code);
    ldb_status_destroy(&s);
  }
  b.data[b.size - 1] ^= 1;
  input = ldb_slice_make(b.data + 6, LDB_FOOTER_ENCODED_LENGTH);
  ldb_status s = ldb_footer_decode(&parsed, &input);
  CHECK_EQ(LDB_CORRUPTION, s.code);
  ldb_status_destroy(&s);
  ldb_buffer_destroy(&b);
}

TEST(format_extra, BlockRestartLayoutAndBidirectionalSeek) {
  ldb_options options;
  ldb_options_init(&options);
  options.block_restart_interval = 2;
  ldb_block_builder builder;
  ldb_block_builder_init(&builder, &options);
  const char* keys[] = {"", "alpha", "alphabet", "beta", "z"};
  for (size_t i = 0; i < 5; i++) {
    ldb_slice k = ldb_slice_str(keys[i]);
    ldb_slice value = ldb_slice_make("v\0x", 3);
    ldb_block_builder_add(&builder, &k, &value);
  }
  ldb_slice data = ldb_block_builder_finish(&builder);
  CHECK_EQ(3, ldb_decode_fixed32(data.data + data.size - 4));
  size_t restart_base = data.size - 16;
  CHECK_EQ(0, ldb_decode_fixed32(data.data + restart_base));
  uint32_t previous = 0;
  for (size_t i = 0; i < 3; i++) {
    uint32_t offset = ldb_decode_fixed32(data.data + restart_base + i * 4);
    CHECK_LT(offset, restart_base);
    if (i) CHECK_GT(offset, previous);
    CHECK_EQ(0, data.data[offset]);  // shared prefix length at restart
    previous = offset;
  }
  ldb_block_contents contents = {data, NULL, 0};
  ldb_block* block = ldb_block_new(&contents);
  ldb_iterator* it = ldb_block_new_iterator(block, options.comparator);
  ldb_iter_seek_to_first(it);
  for (size_t i = 0; i < 5; i++) {
    CHECK(ldb_iter_valid(it));
    fe_bytes(ldb_iter_key(it), keys[i], strlen(keys[i]));
    fe_bytes(ldb_iter_value(it), "v\0x", 3);
    ldb_iter_next(it);
  }
  CHECK(!ldb_iter_valid(it));
  ldb_iter_seek_to_last(it);
  for (int i = 4; i >= 0; i--) {
    CHECK(ldb_iter_valid(it));
    fe_bytes(ldb_iter_key(it), keys[i], strlen(keys[i]));
    ldb_iter_prev(it);
  }
  CHECK(!ldb_iter_valid(it));
  ldb_slice target = ldb_slice_str("alphaz");
  ldb_iter_seek(it, &target);
  CHECK(ldb_iter_valid(it));
  fe_bytes(ldb_iter_key(it), "beta", 4);
  fe_ok(ldb_iter_status(it));
  ldb_iterator_destroy(it);
  ldb_block_destroy(block);
  ldb_block_builder_reset(&builder);
  CHECK(ldb_block_builder_empty(&builder));
  data = ldb_block_builder_finish(&builder);
  CHECK_EQ(8, data.size);
  CHECK_EQ(0, ldb_decode_fixed32(data.data));
  CHECK_EQ(1, ldb_decode_fixed32(data.data + 4));
  ldb_block_builder_destroy(&builder);
}
