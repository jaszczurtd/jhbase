#include "jb/utf8.h"
#include "unity.h"
#include <jb/array.h>
#include <string.h>
#ifdef JB_TEST_UNICODE
#include "jb/unicode.h"
#include "test_file.h"
#include <stdio.h>
#include <stdlib.h>
#endif

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

#ifdef JB_TEST_UNICODE
static int32_t normalization_work[4096];

static void check_nfc(const char *input, size_t length, const char *expected,
                      size_t expected_length) {
  size_t needed = 0, written = SIZE_MAX;
  const char *text = NULL;
  TEST_ASSERT_EQUAL(HAL_OK, jb_unicode_nfc_measure(input, length, 1000000, &needed));
  TEST_ASSERT_TRUE(needed <= COUNTOF(normalization_work));
  TEST_ASSERT_EQUAL(
      HAL_OK, jb_unicode_nfc(input, length, normalization_work, needed, 1000000, &text, &written));
  TEST_ASSERT_EQUAL(expected_length, written);
  if (written != 0) {
    TEST_ASSERT_EQUAL_MEMORY(expected, text, written);
  }
  TEST_ASSERT_EQUAL_CHAR(0, text[written]);
}

/* Real regression: 2.8.0 adds offsets to NULL in both query paths (UBSan). */
static void test_nfc_query_and_embedded_nul(void) {
  check_nfc("abc", 3, "abc", 3);
  check_nfc("\xc3\xa9", 2, "\xc3\xa9", 2);
  check_nfc("e\xcc\x81", 3, "\xc3\xa9", 2);
  check_nfc(NULL, 0, "", 0);
  check_nfc("a\0e\xcc\x81", 5, "a\0\xc3\xa9", 4);
  /* The unit validator must reject k-OHM SIGN as non-NFC. */
  check_nfc("k\xe2\x84\xa6", 4, "k\xce\xa9", 3);
  /* Compatibility-only characters and case are preserved. */
  check_nfc("A\xef\xac\x81", 4, "A\xef\xac\x81", 4);
}

static void test_nfc_rejects_bad_input_and_resource_limits(void) {
  size_t needed = 42, written = 42;
  const char *text = "old";
  int32_t work[5] = {1, 2, 3, 4, 5};
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_unicode_nfc_measure("a\0\xff", 3, 100, &needed));
  TEST_ASSERT_EQUAL(42, needed);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc_measure(NULL, 1, 100, &needed));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc_measure("a", 1, 100, NULL));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc_measure("a", SIZE_MAX, SIZE_MAX, &needed));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc_measure("abc", 3, 2, &needed));
  TEST_ASSERT_EQUAL(42, needed);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc_measure("abc", 3, 14, &needed));
  TEST_ASSERT_EQUAL(HAL_OK, jb_unicode_nfc_measure("abc", 3, 15, &needed));
  TEST_ASSERT_EQUAL(4, needed);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc("abc", 3, work, 3, 100, &text, &written));
  TEST_ASSERT_NULL(text);
  TEST_ASSERT_EQUAL(0, written);
  TEST_ASSERT_EQUAL_INT32(1, work[0]);
  TEST_ASSERT_EQUAL(
      HAL_EINVAL, jb_unicode_nfc((const char *)work, 1, work, COUNTOF(work), 100, &text, &written));
  int32_t overlap_work[8];
  memset(overlap_work, 'a', sizeof(overlap_work));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc((const char *)overlap_work + 1, 1, overlap_work,
                                               COUNTOF(overlap_work), 100, &text, &written));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc((const char *)overlap_work,
                                               sizeof(overlap_work[0]) + 1, overlap_work + 1,
                                               COUNTOF(overlap_work) - 1, 100, &text, &written));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc("a", 1, NULL, 0, 100, &text, &written));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc("a", 1, work, SIZE_MAX, 100, &text, &written));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc("a", 1, work, 5, 100, NULL, &written));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_nfc("a", 1, work, 5, 100, &text, NULL));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_unicode_nfc("\xff", 1, work, 5, 100, &text, &written));
  TEST_ASSERT_EQUAL(HAL_OK, jb_unicode_nfc_measure(NULL, 0, 0, &needed));
  TEST_ASSERT_EQUAL(1, needed);
  /* A combining-mark run must be bounded before upstream's quadratic sort. */
  static char marks[258];
  marks[0] = 'a';
  for (size_t at = 1; at < sizeof(marks) - 1; at += 2) {
    marks[at] = '\xcc';
    marks[at + 1] = (at % 4 == 1) ? '\x95' : '\x80';
  }
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_unicode_nfc_measure(marks, sizeof(marks) - 1, 1000, &needed));
}

static void test_unicode_properties_for_unit_validation(void) {
  TEST_ASSERT_EQUAL_STRING(JB_EXPECTED_UNICODE, jb_unicode_version());
  static const struct {
    uint32_t scalar;
    bool printable;
    bool space;
  } vectors[] = {{0x20, true, true},     {0x09, false, true},    {0x1c, false, true},
                 {0xa0, false, true},    {0x2028, false, true},  {0x200b, false, false},
                 {0, false, false},      {0x301, true, false},   {0x3a9, true, false},
                 {0x1f600, true, false}, {0xe000, false, false}, {0x10ffff, false, false},
                 {0x1e6c0, true, false}};
  for (size_t i = 0; i < COUNTOF(vectors); ++i) {
    jb_unicode_properties_t properties = {false, false};
    TEST_ASSERT_EQUAL(HAL_OK, jb_unicode_properties(vectors[i].scalar, &properties));
    TEST_ASSERT_EQUAL(vectors[i].printable, properties.printable);
    TEST_ASSERT_EQUAL(vectors[i].space, properties.whitespace);
  }
  jb_unicode_properties_t properties = {true, true};
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_properties(0xd800, &properties));
  TEST_ASSERT_TRUE(properties.printable);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_properties(0x110000, &properties));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_unicode_properties(0, NULL));
}

/* The independent Unicode conformance corpus defines the expected NFC bytes. */
static void test_unicode_normalization_corpus(void) {
  FILE *file = jb_test_file_open(JB_NORMALIZATION_TEST, "rb");
  TEST_ASSERT_NOT_NULL(file);
  static char line[4096], columns[5][1024];
  size_t rows = 0;
  while (fgets(line, (int)sizeof(line), file) != NULL) {
    if (line[0] == '#' || line[0] == '@' || line[0] == '\n' || line[0] == '\r') {
      continue;
    }
    size_t lengths[5] = {0};
    char *at = line;
    for (size_t column = 0; column < COUNTOF(columns); ++column) {
      while (*at != ';') {
        while (*at == ' ') {
          ++at;
        }
        if (*at == ';') {
          break;
        }
        char *end = NULL;
        const unsigned long scalar = strtoul(at, &end, 16);
        TEST_ASSERT_TRUE(end != at && scalar <= 0x10fffful);
        size_t bytes = 0;
        TEST_ASSERT_EQUAL(HAL_OK,
                          jb_utf8_encode((uint32_t)scalar, columns[column] + lengths[column],
                                         sizeof(columns[column]) - lengths[column], &bytes));
        lengths[column] += bytes;
        at = end;
      }
      ++at;
    }
    for (size_t column = 0; column < COUNTOF(columns); ++column) {
      const size_t expected = column < 3 ? 1 : 3;
      check_nfc(columns[column], lengths[column], columns[expected], lengths[expected]);
    }
    ++rows;
  }
  TEST_ASSERT_EQUAL(0, ferror(file));
  TEST_ASSERT_EQUAL(0, fclose(file));
  TEST_ASSERT_TRUE(rows > 10000);
}
#endif

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
#ifdef JB_TEST_UNICODE
  RUN_TEST(test_nfc_query_and_embedded_nul);
  RUN_TEST(test_nfc_rejects_bad_input_and_resource_limits);
  RUN_TEST(test_unicode_properties_for_unit_validation);
  RUN_TEST(test_unicode_normalization_corpus);
#endif
  return UNITY_END();
}
