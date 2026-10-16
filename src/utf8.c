#include "jb/utf8.h"

hal_status_t jb_utf8_decode(const char *input, size_t length, uint32_t *out_scalar,
                            size_t *out_consumed) {
  if ((input == NULL && length != 0) || out_scalar == NULL || out_consumed == NULL) {
    return HAL_EINVAL;
  }
  if (length == 0) {
    return HAL_EPROTO;
  }
  const unsigned char lead = (unsigned char)input[0];
  size_t count;
  uint32_t scalar;
  uint32_t minimum;
  if (lead < 0x80u) {
    count = 1;
    scalar = lead;
    minimum = 0;
  } else if (lead >= 0xc2u && lead <= 0xdfu) {
    count = 2;
    scalar = lead & 0x1fu;
    minimum = 0x80;
  } else if (lead >= 0xe0u && lead <= 0xefu) {
    count = 3;
    scalar = lead & 0x0fu;
    minimum = 0x800;
  } else if (lead >= 0xf0u && lead <= 0xf4u) {
    count = 4;
    scalar = lead & 0x07u;
    minimum = 0x10000;
  } else {
    return HAL_EPROTO;
  }
  if (count > length) {
    return HAL_EPROTO;
  }
  for (size_t i = 1; i < count; ++i) {
    const unsigned char byte = (unsigned char)input[i];
    if ((byte & 0xc0u) != 0x80u) {
      return HAL_EPROTO;
    }
    scalar = (scalar << 6) | (byte & 0x3fu);
  }
  if (scalar < minimum || scalar > 0x10ffffu || (scalar >= 0xd800u && scalar <= 0xdfffu)) {
    return HAL_EPROTO;
  }
  *out_scalar = scalar;
  *out_consumed = count;
  return HAL_OK;
}

hal_status_t jb_utf8_encode(uint32_t scalar, char *buffer, size_t capacity, size_t *required) {
  if (required == NULL || (buffer == NULL && capacity != 0) || scalar > 0x10ffffu ||
      (scalar >= 0xd800u && scalar <= 0xdfffu)) {
    return HAL_EINVAL;
  }
  const size_t count = scalar < 0x80u ? 1 : scalar < 0x800u ? 2 : scalar < 0x10000u ? 3 : 4;
  *required = count;
  if (buffer == NULL) {
    return HAL_OK;
  }
  if (capacity < count) {
    return HAL_EOVERFLOW;
  }
  if (count == 1) {
    buffer[0] = (char)scalar;
  } else {
    for (size_t i = count - 1; i != 0; --i) {
      buffer[i] = (char)(0x80u | (scalar & 0x3fu));
      scalar >>= 6;
    }
    const unsigned char prefix = count == 2 ? 0xc0u : count == 3 ? 0xe0u : 0xf0u;
    buffer[0] = (char)(prefix | scalar);
  }
  return HAL_OK;
}

hal_status_t jb_utf8_validate(const char *input, size_t length, size_t *error_offset) {
  if (input == NULL && length != 0) {
    return HAL_EINVAL;
  }
  size_t offset = 0;
  while (offset < length) {
    uint32_t scalar;
    size_t consumed;
    const hal_status_t status = jb_utf8_decode(input + offset, length - offset, &scalar, &consumed);
    if (status != HAL_OK) {
      if (error_offset != NULL) {
        *error_offset = offset;
      }
      return status;
    }
    offset += consumed;
  }
  if (error_offset != NULL) {
    *error_offset = SIZE_MAX;
  }
  return HAL_OK;
}
