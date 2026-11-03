#ifndef JB_STRING_H
#define JB_STRING_H

/** @file string.h
 * Owned byte strings with explicit length and a trailing NUL. Embedded NULs
 * are preserved; no UTF-8 validation or character indexing is implied.
 * Initialize an owner with {0} in C or {} in C++, call init, then destroy. Do not shallow
 * copy a live owner or edit its fields. Synchronize concurrent accesses whenever
 * any participant mutates the owner.
 * All status-returning mutations leave the owner and its bytes unchanged on
 * error. A successful mutation may invalidate every borrowed data pointer.
 */
#include "jb/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Single owner. Fields are private despite the public stack-allocatable layout. */
typedef struct {
  char *data;
  size_t length, capacity;
  jb_allocator_t allocator;
} jb_string_t;

/** Initialize a zero owner without allocating. allocator and string are required.
 * Copies valid callbacks; returns HAL_EINVAL for NULL/missing callbacks or
 * HAL_ESTATE for a nonzero owner. allocator context must outlive the string.
 */
hal_status_t jb_string_init(jb_string_t *string, const jb_allocator_t *allocator);
/** Reserve exactly capacity payload bytes when growing, plus the trailing NUL.
 * Never shrinks. Returns HAL_OK, HAL_EINVAL for NULL, HAL_ESTATE for an inactive
 * owner, HAL_EOVERFLOW for size overflow or HAL_ENOMEM for allocator failure.
 */
hal_status_t jb_string_reserve(jb_string_t *string, size_t capacity);
/** Replace bytes. data may be NULL only at length zero. A source span inside
 * the string is supported, including its trailing NUL, but must not extend
 * into unused capacity. Returns reserve's errors or HAL_EINVAL for a bad span.
 */
hal_status_t jb_string_assign(jb_string_t *string, const char *data, size_t length);
/** Append exact bytes; arguments, internal aliases and errors follow assign. */
hal_status_t jb_string_append(jb_string_t *string, const char *data, size_t length);
/** Insert bytes before index (0..size). Internal aliases are copied before any
 * change and may allocate even with spare capacity. Bad index gives HAL_EINVAL;
 * other arguments and errors follow assign.
 */
hal_status_t jb_string_insert(jb_string_t *string, size_t index, const char *data, size_t length);
/** Erase length bytes at index; index may equal size only with length zero.
 * No allocation. Returns HAL_EINVAL for NULL/bad range or HAL_ESTATE if inactive.
 */
hal_status_t jb_string_erase(jb_string_t *string, size_t index, size_t length);
/** Set the payload length, filling new bytes with fill (including NUL).
 * Does not shrink capacity; errors follow reserve.
 */
hal_status_t jb_string_resize(jb_string_t *string, size_t length, char fill);
/** Remove payload bytes, keeping allocation and allocator. NULL/inactive is a no-op. */
void jb_string_clear(jb_string_t *string);
/** Release owned storage and reset the owner to {0}. NULL/zero owner is harmless. */
void jb_string_destroy(jb_string_t *string);
/** Borrow NUL-terminated bytes. Empty initialized strings return ""; NULL or
 * inactive owners return NULL. size, not strlen, includes embedded NUL bytes.
 */
const char *jb_string_data(const jb_string_t *string);
/** Borrow writable storage through an unchanged descriptor. Returns NULL for
 * an inactive owner or an empty string without an allocation. The caller may
 * replace payload bytes but must preserve length and data[length] == '\0'.
 * No access beyond the reserved storage is allowed. Resize before filling a
 * larger payload. A const descriptor does not make its allocation read-only.
 */
char *jb_string_mutable_data(const jb_string_t *string);
/** Compare payloads lexicographically as unsigned bytes, including embedded NUL.
 * A proper prefix orders before its longer string. Required ordering receives
 * a negative, zero or positive result. HAL_EINVAL reports NULL arguments and
 * HAL_ESTATE an inactive owner; failures preserve ordering. No allocation.
 */
hal_status_t jb_string_compare(const jb_string_t *left, const jb_string_t *right, int *ordering);
/** Payload bytes, excluding the trailing NUL; zero for NULL/inactive owners. */
size_t jb_string_size(const jb_string_t *string);
/** Reserved payload bytes; zero for NULL/inactive owners. */
size_t jb_string_capacity(const jb_string_t *string);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(JB_ALIGNOF(jb_string_t) == sizeof(size_t) &&
                     sizeof(jb_string_t) == 6 * sizeof(size_t) &&
                     offsetof(jb_string_t, length) == sizeof(size_t) &&
                     offsetof(jb_string_t, capacity) == 2 * sizeof(size_t) &&
                     offsetof(jb_string_t, allocator) == 3 * sizeof(size_t),
                 "jb requires a naturally aligned string");
#endif
