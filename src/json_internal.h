#ifndef JB_JSON_INTERNAL_H
#define JB_JSON_INTERNAL_H

#include "jb/json.h"
#include "jb/utf8.h"

/* Decode a JSON string scalar; offsets refer to the supplied span. */
hal_status_t jb_json_scalar(jb_json_span_t input, size_t *position, uint32_t *out,
                            jb_json_error_t *error);
/* Compare two already validated strings; right can contain plain UTF-8. */
hal_status_t jb_json_strings_equal(jb_json_span_t left, jb_json_span_t right, bool right_escaped,
                                   size_t max_work, size_t *used, bool *out);
/* Decode a validated, quote-free string with transactional capacity checking.
 * With terminate, required counts a NUL terminator and U+0000 gives HAL_ETYPE. */
hal_status_t jb_json_copy_string(jb_json_span_t raw, char *buffer, size_t capacity,
                                 size_t *required, bool terminate);

#endif
