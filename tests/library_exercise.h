#ifndef JB_LIBRARY_EXERCISE_H
#define JB_LIBRARY_EXERCISE_H
#include "jb/json_number.h"
#include "jb/json_writer.h"
#include "jb/literal.h"
#include <hal/core/hal_array.h>

/* Shared by the freestanding linker probe and the host allocation guard. */
static inline int library_exercise(void) {
  static const char input[] = "{\"id\":18446744073709551615,\"gain\":0.22,\"unit\":\"V\"}";
  static jb_json_token_t tokens[4];
  static unsigned char parsing[256], numbers[4096], writing[512];
  static char output[256], literal[48];
  const jb_json_limits_t limits = {sizeof(input) - 1, COUNTOF(tokens), 1, 16, 32, 3, 64};
  const jb_json_workspace_t parse_work = {parsing, sizeof(parsing)},
                            number_work = {numbers, sizeof(numbers)};
  const jb_json_span_t source = {input, sizeof(input) - 1}, id_key = {"id", 2},
                       gain_key = {"gain", 4}, unit_key = {"unit", 4}, volts = {"V", 1};
  jb_json_doc_t doc;
  if (jb_json_parse(source, &limits, tokens, COUNTOF(tokens), parse_work, &doc, NULL) != HAL_OK) {
    return 1;
  }
  jb_json_node_t root, id, gain, unit;
  if (jb_json_root(&doc, &root) != HAL_OK || jb_json_find(root, id_key, &id) != HAL_OK ||
      jb_json_find(root, gain_key, &gain) != HAL_OK ||
      jb_json_find(root, unit_key, &unit) != HAL_OK) {
    return 2;
  }
  uint64_t identifier;
  float small;
  double wide;
  const jb_json_float_options_t options = {true, true, 1000000};
  if (jb_json_get_u64(id, &identifier) != HAL_OK || identifier != UINT64_MAX ||
      jb_json_get_f32(gain, &options, number_work, &small) != HAL_OK ||
      jb_json_get_f64(gain, &options, number_work, &wide) != HAL_OK) {
    return 3;
  }
  bool same;
  size_t required;
  if (jb_json_string_equal(unit, volts, &same) != HAL_OK || !same ||
      jb_json_string_cstr(unit, literal, sizeof(literal), &required) != HAL_OK || required != 2) {
    return 4;
  }
  if (jb_c_literal(gain, JB_C_F32, number_work, 1000000, literal, sizeof(literal), &required) !=
      HAL_OK) {
    return 5;
  }
  jb_json_writer_t writer;
  const jb_json_writer_limits_t bounds = {sizeof(output), 2, 4, 64, 128};
  jb_json_buffer_t buffer = {output, sizeof(output), 0};
  const jb_json_workspace_t work = {writing, sizeof(writing)};
  if (jb_json_writer_init(&writer, &bounds, work, jb_json_buffer_sink, &buffer) != HAL_OK) {
    return 6;
  }
  (void)jb_json_writer_array_begin(&writer);
  (void)jb_json_writer_node(&writer, root);
  (void)jb_json_writer_f32(&writer, small);
  (void)jb_json_writer_f64(&writer, wide);
  (void)jb_json_writer_u64(&writer, identifier);
  (void)jb_json_writer_i64(&writer, INT64_MIN);
  (void)jb_json_writer_string(&writer, (jb_json_span_t){"a\0b", 3});
  (void)jb_json_writer_array_end(&writer);
  return jb_json_writer_finish(&writer, &required) == HAL_OK ? 0 : 7;
}
#endif
