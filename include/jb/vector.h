#ifndef JB_VECTOR_H
#define JB_VECTOR_H

/** @file vector.h
 * Owned contiguous storage for bytewise movable C elements. There are no
 * constructors, destructors or deep copies. The caller releases resources
 * held by elements before replacing/removing them. Use sizeof(T) as element
 * size; T must require no alignment greater than jb_max_align_t. Do not store
 * C++ objects requiring construction, movement or destruction here.
 * Initialize with {0} in C or {} in C++, then init and destroy. Never shallow-copy a live owner
 * or edit its fields. Synchronize concurrent accesses involving mutation. Failed mutations
 * preserve the owner and its bytes; successful mutations may invalidate
 * borrowed pointers. New elements from resize contain all-zero bytes, which
 * need not represent a null pointer or a valid value of every C type.
 */
#include "jb/allocator.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Single owner; fields are private despite the stack-allocatable layout. */
typedef struct {
  void *data;
  size_t size, capacity, element_size;
  jb_allocator_t allocator;
} jb_vector_t;

/** Initialize a zero owner without allocating. All pointers and a nonzero
 * element_size are required. Returns HAL_EINVAL for invalid arguments/callbacks,
 * HAL_ESTATE for a nonzero owner. Copies the allocator; its context must outlive
 * the vector. element_size must preserve the alignment of successive T values.
 */
hal_status_t jb_vector_init(jb_vector_t *vector, size_t element_size,
                            const jb_allocator_t *allocator);
/** Reserve exactly capacity elements when growing; never shrink. Returns HAL_OK, HAL_EINVAL for
 * NULL, HAL_ESTATE for an inactive owner, HAL_EOVERFLOW for byte-size overflow,
 * or HAL_ENOMEM. The allocator receives no zero-byte requests.
 */
hal_status_t jb_vector_reserve(jb_vector_t *vector, size_t capacity);
/** Reduce capacity to size, releasing an empty buffer. May allocate a smaller
 * block and copy live elements. HAL_ENOMEM preserves the owner and its data;
 * other statuses follow reserve. Success may invalidate borrowed pointers.
 */
hal_status_t jb_vector_shrink_to_fit(jb_vector_t *vector);
/** Replace elements by copying count * element_size bytes. values may be NULL
 * only at count zero. An internal source span must fit the current live elements;
 * unused capacity is not a source. Internal aliases are supported. Returns
 * reserve's errors or HAL_EINVAL for a bad source span.
 */
hal_status_t jb_vector_assign(jb_vector_t *vector, const void *values, size_t count);
/** Append copied elements; arguments, aliases and errors follow assign. */
hal_status_t jb_vector_append(jb_vector_t *vector, const void *values, size_t count);
/** Insert before index (0..size). An internal alias may allocate despite spare
 * capacity. HAL_EINVAL for an invalid index; other rules follow assign.
 */
hal_status_t jb_vector_insert(jb_vector_t *vector, size_t index, const void *values, size_t count);
/** Erase count elements at index, without allocation/destruction. Bad range or
 * NULL gives HAL_EINVAL; inactive owner gives HAL_ESTATE.
 */
hal_status_t jb_vector_erase(jb_vector_t *vector, size_t index, size_t count);
/** Resize to count elements, filling new elements with zero bytes.
 * Does not shrink capacity; errors follow reserve.
 */
hal_status_t jb_vector_resize(jb_vector_t *vector, size_t count);
/** Remove live elements, retaining storage; no destructors. NULL/inactive is a no-op. */
void jb_vector_clear(jb_vector_t *vector);
/** Release storage and reset to {0}; no element destructors. NULL/zero is harmless. */
void jb_vector_destroy(jb_vector_t *vector);
/** Borrow mutable storage through an unchanged descriptor, or NULL if empty
 * without allocation/invalid. A const descriptor does not make its owned bytes
 * const. Changes must preserve element representations; capacity is not size.
 */
void *jb_vector_data(const jb_vector_t *vector);
/** Borrow immutable storage; otherwise follows data. */
const void *jb_vector_const_data(const jb_vector_t *vector);
/** Borrow one mutable live element through an unchanged descriptor, or NULL
 * for an invalid owner/out-of-range index. See data for const descriptor semantics.
 */
void *jb_vector_at(const jb_vector_t *vector, size_t index);
/** Borrow one immutable live element; otherwise follows at. */
const void *jb_vector_const_at(const jb_vector_t *vector, size_t index);
/** Live element count; zero for NULL/inactive owners. */
size_t jb_vector_size(const jb_vector_t *vector);
/** Reserved element count; zero for NULL/inactive owners. */
size_t jb_vector_capacity(const jb_vector_t *vector);
/** Bytes per element; zero for NULL/inactive owners. */
size_t jb_vector_element_size(const jb_vector_t *vector);
/** Transfer the allocation and its element counts to the caller, leaving an
 * empty initialized vector with the same allocator and element_size. All outputs
 * are required and must not overlap one another, the owner or its allocation.
 * The caller releases non-NULL data with the original allocator; skip release
 * when data is NULL. The allocator is never required to accept NULL.
 * This operation only transfers storage allocated by that allocator; it does not
 * adopt external buffers. HAL_EINVAL reports NULL/overlapping outputs, HAL_ESTATE
 * an inactive owner. Failure preserves every output and the owner. No allocation.
 */
hal_status_t jb_vector_detach(jb_vector_t *vector, void **data, size_t *count, size_t *capacity);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(JB_ALIGNOF(jb_vector_t) == sizeof(size_t) &&
                     sizeof(jb_vector_t) == 7 * sizeof(size_t) &&
                     offsetof(jb_vector_t, size) == sizeof(size_t) &&
                     offsetof(jb_vector_t, capacity) == 2 * sizeof(size_t) &&
                     offsetof(jb_vector_t, element_size) == 3 * sizeof(size_t) &&
                     offsetof(jb_vector_t, allocator) == 4 * sizeof(size_t),
                 "jb requires a naturally aligned vector");
#endif
