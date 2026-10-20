#include "jb/unicode.h"
#include "utf8proc.h"

/* The public array must have the upstream element type, not merely its size. */
JB_STATIC_ASSERT(_Generic((int32_t *)0, utf8proc_int32_t *: 1, default: 0),
                 "utf8proc scratch must be an int32_t array");

static const unsigned int nfc_options = UTF8PROC_STABLE | UTF8PROC_COMPOSE;

const char *jb_unicode_version(void) { return utf8proc_unicode_version(); }

hal_status_t jb_unicode_properties(uint32_t scalar, jb_unicode_properties_t *out) {
  if (out == NULL || scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff)) {
    return HAL_EINVAL;
  }
  const utf8proc_property_t *p = utf8proc_get_property((int32_t)scalar);
  const jb_unicode_properties_t result = {
      scalar == 0x20 ||
          (p->category >= UTF8PROC_CATEGORY_LU && p->category <= UTF8PROC_CATEGORY_SO),
      p->category == UTF8PROC_CATEGORY_ZS || p->bidi_class == UTF8PROC_BIDI_CLASS_WS ||
          p->bidi_class == UTF8PROC_BIDI_CLASS_B || p->bidi_class == UTF8PROC_BIDI_CLASS_S};
  *out = result;
  return HAL_OK;
}

static hal_status_t upstream_status(utf8proc_ssize_t result) {
  if (result == UTF8PROC_ERROR_INVALIDUTF8) {
    return HAL_EPROTO;
  }
  return result == UTF8PROC_ERROR_OVERFLOW ? HAL_EOVERFLOW : HAL_EINTERNAL;
}

hal_status_t jb_unicode_nfc_measure(const char *input, size_t length, size_t max_work,
                                    size_t *required) {
  if (required == NULL || (input == NULL && length != 0)) {
    return HAL_EINVAL;
  }
  if (length > (size_t)PTRDIFF_MAX || length > max_work) {
    return HAL_EOVERFLOW;
  }
  const utf8proc_ssize_t result = utf8proc_decompose(
      (const unsigned char *)input, (utf8proc_ssize_t)length, NULL, 0, nfc_options);
  if (result < 0) {
    return upstream_status(result);
  }
  const size_t count = (size_t)result;
  /* Both the typed workspace and upstream's byte offsets must fit ptrdiff_t. */
  if (count >= (size_t)PTRDIFF_MAX / sizeof(int32_t) || count > (max_work - length) / (count + 1)) {
    return HAL_EOVERFLOW;
  }
  *required = count + 1;
  return HAL_OK;
}

static bool overlaps(const void *left, size_t left_bytes, const void *right, size_t right_bytes) {
  if (left_bytes == 0 || right_bytes == 0) {
    return false;
  }
  const uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
  return a <= b ? b - a < left_bytes : a - b < right_bytes;
}

hal_status_t jb_unicode_nfc(const char *input, size_t length, int32_t *scratch, size_t capacity,
                            size_t max_work, const char **out_text, size_t *out_length) {
  if (out_text != NULL) {
    *out_text = NULL;
  }
  if (out_length != NULL) {
    *out_length = 0;
  }
  if (out_text == NULL || out_length == NULL) {
    return HAL_EINVAL;
  }
  if (scratch == NULL || (uintptr_t)scratch % JB_ALIGNOF(int32_t) != 0) {
    return HAL_EINVAL;
  }
  size_t required;
  const hal_status_t status = jb_unicode_nfc_measure(input, length, max_work, &required);
  if (status != HAL_OK) {
    return status;
  }
  if (capacity < required || capacity > (size_t)PTRDIFF_MAX / sizeof(int32_t)) {
    return HAL_EOVERFLOW;
  }
  if (overlaps(input, length, scratch, capacity * sizeof(int32_t))) {
    return HAL_EINVAL;
  }
  utf8proc_ssize_t count =
      utf8proc_decompose((const unsigned char *)input, (utf8proc_ssize_t)length, scratch,
                         (utf8proc_ssize_t)capacity, nfc_options);
  if (count < 0) {
    return upstream_status(count);
  }
  if ((size_t)count != required - 1) {
    return HAL_EINTERNAL;
  }
  count = utf8proc_reencode(scratch, count, nfc_options);
  if (count < 0) {
    return upstream_status(count);
  }
  *out_text = (const char *)scratch;
  *out_length = (size_t)count;
  return HAL_OK;
}
