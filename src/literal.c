#include "jb/literal.h"
#include "json_internal.h"
#include "number_internal.h"
#include <string.h>

hal_status_t jb_c_literal_workspace_size(jb_json_node_t node, jb_c_literal_type_t type,
                                         size_t *out_bytes) {
  if (out_bytes == NULL || type > JB_C_STRING) {
    return HAL_EINVAL;
  }
  jb_json_type_t kind;
  hal_status_t status = jb_json_type(node, &kind);
  if (status != HAL_OK) {
    return status;
  }
  const jb_json_type_t expected =
      type == JB_C_BOOL ? JB_JSON_BOOL : (type == JB_C_STRING ? JB_JSON_STRING : JB_JSON_NUMBER);
  if (kind != expected) {
    return HAL_ETYPE;
  }
  size_t size = 0;
  if (type == JB_C_F32 || type == JB_C_F64) {
    status = jb_json_float_workspace_size(node, type == JB_C_F32 ? 32u : 64u, &size);
    if (status != HAL_OK) {
      return status;
    }
    if (size < JB_FLOAT_FORMAT_SCRATCH) {
      size = JB_FLOAT_FORMAT_SCRATCH;
    }
  }
  *out_bytes = size;
  return HAL_OK;
}

static hal_status_t c_string(jb_json_node_t node, char *out, size_t *length) {
  jb_json_span_t raw;
  hal_status_t status = jb_json_raw(node, &raw);
  if (status != HAL_OK) {
    return status;
  }
  const jb_json_span_t value = {raw.data + 1, raw.length - 2};
  size_t at = 0, used = 1;
  if (out != NULL) {
    out[0] = '"';
  }
  while (at < value.length) {
    uint32_t scalar;
    char bytes[4];
    size_t width;
    status = jb_json_scalar(value, &at, &scalar, NULL);
    if (status != HAL_OK) {
      return status;
    }
    (void)jb_utf8_encode(scalar, bytes, sizeof(bytes), &width);
    for (size_t i = 0; i < width; ++i) {
      const unsigned char c = (unsigned char)bytes[i];
      char encoded[4];
      size_t count;
      if (c == '"' || c == '\\' || c == '?') {
        encoded[0] = '\\';
        encoded[1] = (char)c;
        count = 2;
      } else if (c >= 0x20 && c < 0x7f) {
        encoded[0] = (char)c;
        count = 1;
      } else {
        encoded[0] = '\\';
        encoded[1] = (char)('0' + (c >> 6));
        encoded[2] = (char)('0' + ((c >> 3) & 7));
        encoded[3] = (char)('0' + (c & 7));
        count = 4;
      }
      if (count > SIZE_MAX - used) {
        return HAL_EOVERFLOW;
      }
      if (out != NULL) {
        memcpy(out + used, encoded, count);
      }
      used += count;
    }
  }
  if (used == SIZE_MAX) {
    return HAL_EOVERFLOW;
  }
  if (out != NULL) {
    out[used] = '"';
  }
  *length = used + 1;
  return HAL_OK;
}

static hal_status_t integer_literal(jb_json_node_t node, jb_c_literal_type_t type, char *text,
                                    size_t *length) {
  const bool is_unsigned = type >= JB_C_U8 && type <= JB_C_U64;
  const unsigned index = is_unsigned ? type - JB_C_U8 : type - JB_C_I8;
  const unsigned width = 8u << index;
  uint64_t magnitude;
  bool negative = false;
  hal_status_t status;
  if (is_unsigned) {
    status = jb_json_get_u64(node, &magnitude);
    if (status == HAL_OK && width != 64 && magnitude >= (UINT64_C(1) << width)) {
      status = HAL_EOVERFLOW;
    }
  } else {
    int64_t value;
    status = jb_json_get_i64(node, &value);
    if (status != HAL_OK) {
      return status;
    }
    negative = value < 0;
    magnitude = negative ? (uint64_t)(-(value + 1)) + 1 : (uint64_t)value;
    if (width != 64 && (negative ? magnitude > (UINT64_C(1) << (width - 1))
                                 : magnitude >= (UINT64_C(1) << (width - 1)))) {
      status = HAL_EOVERFLOW;
    }
  }
  if (status != HAL_OK) {
    return status;
  }
  if (!is_unsigned && negative && (width == 32 || width == 64) &&
      magnitude == (UINT64_C(1) << (width - 1))) {
    const char *minimum = width == 32 ? "(-2147483647 - 1)" : "(-9223372036854775807LL - 1LL)";
    *length = strlen(minimum);
    memcpy(text, minimum, *length);
    return HAL_OK;
  }
  size_t n = 0;
  if (negative) {
    text[n++] = '(';
    text[n++] = '-';
  }
  n += jb_u64_digits(magnitude, text + n);
  const char *suffix = is_unsigned ? (width == 64 ? "ULL" : "U") : (width == 64 ? "LL" : "");
  const size_t suffix_length = strlen(suffix);
  memcpy(text + n, suffix, suffix_length);
  n += suffix_length;
  if (negative) {
    text[n++] = ')';
  }
  *length = n;
  return HAL_OK;
}

hal_status_t jb_c_literal(jb_json_node_t node, jb_c_literal_type_t type, jb_json_workspace_t work,
                          size_t max_work, char *buffer, size_t capacity, size_t *required) {
  if (required == NULL || (buffer == NULL && capacity != 0) ||
      (work.data == NULL && work.size != 0)) {
    return HAL_EINVAL;
  }
  size_t needed;
  hal_status_t status = jb_c_literal_workspace_size(node, type, &needed);
  if (status != HAL_OK) {
    return status;
  }
  if (work.size < needed) {
    return HAL_EOVERFLOW;
  }
  jb_json_span_t raw;
  status = jb_json_raw(node, &raw);
  if (status != HAL_OK) {
    return status;
  }
  jb_budget_t budget = {max_work};
  if (raw.length > SIZE_MAX / 2 || !jb_charge(&budget, raw.length * 2)) {
    return HAL_EOVERFLOW;
  }
  char text[48];
  size_t length;
  if (type == JB_C_STRING) {
    status = c_string(node, NULL, &length);
  } else if (type == JB_C_BOOL) {
    bool value = false;
    status = jb_json_get_bool(node, &value);
    text[0] = value ? '1' : '0';
    length = 1;
  } else if (type == JB_C_F32 || type == JB_C_F64) {
    const unsigned width = type == JB_C_F32 ? 32u : 64u;
    const jb_json_float_options_t options = {true, true, budget.remaining};
    uint64_t bits;
    status = jb_number_bits_budget(raw, width, &options, work, &budget, &bits);
    if (status != HAL_OK) {
      return status;
    }
    const bool negative = (bits & (UINT64_C(1) << (width - 1))) != 0;
    size_t n = negative ? 1u : 0u;
    if (negative) {
      text[0] = '(';
    }
    size_t number_length;
    status = jb_float_format(bits, width, width == 32 ? 9u : 17u, work, &budget, text + n,
                             &number_length);
    if (status != HAL_OK) {
      return status;
    }
    n += number_length;
    if (width == 32) {
      text[n++] = 'f';
    }
    if (negative) {
      text[n++] = ')';
    }
    length = n;
  } else {
    status = integer_literal(node, type, text, &length);
  }
  if (status != HAL_OK) {
    return status;
  }
  if (capacity < length) {
    *required = length;
    return HAL_EOVERFLOW;
  }
  if (type == JB_C_STRING) {
    status = c_string(node, buffer, &length);
  } else {
    memcpy(buffer, text, length);
  }
  if (status == HAL_OK) {
    *required = length;
  }
  return status;
}
