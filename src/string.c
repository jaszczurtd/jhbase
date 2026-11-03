#include "jb/string.h"
#include "container_internal.h"

static jb_container_storage_t string_storage(const jb_string_t *string) {
  const jb_container_storage_t storage = {
      (unsigned char *)string->data, string->length, string->capacity, 1, 1, &string->allocator};
  return storage;
}

static hal_status_t string_state(const jb_string_t *string, jb_container_storage_t *storage) {
  if (string == NULL)
    return HAL_EINVAL;
  *storage = string_storage(string);
  return jb_container_valid(storage) ? HAL_OK : HAL_ESTATE;
}

static hal_status_t string_publish(jb_string_t *string, const jb_container_storage_t *storage,
                                   hal_status_t status) {
  if (status == HAL_OK) {
    string->data = (char *)storage->data;
    string->length = storage->size;
    string->capacity = storage->capacity;
  }
  return status;
}

hal_status_t jb_string_init(jb_string_t *string, const jb_allocator_t *allocator) {
  if (string == NULL || !jb_container_allocator_valid(allocator))
    return HAL_EINVAL;
  if (string->data != NULL || string->length != 0 || string->capacity != 0 ||
      !jb_container_allocator_zero(&string->allocator))
    return HAL_ESTATE;
  string->allocator = *allocator;
  return HAL_OK;
}

hal_status_t jb_string_reserve(jb_string_t *string, size_t capacity) {
  jb_container_storage_t storage;
  const hal_status_t status = string_state(string, &storage);
  if (status != HAL_OK)
    return status;
  return string_publish(string, &storage, jb_container_reserve(&storage, capacity));
}

hal_status_t jb_string_assign(jb_string_t *string, const char *data, size_t length) {
  jb_container_storage_t storage;
  const hal_status_t status = string_state(string, &storage);
  if (status != HAL_OK)
    return status;
  return string_publish(string, &storage, jb_container_assign(&storage, data, length));
}

hal_status_t jb_string_append(jb_string_t *string, const char *data, size_t length) {
  if (string == NULL)
    return HAL_EINVAL;
  return jb_string_insert(string, string->length, data, length);
}

hal_status_t jb_string_insert(jb_string_t *string, size_t index, const char *data, size_t length) {
  jb_container_storage_t storage;
  const hal_status_t status = string_state(string, &storage);
  if (status != HAL_OK)
    return status;
  return string_publish(string, &storage, jb_container_insert(&storage, index, data, length));
}

hal_status_t jb_string_erase(jb_string_t *string, size_t index, size_t length) {
  jb_container_storage_t storage;
  const hal_status_t status = string_state(string, &storage);
  if (status != HAL_OK)
    return status;
  return string_publish(string, &storage, jb_container_erase(&storage, index, length));
}

hal_status_t jb_string_resize(jb_string_t *string, size_t length, char fill) {
  jb_container_storage_t storage;
  const hal_status_t status = string_state(string, &storage);
  if (status != HAL_OK)
    return status;
  return string_publish(string, &storage,
                        jb_container_resize(&storage, length, (unsigned char)fill));
}

void jb_string_clear(jb_string_t *string) {
  jb_container_storage_t storage;
  if (string_state(string, &storage) == HAL_OK) {
    storage.size = 0;
    jb_container_terminate(&storage);
    (void)string_publish(string, &storage, HAL_OK);
  }
}

void jb_string_destroy(jb_string_t *string) {
  if (string == NULL)
    return;
  jb_container_storage_t storage;
  if (string_state(string, &storage) == HAL_OK && storage.data != NULL)
    storage.allocator->release(storage.allocator->context, storage.data);
  *string = (jb_string_t){0};
}

const char *jb_string_data(const jb_string_t *string) {
  jb_container_storage_t storage;
  if (string_state(string, &storage) != HAL_OK)
    return NULL;
  return string->data == NULL ? "" : string->data;
}

char *jb_string_mutable_data(const jb_string_t *string) {
  jb_container_storage_t storage;
  return string_state(string, &storage) == HAL_OK ? string->data : NULL;
}

hal_status_t jb_string_compare(const jb_string_t *left, const jb_string_t *right, int *ordering) {
  if (ordering == NULL)
    return HAL_EINVAL;
  jb_container_storage_t a, b;
  hal_status_t status = string_state(left, &a);
  if (status != HAL_OK)
    return status;
  status = string_state(right, &b);
  if (status != HAL_OK)
    return status;
  const size_t common = a.size < b.size ? a.size : b.size;
  int result = common == 0 ? 0 : memcmp(a.data, b.data, common);
  if (result == 0)
    result = (a.size > b.size) - (a.size < b.size);
  *ordering = result;
  return HAL_OK;
}

size_t jb_string_size(const jb_string_t *string) {
  jb_container_storage_t storage;
  return string_state(string, &storage) == HAL_OK ? string->length : 0;
}

size_t jb_string_capacity(const jb_string_t *string) {
  jb_container_storage_t storage;
  return string_state(string, &storage) == HAL_OK ? string->capacity : 0;
}
