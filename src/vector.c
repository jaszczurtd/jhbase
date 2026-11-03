#include "jb/vector.h"
#include "container_internal.h"
#include "jb/array.h"

static jb_container_storage_t vector_storage(const jb_vector_t *vector) {
  const jb_container_storage_t storage = {
      vector->data, vector->size, vector->capacity, vector->element_size, 0, &vector->allocator};
  return storage;
}

static hal_status_t vector_state(const jb_vector_t *vector, jb_container_storage_t *storage) {
  if (vector == NULL)
    return HAL_EINVAL;
  *storage = vector_storage(vector);
  return jb_container_valid(storage) ? HAL_OK : HAL_ESTATE;
}

static hal_status_t vector_publish(jb_vector_t *vector, const jb_container_storage_t *storage,
                                   hal_status_t status) {
  if (status == HAL_OK) {
    vector->data = storage->data;
    vector->size = storage->size;
    vector->capacity = storage->capacity;
  }
  return status;
}

hal_status_t jb_vector_init(jb_vector_t *vector, size_t element_size,
                            const jb_allocator_t *allocator) {
  if (vector == NULL || element_size == 0 || !jb_container_allocator_valid(allocator))
    return HAL_EINVAL;
  if (vector->data != NULL || vector->size != 0 || vector->capacity != 0 ||
      vector->element_size != 0 || !jb_container_allocator_zero(&vector->allocator))
    return HAL_ESTATE;
  vector->element_size = element_size;
  vector->allocator = *allocator;
  return HAL_OK;
}

hal_status_t jb_vector_reserve(jb_vector_t *vector, size_t capacity) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_reserve(&storage, capacity));
}

hal_status_t jb_vector_shrink_to_fit(jb_vector_t *vector) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_reallocate(&storage, storage.size));
}

hal_status_t jb_vector_assign(jb_vector_t *vector, const void *values, size_t count) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_assign(&storage, values, count));
}

hal_status_t jb_vector_append(jb_vector_t *vector, const void *values, size_t count) {
  if (vector == NULL)
    return HAL_EINVAL;
  return jb_vector_insert(vector, vector->size, values, count);
}

hal_status_t jb_vector_insert(jb_vector_t *vector, size_t index, const void *values, size_t count) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_insert(&storage, index, values, count));
}

hal_status_t jb_vector_erase(jb_vector_t *vector, size_t index, size_t count) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_erase(&storage, index, count));
}

hal_status_t jb_vector_resize(jb_vector_t *vector, size_t count) {
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  return vector_publish(vector, &storage, jb_container_resize(&storage, count, 0));
}

void jb_vector_clear(jb_vector_t *vector) {
  jb_container_storage_t storage;
  if (vector_state(vector, &storage) == HAL_OK)
    vector->size = 0;
}

void jb_vector_destroy(jb_vector_t *vector) {
  if (vector == NULL)
    return;
  jb_container_storage_t storage;
  if (vector_state(vector, &storage) == HAL_OK && storage.data != NULL)
    storage.allocator->release(storage.allocator->context, storage.data);
  *vector = (jb_vector_t){0};
}

const void *jb_vector_const_data(const jb_vector_t *vector) {
  jb_container_storage_t storage;
  return vector_state(vector, &storage) == HAL_OK ? vector->data : NULL;
}

void *jb_vector_data(const jb_vector_t *vector) { return (void *)jb_vector_const_data(vector); }

const void *jb_vector_const_at(const jb_vector_t *vector, size_t index) {
  jb_container_storage_t storage;
  if (vector_state(vector, &storage) != HAL_OK || index >= storage.size)
    return NULL;
  return storage.data + index * storage.element_size;
}

void *jb_vector_at(const jb_vector_t *vector, size_t index) {
  return (void *)jb_vector_const_at(vector, index);
}

size_t jb_vector_size(const jb_vector_t *vector) {
  jb_container_storage_t storage;
  return vector_state(vector, &storage) == HAL_OK ? vector->size : 0;
}

size_t jb_vector_capacity(const jb_vector_t *vector) {
  jb_container_storage_t storage;
  return vector_state(vector, &storage) == HAL_OK ? vector->capacity : 0;
}

size_t jb_vector_element_size(const jb_vector_t *vector) {
  jb_container_storage_t storage;
  return vector_state(vector, &storage) == HAL_OK ? vector->element_size : 0;
}

hal_status_t jb_vector_detach(jb_vector_t *vector, void **data, size_t *count, size_t *capacity) {
  if (data == NULL || count == NULL || capacity == NULL)
    return HAL_EINVAL;
  jb_container_storage_t storage;
  const hal_status_t status = vector_state(vector, &storage);
  if (status != HAL_OK)
    return status;
  const void *outputs[] = {data, count, capacity};
  const size_t bytes[] = {sizeof(*data), sizeof(*count), sizeof(*capacity)};
  for (size_t i = 0; i < COUNTOF(outputs); ++i) {
    if (jb_container_overlap(outputs[i], bytes[i], vector, sizeof(*vector)) ||
        jb_container_overlap(outputs[i], bytes[i], storage.data,
                             storage.capacity * storage.element_size))
      return HAL_EINVAL;
    for (size_t j = 0; j < i; ++j)
      if (jb_container_overlap(outputs[i], bytes[i], outputs[j], bytes[j]))
        return HAL_EINVAL;
  }
  *data = vector->data;
  *count = vector->size;
  *capacity = vector->capacity;
  vector->data = NULL;
  vector->size = 0;
  vector->capacity = 0;
  return HAL_OK;
}
