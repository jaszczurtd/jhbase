#include "jb/json_writer.h"
#include <jb/array.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
static hal_status_t operation(jb_json_writer_t *w, unsigned op, jb_json_span_t bytes) {
  switch (op) {
  case 0:
    return jb_json_writer_object_begin(w);
  case 1:
    return jb_json_writer_object_end(w);
  case 2:
    return jb_json_writer_array_begin(w);
  case 3:
    return jb_json_writer_array_end(w);
  case 4:
    return jb_json_writer_key(w, bytes);
  case 5:
    return jb_json_writer_string(w, bytes);
  case 6:
    return jb_json_writer_number(w, bytes);
  case 7:
    return jb_json_writer_null(w);
  case 8:
    return jb_json_writer_bool(w, bytes.length != 0);
  case 9:
    return jb_json_writer_u64(w, bytes.length);
  case 10:
    return jb_json_writer_i64(w, -(int64_t)bytes.length);
  case 11: {
    uint64_t bits = 0;
    memcpy(&bits, bytes.data, bytes.length < sizeof(bits) ? bytes.length : sizeof(bits));
    double value;
    memcpy(&value, &bits, sizeof(value));
    return jb_json_writer_f64(w, value);
  }
  default: {
    size_t count;
    return jb_json_writer_finish(w, &count);
  }
  }
}
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  static unsigned char first_work[32769], second_work[32769], parser_work[4097];
  static char output[4096];
  static jb_json_token_t tokens[4096];
  const jb_json_writer_limits_t limits = {sizeof(output), 16, 32, 1024, 8192};
  jb_json_buffer_t buffer = {output, sizeof(output), 0};
  size_t counted = 0;
  jb_json_writer_t first, second;
  if (jb_json_writer_init(&first, &limits,
                          (jb_json_workspace_t){first_work + 1, sizeof(first_work) - 1},
                          jb_json_buffer_sink, &buffer) != HAL_OK ||
      jb_json_writer_init(&second, &limits,
                          (jb_json_workspace_t){second_work + 1, sizeof(second_work) - 1},
                          jb_json_count_sink, &counted) != HAL_OK) {
    abort();
  }
  size_t at = 0;
  while (at < size) {
    const unsigned op = data[at++] % 13;
    size_t length = at < size ? data[at++] % 33u : 0;
    if (length > size - at) {
      length = size - at;
    }
    const jb_json_span_t bytes = {(const char *)data + at, length};
    at += length;
    const hal_status_t a = operation(&first, op, bytes), b = operation(&second, op, bytes);
    if (a != b || counted != buffer.used) {
      abort();
    }
  }
  size_t length;
  if (jb_json_writer_finish(&first, &length) == HAL_OK) {
    jb_json_doc_t doc;
    const jb_json_limits_t parse_limits = {sizeof(output), COUNTOF(tokens), 16, 4096, 4096,
                                           4096,           1000000};
    if (length != buffer.used ||
        jb_json_parse((jb_json_span_t){output, length}, &parse_limits, tokens, COUNTOF(tokens),
                      (jb_json_workspace_t){parser_work + 1, sizeof(parser_work) - 1}, &doc,
                      NULL) != HAL_OK) {
      abort();
    }
  }
  return 0;
}
