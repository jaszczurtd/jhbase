#include "jb/json.h"
#include "jb/utf8.h"
#include <jb/array.h>
#include <stdlib.h>
#include <string.h>
#ifdef JB_TEST_UNICODE
#include "jb/unicode.h"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static jb_json_token_t tokens[256];
  static unsigned char scratch[4097];
  static char output[4096];
  const jb_json_limits_t limits = {4096, COUNTOF(tokens), 32, 2048, 256, 128, 65536};
  jb_json_doc_t doc;
  jb_json_error_t error;
  jb_json_requirements_t needed;
  const jb_json_span_t input = {(const char *)data, size};
  const jb_json_workspace_t work = {scratch + 1, sizeof(scratch) - 1};
  const hal_status_t measured = jb_json_measure(input, &limits, work, &needed, &error);
  const hal_status_t parsed =
      jb_json_parse(input, &limits, tokens, COUNTOF(tokens), work, &doc, &error);
  (void)jb_utf8_validate(input.data, input.length, NULL);
#ifdef JB_TEST_UNICODE
  static int32_t unicode_work[4097], unicode_again[4097];
  const char *normalized = NULL, *again = NULL;
  size_t normalized_length = 0, again_length = 0;
  if (jb_unicode_nfc(input.data, input.length, unicode_work, COUNTOF(unicode_work), 1000000,
                     &normalized, &normalized_length) == HAL_OK) {
    if (jb_utf8_validate(normalized, normalized_length, NULL) != HAL_OK ||
        jb_unicode_nfc(normalized, normalized_length, unicode_again, COUNTOF(unicode_again),
                       SIZE_MAX, &again, &again_length) != HAL_OK ||
        normalized_length != again_length || memcmp(normalized, again, again_length) != 0) {
      abort();
    }
  }
#endif
  if (parsed == HAL_OK) {
    if (measured != HAL_OK || needed.node_count != doc.token_count || !doc.valid) {
      abort();
    }
    for (size_t i = 0; i < doc.token_count; ++i) {
      const jb_json_token_t *token = &tokens[i];
      if (token->offset > size || token->length > size - token->offset ||
          (i != 0 && token->parent >= i)) {
        abort();
      }
      size_t count = 0;
      size_t at = token->first_child;
      while (at != JB_JSON_NODE_NONE) {
        if (at <= i || at >= doc.token_count || tokens[at].parent != i ||
            ++count > doc.token_count) {
          abort();
        }
        at = tokens[at].next_sibling;
      }
      if (count != token->child_count) {
        abort();
      }
      if (token->type == JB_JSON_STRING) {
        const jb_json_node_t node = {&doc, i};
        size_t required, terminated;
        bool same = false;
        if (jb_json_string_copy(node, output, sizeof(output), &required) != HAL_OK ||
            jb_utf8_validate(output, required, NULL) != HAL_OK ||
            jb_json_string_equal(node, (jb_json_span_t){output, required}, &same) != HAL_OK ||
            !same) {
          abort();
        }
        /* A decoded NUL has no C string; otherwise cstr adds exactly the terminator. */
        const bool has_nul = memchr(output, 0, required) != NULL;
        const hal_status_t cstr = jb_json_string_cstr(node, output, sizeof(output), &terminated);
        if (has_nul ? cstr != HAL_ETYPE
                    : (cstr != HAL_OK || terminated != required + 1 || output[required] != '\0')) {
          abort();
        }
      }
    }
  } else if (doc.valid) {
    abort();
  }
  return 0;
}
