// test_batch.c - port of leveldb's write_batch_test (core scenarios)
#include "harness.h"

#include <assert.h>

TEST(write_batch, Empty) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  CHECK_EQ(0, (long long)ldb_write_batch_count(&batch));
  CHECK_STATUS_OK(ldb_write_batch_insert_into(&batch, NULL, NULL));
  ldb_write_batch_destroy(&batch);
}

static void batch_put_handler(void* state, const ldb_slice* k,
                              const ldb_slice* v) {
  ldb_buffer* out = (ldb_buffer*)state;
  ldb_buffer_append_str(out, "Put(");
  ldb_buffer_append(out, k->data, k->size);
  ldb_buffer_append_str(out, ", ");
  ldb_buffer_append(out, v->data, v->size);
  ldb_buffer_append_str(out, ")\n");
}

static void batch_delete_handler(void* state, const ldb_slice* k) {
  ldb_buffer* out = (ldb_buffer*)state;
  ldb_buffer_append_str(out, "Delete(");
  ldb_buffer_append(out, k->data, k->size);
  ldb_buffer_append_str(out, ")\n");
}

TEST(write_batch, Multiple) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_write_batch_clear(&batch);
  ldb_slice k1 = ldb_slice_str("foo");
  ldb_slice v1 = ldb_slice_str("bar");
  ldb_write_batch_put(&batch, &k1, &v1);
  ldb_slice k2 = ldb_slice_str("box");
  ldb_write_batch_put(&batch, &k2, &v1);
  ldb_slice k3 = ldb_slice_str("baz");
  ldb_write_batch_delete(&batch, &k3);
  CHECK_EQ(3, (long long)ldb_write_batch_count(&batch));

  ldb_buffer state;
  ldb_buffer_init(&state);
  ldb_write_batch_iterate(&batch, &state, batch_put_handler,
                          batch_delete_handler);
  char* text = (char*)malloc(state.size + 1);
  memcpy(text, state.data, state.size);
  text[state.size] = 0;
  CHECK_STR_CONTAINS(text, "Put(foo, bar)");
  CHECK_STR_CONTAINS(text, "Put(box, bar)");
  CHECK_STR_CONTAINS(text, "Delete(baz)");
  free(text);
  ldb_buffer_destroy(&state);
  ldb_write_batch_destroy(&batch);
}

TEST(write_batch, InsertIntoMemTable) {
  ldb_ikc icmp;
  ldb_ikc_init(&icmp, ldb_bytewise_comparator());
  ldb_memtable* mem = ldb_memtable_new(&icmp);
  ldb_memtable_ref(mem);

  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_write_batch_clear(&batch);
  ldb_write_batch_set_sequence(&batch, 100);
  ldb_slice k1 = ldb_slice_str("k1");
  ldb_slice v1 = ldb_slice_str("v1");
  ldb_write_batch_put(&batch, &k1, &v1);
  ldb_slice k2 = ldb_slice_str("k2");
  ldb_slice v2 = ldb_slice_str("v2");
  ldb_write_batch_put(&batch, &k2, &v2);
  ldb_slice k3 = ldb_slice_str("k3");
  ldb_write_batch_delete(&batch, &k3);
  CHECK_EQ(3, (long long)ldb_write_batch_count(&batch));

  uint64_t last_seq = 0;
  CHECK_STATUS_OK(ldb_write_batch_insert_into(&batch, mem, &last_seq));
  CHECK_EQ(102, (long long)last_seq);

  // Verify contents
  ldb_buffer value;
  ldb_buffer_init(&value);
  ldb_status s = ldb_status_ok();
  {
    ldb_lookup_key lk;
    ldb_lookup_key_init(&lk, &k1, 1000);
    CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));
    CHECK_BUF_EQ("v1", value);
    ldb_lookup_key_destroy(&lk);
  }
  {
    ldb_lookup_key lk;
    ldb_lookup_key_init(&lk, &k2, 1000);
    CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));
    CHECK_BUF_EQ("v2", value);
    ldb_lookup_key_destroy(&lk);
  }
  {
    ldb_lookup_key lk;
    ldb_lookup_key_init(&lk, &k3, 1000);
    CHECK_EQ(1, ldb_memtable_get(mem, &lk, &value, &s));
    CHECK_EQ(0, ldb_ok(s));  // deleted
    ldb_status_destroy(&s);
    s = ldb_status_ok();
  }
  ldb_buffer_destroy(&value);
  ldb_memtable_unref(mem);
  ldb_write_batch_destroy(&batch);
}

TEST(write_batch, Sequence) {
  ldb_write_batch batch;
  ldb_write_batch_init(&batch);
  ldb_write_batch_clear(&batch);
  CHECK_EQ(0, (long long)ldb_write_batch_sequence(&batch));
  ldb_write_batch_set_sequence(&batch, 42);
  CHECK_EQ(42, (long long)ldb_write_batch_sequence(&batch));
  ldb_slice k = ldb_slice_str("x");
  ldb_slice v = ldb_slice_str("y");
  ldb_write_batch_put(&batch, &k, &v);
  CHECK_EQ(42, (long long)ldb_write_batch_sequence(&batch));
  ldb_write_batch_set_sequence(&batch, 7);
  CHECK_EQ(7, (long long)ldb_write_batch_sequence(&batch));
  ldb_write_batch_destroy(&batch);
}

TEST(write_batch, Append) {
  ldb_write_batch b1, b2;
  ldb_write_batch_init(&b1);
  ldb_write_batch_init(&b2);
  ldb_write_batch_clear(&b1);
  ldb_write_batch_clear(&b2);

  // Append empty to empty
  ldb_write_batch_append(&b1, &b2);
  CHECK_EQ(0, (long long)ldb_write_batch_count(&b1));

  // b1 = put a
  ldb_slice ka = ldb_slice_str("a");
  ldb_slice va = ldb_slice_str("va");
  ldb_write_batch_put(&b1, &ka, &va);
  // append empty b2 to b1
  ldb_write_batch_append(&b1, &b2);
  CHECK_EQ(1, (long long)ldb_write_batch_count(&b1));

  // b2 = put b, delete c
  ldb_slice kb = ldb_slice_str("b");
  ldb_slice vb = ldb_slice_str("vb");
  ldb_write_batch_put(&b2, &kb, &vb);
  ldb_slice kc = ldb_slice_str("c");
  ldb_write_batch_delete(&b2, &kc);
  ldb_write_batch_append(&b1, &b2);
  CHECK_EQ(3, (long long)ldb_write_batch_count(&b1));

  ldb_buffer state;
  ldb_buffer_init(&state);
  ldb_write_batch_iterate(&b1, &state, batch_put_handler,
                          batch_delete_handler);
  char* text = (char*)malloc(state.size + 1);
  memcpy(text, state.data, state.size);
  text[state.size] = '\0';
  CHECK_STR_CONTAINS(text, "Put(a, va)");
  CHECK_STR_CONTAINS(text, "Put(b, vb)");
  CHECK_STR_CONTAINS(text, "Delete(c)");
  free(text);
  ldb_buffer_destroy(&state);
  ldb_write_batch_destroy(&b1);
  ldb_write_batch_destroy(&b2);
}
