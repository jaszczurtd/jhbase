#ifndef JB_TEST_SUPPORT_H
#define JB_TEST_SUPPORT_H
#include "jb/json.h"
#include "unity.h"
#include <jb/array.h>
#include <string.h>

typedef struct {
  jb_json_doc_t doc;
  jb_json_token_t tokens[128];
  unsigned char scratch[4097];
} test_document_t;

static inline jb_json_node_t test_parse(const char *text, test_document_t *d) {
  const jb_json_limits_t limits = {4096, COUNTOF(d->tokens), 16, 2048, 2048, 128, 65536};
  const jb_json_span_t input = {text, strlen(text)};
  const jb_json_workspace_t work = {d->scratch + 1, sizeof(d->scratch) - 1};
  TEST_ASSERT_EQUAL(
      HAL_OK, jb_json_parse(input, &limits, d->tokens, COUNTOF(d->tokens), work, &d->doc, NULL));
  jb_json_node_t root;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_root(&d->doc, &root));
  return root;
}
#endif
