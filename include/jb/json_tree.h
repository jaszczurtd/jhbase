#ifndef JB_JSON_TREE_H
#define JB_JSON_TREE_H

/** @file json_tree.h
 * Owned JSON values for Linux/Windows host configuration, linked with jb::host.
 * Keys and strings are decoded UTF-8; numbers retain their original tokens.
 * Import and mutations copy their inputs. Successful mutation invalidates all
 * borrowed views/spans of that tree; failure leaves the tree unchanged.
 * Separate trees may be used concurrently. A tree being mutated has one owner;
 * sinks must not mutate or reenter the tree being formatted. No exceptions
 * cross this C API. The immutable parser and firmware writer remain unchanged.
 */
#include "jb/json_writer.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Opaque owner; destroy after its last view and span have been used. */
typedef struct jb_json_tree jb_json_tree_t;

/** Borrowed value. Its tree must remain alive and unchanged. */
typedef struct {
  const jb_json_tree_t *tree;
  size_t index;
} jb_json_tree_view_t;

/** Finite limits for each resulting tree/operation; zero never means unlimited. */
typedef struct {
  size_t max_nodes;             /**< Values including the root. */
  size_t max_depth;             /**< Simultaneously nested containers; scalar depth is zero. */
  size_t max_text_bytes;        /**< Sum of decoded keys/strings and original number bytes. */
  size_t max_key_compare_bytes; /**< Work allowance: one plus compared key byte lengths. */
} jb_json_tree_limits_t;

/** Host formatting. Output has no trailing LF or NUL; the caller adds either. */
typedef struct {
  size_t max_output_bytes; /**< Finite limit including whitespace and escapes. */
  uint32_t indent;         /**< Zero for compact JSON, or 2/4 spaces per container. */
  bool sort_keys;          /**< Sort decoded keys by Unicode scalar order. */
  bool ensure_ascii;       /**< Escape non-ASCII and DEL with lowercase \uXXXX. */
} jb_json_tree_format_t;

/** Copy a parsed subtree into an owned host tree; source buffers may then die.
 * limits/out are required and *out must be NULL. Returns HAL_OK, HAL_EINVAL,
 * HAL_ESTATE for an invalid source, HAL_EOVERFLOW for limits, or HAL_ENOMEM.
 * On failure *out is unchanged; no partial tree escapes.
 */
hal_status_t jb_json_tree_create(jb_json_node_t source, const jb_json_tree_limits_t *limits,
                                 jb_json_tree_t **out);
/** Deep-copy a host subtree under the supplied limits, with create's out rules. */
hal_status_t jb_json_tree_copy(jb_json_tree_view_t source, const jb_json_tree_limits_t *limits,
                               jb_json_tree_t **out);
/** Destroy an owner; NULL is harmless. All of its views/spans become invalid. */
void jb_json_tree_destroy(jb_json_tree_t *tree);
/** Borrow the root; tree/out required. Outputs change only on success. */
hal_status_t jb_json_tree_root(const jb_json_tree_t *tree, jb_json_tree_view_t *out);
/** Read a value kind. Invalid views return HAL_ESTATE; NULL out HAL_EINVAL. */
hal_status_t jb_json_tree_type(jb_json_tree_view_t value, jb_json_type_t *out);
/** Count object members/array items; scalars return HAL_ETYPE. */
hal_status_t jb_json_tree_count(jb_json_tree_view_t value, size_t *out);
/** Find a decoded UTF-8 key, including NUL. HAL_ENOENT means absent;
 * HAL_ETYPE means a non-object; malformed UTF-8 gives HAL_EPROTO and exhausted
 * comparison allowance HAL_EOVERFLOW. NULL key data is valid only at length zero.
 */
hal_status_t jb_json_tree_find(jb_json_tree_view_t object, jb_json_span_t key,
                               jb_json_tree_view_t *out);
/** Borrow an array item or object member by insertion index.
 * Scalars give HAL_ETYPE; a missing index HAL_ENOENT.
 */
hal_status_t jb_json_tree_at(jb_json_tree_view_t array, size_t index, jb_json_tree_view_t *out);
/** Borrow the decoded key of an object member, including NUL. A root or array
 * item has no key and gives HAL_EINVAL; the span outlives no mutation.
 */
hal_status_t jb_json_tree_key(jb_json_tree_view_t member, jb_json_span_t *out);
/** Borrow decoded string bytes or original number token, without an implied
 * terminator. Embedded NUL is preserved; other types give HAL_ETYPE.
 */
hal_status_t jb_json_tree_text(jb_json_tree_view_t value, jb_json_span_t *out);
/** Read a boolean without coercion; other types give HAL_ETYPE. */
hal_status_t jb_json_tree_get_bool(jb_json_tree_view_t value, bool *out);

/** Apply overlay transactionally under tree's limits. Objects merge recursively;
 * arrays and all other values replace the base. Existing object key positions
 * stay unchanged; new keys append in overlay order. Self-merge is supported.
 * Bad arguments give HAL_EINVAL/ESTATE; limits HAL_EOVERFLOW; OOM HAL_ENOMEM.
 */
hal_status_t jb_json_tree_merge(jb_json_tree_t *tree, jb_json_tree_view_t overlay);
/** Set one root object member, replacing its whole value and preserving position.
 * A new UTF-8 key appends. value is copied and may belong to tree. Wrong root
 * type gives HAL_ETYPE; validation/limits/OOM follow find/merge's rules.
 */
hal_status_t jb_json_tree_set(jb_json_tree_t *tree, jb_json_span_t key, jb_json_tree_view_t value);
/** Remove one root object member, preserving other members' order.
 * HAL_ENOENT leaves the tree unchanged; other errors follow set's rules.
 */
hal_status_t jb_json_tree_erase(jb_json_tree_t *tree, jb_json_span_t key);
/** Append a copied value to a root array, transactionally; wrong root HAL_ETYPE. */
hal_status_t jb_json_tree_append(jb_json_tree_t *tree, jb_json_tree_view_t value);

/** Format one subtree to a synchronous sink. options/sink/written required;
 * sink user may be NULL. indent outside {0,2,4} gives HAL_EINVAL. Strings use
 * the core JSON writer; numeric tokens retain their bytes. Sink errors follow
 * jb_json_sink_fn, and earlier chunks may already have been accepted on failure.
 * *written counts accepted bytes after argument validation, including on error.
 * Traversal and sorting are bounded by the tree's limits. HAL_ENOMEM reports OOM.
 */
hal_status_t jb_json_tree_format(jb_json_tree_view_t value, const jb_json_tree_format_t *options,
                                 jb_json_sink_fn sink, void *user, size_t *written);

#ifdef __cplusplus
}
#endif

JB_STATIC_ASSERT(sizeof(jb_json_tree_view_t) == 2 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_tree_view_t) == sizeof(size_t) &&
                     offsetof(jb_json_tree_view_t, index) == sizeof(size_t),
                 "jb JSON tree view layout");
JB_STATIC_ASSERT(sizeof(jb_json_tree_limits_t) == 4 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_tree_limits_t) == sizeof(size_t),
                 "jb JSON tree limits layout");
JB_STATIC_ASSERT(offsetof(jb_json_tree_format_t, indent) == sizeof(size_t) &&
                     offsetof(jb_json_tree_format_t, sort_keys) == sizeof(size_t) + 4 &&
                     offsetof(jb_json_tree_format_t, ensure_ascii) == sizeof(size_t) + 5 &&
                     sizeof(jb_json_tree_format_t) == (sizeof(size_t) == 8 ? 16u : 12u) &&
                     JB_ALIGNOF(jb_json_tree_format_t) == sizeof(size_t),
                 "jb JSON tree format layout");
#endif
