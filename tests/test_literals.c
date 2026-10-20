#include "jb/literal.h"
#include "test_file.h"
#include "test_support.h"
#include <inttypes.h>
#include <stdio.h>

static char fixture[131072];
static jb_json_token_t fixture_tokens[8192];
static unsigned char parse_scratch[65537], number_scratch[65537];
static jb_json_doc_t fixture_doc;
static test_document_t scalar;
static const char *fixture_path;
static jb_json_workspace_t work(void) {
  return (jb_json_workspace_t){number_scratch + 1, sizeof(number_scratch) - 1};
}
void setUp(void) {}
void tearDown(void) {}
static jb_json_node_t member(jb_json_node_t object, const char *name) {
  jb_json_node_t value;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(object, (jb_json_span_t){name, strlen(name)}, &value));
  return value;
}
static void text(jb_json_node_t node, char *out, size_t capacity) {
  size_t n;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(node, out, capacity - 1, &n));
  out[n] = 0;
}
static jb_c_literal_type_t selector(const char *name) {
  static const char *const names[] = {"bool",  "uint8", "uint16", "uint32", "uint64", "int8",
                                      "int16", "int32", "int64",  "float",  "double", "string"};
  size_t i = 0;
  while (i < COUNTOF(names) && strcmp(names[i], name) != 0) {
    ++i;
  }
  TEST_ASSERT_LESS_THAN_MESSAGE(COUNTOF(names), i, "Unknown fixture cType");
  return (jb_c_literal_type_t)i;
}
static jb_json_node_t cases(void) {
  FILE *file = jb_test_file_open(fixture_path, "rb");
  TEST_ASSERT_NOT_NULL(file);
  const size_t n = fread(fixture, 1, sizeof(fixture), file);
  const int error = ferror(file);
  const int ended = feof(file);
  fclose(file);
  TEST_ASSERT_EQUAL_INT(0, error);
  TEST_ASSERT_TRUE(ended);
  const jb_json_limits_t limits = {sizeof(fixture), COUNTOF(fixture_tokens), 64, 65536, 2048, 8192,
                                   100000000};
  TEST_ASSERT_EQUAL(
      HAL_OK,
      jb_json_parse((jb_json_span_t){fixture, n}, &limits, fixture_tokens, COUNTOF(fixture_tokens),
                    (jb_json_workspace_t){parse_scratch + 1, sizeof(parse_scratch) - 1},
                    &fixture_doc, NULL));
  jb_json_node_t root;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_root(&fixture_doc, &root));
  return member(root, "numericCases");
}
static jb_json_node_t input(jb_json_node_t item) {
  jb_json_node_t value = member(item, "input"), encoding;
  if (jb_json_find(item, (jb_json_span_t){"inputEncoding", 13}, &encoding) == HAL_OK) {
    char token[2049];
    text(value, token, sizeof(token));
    /* test_parse borrows input, so retain this token beyond the helper call. */
    static char stable[2049];
    memcpy(stable, token, strlen(token) + 1);
    value = test_parse(stable, &scalar);
  }
  return value;
}
static void test_hal_numeric_fixture(void) {
  const jb_json_node_t list = cases();
  size_t count;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_count(list, &count));
  TEST_ASSERT_GREATER_THAN(0, count);
  for (size_t i = 0; i < count; ++i) {
    jb_json_node_t item;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_at(list, i, &item));
    char type[16];
    text(member(item, "cType"), type, sizeof(type));
    const jb_c_literal_type_t selected = selector(type);
    const jb_json_node_t value = input(item);
    char actual[128];
    size_t length = 77;
    jb_json_node_t error;
    const hal_status_t status =
        jb_c_literal(value, selected, work(), 10000000, actual, sizeof(actual), &length);
    if (jb_json_find(item, (jb_json_span_t){"error", 5}, &error) == HAL_OK) {
      TEST_ASSERT_EQUAL(HAL_EOVERFLOW, status);
      TEST_ASSERT_EQUAL(77, length);
      continue;
    }
    TEST_ASSERT_EQUAL(HAL_OK, status);
    char expected[128];
    text(member(item, "literal"), expected, sizeof(expected));
    TEST_ASSERT_EQUAL(strlen(expected), length);
    TEST_ASSERT_EQUAL_MEMORY(expected, actual, length);
    jb_json_node_t hash;
    if (jb_json_find(item, (jb_json_span_t){"hashValue", 9}, &hash) == HAL_OK) {
      uint64_t bits;
      const jb_json_float_options_t options = {true, true, 10000000};
      if (selected == JB_C_F32) {
        uint32_t small;
        TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f32_bits(value, &options, work(), &small));
        bits = small;
      } else {
        TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_f64_bits(value, &options, work(), &bits));
      }
      char hex[17];
      (void)snprintf(hex, sizeof(hex), "%0*" PRIx64, selected == JB_C_F32 ? 8 : 16, bits);
      text(hash, expected, sizeof(expected));
      TEST_ASSERT_EQUAL_STRING(expected, hex);
    }
  }
}
static void test_string_and_negative_literals(void) {
  static const struct {
    const char *input;
    jb_c_literal_type_t type;
    const char *expected;
  } vectors[] = {{"\"a\\\"b\\\\c\\n\\u00b5A?\"", JB_C_STRING, "\"a\\\"b\\\\c\\012\\302\\265A\\?\""},
                 {"\"\\u00001\"", JB_C_STRING, "\"\\0001\""},
                 {"-0.22", JB_C_F32, "(-2.19999999e-01f)"},
                 {"-1", JB_C_F64, "(-1.0000000000000000e+00)"},
                 {"-5", JB_C_I64, "(-5LL)"},
                 {"7", JB_C_I16, "7"}};
  for (size_t i = 0; i < COUNTOF(vectors); ++i) {
    const jb_json_node_t node = test_parse(vectors[i].input, &scalar);
    char result[128];
    size_t length;
    TEST_ASSERT_EQUAL(HAL_OK, jb_c_literal(node, vectors[i].type, work(), 10000000, result,
                                           sizeof(result), &length));
    TEST_ASSERT_EQUAL(strlen(vectors[i].expected), length);
    TEST_ASSERT_EQUAL_MEMORY(vectors[i].expected, result, length);
  }
}
static void test_literal_errors_and_transactional_output(void) {
  jb_json_node_t node = test_parse("0.22", &scalar);
  char output[128];
  memset(output, 'x', sizeof(output));
  size_t length = 77;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    jb_c_literal(node, JB_C_F32, work(), 1000000, output, 1, &length));
  TEST_ASSERT_EQUAL(15, length);
  TEST_ASSERT_EACH_EQUAL_CHAR('x', output, sizeof(output));
  length = 77;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    jb_c_literal(node, JB_C_F32, work(), 0, output, sizeof(output), &length));
  TEST_ASSERT_EQUAL(77, length);
  TEST_ASSERT_EQUAL(HAL_ETYPE,
                    jb_c_literal(node, JB_C_U64, work(), 1000000, output, sizeof(output), &length));
  TEST_ASSERT_EQUAL(
      HAL_ETYPE, jb_c_literal(node, JB_C_BOOL, work(), 1000000, output, sizeof(output), &length));
  node = test_parse("256", &scalar);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    jb_c_literal(node, JB_C_U8, work(), 1000000, output, sizeof(output), &length));
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_c_literal(node, 99, work(), 1000000, output, sizeof(output), &length));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_c_literal(node, JB_C_U64, work(), 1000000, NULL, 1, &length));
}
static int emit_probe(const char *path) {
  FILE *file = jb_test_file_open(path, "wb");
  if (file == NULL) {
    return 2;
  }
  fputs("#include <stdint.h>\n#include <string.h>\nint main(void) {\n", file);
  const jb_json_node_t list = cases();
  size_t count = 0;
  (void)jb_json_count(list, &count);
  for (size_t i = 0; i < count; ++i) {
    jb_json_node_t item, hash;
    (void)jb_json_at(list, i, &item);
    if (jb_json_find(item, (jb_json_span_t){"hashValue", 9}, &hash) != HAL_OK) {
      continue;
    }
    char type[16], hex[17], literal[128];
    size_t length;
    text(member(item, "cType"), type, sizeof(type));
    text(hash, hex, sizeof(hex));
    if (jb_c_literal(input(item), selector(type), work(), 10000000, literal, sizeof(literal) - 1,
                     &length) != HAL_OK) {
      fclose(file);
      return 3;
    }
    literal[length] = 0;
    const unsigned width = strcmp(type, "float") == 0 ? 32u : 64u;
    fprintf(file,
            "{ %s value = %s; uint%u_t bits; memcpy(&bits, &value, sizeof(bits)); "
            "if (bits != UINT%u_C(0x%s)) return %u; }\n",
            type, literal, width, width, hex, (unsigned)i + 1);
  }
  fputs("return 0;\n}\n", file);
  return fclose(file) == 0 ? 0 : 4;
}
int main(int argc, char **argv) {
  if (argc == 4 && strcmp(argv[1], "--emit-probe") == 0) {
    fixture_path = argv[3];
    return emit_probe(argv[2]);
  }
  if (argc != 2) {
    fputs("Expected HAL cases.json path\n", stderr);
    return 2;
  }
  fixture_path = argv[1];
  UNITY_BEGIN();
  RUN_TEST(test_hal_numeric_fixture);
  RUN_TEST(test_string_and_negative_literals);
  RUN_TEST(test_literal_errors_and_transactional_output);
  return UNITY_END();
}
