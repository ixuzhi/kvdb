// test_util.c - ports of leveldb's crc32c_test, coding_test, bloom_test,
// filename_test and dbformat_test (core cases).
#include "harness.h"

#include <assert.h>

// =================================================================== crc32c_test
TEST(crc32c, StandardResults) {
  // From rfc3720 section B.4.
  char buf[32];
  memset(buf, 0, sizeof(buf));
  CHECK_EQ(0x8a9136aa, ldb_crc32c_value(buf, sizeof(buf)));

  memset(buf, 0xff, sizeof(buf));
  CHECK_EQ(0x62a8ab43, ldb_crc32c_value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) {
    buf[i] = (char)i;
  }
  CHECK_EQ(0x46dd794e, ldb_crc32c_value(buf, sizeof(buf)));

  for (int i = 0; i < 32; i++) {
    buf[i] = (char)(31 - i);
  }
  CHECK_EQ(0x113fdb5c, ldb_crc32c_value(buf, sizeof(buf)));

  unsigned char data[48] = {
      0x01, 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
      0x00, 0x00, 0x00, 0x14, 0x00, 0x00, 0x00, 0x18, 0x28, 0x00, 0x00, 0x00,
      0x00, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
  CHECK_EQ(0xd9963a56, ldb_crc32c_value((const char*)data, sizeof(data)));
}

TEST(crc32c, Values) {
  CHECK_NE(ldb_crc32c_value("a", 1), 0);
  CHECK_NE(ldb_crc32c_value("0123456789", 10), 0);
  // Standard check value: CRC-32C of "123456789"
  CHECK_EQ(0xe3069283, ldb_crc32c_value("123456789", 9));
}

TEST(crc32c, Extend) {
  {
    uint32_t v = ldb_crc32c_value("a", 1);
    v = ldb_crc32c_extend(v, "bc", 2);
    CHECK_EQ(v, ldb_crc32c_value("abc", 3));
  }
  {
    uint32_t v = ldb_crc32c_value("abc", 3);
    v = ldb_crc32c_extend(v, "def", 3);
    CHECK_EQ(v, ldb_crc32c_value("abcdef", 6));
  }
}

TEST(crc32c, Mask) {
  uint32_t v = ldb_crc32c_value("0123456789", 10);
  uint32_t masked = ldb_crc32c_mask(v);
  CHECK_NE(masked, v);
  CHECK_EQ(ldb_crc32c_unmask(masked), v);
  masked = ldb_crc32c_mask(ldb_crc32c_mask(v));
  CHECK_NE(masked, v);
  CHECK_EQ(ldb_crc32c_unmask(ldb_crc32c_unmask(masked)), v);
}

// =================================================================== coding_test
TEST(coding, Fixed32) {
  for (uint32_t i = 0; i < 32; i++) {
    ldb_buffer v;
    ldb_buffer_init(&v);
    ldb_put_fixed32(&v, 0x12345678u + i);
    CHECK_EQ(4, v.size);
    CHECK_EQ((0x78u + i) & 0xff, (uint32_t)v.data[0] & 0xff);
    ldb_buffer_destroy(&v);
  }
}

TEST(coding, Fixed64) {
  for (uint32_t i = 0; i < 32; i++) {
    ldb_buffer v;
    ldb_buffer_init(&v);
    uint64_t value = 0x12345678abcdef01ull + i;
    ldb_put_fixed64(&v, value);
    CHECK_EQ(8, v.size);
    CHECK_EQ(value, ldb_decode_fixed64(v.data));
    ldb_buffer_destroy(&v);
  }
}

TEST(coding, Varint32) {
  for (uint32_t i = 0; i < (32 * 32); i++) {
    uint32_t v = (i / 32) << (i % 32);
    ldb_buffer buf;
    ldb_buffer_init(&buf);
    ldb_put_varint32(&buf, v);
    uint32_t out;
    const char* p = buf.data;
    CHECK_EQ(1, ldb_get_varint32(&p, buf.data + buf.size, &out));
    CHECK_EQ(v, out);
    ldb_buffer_destroy(&buf);
  }
}

TEST(coding, Varint32Overflow) {
  uint32_t out;
  const char* p = "\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a";
  const char* q = p;
  CHECK_EQ(0, ldb_get_varint32(&q, p + 10, &out));
}

TEST(coding, Varint64) {
  for (int shift = 0; shift <= 63; shift++) {
    for (uint64_t v = 0; v < 10; v++) {
      uint64_t value = v << shift;
      ldb_buffer buf;
      ldb_buffer_init(&buf);
      ldb_put_varint64(&buf, value);
      uint64_t out;
      const char* p = buf.data;
      CHECK_EQ(1, ldb_get_varint64(&p, buf.data + buf.size, &out));
      CHECK_EQ(value, out);
      ldb_buffer_destroy(&buf);
    }
  }
}

TEST(coding, Varint64Overflow) {
  uint64_t out;
  const char* p = "\x81\x82\x83\x84\x85\x86\x87\x88\x89\x8a\x91";
  const char* q = p;
  CHECK_EQ(0, ldb_get_varint64(&q, p + 11, &out));
}

TEST(coding, Names) {
  CHECK_STR_EQ(ldb_bytewise_comparator()->name(ldb_bytewise_comparator()),
               "leveldb.BytewiseComparator");
  CHECK_STR_EQ(ldb_ikc_name(), "leveldb.InternalKeyComparator");
}

// =================================================================== bloom_test
typedef struct bloom_tester {
  const ldb_filterpolicy* policy;
  ldb_buffer filter;
  ldb_buffer keys_data;  // stable key storage
  size_t* key_offsets;
  size_t keys_count;
} bloom_tester;

static void tester_init(bloom_tester* t, int bits_per_key) {
  t->policy = ldb_new_bloom_filter_policy(bits_per_key);
  ldb_buffer_init(&t->filter);
  ldb_buffer_init(&t->keys_data);
  t->key_offsets = NULL;
  t->keys_count = 0;
}

static void tester_destroy(bloom_tester* t) {
  if (t->policy->destroy) t->policy->destroy((ldb_filterpolicy*)t->policy);
  ldb_buffer_destroy(&t->filter);
  ldb_buffer_destroy(&t->keys_data);
  free(t->key_offsets);
}

static void tester_build(bloom_tester* t) {
  ldb_slice* slices = (ldb_slice*)malloc(sizeof(ldb_slice) * (t->keys_count ? t->keys_count : 1));
  for (size_t i = 0; i < t->keys_count; i++) {
    slices[i] = ldb_slice_make(t->keys_data.data + t->key_offsets[i],
                               (i + 1 < t->keys_count ? t->key_offsets[i + 1]
                                                      : t->keys_data.size) -
                                   t->key_offsets[i]);
  }
  t->policy->create_filter(t->policy, slices, (int)t->keys_count, &t->filter);
  free(slices);
}

static void tester_add(bloom_tester* t, const char* k) {
  t->key_offsets = (size_t*)realloc(t->key_offsets,
                                    sizeof(size_t) * (t->keys_count + 1));
  t->key_offsets[t->keys_count] = t->keys_data.size;
  t->keys_count++;
  ldb_buffer_append_str(&t->keys_data, k);
}

TEST(bloom, EmptyFilter) {
  bloom_tester t;
  tester_init(&t, 10);
  tester_build(&t);
  // leveldb builds a 9-byte all-zero filter (8 bits bytes + k byte);
  // lookups on it must not match.
  ldb_slice hello = ldb_slice_str("hello");
  ldb_slice fdata = ldb_bslice(&t.filter);
  CHECK_EQ(0, t.policy->key_may_match(t.policy, &hello, &fdata));
  tester_destroy(&t);
}

TEST(bloom, SmallFilter) {
  bloom_tester t;
  tester_init(&t, 10);
  tester_add(&t, "hello");
  tester_add(&t, "world");
  tester_build(&t);
  CHECK((long long)t.filter.size >= 2);
  ldb_slice hello = ldb_slice_str("hello");
  ldb_slice world = ldb_slice_str("world");
  ldb_slice fdata = ldb_bslice(&t.filter);
  CHECK_EQ(1, t.policy->key_may_match(t.policy, &hello, &fdata));
  CHECK_EQ(1, t.policy->key_may_match(t.policy, &world, &fdata));
  ldb_slice x = ldb_slice_str("x");
  ldb_slice foo = ldb_slice_str("foo");
  CHECK_EQ(0, t.policy->key_may_match(t.policy, &x, &fdata));
  CHECK_EQ(0, t.policy->key_may_match(t.policy, &foo, &fdata));
  tester_destroy(&t);
}

static void tester_reset(bloom_tester* t) {
  ldb_buffer_clear(&t->filter);
  ldb_buffer_clear(&t->keys_data);
  t->keys_count = 0;
}

static double bloom_false_positive_rate(bloom_tester* t) {
  int matches = 0;
  for (int i = 0; i < 10000; i++) {
    char buf[32];
    snprintf(buf, sizeof(buf), "key%d", i + 1000000000);
    ldb_slice probe = ldb_slice_str(buf);
    int present = 0;
    for (size_t k = 0; k < t->keys_count; k++) {
      ldb_slice kk = ldb_slice_make(
          t->keys_data.data + t->key_offsets[k],
          (k + 1 < t->keys_count ? t->key_offsets[k + 1] : t->keys_data.size) -
              t->key_offsets[k]);
      if (ldb_slice_equals(&kk, &probe)) {
        present = 1;
        break;
      }
    }
    if (!present) {
      ldb_slice fdata = ldb_bslice(&t->filter);
      if (t->policy->key_may_match(t->policy, &probe, &fdata)) {
        matches++;
      }
    }
  }
  return matches / 10000.0;
}

static int bloom_next_length(int length) {
  if (length < 10) {
    length += 1;
  } else if (length < 100) {
    length += 10;
  } else if (length < 1000) {
    length += 100;
  } else {
    length += 1000;
  }
  return length;
}

// Port of leveldb's VaryingLengths: fixed 10 bits/key, varying key count.
TEST(bloom, VaryingLengths) {
  bloom_tester t;
  tester_init(&t, 10);

  int mediocre_filters = 0;
  int good_filters = 0;

  for (int length = 1; length <= 10000; length = bloom_next_length(length)) {
    tester_reset(&t);
    for (int i = 0; i < length; i++) {
      char buf[32];
      snprintf(buf, sizeof(buf), "key%d", i);
      tester_add(&t, buf);
    }
    tester_build(&t);
    CHECK_LE((long long)t.filter.size, (long long)((length * 10 / 8) + 40));

    // All added keys must match
    for (int i = 0; i < length; i++) {
      char buf[32];
      snprintf(buf, sizeof(buf), "key%d", i);
      ldb_slice key = ldb_slice_str(buf);
      ldb_slice fdata = ldb_bslice(&t.filter);
      CHECK_EQ(1, t.policy->key_may_match(t.policy, &key, &fdata));
    }

    double rate = bloom_false_positive_rate(&t);
    CHECK(rate <= 0.02);  // Must not be over 2%
    if (rate > 0.0125) {
      mediocre_filters++;  // Allowed, but not too often
    } else {
      good_filters++;
    }
  }
  CHECK_LE(mediocre_filters, good_filters / 5);
  tester_destroy(&t);
}

TEST(bloom, Name) {
  bloom_tester t;
  tester_init(&t, 10);
  CHECK_STR_EQ(t.policy->name(t.policy), "leveldb.BuiltinBloomFilter2");
  tester_destroy(&t);
}

// =================================================================== dbformat_test
TEST(dbformat, InternalKeyEncodeDecode) {
  ldb_buffer ikey;
  ldb_buffer_init(&ikey);
  ldb_slice user = ldb_slice_str("foo");
  ldb_append_internal_key(&ikey, &user, 42, LDB_TYPE_VALUE);
  CHECK_EQ(3 + 8, (long long)ikey.size);
  ldb_parsed_internal_key parsed;
  ldb_slice islice = ldb_buffer_slice(&ikey);
  CHECK_EQ(1, ldb_parse_internal_key(&islice, &parsed));
  CHECK_EQ(42, (long long)parsed.sequence);
  CHECK_EQ(LDB_TYPE_VALUE, parsed.type);
  CHECK_EQ(1, ldb_slice_equals(&parsed.user_key, &user));
  ldb_buffer_destroy(&ikey);
}

TEST(dbformat, InternalKeyOrdering) {
  ldb_buffer a, b, c;
  ldb_buffer_init(&a);
  ldb_buffer_init(&b);
  ldb_buffer_init(&c);
  ldb_slice user = ldb_slice_str("foo");
  ldb_append_internal_key(&a, &user, 100, LDB_TYPE_VALUE);
  ldb_append_internal_key(&b, &user, 200, LDB_TYPE_VALUE);
  ldb_append_internal_key(&c, &user, 100, LDB_TYPE_DELETION);
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_slice as = ldb_buffer_slice(&a);
  ldb_slice bs = ldb_buffer_slice(&b);
  ldb_slice cs = ldb_buffer_slice(&c);
  // Higher sequence sorts first
  CHECK_LT(ldb_ikc_compare(&icmp, &bs, &as), 0);
  CHECK_GT(ldb_ikc_compare(&icmp, &as, &bs), 0);
  // Same sequence: higher type sorts first (value > deletion)
  CHECK_LT(ldb_ikc_compare(&icmp, &as, &cs), 0);
  CHECK_GT(ldb_ikc_compare(&icmp, &cs, &as), 0);
  ldb_buffer_destroy(&a);
  ldb_buffer_destroy(&b);
  ldb_buffer_destroy(&c);
}

TEST(dbformat, LookupKey) {
  ldb_slice user = ldb_slice_str("foobar");
  ldb_lookup_key lk;
  ldb_lookup_key_init(&lk, &user, 1000);
  CHECK_EQ(1, ldb_slice_equals(&lk.user_key, &user));
  CHECK_EQ((long long)(user.size + 8), (long long)lk.internal_key.size);
  CHECK_EQ((long long)(user.size + 8 + 1), (long long)lk.memtable_key.size);
  ldb_lookup_key_destroy(&lk);
}

// =================================================================== filename_test
TEST(filename, Parse) {
  uint64_t number;
  int type;
  CHECK_EQ(1, ldb_parse_file_name("100.log", &number, &type));
  CHECK_EQ(100, (long long)number);
  CHECK_EQ(LDB_K_LOG_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("0.log", &number, &type));
  CHECK_EQ(0, (long long)number);
  CHECK_EQ(LDB_K_LOG_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("0.sst", &number, &type));
  CHECK_EQ(0, (long long)number);
  CHECK_EQ(LDB_K_TABLE_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("0.ldb", &number, &type));
  CHECK_EQ(0, (long long)number);
  CHECK_EQ(LDB_K_TABLE_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("0.dbtmp", &number, &type));
  CHECK_EQ(0, (long long)number);
  CHECK_EQ(LDB_K_TEMP_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("MANIFEST-2", &number, &type));
  CHECK_EQ(2, (long long)number);
  CHECK_EQ(LDB_K_DESCRIPTOR_FILE, type);

  CHECK_EQ(1, ldb_parse_file_name("CURRENT", &number, &type));
  CHECK_EQ(LDB_K_CURRENT_FILE, type);
  CHECK_EQ(1, ldb_parse_file_name("LOCK", &number, &type));
  CHECK_EQ(LDB_K_DB_LOCK_FILE, type);
  CHECK_EQ(1, ldb_parse_file_name("LOG", &number, &type));
  CHECK_EQ(LDB_K_INFO_LOG_FILE, type);
  CHECK_EQ(1, ldb_parse_file_name("LOG.old", &number, &type));
  CHECK_EQ(LDB_K_INFO_LOG_FILE, type);

  // Errors
  CHECK_EQ(0, ldb_parse_file_name("x", &number, &type));
  CHECK_EQ(0, ldb_parse_file_name("100x", &number, &type));
  CHECK_EQ(0, ldb_parse_file_name("3x.ldb", &number, &type));
  CHECK_EQ(0, ldb_parse_file_name("3.ldbxx", &number, &type));
  CHECK_EQ(0, ldb_parse_file_name("Manifest-2", &number, &type));
}

TEST(filename, Build) {
  char* n = ldb_log_file_name("/x/y", 123);
  CHECK_STR_EQ(n, "/x/y/000123.log");
  free(n);
  n = ldb_table_file_name("/x/y", 123);
  CHECK_STR_EQ(n, "/x/y/000123.ldb");
  free(n);
  n = ldb_sst_table_file_name("/x/y", 123);
  CHECK_STR_EQ(n, "/x/y/000123.sst");
  free(n);
  n = ldb_descriptor_file_name("/x/y", 1);
  CHECK_STR_EQ(n, "/x/y/MANIFEST-000001");
  free(n);
  n = ldb_current_file_name("/x/y");
  CHECK_STR_EQ(n, "/x/y/CURRENT");
  free(n);
  n = ldb_lock_file_name("/x/y");
  CHECK_STR_EQ(n, "/x/y/LOCK");
  free(n);
  n = ldb_temp_file_name("/x/y", 66);
  CHECK_STR_EQ(n, "/x/y/000066.dbtmp");
  free(n);
  n = ldb_info_log_file_name("/x/y");
  CHECK_STR_EQ(n, "/x/y/LOG");
  free(n);
  n = ldb_old_info_log_file_name("/x/y");
  CHECK_STR_EQ(n, "/x/y/LOG.old");
  free(n);
}

// =================================================================== hash_test
TEST(hash, Deterministic) {
  uint32_t h1 = ldb_hash("abc", 3, 0);
  uint32_t h2 = ldb_hash("abc", 3, 0);
  CHECK_EQ(h1, h2);
  CHECK_NE(h1, ldb_hash("abd", 3, 0));
  CHECK_NE(h1, ldb_hash("abc", 3, 1));
}

// =================================================================== snappy_test
TEST(snappy, RoundTrip) {
  const char* samples[] = {
      "",
      "a",
      "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
      "LevelDB is a fast key-value storage engine written at Google.",
      "the quick brown fox jumps over the lazy dog. "
      "the quick brown fox jumps over the lazy dog. "};
  for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
    const char* src = samples[i];
    size_t n = strlen(src);
    ldb_buffer compressed;
    ldb_buffer_init(&compressed);
    CHECK_EQ(1, ldb_snappy_compress(src, n, &compressed));
    if (n > 0) {
      size_t ulen = 0;
      CHECK_EQ(1, ldb_snappy_get_uncompressed_length(compressed.data,
                                                     compressed.size, &ulen));
      CHECK_EQ((long long)n, (long long)ulen);
      char* out = (char*)malloc(ulen + 1);
      CHECK_EQ(1, ldb_snappy_uncompress(compressed.data, compressed.size, out,
                                        ulen));
      out[ulen] = '\0';
      CHECK_EQ(0, memcmp(out, src, n));
      free(out);
    }
    ldb_buffer_destroy(&compressed);
  }
}

TEST(snappy, LargeData) {
  size_t n = 100000;
  char* src = (char*)malloc(n);
  for (size_t i = 0; i < n; i++) {
    src[i] = (char)('a' + (i * 7) % 26);
  }
  ldb_buffer compressed;
  ldb_buffer_init(&compressed);
  CHECK_EQ(1, ldb_snappy_compress(src, n, &compressed));
  size_t ulen = 0;
  CHECK_EQ(1, ldb_snappy_get_uncompressed_length(compressed.data,
                                                 compressed.size, &ulen));
  CHECK_EQ((long long)n, (long long)ulen);
  char* out = (char*)malloc(ulen);
  CHECK_EQ(1, ldb_snappy_uncompress(compressed.data, compressed.size, out,
                                    ulen));
  CHECK_EQ(0, memcmp(out, src, n));
  free(src);
  free(out);
  ldb_buffer_destroy(&compressed);
}

TEST(snappy, Repetitive) {
  // Highly compressible data must shrink by > 12.5% (required for leveldb
  // to store the compressed form).
  size_t n = 10000;
  char* src = (char*)malloc(n);
  memset(src, 'x', n);
  ldb_buffer compressed;
  ldb_buffer_init(&compressed);
  CHECK_EQ(1, ldb_snappy_compress(src, n, &compressed));
  CHECK((long long)compressed.size < (long long)(n - n / 8));
  char* out = (char*)malloc(n);
  CHECK_EQ(1, ldb_snappy_uncompress(compressed.data, compressed.size, out, n));
  CHECK_EQ(0, memcmp(out, src, n));
  free(src);
  free(out);
  ldb_buffer_destroy(&compressed);
}
