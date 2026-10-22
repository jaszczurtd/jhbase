#ifndef JB_JSON_H
#define JB_JSON_H

/** @file json.h
 * Immutable JSON tokens in caller memory. Input, tokens, output objects and
 * scratch must not overlap. Input and tokens stay unchanged while any document
 * or node is used; parser scratch may be reused immediately after parsing.
 * No heap, variable-length arrays, input-dependent recursion or global state.
 * Wrong-kind reads return HAL_ETYPE; bad arguments return HAL_EINVAL.
 */
#include "jb/status.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Borrowed bytes with no implied terminator; embedded NUL is preserved. */
typedef struct {
  const char *data; /**< NULL is permitted only for zero length. */
  size_t length;    /**< Available bytes. */
} jb_json_span_t;

/** Fixed-width JSON kind, independent of compiler enum flags. */
typedef uint32_t jb_json_type_t;
enum { JB_JSON_NULL, JB_JSON_BOOL, JB_JSON_NUMBER, JB_JSON_STRING, JB_JSON_ARRAY, JB_JSON_OBJECT };

/** Absent relation; never an index into token storage. */
#define JB_JSON_NODE_NONE SIZE_MAX

/** Parser-owned record, published read-only in preorder (root index zero). */
typedef struct {
  jb_json_type_t type;                      /**< One of the six JB_JSON_* kinds. */
  size_t offset, length;                    /**< Full value, including delimiters. */
  size_t key_offset, key_length;            /**< Member name bytes, excluding quotes. */
  size_t parent, first_child, next_sibling; /**< Token indices or NODE_NONE. */
  size_t child_count;                       /**< Direct members/items. */
  bool has_key;                             /**< An empty name still has a key. */
} jb_json_token_t;

/** Borrowed immutable document; only a successful parse sets valid. */
typedef struct {
  jb_json_span_t input;          /**< Caller-owned original JSON. */
  const jb_json_token_t *tokens; /**< Caller-owned token records. */
  size_t token_count;            /**< Number of published value records. */
  bool valid;                    /**< False after any failed parse. */
} jb_json_doc_t;

/** Borrowed node; the document object and its buffers must remain alive. */
typedef struct {
  const jb_json_doc_t *doc; /**< Document returned by parse. */
  size_t index;             /**< Value record index. */
} jb_json_node_t;

/** Mutable byte scratch; even an unaligned unsigned char array is supported. */
typedef struct {
  void *data;  /**< NULL is permitted only for zero size. */
  size_t size; /**< Bytes available. */
} jb_json_workspace_t;

/** Finite allowances. Zero always permits zero, never unlimited work. */
typedef struct {
  size_t max_input_bytes;       /**< Entire input, including whitespace. */
  size_t max_nodes;             /**< Total values, root included. */
  size_t max_depth;             /**< Simultaneously open containers. */
  size_t max_string_bytes;      /**< Decoded UTF-8 bytes per string/key. */
  size_t max_number_bytes;      /**< Original number token bytes. */
  size_t max_members;           /**< Direct children per container. */
  size_t max_key_compare_bytes; /**< One unit per comparison, plus both decoded
                                    keys' bytes actually compared. */
} jb_json_limits_t;

/** Fixed-width diagnostic reason; the function result remains hal_status_t. */
typedef uint32_t jb_json_error_code_t;
enum {
  JB_JSON_ERROR_NONE,
  JB_JSON_ERROR_SYNTAX,
  JB_JSON_ERROR_UTF8,
  JB_JSON_ERROR_ESCAPE,
  JB_JSON_ERROR_DUPLICATE_KEY,
  JB_JSON_ERROR_TRAILING_DATA,
  JB_JSON_ERROR_LIMIT,
  JB_JSON_ERROR_TOKEN_CAPACITY,
  JB_JSON_ERROR_WORKSPACE
};

/** First failure in byte order; EOF has offset equal to input length. */
typedef struct {
  jb_json_error_code_t code; /**< NONE after success or argument failure. */
  size_t offset;             /**< Failure byte or opening quote of duplicate key. */
  size_t related_offset;     /**< Earlier key's opening quote, otherwise SIZE_MAX. */
  size_t required_capacity;  /**< Known required count/bytes; zero if unknown. */
} jb_json_error_t;

/** Sizing result; measurement does not check duplicate names. */
typedef struct {
  size_t node_count; /**< Value records needed. */
  size_t depth;      /**< Maximum number of simultaneously open containers. */
} jb_json_requirements_t;

/** Compute scratch bytes for limits->max_depth, with checked arithmetic.
 * Required pointers: limits and out_bytes. Returns HAL_EINVAL for bad pointers,
 * HAL_EOVERFLOW for size overflow, otherwise HAL_OK. Output changes on success.
 */
hal_status_t jb_json_parse_workspace_size(const jb_json_limits_t *limits, size_t *out_bytes);

/** Check syntax, UTF-8 and limits, counting values without publishing tokens.
 * Required pointers: limits and out. Input/work allow NULL only at zero size.
 * error is optional. Returns HAL_OK, HAL_EINVAL, HAL_EPROTO or HAL_EOVERFLOW.
 * Duplicate names are checked only by parse; successful measure is not acceptance.
 */
hal_status_t jb_json_measure(jb_json_span_t input, const jb_json_limits_t *limits,
                             jb_json_workspace_t work, jb_json_requirements_t *out,
                             jb_json_error_t *error);

/** Parse one complete value with unique decoded member names and no BOM.
 * Required pointers: limits and out_doc. tokens may be NULL at capacity zero.
 * Scratch must fit parse_workspace_size(limits), also for scalar documents.
 * error is optional. Returns HAL_OK, HAL_EINVAL, HAL_EPROTO or HAL_EOVERFLOW.
 * Failure invalidates out_doc; token storage may contain partial records.
 * Reusing the token array invalidates every prior document using that array.
 */
hal_status_t jb_json_parse(jb_json_span_t input, const jb_json_limits_t *limits,
                           jb_json_token_t *tokens, size_t token_capacity, jb_json_workspace_t work,
                           jb_json_doc_t *out_doc, jb_json_error_t *error);

/** Locate offset (including EOF), returning one-based line and byte column.
 * LF starts a new line. All result pointers are required. HAL_EINVAL rejects
 * bad arguments or an offset past EOF; HAL_EOVERFLOW rejects counter overflow.
 */
hal_status_t jb_json_locate(jb_json_span_t input, size_t offset, size_t *out_line,
                            size_t *out_column);

/** Get root. Required pointers: doc/out. Invalid documents give HAL_ESTATE. */
hal_status_t jb_json_root(const jb_json_doc_t *doc, jb_json_node_t *out);
/** Get kind. Required out; an invalid node gives HAL_ESTATE. */
hal_status_t jb_json_type(jb_json_node_t node, jb_json_type_t *out);
/** Count direct members/items. Required out; wrong kind gives HAL_ETYPE. */
hal_status_t jb_json_count(jb_json_node_t node, size_t *out);
/** First member/item, or HAL_ENOENT when empty. Required out. */
hal_status_t jb_json_first(jb_json_node_t container, jb_json_node_t *out);
/** Next sibling, or HAL_ENOENT at end. Required out. */
hal_status_t jb_json_next(jb_json_node_t node, jb_json_node_t *out);
/** Find decoded, case-sensitive UTF-8 key, including NUL, without NFC.
 * Required out; invalid key UTF-8 gives HAL_EPROTO, wrong kind HAL_ETYPE,
 * absent key HAL_ENOENT. Linear scan; this is not a work-limited operation.
 */
hal_status_t jb_json_find(jb_json_node_t object, jb_json_span_t key, jb_json_node_t *out);
/** Find zero-based array position in linear time. Required out.
 * Wrong kind gives HAL_ETYPE; missing position gives HAL_ENOENT.
 */
hal_status_t jb_json_at(jb_json_node_t array, size_t position, jb_json_node_t *out);
/** Borrow original value bytes including delimiters. Required out. */
hal_status_t jb_json_raw(jb_json_node_t node, jb_json_span_t *out);
/** Borrow original number token. Required out; wrong kind gives HAL_ETYPE. */
hal_status_t jb_json_number_token(jb_json_node_t node, jb_json_span_t *out);
/** Read boolean without coercion. Required out; wrong kind HAL_ETYPE. */
hal_status_t jb_json_get_bool(jb_json_node_t node, bool *out);

/** Decode string bytes without a terminator. The required pointer must not be NULL.
 * (NULL,0) measures. On HAL_EOVERFLOW, reports full required bytes and leaves
 * buffer untouched. Wrong kind gives HAL_ETYPE, bad pointers HAL_EINVAL.
 */
hal_status_t jb_json_string_copy(jb_json_node_t node, char *buffer, size_t capacity,
                                 size_t *required);
/** Decode a member name with the same buffer rules as string_copy.
 * A node without a key gives HAL_EINVAL. The required pointer must not be NULL.
 */
hal_status_t jb_json_key_copy(jb_json_node_t member, char *buffer, size_t capacity,
                              size_t *required);
/** Compare the decoded string with plain UTF-8 bytes, without NFC or case folding.
 * Required out; value may contain NUL. Wrong kind gives HAL_ETYPE, invalid
 * value UTF-8 HAL_EPROTO, a NULL value with nonzero length HAL_EINVAL.
 */
hal_status_t jb_json_string_equal(jb_json_node_t node, jb_json_span_t value, bool *out);
/** Decode as a NUL-terminated C string; required counts the terminator.
 * Buffer rules follow string_copy. A value containing U+0000 gives HAL_ETYPE
 * instead of a silently shortened string; both outputs stay unchanged.
 */
hal_status_t jb_json_string_cstr(jb_json_node_t node, char *buffer, size_t capacity,
                                 size_t *required);

#ifdef __cplusplus
}
#endif

/* Expected offsets use scalar alignments, never a second struct under pack. */
#define JB_JSON_LAYOUT(type, words)                                                                \
  JB_STATIC_ASSERT(sizeof(type) == (words) * sizeof(size_t) && JB_ALIGNOF(type) == sizeof(size_t), \
                   "jb JSON layout: " #type)
#define JB_JSON_FIELD(type, field, words)                                                          \
  JB_STATIC_ASSERT(offsetof(type, field) == (words) * sizeof(size_t),                              \
                   "jb JSON offset: " #type "." #field)
JB_JSON_LAYOUT(jb_json_span_t, 2);
JB_JSON_FIELD(jb_json_span_t, data, 0);
JB_JSON_FIELD(jb_json_span_t, length, 1);
JB_JSON_LAYOUT(jb_json_token_t, 10);
JB_JSON_FIELD(jb_json_token_t, type, 0);
JB_JSON_FIELD(jb_json_token_t, offset, 1);
JB_JSON_FIELD(jb_json_token_t, length, 2);
JB_JSON_FIELD(jb_json_token_t, key_offset, 3);
JB_JSON_FIELD(jb_json_token_t, key_length, 4);
JB_JSON_FIELD(jb_json_token_t, parent, 5);
JB_JSON_FIELD(jb_json_token_t, first_child, 6);
JB_JSON_FIELD(jb_json_token_t, next_sibling, 7);
JB_JSON_FIELD(jb_json_token_t, child_count, 8);
JB_JSON_FIELD(jb_json_token_t, has_key, 9);
JB_JSON_LAYOUT(jb_json_doc_t, 5);
JB_JSON_FIELD(jb_json_doc_t, input, 0);
JB_JSON_FIELD(jb_json_doc_t, tokens, 2);
JB_JSON_FIELD(jb_json_doc_t, token_count, 3);
JB_JSON_FIELD(jb_json_doc_t, valid, 4);
JB_JSON_LAYOUT(jb_json_node_t, 2);
JB_JSON_FIELD(jb_json_node_t, doc, 0);
JB_JSON_FIELD(jb_json_node_t, index, 1);
JB_JSON_LAYOUT(jb_json_workspace_t, 2);
JB_JSON_FIELD(jb_json_workspace_t, data, 0);
JB_JSON_FIELD(jb_json_workspace_t, size, 1);
JB_STATIC_ASSERT(sizeof(jb_json_limits_t) == 7 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_limits_t) == sizeof(size_t),
                 "jb JSON limits layout");
JB_JSON_FIELD(jb_json_limits_t, max_input_bytes, 0);
JB_JSON_FIELD(jb_json_limits_t, max_nodes, 1);
JB_JSON_FIELD(jb_json_limits_t, max_depth, 2);
JB_JSON_FIELD(jb_json_limits_t, max_string_bytes, 3);
JB_JSON_FIELD(jb_json_limits_t, max_number_bytes, 4);
JB_JSON_FIELD(jb_json_limits_t, max_members, 5);
JB_JSON_FIELD(jb_json_limits_t, max_key_compare_bytes, 6);
JB_JSON_LAYOUT(jb_json_error_t, 4);
JB_JSON_FIELD(jb_json_error_t, code, 0);
JB_JSON_FIELD(jb_json_error_t, offset, 1);
JB_JSON_FIELD(jb_json_error_t, related_offset, 2);
JB_JSON_FIELD(jb_json_error_t, required_capacity, 3);
JB_JSON_LAYOUT(jb_json_requirements_t, 2);
JB_JSON_FIELD(jb_json_requirements_t, node_count, 0);
JB_JSON_FIELD(jb_json_requirements_t, depth, 1);
#undef JB_JSON_FIELD
#undef JB_JSON_LAYOUT
#endif
