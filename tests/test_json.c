#include "jb/json.h"
#include "unity.h"
#include <jb/array.h>
#include <string.h>

static jb_json_token_t tokens[600];
static unsigned char scratch[65537];
static jb_json_doc_t doc;
static jb_json_error_t error;
static jb_json_limits_t limits;

void setUp(void) {
  limits = (jb_json_limits_t){4096, COUNTOF(tokens), 16, 2048, 256, 128, 65536};
  doc = (jb_json_doc_t){0};
  error = (jb_json_error_t){99, 99, 99, 99};
}
void tearDown(void) {}

static jb_json_workspace_t work(void) {
  return (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 1};
}

static hal_status_t parse_bytes(const char *bytes, size_t length) {
  return jb_json_parse((jb_json_span_t){bytes, length}, &limits, tokens, COUNTOF(tokens), work(),
                       &doc, &error);
}

static hal_status_t parse(const char *text) { return parse_bytes(text, strlen(text)); }

static jb_json_node_t root(void) {
  jb_json_node_t node = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_root(&doc, &node));
  return node;
}

static void test_scalar_roots_and_exact_tokens(void) {
  static const struct {
    const char *text;
    jb_json_type_t type;
  } cases[] = {{"null", JB_JSON_NULL},
               {"true", JB_JSON_BOOL},
               {"false", JB_JSON_BOOL},
               {"0", JB_JSON_NUMBER},
               {"-0", JB_JSON_NUMBER},
               {"18446744073709551615", JB_JSON_NUMBER},
               {"1.8446744073709551615e19", JB_JSON_NUMBER},
               {"1.0000000596046447753906250000000000000000001", JB_JSON_NUMBER},
               {"1e1000000000", JB_JSON_NUMBER},
               {"1e-1000000000", JB_JSON_NUMBER},
               {"\"\"", JB_JSON_STRING},
               {"[]", JB_JSON_ARRAY},
               {"{}", JB_JSON_OBJECT}};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, parse(cases[i].text), cases[i].text);
    TEST_ASSERT_TRUE(doc.valid);
    TEST_ASSERT_EQUAL(1, doc.token_count);
    TEST_ASSERT_EQUAL(JB_JSON_ERROR_NONE, error.code);
    TEST_ASSERT_EQUAL(SIZE_MAX, error.related_offset);
    jb_json_type_t type;
    jb_json_span_t raw;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_type(root(), &type));
    TEST_ASSERT_EQUAL_UINT32(cases[i].type, type);
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_raw(root(), &raw));
    TEST_ASSERT_EQUAL(strlen(cases[i].text), raw.length);
    TEST_ASSERT_EQUAL_MEMORY(cases[i].text, raw.data, raw.length);
    TEST_ASSERT_EQUAL(SIZE_MAX, tokens[0].parent);
    TEST_ASSERT_FALSE(tokens[0].has_key);
  }
}

static void test_reject_malformed_grammar(void) {
  static const char *const bad[] = {"",
                                    " ",
                                    "[",
                                    "{",
                                    "[1",
                                    "{\"a\"",
                                    "{\"a\":",
                                    "{\"a\":}",
                                    "[1,]",
                                    "{\"a\":1,}",
                                    "[,1]",
                                    "{,}",
                                    "{a:1}",
                                    "{\"a\" 1}",
                                    "[1 2]",
                                    "[1}",
                                    "{]",
                                    "[}",
                                    "{{}}",
                                    "[1,,2]",
                                    "[1:2]",
                                    "null false",
                                    "trueX",
                                    "null\v",
                                    "null\f",
                                    "//comment\nnull",
                                    "/*comment*/null",
                                    "[/*x*/]",
                                    "NaN",
                                    "Infinity",
                                    "-Infinity",
                                    "True",
                                    "FALSE",
                                    "nil",
                                    "nul",
                                    "truX",
                                    "fals",
                                    "falsX",
                                    "+1",
                                    "01",
                                    "-01",
                                    "-",
                                    ".1",
                                    "1.",
                                    "1e",
                                    "1e+",
                                    "1e-",
                                    "1.e1",
                                    "0x10",
                                    "1_0",
                                    "--1",
                                    "[1e,0]",
                                    "\"abc",
                                    "\"a\n\"",
                                    "\"\\x00\"",
                                    "\"\\v\"",
                                    "\"\\u12x4\"",
                                    "\"\\u123\"",
                                    "\"\\",
                                    "\"\\ud800\"",
                                    "\"\\udfff\"",
                                    "\"\\ud800\\u0000\"",
                                    "\"\\ud800\\ud800\"",
                                    "\"\\udc00\\ud800\"",
                                    "\"\\ud800\\u12"};
  for (size_t i = 0; i < COUNTOF(bad); ++i) {
    TEST_ASSERT_EQUAL_MESSAGE(HAL_EPROTO, parse(bad[i]), bad[i]);
    TEST_ASSERT_FALSE(doc.valid);
    TEST_ASSERT_NULL(doc.tokens);
    TEST_ASSERT_NOT_EQUAL(JB_JSON_ERROR_NONE, error.code);
    TEST_ASSERT_TRUE(error.offset <= strlen(bad[i]));
  }
}

static void test_reject_raw_utf8_surrogates_and_bom(void) {
  static const char *const bad[] = {"\"\xc0\x80\"",  "\"\xed\xa0\x80\"", "\"\xf4\x90\x80\x80\"",
                                    "\"\x80\"",      "\"\xe2\x82\"",     "\"\xff\"",
                                    "\xef\xbb\xbf{}"};
  for (size_t i = 0; i < COUNTOF(bad); ++i) {
    TEST_ASSERT_EQUAL(HAL_EPROTO, parse(bad[i]));
    TEST_ASSERT_EQUAL(i == COUNTOF(bad) - 1 ? JB_JSON_ERROR_SYNTAX : JB_JSON_ERROR_UTF8,
                      error.code);
  }
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"\xef\xbb\xbf\""));
}

static void test_explicit_lengths_nul_and_nonterminated_input(void) {
  const char data[] = {'n', 'u', 'l', 'l', '\0', '[', ']'};
  TEST_ASSERT_EQUAL(HAL_OK, parse_bytes(data, 4));
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse_bytes(data, sizeof(data)));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_TRAILING_DATA, error.code);
  TEST_ASSERT_EQUAL(4, error.offset);
  const char string[] = {'"', 'a', '\0', 'b', '"'};
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse_bytes(string, sizeof(string)));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_SYNTAX, error.code);
  TEST_ASSERT_EQUAL(2, error.offset);
  TEST_ASSERT_EQUAL(HAL_OK, parse_bytes("false!", 5));
}

static void test_structure_offsets_and_constant_time_siblings(void) {
  static const char input[] = "{\"a\":[1,{\"b\":false}],\"z\":null}";
  TEST_ASSERT_EQUAL(HAL_OK, parse(input));
  TEST_ASSERT_EQUAL(6, doc.token_count);
  TEST_ASSERT_EQUAL(sizeof(input) - 1, tokens[0].length);
  TEST_ASSERT_EQUAL(1, tokens[0].first_child);
  TEST_ASSERT_EQUAL(2, tokens[0].child_count);
  TEST_ASSERT_EQUAL(5, tokens[1].offset);
  TEST_ASSERT_EQUAL(15, tokens[1].length);
  TEST_ASSERT_EQUAL(5, tokens[1].next_sibling);
  TEST_ASSERT_EQUAL(2, tokens[1].first_child);
  TEST_ASSERT_EQUAL(3, tokens[2].next_sibling);
  TEST_ASSERT_EQUAL(1, tokens[3].parent);
  TEST_ASSERT_EQUAL(4, tokens[3].first_child);
  TEST_ASSERT_EQUAL(SIZE_MAX, tokens[3].next_sibling);
  jb_json_node_t a, item, b, z;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_first(root(), &a));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_at(a, 1, &item));
  TEST_ASSERT_EQUAL(3, item.index);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(item, (jb_json_span_t){"b", 1}, &b));
  bool value = true;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_bool(b, &value));
  TEST_ASSERT_FALSE(value);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_next(a, &z));
  TEST_ASSERT_EQUAL(5, z.index);
  size_t count = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_count(a, &count));
  TEST_ASSERT_EQUAL(2, count);
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_json_next(z, &item));
  TEST_ASSERT_EQUAL(3, item.index);
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_json_at(a, SIZE_MAX, &item));
}

static void test_empty_and_nul_keys_and_values(void) {
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"\":0,\"a\\u0000b\":\"x\\u0000y\",\"\\u0061\":true}"));
  jb_json_node_t member;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){NULL, 0}, &member));
  TEST_ASSERT_TRUE(tokens[member.index].has_key);
  TEST_ASSERT_EQUAL(0, tokens[member.index].key_length);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"a\0b", 3}, &member));
  char output[4] = {'!', '!', '!', '!'};
  size_t needed = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(member, output, 3, &needed));
  TEST_ASSERT_EQUAL(3, needed);
  TEST_ASSERT_EQUAL_MEMORY("x\0y!", output, sizeof(output));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_key_copy(member, output, 3, &needed));
  TEST_ASSERT_EQUAL_MEMORY("a\0b!", output, sizeof(output));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"a", 1}, &member));
  bool value = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_get_bool(member, &value));
  TEST_ASSERT_TRUE(value);
}

static void test_all_escapes_and_surrogate_pair(void) {
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"\\\"\\\\\\/\\b\\f\\n\\r\\t\\u0000\\uD834\\uDD1E\""));
  static const char expected[] = {'"',  '\\', '/',    '\b',   '\f',   '\n',  '\r',
                                  '\t', '\0', '\xf0', '\x9d', '\x84', '\x9e'};
  char output[32];
  memset(output, '!', sizeof(output));
  size_t needed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(root(), NULL, 0, &needed));
  TEST_ASSERT_EQUAL(sizeof(expected), needed);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_string_copy(root(), output, needed - 1, &needed));
  for (size_t i = 0; i < sizeof(output); ++i) {
    TEST_ASSERT_EQUAL_CHAR('!', output[i]);
  }
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(root(), output, needed, &needed));
  TEST_ASSERT_EQUAL_MEMORY(expected, output, sizeof(expected));
  TEST_ASSERT_EQUAL_CHAR('!', output[needed]);
}

static void test_string_equal_and_cstr(void) {
  TEST_ASSERT_EQUAL(HAL_OK,
                    parse("{\"a\":\"\\u0061\\u20ac\",\"n\":\"x\\u0000y\",\"e\":\"\",\"b\":true}"));
  jb_json_node_t a, n, e, b;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"a", 1}, &a));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"n", 1}, &n));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"e", 1}, &e));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"b", 1}, &b));
  bool equal = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(a, (jb_json_span_t){"a\xe2\x82\xac", 4}, &equal));
  TEST_ASSERT_TRUE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(a, (jb_json_span_t){"a\xe2\x82\xac!", 5}, &equal));
  TEST_ASSERT_FALSE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(a, (jb_json_span_t){"a", 1}, &equal));
  TEST_ASSERT_FALSE(equal);
  /* The comparison value is plain UTF-8, never JSON escape syntax. */
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_string_equal(a, (jb_json_span_t){"\\u0061\xe2\x82\xac", 9}, &equal));
  TEST_ASSERT_FALSE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(n, (jb_json_span_t){"x\0y", 3}, &equal));
  TEST_ASSERT_TRUE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(n, (jb_json_span_t){"x\0z", 3}, &equal));
  TEST_ASSERT_FALSE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(e, (jb_json_span_t){NULL, 0}, &equal));
  TEST_ASSERT_TRUE(equal);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_equal(e, (jb_json_span_t){"", 1}, &equal));
  TEST_ASSERT_FALSE(equal);
  equal = true;
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_string_equal(a, (jb_json_span_t){"a\xff", 2}, &equal));
  /* Invalid bytes after an earlier difference still fail the whole value. */
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_string_equal(a, (jb_json_span_t){"b\xff", 2}, &equal));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_equal(b, (jb_json_span_t){"true", 4}, &equal));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_equal(a, (jb_json_span_t){NULL, 1}, &equal));
  TEST_ASSERT_TRUE(equal);

  char output[8];
  memset(output, '!', sizeof(output));
  size_t needed = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_cstr(a, NULL, 0, &needed));
  TEST_ASSERT_EQUAL(5, needed);
  needed = 99;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_string_cstr(a, output, 4, &needed));
  TEST_ASSERT_EQUAL(5, needed);
  TEST_ASSERT_EQUAL_CHAR('!', output[0]);
  needed = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_cstr(a, output, 5, &needed));
  TEST_ASSERT_EQUAL(5, needed);
  TEST_ASSERT_EQUAL_STRING("a\xe2\x82\xac", output);
  TEST_ASSERT_EQUAL_CHAR('!', output[5]);
  needed = 99;
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_cstr(n, output, sizeof(output), &needed));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_cstr(n, NULL, 0, &needed));
  TEST_ASSERT_EQUAL(99, needed);
  TEST_ASSERT_EQUAL_STRING("a\xe2\x82\xac", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(n, output, sizeof(output), &needed));
  TEST_ASSERT_EQUAL(3, needed);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_string_cstr(e, output, 0, &needed));
  TEST_ASSERT_EQUAL(1, needed);
  output[0] = '!';
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_cstr(e, output, 1, &needed));
  TEST_ASSERT_EQUAL(1, needed);
  TEST_ASSERT_EQUAL_CHAR('\0', output[0]);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_cstr(b, output, sizeof(output), &needed));
  TEST_ASSERT_EQUAL(1, needed);
}

static void test_escape_equivalent_duplicate_names_fail(void) {
  static const char *const bad[] = {"{\"a\":0,\"a\":1}", "{\"a\":0,\"\\u0061\":1}",
                                    "{\"\":0,\"\":1}", "{\"a\\u0000b\":0,\"a\\u0000b\":1}",
                                    "{\"\xf0\x9d\x84\x9e\":0,\"\\ud834\\udd1e\":1}"};
  for (size_t i = 0; i < COUNTOF(bad); ++i) {
    TEST_ASSERT_EQUAL(HAL_EPROTO, parse(bad[i]));
    TEST_ASSERT_EQUAL(JB_JSON_ERROR_DUPLICATE_KEY, error.code);
    TEST_ASSERT_EQUAL(1, error.related_offset);
    TEST_ASSERT_EQUAL_CHAR('"', bad[i][error.offset]);
    TEST_ASSERT_TRUE(error.offset > error.related_offset);
  }
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"a\":{\"a\":0},\"b\":{\"a\":1}}"));
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"\xc3\xa9\":0,\"e\xcc\x81\":1}"));
}

static void test_measure_sizes_but_does_not_accept_duplicates(void) {
  static const char text[] = "{\"a\":[1,2],\"a\":3}";
  const jb_json_span_t input = {text, sizeof(text) - 1};
  jb_json_requirements_t needed = {99, 99};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_measure(input, &limits, work(), &needed, &error));
  TEST_ASSERT_EQUAL(5, needed.node_count);
  TEST_ASSERT_EQUAL(2, needed.depth);
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse_bytes(input.data, input.length));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_DUPLICATE_KEY, error.code);
  TEST_ASSERT_EQUAL(HAL_EPROTO,
                    jb_json_measure((jb_json_span_t){"[", 1}, &limits, work(), &needed, NULL));
  TEST_ASSERT_EQUAL(5, needed.node_count);
  TEST_ASSERT_EQUAL(2, needed.depth);
}

static void test_capacity_failure_invalidates_document(void) {
  TEST_ASSERT_EQUAL(HAL_OK, parse("[1]"));
  const jb_json_span_t input = {"[1]", 3};
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_parse(input, &limits, tokens, 1, work(), &doc, &error));
  TEST_ASSERT_FALSE(doc.valid);
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_TOKEN_CAPACITY, error.code);
  TEST_ASSERT_EQUAL(1, error.offset);
  TEST_ASSERT_EQUAL(2, error.required_capacity);
  jb_json_node_t node = {0};
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_root(&doc, &node));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_parse(input, &limits, NULL, 0, work(), &doc, NULL));
}

static void test_input_node_and_number_limits(void) {
  limits.max_input_bytes = 3;
  TEST_ASSERT_EQUAL(HAL_OK, parse("[0]"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("[0] "));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_LIMIT, error.code);
  limits.max_input_bytes = 4096;
  limits.max_nodes = 2;
  TEST_ASSERT_EQUAL(HAL_OK, parse("[0]"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("[0,1]"));
  limits.max_nodes = 0;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("null"));
  limits.max_nodes = COUNTOF(tokens);
  limits.max_number_bytes = 3;
  TEST_ASSERT_EQUAL(HAL_OK, parse("1e2"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("1e20"));
  limits.max_number_bytes = 0;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("0"));
  TEST_ASSERT_EQUAL(HAL_OK, parse("true"));
}

static void test_depth_and_member_limits(void) {
  limits.max_depth = 1;
  TEST_ASSERT_EQUAL(HAL_OK, parse("[0]"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("[[]]"));
  TEST_ASSERT_EQUAL(1, error.offset);
  limits.max_depth = 0;
  TEST_ASSERT_EQUAL(HAL_OK, parse("null"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("[]"));
  limits.max_depth = 16;
  limits.max_members = 1;
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"a\":0}"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("{\"a\":0,\"b\":1}"));
  limits.max_members = 0;
  TEST_ASSERT_EQUAL(HAL_OK, parse("{}"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("[0]"));
}

static void test_decoded_string_and_key_limits(void) {
  limits.max_string_bytes = 4;
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"\\ud834\\udd1e\""));
  limits.max_string_bytes = 3;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("\"\\ud834\\udd1e\""));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_LIMIT, error.code);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("{\"abcd\":0}"));
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"\\u20ac\""));
  limits.max_string_bytes = 0;
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"\""));
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"\":0}"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("\"\\u0000\""));
}

static void test_key_comparison_budget_includes_empty_keys(void) {
  limits.max_key_compare_bytes = 0;
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"a\":0}"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("{\"a\":0,\"b\":1}"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("{\"\":0,\"\":1}"));
  limits.max_key_compare_bytes = 1;
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse("{\"\":0,\"\":1}"));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_DUPLICATE_KEY, error.code);
  limits.max_key_compare_bytes = 2;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("{\"a\":0,\"b\":1}"));
  limits.max_key_compare_bytes = 3;
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"a\":0,\"b\":1}"));
}

static void test_workspace_exact_size_canaries_and_overflow(void) {
  size_t bytes = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse_workspace_size(&limits, &bytes));
  memset(scratch, 0xa5, sizeof(scratch));
  const jb_json_span_t input = {"[{}]", 4};
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW,
                    jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                                  (jb_json_workspace_t){scratch + 1, bytes - 1}, &doc, &error));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_WORKSPACE, error.code);
  TEST_ASSERT_EQUAL(bytes, error.required_capacity);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                                          (jb_json_workspace_t){scratch + 1, bytes}, &doc, NULL));
  TEST_ASSERT_EQUAL_UINT8(0xa5, scratch[0]);
  TEST_ASSERT_EQUAL_UINT8(0xa5, scratch[bytes + 1]);
  limits.max_depth = SIZE_MAX;
  bytes = 99;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_parse_workspace_size(&limits, &bytes));
  TEST_ASSERT_EQUAL(99, bytes);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, parse("null"));
}

static void test_zero_depth_needs_no_scratch(void) {
  limits.max_depth = 0;
  size_t bytes = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse_workspace_size(&limits, &bytes));
  TEST_ASSERT_EQUAL(0, bytes);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse((jb_json_span_t){"0", 1}, &limits, tokens, 1,
                                          (jb_json_workspace_t){NULL, 0}, &doc, NULL));
}

static void test_deep_nesting_uses_caller_scratch(void) {
  static char input[513];
  memset(input, '[', 256);
  input[256] = '0';
  memset(input + 257, ']', 256);
  limits.max_depth = 256;
  TEST_ASSERT_EQUAL(HAL_OK, parse_bytes(input, sizeof(input)));
  TEST_ASSERT_EQUAL(257, doc.token_count);
  TEST_ASSERT_EQUAL(sizeof(input), tokens[0].length);
  TEST_ASSERT_EQUAL(255, tokens[256].parent);
  jb_json_requirements_t requirements;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_measure((jb_json_span_t){input, sizeof(input)}, &limits, work(),
                                            &requirements, NULL));
  TEST_ASSERT_EQUAL(256, requirements.depth);
  TEST_ASSERT_EQUAL(257, requirements.node_count);
}

static void test_scratch_reuse_and_independent_documents(void) {
  static jb_json_token_t other_tokens[4];
  TEST_ASSERT_EQUAL(HAL_OK, parse("{\"a\":\"abc\"}"));
  jb_json_doc_t other;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse((jb_json_span_t){"[true]", 6}, &limits, other_tokens,
                                          COUNTOF(other_tokens), work(), &other, NULL));
  memset(scratch, 0, sizeof(scratch));
  jb_json_node_t member;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_find(root(), (jb_json_span_t){"a", 1}, &member));
  char output[3];
  size_t bytes;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(member, output, sizeof(output), &bytes));
  TEST_ASSERT_EQUAL_MEMORY("abc", output, 3);
  TEST_ASSERT_EQUAL(2, other.token_count);
  TEST_ASSERT_EQUAL(JB_JSON_BOOL, other.tokens[1].type);
}

static void test_read_errors_leave_outputs_unchanged(void) {
  TEST_ASSERT_EQUAL(HAL_OK, parse("[]"));
  jb_json_node_t node = {&doc, 0};
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_json_first(root(), &node));
  TEST_ASSERT_EQUAL(0, node.index);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_find(root(), (jb_json_span_t){"x", 1}, &node));
  size_t bytes = 99;
  bool boolean = true;
  jb_json_span_t span = {NULL, 99};
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_get_bool(root(), &boolean));
  TEST_ASSERT_TRUE(boolean);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_number_token(root(), &span));
  TEST_ASSERT_EQUAL(99, span.length);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_key_copy(root(), NULL, 0, &bytes));
  TEST_ASSERT_EQUAL(99, bytes);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_cstr(root(), NULL, 0, &bytes));
  TEST_ASSERT_EQUAL(99, bytes);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_string_equal(root(), (jb_json_span_t){"x", 1}, &boolean));
  TEST_ASSERT_TRUE(boolean);
  node.index = doc.token_count;
  jb_json_type_t type = 99;
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_type(node, &type));
  TEST_ASSERT_EQUAL_UINT32(99, type);
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_string_cstr(node, NULL, 0, &bytes));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_json_string_equal(node, (jb_json_span_t){"x", 1}, &boolean));
  TEST_ASSERT_EQUAL(99, bytes);
  TEST_ASSERT_TRUE(boolean);
  TEST_ASSERT_EQUAL(HAL_OK, parse("{}"));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_find(root(), (jb_json_span_t){"\xff", 1}, &node));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_json_find(root(), (jb_json_span_t){"x", 1}, &node));
  TEST_ASSERT_EQUAL(HAL_OK, parse("1e400"));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_number_token(root(), &span));
  TEST_ASSERT_EQUAL_MEMORY("1e400", span.data, span.length);
}

static void test_required_arguments(void) {
  const jb_json_span_t input = {"0", 1};
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_json_parse(input, NULL, tokens, COUNTOF(tokens), work(), &doc, &error));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_NONE, error.code);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_parse(input, &limits, NULL, 1, work(), &doc, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                                              (jb_json_workspace_t){NULL, 1}, &doc, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_parse((jb_json_span_t){NULL, 1}, &limits, tokens,
                                              COUNTOF(tokens), work(), &doc, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_json_parse(input, &limits, tokens, COUNTOF(tokens), work(), NULL, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_measure(input, &limits, work(), NULL, NULL));
  size_t bytes;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_parse_workspace_size(NULL, &bytes));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_parse_workspace_size(&limits, NULL));
  TEST_ASSERT_EQUAL(HAL_OK, parse("\"a\""));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_copy(root(), NULL, 1, &bytes));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_copy(root(), NULL, 0, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_cstr(root(), NULL, 1, &bytes));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_cstr(root(), NULL, 0, NULL));
  bool equal;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_equal(root(), input, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_string_equal(root(), (jb_json_span_t){NULL, 1}, &equal));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_root(NULL, &(jb_json_node_t){0}));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_root(&doc, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_raw(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_type(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_count(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_first(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_next(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_at(root(), 0, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_find(root(), input, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_number_token(root(), NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_get_bool(root(), NULL));
}

static void test_byte_locations_and_first_diagnostics(void) {
  size_t line = 99, column = 99;
  const jb_json_span_t input = {"\xc2\xb5\r\n[1,]", 9};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_locate(input, 2, &line, &column));
  TEST_ASSERT_EQUAL(1, line);
  TEST_ASSERT_EQUAL(3, column);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_locate(input, input.length, &line, &column));
  TEST_ASSERT_EQUAL(2, line);
  TEST_ASSERT_EQUAL(6, column);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_locate(input, 10, &line, &column));
  TEST_ASSERT_EQUAL(2, line);
  TEST_ASSERT_EQUAL(6, column);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_locate(input, 0, NULL, &column));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_locate(input, 0, &line, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_locate((jb_json_span_t){NULL, 1}, 0, &line, &column));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_locate((jb_json_span_t){NULL, 0}, 0, &line, &column));
  TEST_ASSERT_EQUAL(1, line);
  TEST_ASSERT_EQUAL(1, column);
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse("[1,]"));
  TEST_ASSERT_EQUAL(3, error.offset);
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_SYNTAX, error.code);
  TEST_ASSERT_EQUAL(HAL_EPROTO, parse("[1"));
  TEST_ASSERT_EQUAL(2, error.offset);
}

static void test_missing_outputs_take_precedence_over_limits(void) {
  limits.max_input_bytes = 0;
  const jb_json_span_t input = {"0", 1};
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_json_parse(input, &limits, tokens, COUNTOF(tokens), work(), NULL, &error));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_NONE, error.code);
  error.code = 99;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_measure(input, &limits, work(), NULL, &error));
  TEST_ASSERT_EQUAL(JB_JSON_ERROR_NONE, error.code);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_scalar_roots_and_exact_tokens);
  RUN_TEST(test_reject_malformed_grammar);
  RUN_TEST(test_reject_raw_utf8_surrogates_and_bom);
  RUN_TEST(test_explicit_lengths_nul_and_nonterminated_input);
  RUN_TEST(test_structure_offsets_and_constant_time_siblings);
  RUN_TEST(test_empty_and_nul_keys_and_values);
  RUN_TEST(test_all_escapes_and_surrogate_pair);
  RUN_TEST(test_string_equal_and_cstr);
  RUN_TEST(test_escape_equivalent_duplicate_names_fail);
  RUN_TEST(test_measure_sizes_but_does_not_accept_duplicates);
  RUN_TEST(test_capacity_failure_invalidates_document);
  RUN_TEST(test_input_node_and_number_limits);
  RUN_TEST(test_depth_and_member_limits);
  RUN_TEST(test_decoded_string_and_key_limits);
  RUN_TEST(test_key_comparison_budget_includes_empty_keys);
  RUN_TEST(test_workspace_exact_size_canaries_and_overflow);
  RUN_TEST(test_zero_depth_needs_no_scratch);
  RUN_TEST(test_deep_nesting_uses_caller_scratch);
  RUN_TEST(test_scratch_reuse_and_independent_documents);
  RUN_TEST(test_read_errors_leave_outputs_unchanged);
  RUN_TEST(test_required_arguments);
  RUN_TEST(test_byte_locations_and_first_diagnostics);
  RUN_TEST(test_missing_outputs_take_precedence_over_limits);
  return UNITY_END();
}
