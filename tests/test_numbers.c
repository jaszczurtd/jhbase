#include "jb/json_number.h"
#include "test_support.h"
#include <fenv.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static test_document_t documents[2];
static unsigned char scratch[65538];
static jb_json_workspace_t work(void) {
  return (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 2};
}
void setUp(void) {}
void tearDown(void) {}

static void test_exact_unsigned(void) {
  static const struct {
    const char *text;
    hal_status_t status;
    uint64_t value;
  } cases[] = {{"18446744073709551615", HAL_OK, UINT64_MAX},
               {"1.8446744073709551615e19", HAL_OK, UINT64_MAX},
               {"18446744073709551615.000", HAL_OK, UINT64_MAX},
               {"1e2", HAL_OK, 100},
               {"100.00", HAL_OK, 100},
               {"-0e999999999999999999", HAL_OK, 0},
               {"1.5", HAL_ETYPE, 0},
               {"1e-999999999999999999", HAL_ETYPE, 0},
               {"18446744073709551616", HAL_EOVERFLOW, 0},
               {"-1", HAL_EOVERFLOW, 0},
               {"1e999999999999999999", HAL_EOVERFLOW, 0}};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    uint64_t result = 123;
    TEST_ASSERT_EQUAL(cases[i].status,
                      jb_json_get_u64(test_parse(cases[i].text, &documents[0]), &result));
    TEST_ASSERT_EQUAL_UINT64(cases[i].status == HAL_OK ? cases[i].value : 123, result);
  }
}
static void test_exact_signed(void) {
  static const struct {
    const char *text;
    hal_status_t status;
    int64_t value;
  } cases[] = {
      {"-9223372036854775808", HAL_OK, INT64_MIN},     {"9223372036854775807", HAL_OK, INT64_MAX},
      {"-9.223372036854775808e18", HAL_OK, INT64_MIN}, {"-12.00", HAL_OK, -12},
      {"9223372036854775808", HAL_EOVERFLOW, 0},       {"-9223372036854775809", HAL_EOVERFLOW, 0},
      {"-1.000000000000000000000001", HAL_ETYPE, 0}};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    int64_t result = 123;
    TEST_ASSERT_EQUAL(cases[i].status,
                      jb_json_get_i64(test_parse(cases[i].text, &documents[0]), &result));
    TEST_ASSERT_EQUAL_INT64(cases[i].status == HAL_OK ? cases[i].value : 123, result);
  }
}
static void test_integrality(void) {
  static const char *const integers[] = {"0e-99999999999999999999", "1e999999999999999999999",
                                         "1.00e2", "10e-1", "-0.0"};
  for (size_t i = 0; i < COUNTOF(integers); ++i) {
    bool result = false;
    TEST_ASSERT_EQUAL(HAL_OK,
                      jb_json_number_is_integer(test_parse(integers[i], &documents[0]), &result));
    TEST_ASSERT_TRUE(result);
  }
  bool result = true;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_number_is_integer(
                                test_parse("1.1e-99999999999999999999", &documents[0]), &result));
  TEST_ASSERT_FALSE(result);
}
static void test_decimal_comparison(void) {
  static const struct {
    const char *a, *b;
    int order;
  } cases[] = {{"100.0", "1e2", 0},
               {"-0e999999999999999999999", "0e-999999999999999999999", 0},
               {"9007199254740992.0", "9007199254740993.0", -1},
               {"-1.1", "-1.01", -1},
               {"10e999999999999999999999998", "1e999999999999999999999999", 0},
               {"10e-999999999999999999999999", "1e-999999999999999999999998", 0},
               {"1e999999999999999999999999", "2e999999999999999999999999", -1},
               {"1e-999999999999999999999999", "1e-999999999999999999999998", -1},
               {"0", "-1", 1},
               {"1", "-1", 1},
               {"-2e-1", "-0.20000000000000000001", 1},
               {"10e-1", "1", 0},
               {"1e000000000000000000000", "1", 0}};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    const jb_json_node_t a = test_parse(cases[i].a, &documents[0]);
    const jb_json_node_t b = test_parse(cases[i].b, &documents[1]);
    int result = 99;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_number_compare(a, b, work(), 1000000, &result));
    TEST_ASSERT_EQUAL_INT(cases[i].order, result);
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_number_compare(b, a, work(), 1000000, &result));
    TEST_ASSERT_EQUAL_INT(-cases[i].order, result);
  }
}
static void test_comparison_against_bounded_integer_arithmetic(void) {
  uint32_t random = 3629;
  for (size_t iteration = 0; iteration < 1000; ++iteration) {
    random = random * 1664525u + 1013904223u;
    const uint32_t a = random % 1000000u;
    const int ae = (int)(random % 13u) - 6;
    const bool an = (random & 1u) != 0;
    random = random * 1664525u + 1013904223u;
    const uint32_t b = random % 1000000u;
    const int be = (int)(random % 13u) - 6;
    const bool bn = (random & 1u) != 0;
    char left[48], right[48];
    (void)snprintf(left, sizeof(left), "%s%" PRIu32 "e%d", an ? "-" : "", a, ae);
    (void)snprintf(right, sizeof(right), "%s%" PRIu32 "e%d", bn ? "-" : "", b, be);
    int64_t av = a, bv = b;
    const int floor = ae < be ? ae : be;
    for (int i = floor; i < ae; ++i) {
      av *= 10;
    }
    for (int i = floor; i < be; ++i) {
      bv *= 10;
    }
    if (an) {
      av = -av;
    }
    if (bn) {
      bv = -bv;
    }
    const int expected = av == bv ? 0 : (av < bv ? -1 : 1);
    const jb_json_node_t lhs = test_parse(left, &documents[0]);
    const jb_json_node_t rhs = test_parse(right, &documents[1]);
    int order = 99;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_number_compare(lhs, rhs, work(), 1000000, &order));
    TEST_ASSERT_EQUAL_INT(expected, order);
  }
}

static void test_ieee_fixed_vectors(void) {
  static const struct {
    const char *token;
    unsigned width;
    uint64_t bits;
  } cases[] = {{"0.22", 32, UINT64_C(0x3e6147ae)},
               {"0.22", 64, UINT64_C(0x3fcc28f5c28f5c29)},
               {"1.000000059604644775390625", 32, UINT64_C(0x3f800000)},
               {"1.000000178813934326171875", 32, UINT64_C(0x3f800002)},
               {"1.0000000596046447753906250000000000000000001", 32, UINT64_C(0x3f800001)},
               {"3.4028235e38", 32, UINT64_C(0x7f7fffff)},
               {"1e-45", 32, 1},
               {"4e-324", 64, 1},
               {"1.7976931348623157e308", 64, UINT64_C(0x7fefffffffffffff)},
               {"2.2250738585072014e-308", 64, UINT64_C(0x0010000000000000)},
               {"9007199254740993", 64, UINT64_C(0x4340000000000000)}};
  const jb_json_float_options_t options = {true, true, 10000000};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    const jb_json_node_t node = test_parse(cases[i].token, &documents[0]);
    uint64_t bits = 0;
    if (cases[i].width == 32) {
      uint32_t b;
      TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f32_bits(node, &options, work(), &b));
      bits = b;
    } else {
      TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(node, &options, work(), &bits));
    }
    TEST_ASSERT_EQUAL_HEX64(cases[i].bits, bits);
  }
}
static void test_ieee_errors_and_zero_policy(void) {
  jb_json_float_options_t options = {true, false, 10000000};
  static const char *const overflow[] = {"3.5e38", "1e400", "1e99999999999999999999999"};
  for (size_t i = 0; i < COUNTOF(overflow); ++i) {
    uint32_t bits = 123;
    TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_get_f32_bits(test_parse(overflow[i], &documents[0]),
                                                          &options, work(), &bits));
    TEST_ASSERT_EQUAL_UINT32(123, bits);
  }
  uint64_t bits = 123;
  jb_json_node_t node = test_parse("-1e-9999999999999999999999999", &documents[0]);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_get_f64_bits(node, &options, work(), &bits));
  options.reject_nonzero_to_zero = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(node, &options, work(), &bits));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x8000000000000000), bits);
  options.normalize_negative_zero = true;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(node, &options, work(), &bits));
  TEST_ASSERT_EQUAL_UINT64(0, bits);
  node = test_parse("-0e9999999999999999999999", &documents[0]);
  options.reject_nonzero_to_zero = true;
  options.normalize_negative_zero = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(node, &options, work(), &bits));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x8000000000000000), bits);
}
static void test_scratch_budget_and_arguments(void) {
  const jb_json_node_t node = test_parse("1.25", &documents[0]);
  size_t needed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_float_workspace_size(node, 64, &needed));
  TEST_ASSERT_LESS_THAN(sizeof(scratch) - 2, needed);
  memset(scratch, 0xa5, sizeof(scratch));
  const jb_json_workspace_t exact = {scratch + 1, needed};
  jb_json_float_options_t options = {true, false, 1000000};
  uint64_t bits = 77;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(node, &options, exact, &bits));
  TEST_ASSERT_EQUAL_UINT8(0xa5, scratch[0]);
  TEST_ASSERT_EQUAL_UINT8(0xa5, scratch[needed + 1]);
  bits = 77;
  TEST_ASSERT_EQUAL(
      HAL_EOVERFLOW,
      jb_json_get_f64_bits(node, &options, (jb_json_workspace_t){scratch + 1, needed - 1}, &bits));
  TEST_ASSERT_EQUAL_UINT64(77, bits);
  options.max_work = 0;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_get_f64_bits(node, &options, exact, &bits));
  TEST_ASSERT_EQUAL_UINT64(77, bits);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_get_f64_bits(node, NULL, exact, &bits));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_float_workspace_size(node, 16, &needed));
  const jb_json_node_t boolean = test_parse("true", &documents[1]);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_get_u64(boolean, &bits));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_get_u64(boolean, NULL));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_get_u64((jb_json_node_t){0}, &bits));
  int order = 77;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_number_compare(node, node, exact, 0, &order));
  TEST_ASSERT_EQUAL_INT(77, order);
}
static void test_native_and_fpu_mode(void) {
  const jb_json_node_t node =
      test_parse("1.0000000596046447753906250000000000000000001", &documents[0]);
  const jb_json_float_options_t options = {true, false, 1000000};
  const int original = fegetround();
  TEST_ASSERT_EQUAL_INT(0, fesetround(FE_DOWNWARD));
  uint32_t bits;
  const hal_status_t status = jb_json_get_f32_bits(node, &options, work(), &bits);
  TEST_ASSERT_EQUAL_INT(0, fesetround(original));
  TEST_ASSERT_EQUAL(HAL_OK, status);
  TEST_ASSERT_EQUAL_HEX32(0x3f800001, bits);
  float value;
  double wide;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f32(node, &options, work(), &value));
  uint32_t native;
  memcpy(&native, &value, sizeof(native));
  TEST_ASSERT_EQUAL_HEX32(bits, native);
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_get_f64(test_parse("1.25", &documents[0]), &options, work(), &wide));
  TEST_ASSERT_EQUAL_DOUBLE(1.25, wide);
}
static void test_against_native_direct_decimal_reads(void) {
  uint32_t random = 8259;
  const jb_json_float_options_t options = {false, false, 10000000};
  for (size_t i = 0; i < 2000; ++i) {
    random = random * 1664525u + 1013904223u;
    char token[64];
    (void)snprintf(token, sizeof(token), "%s%" PRIu32 ".%09" PRIu32 "e%d", (random & 1) ? "-" : "",
                   random % 1000000u, random, (int)(random % 700u) - 350);
    const jb_json_node_t node = test_parse(token, &documents[0]);
    const double reference = strtod(token, NULL);
    const float small_reference = strtof(token, NULL);
    uint64_t bits;
    uint32_t small;
    const hal_status_t status = jb_json_get_f64_bits(node, &options, work(), &bits);
    const hal_status_t small_status = jb_json_get_f32_bits(node, &options, work(), &small);
    if (isinf(reference)) {
      TEST_ASSERT_EQUAL(HAL_EOVERFLOW, status);
    } else {
      uint64_t expected;
      memcpy(&expected, &reference, sizeof(expected));
      TEST_ASSERT_EQUAL(HAL_OK, status);
      TEST_ASSERT_EQUAL_HEX64(expected, bits);
    }
    if (isinf(small_reference)) {
      TEST_ASSERT_EQUAL(HAL_EOVERFLOW, small_status);
    } else {
      uint32_t expected;
      memcpy(&expected, &small_reference, sizeof(expected));
      TEST_ASSERT_EQUAL(HAL_OK, small_status);
      TEST_ASSERT_EQUAL_HEX32(expected, small);
    }
  }
}
int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_exact_unsigned);
  RUN_TEST(test_exact_signed);
  RUN_TEST(test_integrality);
  RUN_TEST(test_decimal_comparison);
  RUN_TEST(test_comparison_against_bounded_integer_arithmetic);
  RUN_TEST(test_ieee_fixed_vectors);
  RUN_TEST(test_ieee_errors_and_zero_policy);
  RUN_TEST(test_scratch_budget_and_arguments);
  RUN_TEST(test_native_and_fpu_mode);
  RUN_TEST(test_against_native_direct_decimal_reads);
  return UNITY_END();
}
