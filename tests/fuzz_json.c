#include "jb/json.h"
#include "jb/utf8.h"
#include <hal/core/hal_array.h>
#include <stdlib.h>

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
        size_t required;
        if (jb_json_string_copy((jb_json_node_t){&doc, i}, output, sizeof(output), &required) !=
                HAL_OK ||
            jb_utf8_validate(output, required, NULL) != HAL_OK) {
          abort();
        }
      }
    }
  } else if (doc.valid) {
    abort();
  }
  return 0;
}
