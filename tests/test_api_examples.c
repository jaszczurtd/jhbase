#include "test_support.h"
#include JB_DOC_EXAMPLES

static test_document_t document;
void setUp(void) {}
void tearDown(void) {}

static void test_utf8_example(void) {
  char bytes[8];
  memset(bytes, '!', sizeof(bytes));
  const char input[] = {(char)0xe2, (char)0x82, (char)0xac, 'A'};
  size_t length = 99, bad = 99;
  TEST_ASSERT_EQUAL(
      HAL_OK, example_first_scalar(input, sizeof(input), bytes, sizeof(bytes), &length, &bad));
  TEST_ASSERT_EQUAL(3, length);
  TEST_ASSERT_EQUAL(SIZE_MAX, bad);
  TEST_ASSERT_EQUAL_MEMORY(input, bytes, length);
  TEST_ASSERT_EQUAL_CHAR('!', bytes[length]);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    example_first_scalar(input, sizeof(input), bytes, 2, &length, &bad));
  const char broken[] = {'A', (char)0xff};
  TEST_ASSERT_EQUAL(HAL_EPROTO, example_first_scalar(broken, sizeof(broken), bytes, sizeof(bytes),
                                                     &length, &bad));
  TEST_ASSERT_EQUAL(1, bad);
}
static void test_boolean_and_measure_examples(void) {
  bool value = false;
  static const char input[] = "{\"enabled\":true}";
  TEST_ASSERT_EQUAL(HAL_OK, example_read_enabled(input, sizeof(input) - 1, &value));
  TEST_ASSERT_TRUE(value);
  static const char wrong[] = "{\"enabled\":\"true\"}";
  TEST_ASSERT_EQUAL(HAL_ETYPE, example_read_enabled(wrong, sizeof(wrong) - 1, &value));
  TEST_ASSERT_TRUE(value);
  TEST_ASSERT_EQUAL(HAL_ENOENT, example_read_enabled("{}", 2, &value));
  size_t count = 99;
  static const char tree[] = "{\"a\":[1,2],\"b\":null}";
  TEST_ASSERT_EQUAL(HAL_OK, example_measured_parse(tree, sizeof(tree) - 1, &count));
  TEST_ASSERT_EQUAL(5, count);
  static const char duplicate[] = "{\"a\":1,\"a\":2}";
  TEST_ASSERT_EQUAL(HAL_EPROTO, example_measured_parse(duplicate, sizeof(duplicate) - 1, &count));
  TEST_ASSERT_EQUAL(5, count);
}
static void test_array_examples(void) {
  (void)test_parse("{\"flags\":[true,false,true]}", &document);
  size_t count = 99, trues = 99;
  TEST_ASSERT_EQUAL(HAL_OK, example_flags_summary(&document.doc, &count, &trues));
  TEST_ASSERT_EQUAL(3, count);
  TEST_ASSERT_EQUAL(2, trues);
  bool flag = true;
  TEST_ASSERT_EQUAL(HAL_OK, example_flag_at(&document.doc, 1, &flag));
  TEST_ASSERT_FALSE(flag);
  TEST_ASSERT_EQUAL(HAL_ENOENT, example_flag_at(&document.doc, 3, &flag));
  (void)test_parse("{\"flags\":[true,1]}", &document);
  count = 99;
  trues = 98;
  TEST_ASSERT_EQUAL(HAL_ETYPE, example_flags_summary(&document.doc, &count, &trues));
  TEST_ASSERT_EQUAL(99, count);
  TEST_ASSERT_EQUAL(98, trues);
}
static void test_number_text_example(void) {
  (void)test_parse("{\"id\":1.8446744073709551615e19}", &document);
  char bytes[64];
  memset(bytes, '!', sizeof(bytes));
  size_t length = 99;
  static const char expected[] = "1.8446744073709551615e19";
  TEST_ASSERT_EQUAL(HAL_OK, example_number_text(&document.doc, bytes, sizeof(bytes), &length));
  TEST_ASSERT_EQUAL(sizeof(expected) - 1, length);
  TEST_ASSERT_EQUAL_MEMORY(expected, bytes, length);
  TEST_ASSERT_EQUAL_CHAR('!', bytes[length]);
  length = 99;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, example_number_text(&document.doc, bytes, 3, &length));
  TEST_ASSERT_EQUAL(99, length);
  (void)test_parse("{\"id\":\"1\"}", &document);
  TEST_ASSERT_EQUAL(HAL_ETYPE, example_number_text(&document.doc, bytes, sizeof(bytes), &length));
}
static void test_string_and_diagnostic_examples(void) {
  (void)test_parse("{\"label\":\"A\\u0000B\"}", &document);
  TEST_ASSERT_EQUAL(HAL_OK, example_print_label(&document.doc));
  (void)test_parse("{\"label\":true}", &document);
  TEST_ASSERT_EQUAL(HAL_ETYPE, example_print_label(&document.doc));
  TEST_ASSERT_EQUAL(HAL_EPROTO, example_json_error());
}
int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_utf8_example);
  RUN_TEST(test_boolean_and_measure_examples);
  RUN_TEST(test_array_examples);
  RUN_TEST(test_number_text_example);
  RUN_TEST(test_string_and_diagnostic_examples);
  return UNITY_END();
}
