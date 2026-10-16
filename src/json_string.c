#include "json_internal.h"

static hal_status_t bad_string(jb_json_error_t *error, jb_json_error_code_t code, size_t offset) {
  if (error != NULL) {
    *error = (jb_json_error_t){code, offset, SIZE_MAX, 0};
  }
  return HAL_EPROTO;
}

static hal_status_t hex_quad(jb_json_span_t input, size_t *position, uint32_t *out,
                             jb_json_error_t *error) {
  uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i) {
    if (*position == input.length) {
      return bad_string(error, JB_JSON_ERROR_ESCAPE, *position);
    }
    const unsigned char c = (unsigned char)input.data[*position];
    unsigned digit;
    if (c >= '0' && c <= '9') {
      digit = c - '0';
    } else if (c >= 'a' && c <= 'f') {
      digit = c - 'a' + 10u;
    } else if (c >= 'A' && c <= 'F') {
      digit = c - 'A' + 10u;
    } else {
      return bad_string(error, JB_JSON_ERROR_ESCAPE, *position);
    }
    value = value * 16u + digit;
    ++*position;
  }
  *out = value;
  return HAL_OK;
}

hal_status_t jb_json_scalar(jb_json_span_t input, size_t *position, uint32_t *out,
                            jb_json_error_t *error) {
  const size_t start = *position;
  if (start == input.length) {
    return bad_string(error, JB_JSON_ERROR_SYNTAX, start);
  }
  const unsigned char c = (unsigned char)input.data[start];
  if (c < 0x20u) {
    return bad_string(error, JB_JSON_ERROR_SYNTAX, start);
  }
  if (c != '\\') {
    size_t consumed;
    if (jb_utf8_decode(input.data + start, input.length - start, out, &consumed) != HAL_OK) {
      return bad_string(error, JB_JSON_ERROR_UTF8, start);
    }
    *position += consumed;
    return HAL_OK;
  }
  ++*position;
  if (*position == input.length) {
    return bad_string(error, JB_JSON_ERROR_ESCAPE, *position);
  }
  const char escaped = input.data[(*position)++];
  switch (escaped) {
  case '"':
  case '\\':
  case '/':
    *out = (unsigned char)escaped;
    return HAL_OK;
  case 'b':
    *out = 8;
    return HAL_OK;
  case 'f':
    *out = 12;
    return HAL_OK;
  case 'n':
    *out = 10;
    return HAL_OK;
  case 'r':
    *out = 13;
    return HAL_OK;
  case 't':
    *out = 9;
    return HAL_OK;
  case 'u':
    break;
  default:
    return bad_string(error, JB_JSON_ERROR_ESCAPE, *position - 1);
  }
  uint32_t scalar;
  hal_status_t status = hex_quad(input, position, &scalar, error);
  if (status != HAL_OK) {
    return status;
  }
  if (scalar >= 0xd800u && scalar <= 0xdbffu) {
    if (input.length - *position < 2 || input.data[*position] != '\\' ||
        input.data[*position + 1] != 'u') {
      return bad_string(error, JB_JSON_ERROR_ESCAPE, start);
    }
    *position += 2;
    uint32_t low;
    status = hex_quad(input, position, &low, error);
    if (status != HAL_OK) {
      return status;
    }
    if (low < 0xdc00u || low > 0xdfffu) {
      return bad_string(error, JB_JSON_ERROR_ESCAPE, start);
    }
    scalar = 0x10000u + (scalar - 0xd800u) * 0x400u + (low - 0xdc00u);
  } else if (scalar >= 0xdc00u && scalar <= 0xdfffu) {
    return bad_string(error, JB_JSON_ERROR_ESCAPE, start);
  }
  *out = scalar;
  return HAL_OK;
}

hal_status_t jb_json_strings_equal(jb_json_span_t left, jb_json_span_t right, bool right_escaped,
                                   size_t max_work, size_t *used, bool *out) {
  size_t a = 0, b = 0;
  if (*used == max_work) {
    return HAL_EOVERFLOW;
  }
  ++*used; /* Empty keys also consume comparison work. */
  while (a < left.length && b < right.length) {
    uint32_t ca, cb;
    size_t width_a, width_b;
    hal_status_t status = jb_json_scalar(left, &a, &ca, NULL);
    if (status != HAL_OK) {
      return status;
    }
    if (right_escaped) {
      status = jb_json_scalar(right, &b, &cb, NULL);
    } else {
      size_t consumed;
      status = jb_utf8_decode(right.data + b, right.length - b, &cb, &consumed);
      if (status == HAL_OK) {
        b += consumed;
      }
    }
    if (status != HAL_OK) {
      return status;
    }
    (void)jb_utf8_encode(ca, NULL, 0, &width_a);
    (void)jb_utf8_encode(cb, NULL, 0, &width_b);
    const size_t cost = width_a + width_b;
    if (cost > max_work - *used) {
      return HAL_EOVERFLOW;
    }
    *used += cost;
    if (ca != cb) {
      *out = false;
      return HAL_OK;
    }
  }
  *out = a == left.length && b == right.length;
  return HAL_OK;
}

hal_status_t jb_json_copy_string(jb_json_span_t raw, char *buffer, size_t capacity,
                                 size_t *required) {
  if (required == NULL || (buffer == NULL && capacity != 0)) {
    return HAL_EINVAL;
  }
  size_t length = 0, position = 0;
  while (position < raw.length) {
    uint32_t scalar;
    size_t width;
    hal_status_t status = jb_json_scalar(raw, &position, &scalar, NULL);
    if (status != HAL_OK) {
      return status;
    }
    (void)jb_utf8_encode(scalar, NULL, 0, &width);
    if (width > SIZE_MAX - length) {
      return HAL_EOVERFLOW;
    }
    length += width;
  }
  *required = length;
  if (buffer == NULL) {
    return HAL_OK;
  }
  if (length > capacity) {
    return HAL_EOVERFLOW;
  }
  position = 0;
  size_t written = 0;
  while (position < raw.length) {
    uint32_t scalar;
    size_t width;
    hal_status_t status = jb_json_scalar(raw, &position, &scalar, NULL);
    if (status != HAL_OK) {
      return status;
    }
    status = jb_utf8_encode(scalar, buffer + written, capacity - written, &width);
    if (status != HAL_OK) {
      return status;
    }
    written += width;
  }
  return HAL_OK;
}
