#include "core_json.h"
#include "jb/json.h"
#include "yyjson.h"
#include <jb/array.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static jb_json_token_t tokens[256];
static unsigned char scratch[4097];
static max_align_t pool[8192];
static const jb_json_limits_t limits = {4096, COUNTOF(tokens), 32, 2048, 2048, 128, 1000000};
typedef bool (*parse_fn)(jb_json_span_t input);
static bool native_parse(jb_json_span_t input) {
  jb_json_doc_t doc;
  return jb_json_parse(input, &limits, tokens, COUNTOF(tokens),
                       (jb_json_workspace_t){scratch + 1, sizeof(scratch) - 1}, &doc,
                       NULL) == HAL_OK;
}
static bool core_parse(jb_json_span_t input) {
  return JSON_Validate(input.data, input.length) == JSONSuccess;
}
static bool yy_parse(jb_json_span_t input) {
  yyjson_alc allocator;
  if (!yyjson_alc_pool_init(&allocator, pool, sizeof(pool))) {
    return false;
  }
  yyjson_doc *doc = yyjson_read_opts((char *)(void *)input.data, input.length,
                                     YYJSON_READ_NUMBER_AS_RAW, &allocator, NULL);
  if (doc == NULL) {
    return false;
  }
  yyjson_doc_free(doc);
  return true;
}

static int vectors(void) {
  static const struct {
    const char *name, *input;
    size_t length;
    bool accept;
  } cases[] = {{"scalar", "null", 4, true},
               {"full-u64", "18446744073709551615", 20, true},
               {"unbounded-number", "1e400", 5, true},
               {"nul-string", "\"a\\u0000b\"", 10, true},
               {"empty-key", "{\"\":0}", 6, true},
               {"surrogate-pair", "\"\\ud83d\\ude00\"", 14, true},
               {"duplicate-key", "{\"a\":1,\"a\":2}", 13, false},
               {"escaped-duplicate", "{\"a\":1,\"\\u0061\":2}", 18, false},
               {"nul-duplicate", "{\"\\u0000\":1,\"\\u0000\":2}", 23, false},
               {"lone-high-surrogate", "\"\\ud800\"", 8, false},
               {"lone-low-surrogate", "\"\\udc00\"", 8, false},
               {"overlong-utf8", "\"\xc0\x80\"", 4, false},
               {"utf8-surrogate", "\"\xed\xa0\x80\"", 5, false},
               {"truncated-utf8", "\"\xf0\x9f\"", 4, false},
               {"raw-nul", "\"a\0b\"", 5, false},
               {"suffix-after-nul", "[]\0x", 4, false},
               {"bom", "\xef\xbb\xbf{}", 5, false},
               {"leading-zero", "01", 2, false},
               {"trailing-comma", "[1,]", 4, false},
               {"missing-exponent", "1e+", 3, false},
               {"extra-root", "{}[]", 4, false},
               {"unclosed", "[[", 2, false}};
  int failed = 0;
  puts("case,required,jhbase,coreJSON,yyjson");
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    const jb_json_span_t input = {cases[i].input, cases[i].length};
    const bool a = native_parse(input), b = core_parse(input), c = yy_parse(input);
    printf("%s,%d,%d,%d,%d\n", cases[i].name, cases[i].accept, a, b, c);
    if (a != cases[i].accept) {
      failed = 1;
    }
  }
  char deep[66];
  memset(deep, '[', 33);
  memset(deep + 33, ']', 33);
  const jb_json_span_t input = {deep, sizeof(deep)};
  const bool too_deep = native_parse(input);
  printf("depth-33,0,%d,%d,%d\n", too_deep, core_parse(input), yy_parse(input));
  if (too_deep) {
    failed = 1;
  }
  return failed;
}

static void benchmark(void) {
  static const char sample[] =
      "{\"id\":18446744073709551615,\"name\":\"\\u00b5\",\"values\":[1e400,true,null,{},[1,2,3]]}";
  static const struct {
    const char *name;
    parse_fn parse;
  } engines[] = {{"jhbase-full-token-parse", native_parse},
                 {"coreJSON-validation-only", core_parse},
                 {"yyjson-raw-pool-dom", yy_parse}};
  const jb_json_span_t input = {sample, sizeof(sample) - 1};
  for (size_t i = 0; i < COUNTOF(engines); ++i) {
    size_t accepted = 0;
    const clock_t begin = clock();
    for (size_t iteration = 0; iteration < 100000; ++iteration) {
      accepted += engines[i].parse(input) ? 1u : 0u;
    }
    const double seconds = (double)(clock() - begin) / CLOCKS_PER_SEC;
    printf("timing,%s,100000,%.6f,%zu\n", engines[i].name, seconds, accepted);
  }
  size_t needed;
  (void)jb_json_parse_workspace_size(&limits, &needed);
  printf("memory,jhbase-parser-scratch,%zu\n", needed);
  printf("memory,jhbase-token-record,%zu\n", sizeof(jb_json_token_t));
  printf("memory,yyjson-conservative-pool-bound,%zu\n",
         yyjson_read_max_memory_usage(input.length, YYJSON_READ_NUMBER_AS_RAW));
}
int main(void) {
  const int result = vectors();
  benchmark();
  return result;
}
