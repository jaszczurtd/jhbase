#include "jb/json_number.h"
#include "jb/literal.h"
#include <inttypes.h>
#include <jb/array.h>
#include <stdio.h>
#include <string.h>

int main(void) {
  static const char input[] = "{\"id\":18446744073709551615,\"gain\":0.22}";
  static jb_json_token_t tokens[3];
  static unsigned char parser_scratch[256], number_scratch[4096];
  const jb_json_limits_t limits = {sizeof(input) - 1, COUNTOF(tokens), 1, 8, 32, 2, 64};
  const jb_json_workspace_t parser_work = {parser_scratch, sizeof(parser_scratch)};
  const jb_json_workspace_t number_work = {number_scratch, sizeof(number_scratch)};
  const jb_json_span_t source = {input, sizeof(input) - 1}, id_key = {"id", 2},
                       gain_key = {"gain", 4};
  jb_json_doc_t doc;
  size_t needed;
  if (jb_json_parse_workspace_size(&limits, &needed) != HAL_OK || needed > parser_work.size ||
      jb_json_parse(source, &limits, tokens, COUNTOF(tokens), parser_work, &doc, NULL) != HAL_OK) {
    return 1;
  }
  jb_json_node_t root, id, gain;
  if (jb_json_root(&doc, &root) != HAL_OK || jb_json_find(root, id_key, &id) != HAL_OK ||
      jb_json_find(root, gain_key, &gain) != HAL_OK) {
    return 2;
  }
  uint64_t identifier;
  uint32_t gain_bits;
  const jb_json_float_options_t options = {true, true, 1000000};
  if (jb_json_get_u64(id, &identifier) != HAL_OK ||
      jb_json_float_workspace_size(gain, 32, &needed) != HAL_OK || needed > number_work.size ||
      jb_json_get_f32_bits(gain, &options, number_work, &gain_bits) != HAL_OK) {
    return 3;
  }
  char literal[48];
  size_t length;
  if (jb_c_literal_workspace_size(gain, JB_C_F32, &needed) != HAL_OK || needed > number_work.size ||
      jb_c_literal(gain, JB_C_F32, number_work, 1000000, literal, sizeof(literal), &length) !=
          HAL_OK) {
    return 4;
  }
  if (identifier != UINT64_MAX || gain_bits != UINT32_C(0x3e6147ae) || length != 15 ||
      memcmp(literal, "2.19999999e-01f", length) != 0) {
    return 5;
  }
  return printf("id=%" PRIu64 " gain=%08" PRIx32 " literal=%.*s\n", identifier, gain_bits,
                (int)length, literal) < 0
             ? 6
             : 0;
}
