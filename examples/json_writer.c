#include "jb/json_writer.h"
#include <stdio.h>
#include <string.h>

int main(void) {
  static unsigned char scratch[512];
  static char output[128];
  const jb_json_writer_limits_t limits = {sizeof(output), 1, 3, 32, 128};
  const jb_json_workspace_t work = {scratch, sizeof(scratch)};
  jb_json_buffer_t buffer = {output, sizeof(output), 0};
  jb_json_writer_t writer;
  size_t needed;
  if (jb_json_writer_workspace_size(&limits, &needed) != HAL_OK || needed > work.size ||
      jb_json_writer_init(&writer, &limits, work, jb_json_buffer_sink, &buffer) != HAL_OK) {
    return 1;
  }
  const jb_json_span_t ok = {"ok", 2}, id = {"id", 2}, message = {"message", 7};
  static const char bytes[] = {'a', 0, 'b'};
  const jb_json_span_t value = {bytes, sizeof(bytes)};
  /* Each call caches its error; finish reports the first failure. */
  (void)jb_json_writer_object_begin(&writer);
  (void)jb_json_writer_key(&writer, ok);
  (void)jb_json_writer_bool(&writer, true);
  (void)jb_json_writer_key(&writer, id);
  (void)jb_json_writer_u64(&writer, UINT64_MAX);
  (void)jb_json_writer_key(&writer, message);
  (void)jb_json_writer_string(&writer, value);
  (void)jb_json_writer_object_end(&writer);
  size_t length;
  if (jb_json_writer_finish(&writer, &length) != HAL_OK) {
    return 2;
  }
  static const char expected[] =
      "{\"ok\":true,\"id\":18446744073709551615,\"message\":\"a\\u0000b\"}";
  if (length != sizeof(expected) - 1 || memcmp(output, expected, length) != 0) {
    return 3;
  }
  return fwrite(output, 1, length, stdout) == length && putchar('\n') != EOF ? 0 : 4;
}
