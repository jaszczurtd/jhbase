#ifndef JB_UTF8_H
#define JB_UTF8_H

/** @file utf8.h
 * Length-based UTF-8 for Unicode scalar values, including U+0000.
 * Functions allocate no memory and leave ordinary outputs unchanged on failure.
 * Input, output and result objects must not overlap.
 */
#include "jb/status.h"
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Decode the first scalar in input, without consuming later bytes.
 * @param input Bytes; NULL is allowed only when length is zero.
 * @param length Available bytes, without an implied terminator.
 * @param out_scalar Required result, from U+0000 through U+10FFFF excluding surrogates.
 * @param out_consumed Required result, between one and four bytes.
 * @return HAL_OK, HAL_EINVAL for invalid pointers, or HAL_EPROTO for invalid
 * or incomplete UTF-8 (including empty input). Overlong encodings are rejected.
 */
hal_status_t jb_utf8_decode(const char *input, size_t length, uint32_t *out_scalar,
                            size_t *out_consumed);

/** Encode one Unicode scalar, without a terminator or partial output.
 * @param scalar Unicode scalar value; surrogates and values above U+10FFFF fail.
 * @param buffer Destination; NULL with capacity zero measures the required bytes.
 * @param capacity Available destination bytes.
 * @param required Required result; receives one through four bytes even on overflow.
 * @return HAL_OK, HAL_EINVAL for invalid arguments/scalars, or HAL_EOVERFLOW.
 */
hal_status_t jb_utf8_encode(uint32_t scalar, char *buffer, size_t capacity, size_t *required);

/** Validate exactly length bytes; embedded NUL is a valid scalar.
 * @param input Bytes; NULL is allowed for zero length.
 * @param length Number of bytes to validate.
 * @param error_offset Optional first invalid sequence's lead-byte offset;
 * receives SIZE_MAX on success and is unchanged for invalid arguments.
 * @return HAL_OK, HAL_EINVAL for invalid arguments, or HAL_EPROTO.
 */
hal_status_t jb_utf8_validate(const char *input, size_t length, size_t *error_offset);

#ifdef __cplusplus
}
#endif
#endif
