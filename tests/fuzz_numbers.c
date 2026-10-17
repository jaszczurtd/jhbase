#include "jb/json_number.h"
#include "jb/literal.h"
#include <stdlib.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static unsigned char scratch[65538];
  jb_json_token_t token;
  jb_json_doc_t doc;
  const jb_json_limits_t limits = {2048, 1, 0, 0, 2048, 0, 0};
  if (jb_json_parse((jb_json_span_t){(const char *)data, size}, &limits, &token, 1,
                    (jb_json_workspace_t){NULL, 0}, &doc, NULL) != HAL_OK ||
      token.type != JB_JSON_NUMBER) {
    return 0;
  }
  const jb_json_node_t node = {&doc, 0};
  const jb_json_workspace_t work = {scratch + 1, sizeof(scratch) - 2};
  uint64_t u;
  int64_t i;
  bool integral;
  const hal_status_t us = jb_json_get_u64(node, &u), is = jb_json_get_i64(node, &i);
  if (jb_json_number_is_integer(node, &integral) != HAL_OK ||
      ((us == HAL_OK || is == HAL_OK) && !integral) ||
      (us == HAL_OK && is == HAL_OK && (i < 0 || (uint64_t)i != u))) {
    abort();
  }
  const jb_json_float_options_t options = {false, false, 1000000};
  uint32_t small;
  uint64_t bits;
  (void)jb_json_get_f32_bits(node, &options, work, &small);
  (void)jb_json_get_f64_bits(node, &options, work, &bits);
  char literal[64];
  size_t required;
  const jb_c_literal_type_t type = size != 0 && (data[0] & 1) != 0 ? JB_C_F32 : JB_C_F64;
  (void)jb_c_literal(node, type, work, 1000000, literal, sizeof(literal), &required);
  int order = 99;
  const hal_status_t compared = jb_json_number_compare(node, node, work, 1000000, &order);
  if (compared == HAL_OK && order != 0) {
    abort();
  }
  const jb_json_float_options_t no_work = {false, false, 0};
  bits = 123;
  if (jb_json_get_f64_bits(node, &no_work, work, &bits) != HAL_EOVERFLOW || bits != 123) {
    abort();
  }
  return 0;
}
