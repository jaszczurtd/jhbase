#ifndef JB_CONTAINER_INTERNAL_H
#define JB_CONTAINER_INTERNAL_H

#include "jb/allocator.h"
#include <string.h>

/* Local view of an owner. Public wrappers publish fields only after success. */
typedef struct {
  unsigned char *data;
  size_t size, capacity, element_size, tail;
  const jb_allocator_t *allocator;
} jb_container_storage_t;

static inline bool jb_container_allocator_valid(const jb_allocator_t *allocator) {
  return allocator != NULL && allocator->allocate != NULL && allocator->release != NULL;
}

static inline bool jb_container_allocator_zero(const jb_allocator_t *allocator) {
  return allocator->context == NULL && allocator->allocate == NULL && allocator->release == NULL;
}

static inline size_t jb_container_maximum(const jb_container_storage_t *storage) {
  return (SIZE_MAX - storage->tail) / storage->element_size;
}

static inline bool jb_container_valid(const jb_container_storage_t *storage) {
  return jb_container_allocator_valid(storage->allocator) && storage->element_size != 0 &&
         storage->tail <= 1 && storage->size <= storage->capacity &&
         storage->capacity <= jb_container_maximum(storage) &&
         ((storage->data == NULL) == (storage->capacity == 0));
}

/* Integer differences avoid ordering/subtracting pointers to unrelated objects. */
static inline bool jb_container_overlap(const void *a, size_t a_bytes, const void *b,
                                        size_t b_bytes) {
  if (a_bytes == 0 || b_bytes == 0)
    return false;
  const uintptr_t left = (uintptr_t)a;
  const uintptr_t right = (uintptr_t)b;
  return left <= right ? right - left < a_bytes : left - right < b_bytes;
}

static inline hal_status_t jb_container_source(const jb_container_storage_t *storage,
                                               const void *source, size_t bytes, bool *aliased) {
  *aliased = false;
  if (bytes == 0)
    return HAL_OK;
  if (source == NULL)
    return HAL_EINVAL;
  if (storage->data == NULL)
    return HAL_OK;
  const size_t allocated = storage->capacity * storage->element_size + storage->tail;
  if (!jb_container_overlap(storage->data, allocated, source, bytes))
    return HAL_OK;
  const uintptr_t start = (uintptr_t)source;
  const uintptr_t base = (uintptr_t)storage->data;
  const size_t live = storage->size * storage->element_size + storage->tail;
  if (start < base || start - base > live || bytes > live - (size_t)(start - base))
    return HAL_EINVAL;
  *aliased = true;
  return HAL_OK;
}

static inline size_t jb_container_grown_capacity(const jb_container_storage_t *storage,
                                                 size_t required) {
  const size_t maximum = jb_container_maximum(storage);
  size_t capacity = storage->capacity;
  if (capacity < 8)
    capacity = maximum < 8 ? maximum : 8;
  while (capacity < required) {
    const size_t step = capacity / 2 + 1;
    if (step > maximum - capacity)
      return required;
    capacity += step;
  }
  return capacity;
}

static inline void jb_container_terminate(jb_container_storage_t *storage) {
  if (storage->tail != 0 && storage->data != NULL)
    storage->data[storage->size * storage->element_size] = 0;
}

static inline void jb_container_publish(jb_container_storage_t *storage, unsigned char *data,
                                        size_t capacity) {
  if (storage->data != NULL)
    storage->allocator->release(storage->allocator->context, storage->data);
  storage->data = data;
  storage->capacity = capacity;
}

static inline hal_status_t jb_container_reallocate(jb_container_storage_t *storage,
                                                   size_t capacity) {
  if (capacity > jb_container_maximum(storage))
    return HAL_EOVERFLOW;
  if (capacity < storage->size)
    return HAL_EINVAL;
  if (capacity == storage->capacity)
    return HAL_OK;
  unsigned char *data = NULL;
  if (capacity != 0 || storage->tail != 0) {
    data = storage->allocator->allocate(storage->allocator->context,
                                        capacity * storage->element_size + storage->tail);
    if (data == NULL)
      return HAL_ENOMEM;
  }
  if (storage->size != 0)
    memcpy(data, storage->data, storage->size * storage->element_size);
  jb_container_publish(storage, data, capacity);
  jb_container_terminate(storage);
  return HAL_OK;
}

static inline hal_status_t jb_container_reserve(jb_container_storage_t *storage, size_t capacity) {
  return capacity <= storage->capacity ? HAL_OK : jb_container_reallocate(storage, capacity);
}

static inline hal_status_t jb_container_assign(jb_container_storage_t *storage, const void *source,
                                               size_t count) {
  if (count > jb_container_maximum(storage))
    return HAL_EOVERFLOW;
  const size_t bytes = count * storage->element_size;
  bool aliased;
  const hal_status_t status = jb_container_source(storage, source, bytes, &aliased);
  if (status != HAL_OK)
    return status;
  if (count > storage->capacity) {
    const size_t grown = jb_container_grown_capacity(storage, count);
    unsigned char *data = storage->allocator->allocate(
        storage->allocator->context, grown * storage->element_size + storage->tail);
    if (data == NULL)
      return HAL_ENOMEM;
    memcpy(data, source, bytes);
    jb_container_publish(storage, data, grown);
  } else if (bytes != 0) {
    memmove(storage->data, source, bytes);
  }
  storage->size = count;
  jb_container_terminate(storage);
  return HAL_OK;
}

static inline hal_status_t jb_container_insert(jb_container_storage_t *storage, size_t index,
                                               const void *source, size_t count) {
  if (index > storage->size)
    return HAL_EINVAL;
  if (count > jb_container_maximum(storage) - storage->size)
    return HAL_EOVERFLOW;
  const size_t bytes = count * storage->element_size;
  bool aliased;
  const hal_status_t status = jb_container_source(storage, source, bytes, &aliased);
  if (status != HAL_OK || count == 0)
    return status;
  const size_t required = storage->size + count;
  const size_t prefix = index * storage->element_size;
  const size_t suffix = (storage->size - index) * storage->element_size;
  if (required > storage->capacity || aliased) {
    const size_t capacity = required > storage->capacity
                                ? jb_container_grown_capacity(storage, required)
                                : storage->capacity;
    unsigned char *data = storage->allocator->allocate(
        storage->allocator->context, capacity * storage->element_size + storage->tail);
    if (data == NULL)
      return HAL_ENOMEM;
    if (prefix != 0)
      memcpy(data, storage->data, prefix);
    memcpy(data + prefix, source, bytes);
    if (suffix != 0)
      memcpy(data + prefix + bytes, storage->data + prefix, suffix);
    jb_container_publish(storage, data, capacity);
  } else {
    if (suffix != 0)
      memmove(storage->data + prefix + bytes, storage->data + prefix, suffix);
    memcpy(storage->data + prefix, source, bytes);
  }
  storage->size = required;
  jb_container_terminate(storage);
  return HAL_OK;
}

static inline hal_status_t jb_container_erase(jb_container_storage_t *storage, size_t index,
                                              size_t count) {
  if (index > storage->size || count > storage->size - index)
    return HAL_EINVAL;
  const size_t suffix = storage->size - index - count;
  if (count != 0 && suffix != 0)
    memmove(storage->data + index * storage->element_size,
            storage->data + (index + count) * storage->element_size,
            suffix * storage->element_size);
  storage->size -= count;
  jb_container_terminate(storage);
  return HAL_OK;
}

static inline hal_status_t jb_container_resize(jb_container_storage_t *storage, size_t count,
                                               unsigned char fill) {
  if (count > jb_container_maximum(storage))
    return HAL_EOVERFLOW;
  const size_t capacity =
      count > storage->capacity ? jb_container_grown_capacity(storage, count) : storage->capacity;
  const hal_status_t status = jb_container_reserve(storage, capacity);
  if (status != HAL_OK)
    return status;
  if (count > storage->size)
    memset(storage->data + storage->size * storage->element_size, fill,
           (count - storage->size) * storage->element_size);
  storage->size = count;
  jb_container_terminate(storage);
  return HAL_OK;
}

#endif
