#include "jb/json_writer.h"
#include "test_support.h"
#include <math.h>
#include <stdlib.h>

static unsigned char scratch[32769], other_scratch[32769];
static char output[8192];
static jb_json_buffer_t buffer;
static jb_json_writer_t writer;
static jb_json_writer_limits_t limits;
static test_document_t document;
void setUp(void) {
  limits = (jb_json_writer_limits_t){sizeof(output), 16, 128, 4096, 65536};
  buffer = (jb_json_buffer_t){output, sizeof(output), 0};
  writer = (jb_json_writer_t){0};
  memset(output, 0xa5, sizeof(output));
}
void tearDown(void) {}
static void start(void) {
  buffer.used = 0;
  memset(output, 0xa5, sizeof(output));
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_writer_init(&writer, &limits,
                                        (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 1},
                                        jb_json_buffer_sink, &buffer));
}
static void expect(const char *text) {
  size_t length = 123;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&writer, &length));
  TEST_ASSERT_EQUAL(strlen(text), length);
  TEST_ASSERT_EQUAL(length, buffer.used);
  TEST_ASSERT_EQUAL_MEMORY(text, output, length);
  TEST_ASSERT_EQUAL_HEX8(0xa5, output[length]);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&writer, &length));
}
static void test_sequential_values(void) {
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){"items", 5}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_u64(&writer, UINT64_MAX));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_i64(&writer, INT64_MIN));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_bool(&writer, true));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_bool(&writer, false));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_number(&writer, (jb_json_span_t){"1.000e400", 9}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_end(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_end(&writer));
  expect("{\"items\":[18446744073709551615,-9223372036854775808,true,false,null,1.000e400]}");
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_null(&writer));
}
static void test_strings_and_empty_root(void) {
  start();
  static const char value[] = {'a', 0, '\n', '"', '\\', (char)0xc2, (char)0xb5};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_string(&writer, (jb_json_span_t){value, sizeof(value)}));
  expect("\"a\\u0000\\u000a\\\"\\\\\xc2\xb5\"");
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_string(&writer, (jb_json_span_t){NULL, 0}));
  expect("\"\"");
}
static void test_keys_are_copied_and_scoped(void) {
  limits.max_keys = 2;
  limits.max_key_bytes = 2;
  start();
  char name = 'a';
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){&name, 1}));
  name = 'b';
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){"a", 1}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_end(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){&name, 1}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_end(&writer));
  expect("{\"a\":{\"a\":null},\"b\":null}");
  start();
  name = 'a';
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){&name, 1}));
  name = 'b';
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_json_writer_key(&writer, (jb_json_span_t){"a", 1}));
  const size_t used = buffer.used;
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_json_writer_object_end(&writer));
  TEST_ASSERT_EQUAL(used, buffer.used);
}
static void test_empty_and_nul_duplicate_names(void) {
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){NULL, 0}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_json_writer_key(&writer, (jb_json_span_t){NULL, 0}));
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){"a\0b", 3}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_json_writer_key(&writer, (jb_json_span_t){"a\0b", 3}));
}
static void test_limits_and_sizing(void) {
  size_t needed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_workspace_size(&limits, &needed));
  memset(scratch, 0xa5, sizeof(scratch));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    jb_json_writer_init(&writer, &limits,
                                        (jb_json_workspace_t){scratch + 1, needed - 1},
                                        jb_json_buffer_sink, &buffer));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_init(&writer, &limits,
                                                (jb_json_workspace_t){scratch + 1, needed},
                                                jb_json_buffer_sink, &buffer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  expect("null");
  TEST_ASSERT_EQUAL_HEX8(0xa5, scratch[0]);
  TEST_ASSERT_EQUAL_HEX8(0xa5, scratch[needed + 1]);
  limits.max_depth = SIZE_MAX;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_workspace_size(&limits, &needed));
  limits.max_depth = 0;
  start();
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_array_begin(&writer));
  limits.max_depth = 1;
  limits.max_output_bytes = 4;
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  expect("null");
  limits.max_output_bytes = 3;
  start();
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(0, buffer.used);
  limits.max_output_bytes = sizeof(output);
  limits.max_key_compare_bytes = 0;
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){"a", 1}));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&writer));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_key(&writer, (jb_json_span_t){"b", 1}));
}
static void test_state_errors_and_input_validation(void) {
  start();
  size_t size;
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_finish(&writer, &size));
  start();
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_key(&writer, (jb_json_span_t){"x", 1}));
  start();
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_array_end(&writer));
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_null(&writer));
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_object_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_key(&writer, (jb_json_span_t){"x", 1}));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_writer_object_end(&writer));
  static const char *const invalid[] = {"", "01", "1.", "1e", "+1", "1 ", " 1", "true"};
  for (size_t i = 0; i < COUNTOF(invalid); ++i) {
    start();
    TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_writer_number(
                                      &writer, (jb_json_span_t){invalid[i], strlen(invalid[i])}));
    TEST_ASSERT_EQUAL(0, buffer.used);
  }
  start();
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_writer_string(&writer, (jb_json_span_t){"\xc0\x80", 2}));
  start();
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_writer_f64(&writer, INFINITY));
  start();
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_writer_f32(&writer, NAN));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_writer_null(NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_json_writer_init(&writer, &limits, (jb_json_workspace_t){NULL, 1},
                                        jb_json_buffer_sink, &buffer));
}
typedef struct {
  size_t calls;
  hal_status_t failure;
} failing_sink_t;
static hal_status_t fail_after_one(void *user, const char *data, size_t length) {
  failing_sink_t *sink = user;
  (void)data;
  (void)length;
  return sink->calls++ == 0 ? HAL_OK : sink->failure;
}
static void test_sticky_sink_errors(void) {
  static const hal_status_t errors[] = {HAL_EAGAIN, HAL_EIO, HAL_NONE, (hal_status_t)-1000};
  for (size_t i = 0; i < COUNTOF(errors); ++i) {
    failing_sink_t sink = {0, errors[i]};
    TEST_ASSERT_EQUAL(HAL_OK,
                      jb_json_writer_init(&writer, &limits,
                                          (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 1},
                                          fail_after_one, &sink));
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_begin(&writer));
    const hal_status_t expected = i < 2 ? errors[i] : HAL_EINTERNAL;
    TEST_ASSERT_EQUAL(expected, jb_json_writer_null(&writer));
    TEST_ASSERT_EQUAL(expected, jb_json_writer_array_end(&writer));
    size_t size = 123;
    TEST_ASSERT_EQUAL(expected, jb_json_writer_finish(&writer, &size));
    TEST_ASSERT_EQUAL(123, size);
    TEST_ASSERT_EQUAL(2, sink.calls);
  }
}
static void test_buffer_and_count_sinks(void) {
  char bytes[3] = {'x', 'x', 'x'};
  jb_json_buffer_t b = {bytes, sizeof(bytes), 0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_buffer_sink(&b, "ab", 2));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_buffer_sink(&b, "cd", 2));
  TEST_ASSERT_EQUAL(2, b.used);
  TEST_ASSERT_EQUAL_MEMORY("abx", bytes, 3);
  size_t count = SIZE_MAX - 1;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_count_sink(&count, "ab", 2));
  TEST_ASSERT_EQUAL(SIZE_MAX - 1, count);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_count_sink(&count, "a", 1));
  TEST_ASSERT_EQUAL(SIZE_MAX, count);
  count = 0;
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_writer_init(&writer, &limits,
                                        (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 1},
                                        jb_json_count_sink, &count));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_string(&writer, (jb_json_span_t){"a\0b", 3}));
  size_t size;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&writer, &size));
  TEST_ASSERT_EQUAL(10, size);
  TEST_ASSERT_EQUAL(size, count);
}
static void test_parsed_subtree(void) {
  jb_json_node_t node =
      test_parse("{\"a\":[1e400,{},[\"\\u0000\\u00b5\"]],\"\\u0062\":true}", &document);
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_node(&writer, node));
  expect("{\"a\":[1e400,{},[\"\\u0000\xc2\xb5\"]],\"b\":true}");
  jb_json_node_t array;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(node, (jb_json_span_t){"a", 1}, &array));
  limits.max_depth = 2;
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_writer_node(&writer, array));
}
static void test_float_roundtrips_and_compact_spelling(void) {
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_f64(&writer, -0.0));
  expect("-0");
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_f32(&writer, 0.22f));
  expect("0.22");
  start();
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_f64(&writer, 1000000.0));
  expect("1e6");
  uint64_t random = 8259;
  for (size_t i = 0; i < 1000; ++i) {
    random = random * UINT64_C(6364136223846793005) + 1;
    const uint64_t bits = random & UINT64_C(0xffefffffffffffff);
    double value;
    memcpy(&value, &bits, sizeof(value));
    start();
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_f64(&writer, value));
    size_t count;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&writer, &count));
    output[count] = 0;
    const double reference = strtod(output, NULL);
    uint64_t read_bits;
    memcpy(&read_bits, &reference, sizeof(read_bits));
    TEST_ASSERT_EQUAL_HEX64(bits, read_bits);
    const uint32_t small_bits = (uint32_t)random & UINT32_C(0xff7fffff);
    float small;
    memcpy(&small, &small_bits, sizeof(small));
    start();
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_f32(&writer, small));
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&writer, &count));
    output[count] = 0;
    const float small_reference = strtof(output, NULL);
    uint32_t small_read;
    memcpy(&small_read, &small_reference, sizeof(small_read));
    TEST_ASSERT_EQUAL_HEX32(small_bits, small_read);
  }
}
static void test_independent_writers(void) {
  start();
  jb_json_writer_t second;
  size_t count = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_init(
                                &second, &limits,
                                (jb_json_workspace_t){other_scratch + 1, sizeof(other_scratch) - 1},
                                jb_json_count_sink, &count));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_begin(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_null(&second));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_i64(&writer, -1));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_array_end(&writer));
  expect("[-1]");
  size_t n;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_writer_finish(&second, &n));
  TEST_ASSERT_EQUAL(4, n);
}
int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_sequential_values);
  RUN_TEST(test_strings_and_empty_root);
  RUN_TEST(test_keys_are_copied_and_scoped);
  RUN_TEST(test_empty_and_nul_duplicate_names);
  RUN_TEST(test_limits_and_sizing);
  RUN_TEST(test_state_errors_and_input_validation);
  RUN_TEST(test_sticky_sink_errors);
  RUN_TEST(test_buffer_and_count_sinks);
  RUN_TEST(test_parsed_subtree);
  RUN_TEST(test_float_roundtrips_and_compact_spelling);
  RUN_TEST(test_independent_writers);
  return UNITY_END();
}
