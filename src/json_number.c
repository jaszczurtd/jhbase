#include "number_internal.h"
#include <float.h>
#include <limits.h>
#include <string.h>

JB_STATIC_ASSERT(sizeof(float) == 4 && FLT_RADIX == 2 && FLT_MANT_DIG == 24 && FLT_MAX_EXP == 128 &&
                     FLT_MIN_EXP == -125,
                 "jb native float requires IEEE binary32");
JB_STATIC_ASSERT(sizeof(double) == 8 && DBL_MANT_DIG == 53 && DBL_MAX_EXP == 1024 &&
                     DBL_MIN_EXP == -1021,
                 "jb native double requires IEEE binary64");

typedef struct {
  jb_json_span_t token;
  size_t first, last, digits, exponent_at;
  int64_t adjustment, scale;
  bool negative, exponent_negative;
} decimal_t;

static bool length_fits(size_t length) {
#if SIZE_MAX > INT64_MAX
  return length <= (uint64_t)INT64_MAX - 8192u;
#else
  (void)length;
  return true;
#endif
}

hal_status_t jb_number_validate(jb_json_span_t token) {
  if (token.data == NULL && token.length != 0) {
    return HAL_EINVAL;
  }
  if (token.length == 0 || (token.data[0] != '-' && (token.data[0] < '0' || token.data[0] > '9'))) {
    return HAL_EPROTO;
  }
  const jb_json_limits_t limits = {token.length, 1, 0, 0, token.length, 0, 0};
  jb_json_token_t record;
  jb_json_doc_t doc;
  const hal_status_t status =
      jb_json_parse(token, &limits, &record, 1, (jb_json_workspace_t){NULL, 0}, &doc, NULL);
  if (status != HAL_OK) {
    return status;
  }
  return record.type == JB_JSON_NUMBER && record.offset == 0 && record.length == token.length
             ? HAL_OK
             : HAL_EPROTO;
}

/* A saturated exponent suffices for integer/range decisions. Exact comparison
 * builds its original digits separately, so equal huge exponents stay exact. */
static hal_status_t decimal(jb_json_span_t token, jb_budget_t *budget, decimal_t *out) {
  if (!length_fits(token.length) || !jb_charge(budget, token.length)) {
    return HAL_EOVERFLOW;
  }
  decimal_t d = {0};
  d.token = token;
  d.first = token.length;
  d.exponent_at = token.length;
  d.negative = token.data[0] == '-';
  size_t fractional = 0, trailing = 0;
  bool after_dot = false;
  size_t i = d.negative ? 1u : 0u;
  for (; i < token.length && token.data[i] != 'e' && token.data[i] != 'E'; ++i) {
    const char c = token.data[i];
    if (c == '.') {
      after_dot = true;
      continue;
    }
    if (after_dot) {
      ++fractional;
    }
    if (d.first == token.length && c == '0') {
      continue;
    }
    if (d.first == token.length) {
      d.first = i;
    }
    ++d.digits;
    if (c == '0') {
      ++trailing;
    } else {
      trailing = 0;
      d.last = i;
    }
  }
  d.digits -= trailing;
  const int64_t cap = (int64_t)token.length + 4096;
  int64_t exponent = 0;
  if (i < token.length) {
    ++i;
    if (token.data[i] == '-' || token.data[i] == '+') {
      d.exponent_negative = token.data[i] == '-';
      ++i;
    }
    d.exponent_at = i;
    for (; i < token.length; ++i) {
      const int64_t digit = token.data[i] - '0';
      exponent = exponent > (cap - digit) / 10 ? cap : exponent * 10 + digit;
    }
  }
  if (d.exponent_negative) {
    exponent = -exponent;
  }
  d.adjustment = (int64_t)d.digits + (int64_t)trailing - (int64_t)fractional;
  d.scale = exponent + (int64_t)trailing - (int64_t)fractional;
  *out = d;
  return HAL_OK;
}

static hal_status_t node_decimal(jb_json_node_t node, jb_budget_t *budget, decimal_t *out) {
  jb_json_span_t token;
  const hal_status_t status = jb_json_number_token(node, &token);
  return status == HAL_OK ? decimal(token, budget, out) : status;
}

static unsigned next_digit(const decimal_t *d, size_t *position) {
  if (*position > d->last || d->digits == 0) {
    return 0;
  }
  if (d->token.data[*position] == '.') {
    ++*position;
  }
  return (unsigned)(d->token.data[(*position)++] - '0');
}

static hal_status_t magnitude(const decimal_t *d, uint64_t *out) {
  if (d->digits == 0) {
    *out = 0;
    return HAL_OK;
  }
  if (d->scale < 0) {
    return HAL_ETYPE;
  }
  if (d->scale > 20 || d->digits > 20 - (size_t)d->scale) {
    return HAL_EOVERFLOW;
  }
  uint64_t value = 0;
  size_t position = d->first;
  const size_t count = d->digits + (size_t)d->scale;
  for (size_t i = 0; i < count; ++i) {
    const unsigned digit = i < d->digits ? next_digit(d, &position) : 0;
    if (value > (UINT64_MAX - digit) / 10) {
      return HAL_EOVERFLOW;
    }
    value = value * 10 + digit;
  }
  *out = value;
  return HAL_OK;
}

static hal_status_t integer(jb_json_node_t node, bool signed_value, uint64_t *out, bool *negative) {
  decimal_t d;
  jb_budget_t budget = {SIZE_MAX};
  hal_status_t status = node_decimal(node, &budget, &d);
  uint64_t value;
  if (status == HAL_OK) {
    status = magnitude(&d, &value);
  }
  if (status != HAL_OK) {
    return status;
  }
  if ((!signed_value && d.negative && value != 0) ||
      (signed_value && value > (uint64_t)INT64_MAX + (d.negative ? 1u : 0u))) {
    return HAL_EOVERFLOW;
  }
  *out = value;
  *negative = d.negative;
  return HAL_OK;
}

hal_status_t jb_json_get_u64(jb_json_node_t node, uint64_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  uint64_t value;
  bool negative;
  const hal_status_t status = integer(node, false, &value, &negative);
  if (status == HAL_OK) {
    *out = value;
  }
  return status;
}
hal_status_t jb_json_get_i64(jb_json_node_t node, int64_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  uint64_t value;
  bool negative;
  const hal_status_t status = integer(node, true, &value, &negative);
  if (status == HAL_OK) {
    *out = negative && value != 0 ? -(int64_t)(value - 1) - 1 : (int64_t)value;
  }
  return status;
}
hal_status_t jb_json_number_is_integer(jb_json_node_t node, bool *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  decimal_t d;
  jb_budget_t budget = {SIZE_MAX};
  const hal_status_t status = node_decimal(node, &budget, &d);
  if (status == HAL_OK) {
    *out = d.digits == 0 || d.scale >= 0;
  }
  return status;
}

hal_status_t jb_number_scratch_size(size_t length, size_t *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  if (length > SIZE_MAX / (4 * sizeof(uint32_t)) - 128 || !length_fits(length)) {
    return HAL_EOVERFLOW;
  }
  *out = (length + 128) * (4 * sizeof(uint32_t));
  return HAL_OK;
}

static hal_status_t storage(jb_json_workspace_t work, size_t length, jb_budget_t *budget,
                            jb_big_t big[4]) {
  size_t needed;
  const hal_status_t status = jb_number_scratch_size(length, &needed);
  if (status != HAL_OK) {
    return status;
  }
  if (work.data == NULL) {
    return work.size == 0 ? HAL_EOVERFLOW : HAL_EINVAL;
  }
  if (work.size < needed) {
    return HAL_EOVERFLOW;
  }
  const size_t capacity = length + 128;
  for (size_t i = 0; i < 4; ++i) {
    big[i] = (jb_big_t){(unsigned char *)work.data + i * capacity * sizeof(uint32_t), 0, capacity,
                        budget};
  }
  return HAL_OK;
}

hal_status_t jb_json_number_compare_workspace_size(jb_json_node_t left, jb_json_node_t right,
                                                   size_t *out_bytes) {
  if (out_bytes == NULL) {
    return HAL_EINVAL;
  }
  jb_json_span_t a, b;
  hal_status_t status = jb_json_number_token(left, &a);
  if (status == HAL_OK) {
    status = jb_json_number_token(right, &b);
  }
  return status == HAL_OK
             ? jb_number_scratch_size(a.length > b.length ? a.length : b.length, out_bytes)
             : status;
}

static bool decimal_order(const decimal_t *d, jb_big_t *order, jb_big_t *delta, bool *negative) {
  *negative = d->exponent_negative;
  if (!jb_big_small(order, 0)) {
    return false;
  }
  for (size_t i = d->exponent_at; i < d->token.length; ++i) {
    if (!jb_big_mul_add(order, 10, (uint32_t)(d->token.data[i] - '0'))) {
      return false;
    }
  }
  if (order->used == 0) {
    *negative = false;
  }
  const bool adjustment_negative = d->adjustment < 0;
  const uint64_t value = (uint64_t)(adjustment_negative ? -d->adjustment : d->adjustment);
  if (!jb_big_small(delta, value)) {
    return false;
  }
  if (*negative == adjustment_negative) {
    uint64_t carry = 0;
    const size_t count = order->used > delta->used ? order->used : delta->used;
    if (!jb_charge(order->budget, count + 1)) {
      return false;
    }
    for (size_t i = 0; i < count; ++i) {
      const uint64_t sum = (uint64_t)jb_big_get(order, i) + jb_big_get(delta, i) + carry;
      jb_big_set(order, i, (uint32_t)sum);
      carry = sum >> 32;
    }
    order->used = count;
    if (carry != 0) {
      jb_big_set(order, order->used++, (uint32_t)carry);
    }
  } else {
    int comparison;
    if (!jb_big_compare(order, delta, &comparison)) {
      return false;
    }
    if (comparison >= 0) {
      if (!jb_big_sub(order, delta)) {
        return false;
      }
    } else {
      if (!jb_big_sub(delta, order) || !jb_big_shift(order, delta, 0)) {
        return false;
      }
      *negative = adjustment_negative;
    }
  }
  if (order->used == 0) {
    *negative = false;
  }
  return true;
}

hal_status_t jb_json_number_compare(jb_json_node_t left, jb_json_node_t right,
                                    jb_json_workspace_t work, size_t max_work, int *out_order) {
  if (out_order == NULL) {
    return HAL_EINVAL;
  }
  jb_budget_t budget = {max_work};
  decimal_t a, b;
  hal_status_t status = node_decimal(left, &budget, &a);
  if (status == HAL_OK) {
    status = node_decimal(right, &budget, &b);
  }
  if (status != HAL_OK) {
    return status;
  }
  jb_big_t big[4];
  status = storage(work, a.token.length > b.token.length ? a.token.length : b.token.length, &budget,
                   big);
  if (status != HAL_OK) {
    return status;
  }
  int result = 0;
  if (a.digits == 0 || b.digits == 0) {
    result = a.digits == 0 ? (b.digits == 0 ? 0 : (b.negative ? 1 : -1)) : (a.negative ? -1 : 1);
  } else if (a.negative != b.negative) {
    result = a.negative ? -1 : 1;
  } else {
    bool an, bn;
    if (!decimal_order(&a, &big[0], &big[2], &an) || !decimal_order(&b, &big[1], &big[2], &bn)) {
      return HAL_EOVERFLOW;
    }
    if (an != bn) {
      result = an ? -1 : 1;
    } else {
      if (!jb_big_compare(&big[0], &big[1], &result)) {
        return HAL_EOVERFLOW;
      }
      if (an) {
        result = -result;
      }
    }
    if (result == 0) {
      size_t ap = a.first, bp = b.first;
      const size_t count = a.digits > b.digits ? a.digits : b.digits;
      for (size_t i = 0; i < count; ++i) {
        if (!jb_charge(&budget, 2)) {
          return HAL_EOVERFLOW;
        }
        const unsigned av = i < a.digits ? next_digit(&a, &ap) : 0;
        const unsigned bv = i < b.digits ? next_digit(&b, &bp) : 0;
        if (av != bv) {
          result = av > bv ? 1 : -1;
          break;
        }
      }
    }
    if (a.negative) {
      result = -result;
    }
  }
  *out_order = result;
  return HAL_OK;
}

hal_status_t jb_json_float_workspace_size(jb_json_node_t node, unsigned bit_width,
                                          size_t *out_bytes) {
  if (out_bytes == NULL || (bit_width != 32 && bit_width != 64)) {
    return HAL_EINVAL;
  }
  jb_json_span_t token;
  const hal_status_t status = jb_json_number_token(node, &token);
  return status == HAL_OK ? jb_number_scratch_size(token.length, out_bytes) : status;
}

static bool ratio(const decimal_t *d, jb_big_t *n, jb_big_t *denominator) {
  if (!jb_big_small(n, 0) || !jb_big_small(denominator, 1)) {
    return false;
  }
  size_t position = d->first;
  for (size_t i = 0; i < d->digits; ++i) {
    if (!jb_big_mul_add(n, 10, next_digit(d, &position))) {
      return false;
    }
  }
  jb_big_t *scaled = d->scale < 0 ? denominator : n;
  const uint64_t power = (uint64_t)(d->scale < 0 ? -d->scale : d->scale);
  for (uint64_t i = 0; i < power; ++i) {
    if (!jb_big_mul_add(scaled, 10, 0)) {
      return false;
    }
  }
  return true;
}

/* N is replaced by the remainder. The requested quotient has at most 54 bits. */
static bool quotient(jb_big_t *n, const jb_big_t *d, jb_big_t *temp, unsigned precision,
                     uint64_t *out) {
  uint64_t value = 0;
  for (unsigned i = precision + 1; i != 0; --i) {
    int order;
    if (!jb_big_shift(temp, d, i - 1) || !jb_big_compare(n, temp, &order)) {
      return false;
    }
    if (order >= 0) {
      if (!jb_big_sub(n, temp)) {
        return false;
      }
      value |= UINT64_C(1) << (i - 1);
    }
  }
  int order;
  if (!jb_big_shift(temp, n, 1) || !jb_big_compare(temp, d, &order)) {
    return false;
  }
  if (order > 0 || (order == 0 && (value & 1) != 0)) {
    ++value;
  }
  *out = value;
  return true;
}

hal_status_t jb_number_bits_budget(jb_json_span_t token, unsigned width,
                                   const jb_json_float_options_t *options, jb_json_workspace_t work,
                                   jb_budget_t *budget, uint64_t *out) {
  if (options == NULL || out == NULL || (width != 32 && width != 64)) {
    return HAL_EINVAL;
  }
  decimal_t d;
  hal_status_t status = decimal(token, budget, &d);
  if (status != HAL_OK) {
    return status;
  }
  jb_big_t big[4];
  status = storage(work, token.length, budget, big);
  if (status != HAL_OK) {
    return status;
  }
  const uint64_t sign = d.negative ? UINT64_C(1) << (width - 1) : 0;
  const int64_t decimal_exponent = d.scale + (int64_t)d.digits;
  if (d.digits == 0 || decimal_exponent < -400) {
    if (d.digits != 0 && options->reject_nonzero_to_zero) {
      return HAL_EOVERFLOW;
    }
    *out = options->normalize_negative_zero ? 0 : sign;
    return HAL_OK;
  }
  if (decimal_exponent > 400) {
    return HAL_EOVERFLOW;
  }
  if (!ratio(&d, &big[0], &big[1])) {
    return HAL_EOVERFLOW;
  }
  int64_t exponent = (int64_t)jb_big_bits(&big[0]) - (int64_t)jb_big_bits(&big[1]);
  int order;
  if (exponent >= 0) {
    if (!jb_big_shift(&big[2], &big[1], (size_t)exponent) ||
        !jb_big_compare(&big[0], &big[2], &order)) {
      return HAL_EOVERFLOW;
    }
  } else {
    if (!jb_big_shift(&big[2], &big[0], (size_t)-exponent) ||
        !jb_big_compare(&big[2], &big[1], &order)) {
      return HAL_EOVERFLOW;
    }
  }
  if (order < 0) {
    --exponent;
  }
  const unsigned precision = width == 32 ? 24u : 53u;
  const int64_t low = width == 32 ? -126 : -1022;
  const int64_t high = width == 32 ? 127 : 1023;
  if (exponent > high) {
    return HAL_EOVERFLOW;
  }
  if (exponent < low) {
    exponent = low;
  }
  const int64_t quantum = exponent - (int64_t)precision + 1;
  jb_big_t *n = &big[0];
  const jb_big_t *denominator = &big[1];
  if (quantum < 0) {
    if (!jb_big_shift(&big[2], n, (size_t)-quantum)) {
      return HAL_EOVERFLOW;
    }
    n = &big[2];
  } else {
    if (!jb_big_shift(&big[2], denominator, (size_t)quantum)) {
      return HAL_EOVERFLOW;
    }
    denominator = &big[2];
  }
  uint64_t significand;
  if (!quotient(n, denominator, &big[3], precision, &significand)) {
    return HAL_EOVERFLOW;
  }
  if (significand == (UINT64_C(1) << precision)) {
    significand >>= 1;
    ++exponent;
  }
  if (exponent > high || (significand == 0 && options->reject_nonzero_to_zero)) {
    return HAL_EOVERFLOW;
  }
  if (significand == 0) {
    *out = options->normalize_negative_zero ? 0 : sign;
    return HAL_OK;
  }
  const uint64_t hidden = UINT64_C(1) << (precision - 1);
  const uint64_t biased = significand < hidden ? 0 : (uint64_t)(exponent - low + 1);
  *out = sign | (biased << (precision - 1)) | (significand & (hidden - 1));
  return HAL_OK;
}

hal_status_t jb_number_bits(jb_json_span_t token, unsigned width,
                            const jb_json_float_options_t *options, jb_json_workspace_t work,
                            uint64_t *out) {
  if (options == NULL) {
    return HAL_EINVAL;
  }
  jb_budget_t budget = {options->max_work};
  return jb_number_bits_budget(token, width, options, work, &budget, out);
}

hal_status_t jb_json_get_f64_bits(jb_json_node_t node, const jb_json_float_options_t *options,
                                  jb_json_workspace_t work, uint64_t *out) {
  if (options == NULL || out == NULL) {
    return HAL_EINVAL;
  }
  jb_json_span_t token;
  const hal_status_t status = jb_json_number_token(node, &token);
  return status == HAL_OK ? jb_number_bits(token, 64, options, work, out) : status;
}
hal_status_t jb_json_get_f32_bits(jb_json_node_t node, const jb_json_float_options_t *options,
                                  jb_json_workspace_t work, uint32_t *out) {
  if (options == NULL || out == NULL) {
    return HAL_EINVAL;
  }
  jb_json_span_t token;
  uint64_t bits;
  hal_status_t status = jb_json_number_token(node, &token);
  if (status == HAL_OK) {
    status = jb_number_bits(token, 32, options, work, &bits);
  }
  if (status == HAL_OK) {
    *out = (uint32_t)bits;
  }
  return status;
}
hal_status_t jb_json_get_f32(jb_json_node_t node, const jb_json_float_options_t *options,
                             jb_json_workspace_t work, float *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  uint32_t bits;
  const hal_status_t status = jb_json_get_f32_bits(node, options, work, &bits);
  if (status == HAL_OK) {
    memcpy(out, &bits, sizeof(bits));
  }
  return status;
}
hal_status_t jb_json_get_f64(jb_json_node_t node, const jb_json_float_options_t *options,
                             jb_json_workspace_t work, double *out) {
  if (out == NULL) {
    return HAL_EINVAL;
  }
  uint64_t bits;
  const hal_status_t status = jb_json_get_f64_bits(node, options, work, &bits);
  if (status == HAL_OK) {
    memcpy(out, &bits, sizeof(bits));
  }
  return status;
}
