#include "jb/utf8.h"
#include "unity.h"
#include <hal/core/hal_array.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

typedef struct {
  const char *bytes;
  size_t length;
  uint32_t scalar;
} vector_t;

/* Independent boundary vectors from the ranges in RFC 3629 section 4. */
static const vector_t valid[] = {{"\0", 1, 0},
                                 {"A", 1, 0x41},
                                 {"\x7f", 1, 0x7f},
                                 {"\xc2\x80", 2, 0x80},
                                 {"\xdf\xbf", 2, 0x7ff},
                                 {"\xe0\xa0\x80", 3, 0x800},
                                 {"\xed\x9f\xbf", 3, 0xd7ff},
                                 {"\xee\x80\x80", 3, 0xe000},
                                 {"\xef\xbf\xbf", 3, 0xffff},
                                 {"\xf0\x90\x80\x80", 4, 0x10000},
                                 {"\xf4\x8f\xbf\xbf", 4, 0x10ffff},
                                 {"\xc5\x82", 2, 0x142},
                                 {"\xe2\x82\xac", 3, 0x20ac},
                                 {"\xf0\x9f\x98\x80", 4, 0x1f600}};

static void test_decode_known_scalars(void) {
  for (size_t i = 0; i < COUNTOF(valid); ++i) {
    uint32_t scalar = UINT32_MAX;
    size_t consumed = SIZE_MAX;
    TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_decode(valid[i].bytes, valid[i].length, &scalar, &consumed));
    TEST_ASSERT_EQUAL_UINT32(valid[i].scalar, scalar);
    TEST_ASSERT_EQUAL(valid[i].length, consumed);
  }
}

static void test_encode_known_bytes(void) {
  for (size_t i = 0; i < COUNTOF(valid); ++i) {
    char output[5] = {'!', '!', '!', '!', '!'};
    size_t required = 99;
    TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_encode(valid[i].scalar, output, sizeof(output), &required));
    TEST_ASSERT_EQUAL(valid[i].length, required);
    TEST_ASSERT_EQUAL_MEMORY(valid[i].bytes, output, required);
    TEST_ASSERT_EQUAL_CHAR('!', output[required]);
  }
}

static void test_all_valid_prefix_truncations_fail(void) {
  for (size_t i = 0; i < COUNTOF(valid); ++i) {
    for (size_t length = 0; length < valid[i].length; ++length) {
      uint32_t scalar = 42;
      size_t consumed = 42;
      TEST_ASSERT_EQUAL(HAL_EPROTO, jb_utf8_decode(valid[i].bytes, length, &scalar, &consumed));
      TEST_ASSERT_EQUAL_UINT32(42, scalar);
      TEST_ASSERT_EQUAL(42, consumed);
    }
  }
}

static void test_reject_invalid_byte_sequences(void) {
  static const struct {
    const char *bytes;
    size_t length;
  } bad[] = {{"\x80", 1},
             {"\xbf", 1},
             {"\xc0\x80", 2},
             {"\xc1\xbf", 2},
             {"\xe0\x80\x80", 3},
             {"\xe0\x9f\xbf", 3},
             {"\xf0\x80\x80\x80", 4},
             {"\xf0\x8f\xbf\xbf", 4},
             {"\xed\xa0\x80", 3},
             {"\xed\xbf\xbf", 3},
             {"\xf4\x90\x80\x80", 4},
             {"\xf5\x80\x80\x80", 4},
             {"\xf8\x88\x80\x80\x80", 5},
             {"\xfc\x84\x80\x80\x80\x80", 6},
             {"\xfe", 1},
             {"\xff", 1},
             {"\xc2 ", 2},
             {"\xe1\x80 ", 3},
             {"\xf1\x80\x80 ", 4},
             {"\xc2\xc2", 2}};
  for (size_t i = 0; i < COUNTOF(bad); ++i) {
    uint32_t scalar = 42;
    size_t consumed = 42, error = 42;
    TEST_ASSERT_EQUAL(HAL_EPROTO, jb_utf8_decode(bad[i].bytes, bad[i].length, &scalar, &consumed));
    TEST_ASSERT_EQUAL_UINT32(42, scalar);
    TEST_ASSERT_EQUAL(42, consumed);
    TEST_ASSERT_EQUAL(HAL_EPROTO, jb_utf8_validate(bad[i].bytes, bad[i].length, &error));
    TEST_ASSERT_EQUAL(0, error);
  }
}

static void test_all_surrogates_and_out_of_range_scalars_fail(void) {
  char output[4] = {'!', '!', '!', '!'};
  size_t required = 42;
  for (uint32_t scalar = 0xd800; scalar <= 0xdfff; ++scalar) {
    TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_encode(scalar, output, sizeof(output), &required));
    TEST_ASSERT_EQUAL(42, required);
    TEST_ASSERT_EQUAL_MEMORY("!!!!", output, 4);
  }
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_encode(0x110000, output, 4, &required));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_encode(UINT32_MAX, output, 4, &required));
}

static void test_capacity_is_transactional_and_measurement_is_exact(void) {
  for (size_t i = 0; i < COUNTOF(valid); ++i) {
    size_t required = 99;
    TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_encode(valid[i].scalar, NULL, 0, &required));
    TEST_ASSERT_EQUAL(valid[i].length, required);
    char output[4] = {'!', '!', '!', '!'};
    TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                      jb_utf8_encode(valid[i].scalar, output, required - 1, &required));
    TEST_ASSERT_EQUAL_MEMORY("!!!!", output, sizeof(output));
    TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_encode(valid[i].scalar, output, required, &required));
  }
}

static void test_nul_is_data_and_suffix_is_validated(void) {
  const char data[] = {'a', '\0', '\xc2', '\xa0', '\xff'};
  size_t error = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_validate(data, 4, &error));
  TEST_ASSERT_EQUAL(SIZE_MAX, error);
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_utf8_validate(data, sizeof(data), &error));
  TEST_ASSERT_EQUAL(4, error);
  uint32_t scalar;
  size_t consumed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_decode(data, sizeof(data), &scalar, &consumed));
  TEST_ASSERT_EQUAL_UINT32('a', scalar);
  TEST_ASSERT_EQUAL(1, consumed);
}

static void test_argument_validation_and_empty_input(void) {
  uint32_t scalar = 42;
  size_t consumed = 42, error = 42;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_decode(NULL, 1, &scalar, &consumed));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_decode("a", 1, NULL, &consumed));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_decode("a", 1, &scalar, NULL));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_utf8_decode(NULL, 0, &scalar, &consumed));
  TEST_ASSERT_EQUAL_UINT32(42, scalar);
  TEST_ASSERT_EQUAL(42, consumed);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_encode(0, NULL, 1, &consumed));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_encode(0, NULL, 0, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_utf8_validate(NULL, 1, &error));
  TEST_ASSERT_EQUAL(42, error);
  TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_validate(NULL, 0, &error));
  TEST_ASSERT_EQUAL(SIZE_MAX, error);
  TEST_ASSERT_EQUAL(HAL_OK, jb_utf8_validate("abc", 3, NULL));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_decode_known_scalars);
  RUN_TEST(test_encode_known_bytes);
  RUN_TEST(test_all_valid_prefix_truncations_fail);
  RUN_TEST(test_reject_invalid_byte_sequences);
  RUN_TEST(test_all_surrogates_and_out_of_range_scalars_fail);
  RUN_TEST(test_capacity_is_transactional_and_measurement_is_exact);
  RUN_TEST(test_nul_is_data_and_suffix_is_validated);
  RUN_TEST(test_argument_validation_and_empty_input);
  return UNITY_END();
}
