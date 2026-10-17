#ifndef JB_LITERAL_H
#define JB_LITERAL_H
/** @file literal.h
 * Deterministic C literals from JSON scalars. Strings use UTF-8 octal escapes;
 * float/double use 9/17 significant decimal digits after one exact IEEE rounding.
 * No locale or native printf formatting participates in the result.
 */
#include "jb/json_number.h"
#ifdef __cplusplus
extern "C" {
#endif
/** Fixed-width scalar type selector; names denote widths, not native C sizes. */
typedef uint32_t jb_c_literal_type_t;
enum {
  JB_C_BOOL,
  JB_C_U8,
  JB_C_U16,
  JB_C_U32,
  JB_C_U64,
  JB_C_I8,
  JB_C_I16,
  JB_C_I32,
  JB_C_I64,
  JB_C_F32,
  JB_C_F64,
  JB_C_STRING
};
/** Compute sufficient scratch for this node/type. out_bytes required.
 * Non-floating types need zero bytes. Bad selector gives HAL_EINVAL; an
 * incompatible node HAL_ETYPE. Scratch is disjoint from input/output.
 */
hal_status_t jb_c_literal_workspace_size(jb_json_node_t node, jb_c_literal_type_t type,
                                         size_t *out_bytes);
/** Write a C literal without a NUL terminator. required is mandatory and counts
 * bytes. NULL output is allowed at capacity zero. Insufficient capacity sets
 * required and gives HAL_EOVERFLOW without writing output. Other failures leave
 * both outputs unchanged. max_work is a finite digit/limb budget; zero permits
 * no work. Wrong type/fractional integer gives HAL_ETYPE; range, rounded
 * underflow-to-zero, exhausted work/scratch gives HAL_EOVERFLOW. Negative zero
 * is normalized. Negative literals are parenthesized; int32/int64 minima use
 * subtraction forms that compile without unsigned intermediate constants.
 */
hal_status_t jb_c_literal(jb_json_node_t node, jb_c_literal_type_t type, jb_json_workspace_t work,
                          size_t max_work, char *buffer, size_t capacity, size_t *required);
#ifdef __cplusplus
}
#endif
#endif
