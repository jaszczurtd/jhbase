#ifndef JB_JSON_WRITER_H
#define JB_JSON_WRITER_H
/** @file json_writer.h
 * Sequential compact JSON in caller scratch. Input, scratch and output must
 * not overlap. Do not copy an active writer, reenter it from its sink, or modify
 * its scratch. Every operation caches its first error until the next init.
 */
#include "jb/json.h"
#ifdef __cplusplus
extern "C" {
#endif

/** Synchronously accept every byte or return a defined negative HAL status.
 * Do not retain data. HAL_NONE/unknown results are converted to HAL_EINTERNAL.
 * Earlier chunks can already have been sent when a later chunk fails.
 */
typedef hal_status_t (*jb_json_sink_fn)(void *user, const char *data, size_t length);
/** Fixed output buffer with no implied NUL; used counts accepted bytes. */
typedef struct {
  char *data;
  size_t capacity, used;
} jb_json_buffer_t;
/** Finite output bounds; key storage covers all simultaneously open objects. */
typedef struct {
  size_t max_output_bytes;      /**< Bytes for the complete document. */
  size_t max_depth;             /**< Simultaneously open containers. */
  size_t max_keys;              /**< Keys in all currently open objects. */
  size_t max_key_bytes;         /**< Copied decoded UTF-8 bytes of those keys. */
  size_t max_key_compare_bytes; /**< One per comparison plus both compared bytes. */
} jb_json_writer_limits_t;
/** Zero-initializable handle; fields are private. Scratch outlives the writer. */
typedef struct {
  void *state;
  hal_status_t private_status;
} jb_json_writer_t;

/** Compute all state, frame and copied-key scratch bytes.
 * Both pointers required. Checked size overflow gives HAL_EOVERFLOW.
 */
hal_status_t jb_json_writer_workspace_size(const jb_json_writer_limits_t *limits, size_t *out);
/** Start a document. writer/limits/sink required; user is passed unchanged.
 * Unaligned scratch is supported. Insufficient space gives HAL_EOVERFLOW;
 * other bad arguments give HAL_EINVAL. Failed init leaves an inactive handle.
 */
hal_status_t jb_json_writer_init(jb_json_writer_t *writer, const jb_json_writer_limits_t *limits,
                                 jb_json_workspace_t work, jb_json_sink_fn sink, void *user);
/** Append one whole chunk to jb_json_buffer_t user, or HAL_EOVERFLOW with no copy.
 * NULL data is allowed only at zero length; invalid buffer fields give HAL_EINVAL.
 */
hal_status_t jb_json_buffer_sink(void *user, const char *data, size_t length);
/** Count a chunk in size_t user, or HAL_EOVERFLOW without changing the count. */
hal_status_t jb_json_count_sink(void *user, const char *data, size_t length);
/** Open an object as root, array item, or value of a preceding key.
 * Required writer. Bad placement gives HAL_ESTATE; limits HAL_EOVERFLOW.
 */
hal_status_t jb_json_writer_object_begin(jb_json_writer_t *writer);
/** Close the innermost object; a missing value/mismatched end gives HAL_ESTATE. */
hal_status_t jb_json_writer_object_end(jb_json_writer_t *writer);
/** Open an array using the same placement and limit rules as object_begin. */
hal_status_t jb_json_writer_array_begin(jb_json_writer_t *writer);
/** Close the innermost array; mismatched end gives HAL_ESTATE. */
hal_status_t jb_json_writer_array_end(jb_json_writer_t *writer);
/** Write a unique decoded UTF-8 key, including NUL, and copy it into scratch.
 * Caller may reuse key immediately. Invalid UTF-8 gives HAL_EPROTO, duplicate
 * HAL_EEXIST, bad placement HAL_ESTATE; exhausted storage/work HAL_EOVERFLOW.
 */
hal_status_t jb_json_writer_key(jb_json_writer_t *writer, jb_json_span_t key);
/** Write a UTF-8 string, escaping controls, quotes and backslashes.
 * Embedded NUL is preserved. Invalid UTF-8 gives HAL_EPROTO.
 */
hal_status_t jb_json_writer_string(jb_json_writer_t *writer, jb_json_span_t value);
/** Write a boolean value; missing key/second root gives HAL_ESTATE. */
hal_status_t jb_json_writer_bool(jb_json_writer_t *writer, bool value);
/** Write a null value using the same placement rules as bool. */
hal_status_t jb_json_writer_null(jb_json_writer_t *writer);
/** Write an exact unsigned integer without locale-dependent formatting. */
hal_status_t jb_json_writer_u64(jb_json_writer_t *writer, uint64_t value);
/** Write an exact signed integer, including INT64_MIN. */
hal_status_t jb_json_writer_i64(jb_json_writer_t *writer, int64_t value);
/** Validate and write one complete number token, preserving its bytes.
 * Whitespace, malformed syntax and embedded NUL give HAL_EPROTO.
 */
hal_status_t jb_json_writer_number(jb_json_writer_t *writer, jb_json_span_t token);
/** Write the shortest round-trip spelling of a finite native binary32.
 * NaN/Infinity gives HAL_EINVAL. The sign of zero is preserved.
 */
hal_status_t jb_json_writer_f32(jb_json_writer_t *writer, float value);
/** Binary64 version of writer_f32, with the same error and zero policy. */
hal_status_t jb_json_writer_f64(jb_json_writer_t *writer, double value);
/** Write a parsed subtree iteratively. Keys/strings are decoded and escaped;
 * numbers retain their original token. Writer limits include the outer context.
 * An invalid node gives HAL_ESTATE. Input buffers must stay alive throughout.
 */
hal_status_t jb_json_writer_node(jb_json_writer_t *writer, jb_json_node_t node);
/** Require one complete root and no open containers, returning its byte count.
 * Both pointers required. Incomplete document gives HAL_ESTATE. Repeated finish
 * returns the same successful result; any subsequent write gives HAL_ESTATE.
 */
hal_status_t jb_json_writer_finish(jb_json_writer_t *writer, size_t *out_bytes);
#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(sizeof(jb_json_buffer_t) == 3 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_buffer_t) == sizeof(size_t) &&
                     offsetof(jb_json_buffer_t, data) == 0 &&
                     offsetof(jb_json_buffer_t, capacity) == sizeof(size_t) &&
                     offsetof(jb_json_buffer_t, used) == 2 * sizeof(size_t),
                 "jb JSON buffer layout");
JB_STATIC_ASSERT(sizeof(jb_json_writer_limits_t) == 5 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_writer_limits_t) == sizeof(size_t) &&
                     offsetof(jb_json_writer_limits_t, max_output_bytes) == 0 &&
                     offsetof(jb_json_writer_limits_t, max_depth) == sizeof(size_t) &&
                     offsetof(jb_json_writer_limits_t, max_keys) == 2 * sizeof(size_t) &&
                     offsetof(jb_json_writer_limits_t, max_key_bytes) == 3 * sizeof(size_t) &&
                     offsetof(jb_json_writer_limits_t, max_key_compare_bytes) == 4 * sizeof(size_t),
                 "jb JSON writer limits layout");
JB_STATIC_ASSERT(sizeof(jb_json_writer_t) == 2 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_writer_t) == sizeof(size_t) &&
                     offsetof(jb_json_writer_t, state) == 0 &&
                     offsetof(jb_json_writer_t, private_status) == sizeof(size_t) &&
                     sizeof(jb_json_sink_fn) == sizeof(void *),
                 "jb JSON writer layout");
#endif
