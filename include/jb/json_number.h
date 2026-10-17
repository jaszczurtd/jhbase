#ifndef JB_JSON_NUMBER_H
#define JB_JSON_NUMBER_H

/** @file json_number.h
 * Exact decimal reads from immutable JSON nodes. Numeric outputs change only
 * on success. Scratch and outputs must not overlap the document or each other.
 * No heap, locale, FPU rounding mode or intermediate binary64 conversion.
 */
#include "jb/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Convert an exact integer, including decimal/exponent spellings, to uint64.
 * Required out. Wrong kind or fractional value gives HAL_ETYPE; a negative
 * nonzero value or an out-of-range integer gives HAL_EOVERFLOW. -0 is accepted.
 */
hal_status_t jb_json_get_u64(jb_json_node_t node, uint64_t *out);
/** Signed version of get_u64, including INT64_MIN. Required out. */
hal_status_t jb_json_get_i64(jb_json_node_t node, int64_t *out);
/** Test exact integrality without rounding or materializing a large exponent.
 * Required out; wrong kind gives HAL_ETYPE, invalid node HAL_ESTATE.
 */
hal_status_t jb_json_number_is_integer(jb_json_node_t node, bool *out);
/** Compute comparison scratch bytes with checked arithmetic. Required out. */
hal_status_t jb_json_number_compare_workspace_size(jb_json_node_t left, jb_json_node_t right,
                                                   size_t *out_bytes);
/** Compare exact decimal values, returning -1, 0 or 1 through required out_order.
 * Unaligned byte scratch is allowed. Insufficient scratch or exhausted max_work
 * gives HAL_EOVERFLOW. Zero permits no work. Each digit/limb step is charged.
 */
hal_status_t jb_json_number_compare(jb_json_node_t left, jb_json_node_t right,
                                    jb_json_workspace_t work, size_t max_work, int *out_order);

/** Decimal-to-IEEE policy; round-to-nearest with ties to even is always used. */
typedef struct {
  bool reject_nonzero_to_zero;  /**< Treat rounded underflow to zero as overflow. */
  bool normalize_negative_zero; /**< Return positive zero for either signed zero. */
  size_t max_work;              /**< Finite digit/limb step budget; zero permits none. */
} jb_json_float_options_t;

/** Compute conversion scratch for binary32 or binary64 (bit_width 32 or 64).
 * Required out. Unsupported width/bad pointers give HAL_EINVAL.
 */
hal_status_t jb_json_float_workspace_size(jb_json_node_t node, unsigned bit_width,
                                          size_t *out_bytes);
/** Round directly from exact decimal to binary32 bits. All pointers required.
 * Overflow to infinity gives HAL_EOVERFLOW. Subnormals and signed zero follow
 * options. Insufficient scratch/work gives HAL_EOVERFLOW without publishing out.
 */
hal_status_t jb_json_get_f32_bits(jb_json_node_t node, const jb_json_float_options_t *options,
                                  jb_json_workspace_t work, uint32_t *out);
/** Binary64 version of get_f32_bits; no intermediate native floating point. */
hal_status_t jb_json_get_f64_bits(jb_json_node_t node, const jb_json_float_options_t *options,
                                  jb_json_workspace_t work, uint64_t *out);
/** Read native IEEE binary32 using the same policy and representation copy. */
hal_status_t jb_json_get_f32(jb_json_node_t node, const jb_json_float_options_t *options,
                             jb_json_workspace_t work, float *out);
/** Read native IEEE binary64 using the same policy and representation copy. */
hal_status_t jb_json_get_f64(jb_json_node_t node, const jb_json_float_options_t *options,
                             jb_json_workspace_t work, double *out);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(sizeof(jb_json_float_options_t) == 2 * sizeof(size_t) &&
                     JB_ALIGNOF(jb_json_float_options_t) == sizeof(size_t) &&
                     offsetof(jb_json_float_options_t, reject_nonzero_to_zero) == 0 &&
                     offsetof(jb_json_float_options_t, normalize_negative_zero) == 1 &&
                     offsetof(jb_json_float_options_t, max_work) == sizeof(size_t),
                 "jb JSON float options layout");
#endif
