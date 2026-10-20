#ifndef JB_UNICODE_H
#define JB_UNICODE_H

/** @file unicode.h
 * Optional Unicode normalization and character properties. Build with
 * JHBASE_UNICODE=ON and link jb::unicode. Calls allocate no memory and retain
 * no state. JSON parsing never normalizes text implicitly.
 */
#include "jb/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Character properties used by configuration validation. */
typedef struct {
  bool printable;  /**< Letters, marks, numbers, punctuation, symbols and ASCII space. */
  bool whitespace; /**< Unicode separators or bidirectional whitespace controls. */
} jb_unicode_properties_t;

/** Return the pinned Unicode data version as a library-owned static string. */
const char *jb_unicode_version(void);

/** Read character properties without changing the result on failure.
 * @param scalar Unicode scalar value, including U+0000.
 * @param out Required result; does not overlap other arguments.
 * @return HAL_OK or HAL_EINVAL for NULL output, a surrogate or an invalid scalar.
 * Printable excludes separator/control/unassigned categories, except U+0020.
 * Whitespace matches the separator and bidirectional rules used by Python's
 * str.isspace(), evaluated with this module's pinned Unicode data.
 */
hal_status_t jb_unicode_properties(uint32_t scalar, jb_unicode_properties_t *out);

/** Measure the typed workspace needed for NFC, including terminator space.
 * @param input UTF-8 bytes; NULL is allowed only for zero length. NUL is data.
 * @param length Exact byte count, without an implied terminator.
 * @param max_work Work estimate limit, in units of length + n + n*n, where n
 * is the decomposed scalar count. The conservative estimate bounds reordering
 * before calling upstream; it is not a deadline or an interruptible timeout.
 * @param required Required result in int32_t elements, not bytes; unchanged
 * on failure. Empty input needs one element. Result objects must not overlap.
 * @return HAL_OK, HAL_EINVAL for invalid pointers, HAL_EPROTO for invalid
 * UTF-8, HAL_EOVERFLOW for length/work/size limits, or HAL_EINTERNAL.
 * Length limits are checked before reading input. A query allocates nothing.
 */
hal_status_t jb_unicode_nfc_measure(const char *input, size_t length, size_t max_work,
                                    size_t *required);

/** Normalize UTF-8 to NFC using an actual int32_t array supplied by the caller.
 * @param input UTF-8 bytes; NULL is allowed only for zero length.
 * @param length Exact input byte count; embedded NUL and its suffix are preserved.
 * @param scratch Required, naturally aligned int32_t array, separate from input.
 * @param capacity Scratch capacity in elements; measure first with nfc_measure.
 * @param max_work The same work estimate limit as nfc_measure.
 * @param out_text Required result: UTF-8 bytes in scratch, with an extra NUL.
 * The text lasts until the next write to scratch; inspect it only as bytes.
 * @param out_length Required result in bytes, excluding the extra terminator.
 * @return HAL_OK, HAL_EINVAL for invalid pointers/alignment/overlap,
 * HAL_EPROTO for invalid UTF-8, HAL_EOVERFLOW for capacity/length/work limits,
 * or HAL_EINTERNAL. Valid outputs are reset to NULL/zero before work begins;
 * errors invalidate the result, and scratch may have been partially written.
 * Input, scratch and result objects must not overlap. A later call reinitializes
 * the array as int32_t before normalization. No compatibility mapping, case
 * folding or character stripping is performed.
 */
hal_status_t jb_unicode_nfc(const char *input, size_t length, int32_t *scratch, size_t capacity,
                            size_t max_work, const char **out_text, size_t *out_length);

#ifdef __cplusplus
}
#endif
#endif
