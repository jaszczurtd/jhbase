#ifndef JB_JSON_POINTER_H
#define JB_JSON_POINTER_H

/** @file json_pointer.h
 * Locate JSON values and write their plain UTF-8 RFC 6901 paths.
 * Borrowing and non-overlap rules follow json.h. No heap, recursion or scratch.
 * Link jb::base; unused pointer code is omitted by the linker.
 */
#include "jb/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Resolve a plain UTF-8 JSON Pointer from the document root.
 * @param doc Required immutable document returned by parse.
 * @param pointer Exact bytes, including any NUL; NULL is allowed at zero length.
 * Empty selects root. Names decode ~0 and ~1 without Unicode normalization.
 * URI fragments and JSON-quoted representations must be decoded by the caller.
 * @param out Required result, unchanged on error.
 * @return HAL_OK, HAL_EINVAL for bad pointers, HAL_ESTATE for an invalid document,
 * HAL_EPROTO for invalid UTF-8, pointer syntax or array index spelling,
 * HAL_ETYPE when traversing a scalar, HAL_ENOENT for a missing member/item.
 * Array indices use decimal digits without leading zeros; '-' and arbitrarily
 * large valid indices give HAL_ENOENT. Object names such as '01' are literal.
 * The entire pointer syntax is checked before lookup, including missing suffixes.
 * Traversal scans members/items linearly and has no work-budget argument.
 */
hal_status_t jb_json_pointer_get(const jb_json_doc_t *doc, jb_json_span_t pointer,
                                 jb_json_node_t *out);

/** Write the plain UTF-8 JSON Pointer of an existing node, without a terminator.
 * @param node Borrowed immutable node; root produces zero bytes.
 * @param buffer Output bytes; (NULL,0) measures without writing.
 * @param capacity Bytes available. NULL with nonzero capacity is invalid.
 * @param required Required result in bytes, including any embedded NUL.
 * @return HAL_OK, HAL_EINVAL for bad pointers, HAL_ESTATE for an invalid node,
 * HAL_EOVERFLOW for insufficient capacity or size arithmetic overflow.
 * Insufficient capacity reports the full size and leaves buffer untouched;
 * other errors leave required unchanged. Use the returned length, not strlen.
 * Parent links are visited in two passes with constant stack space. Array
 * positions scan siblings; member names use the shared JSON string decoder.
 */
hal_status_t jb_json_pointer_copy(jb_json_node_t node, char *buffer, size_t capacity,
                                  size_t *required);

#ifdef __cplusplus
}
#endif
#endif
