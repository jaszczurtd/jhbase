#ifndef JB_JSON_INTERNAL_H
#define JB_JSON_INTERNAL_H

#include "jb/json.h"
#include "jb/utf8.h"

typedef enum { JB_JSON_TEXT_UTF8, JB_JSON_TEXT_JSON, JB_JSON_TEXT_POINTER } jb_json_text_t;

/* Validate a borrowed node before accessing its parser-owned record. */
hal_status_t jb_json_record(jb_json_node_t node, const jb_json_token_t **out);
/* Find a key whose UTF-8 and, for POINTER, tilde escapes are already validated. */
hal_status_t jb_json_find_encoded(jb_json_node_t object, jb_json_span_t key,
                                  jb_json_text_t encoding, jb_json_node_t *out);
/* Decode plain UTF-8 or an RFC 6901 reference token, including raw NUL. */
hal_status_t jb_json_text_scalar(jb_json_span_t input, size_t *position, jb_json_text_t encoding,
                                 uint32_t *out);

/* Decode a JSON string scalar; offsets refer to the supplied span. */
hal_status_t jb_json_scalar(jb_json_span_t input, size_t *position, uint32_t *out,
                            jb_json_error_t *error);
/* Compare two already validated strings; right can contain plain UTF-8. */
hal_status_t jb_json_strings_equal(jb_json_span_t left, jb_json_span_t right,
                                   jb_json_text_t encoding, size_t max_work, size_t *used,
                                   bool *out);
/* Decode a validated, quote-free string with transactional capacity checking.
 * With terminate, required counts a NUL terminator and U+0000 gives HAL_ETYPE. */
hal_status_t jb_json_copy_string(jb_json_span_t raw, char *buffer, size_t capacity,
                                 size_t *required, bool terminate);

#endif
