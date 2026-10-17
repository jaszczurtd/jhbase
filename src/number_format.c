#include "number_internal.h"
#include "ryu/ryu.h"
#include <string.h>

static size_t exponent_text(int exponent, bool literal, char *out) {
  size_t n = 0;
  if (exponent == 0 && !literal) {
    return 0;
  }
  out[n++] = 'e';
  if (exponent < 0 || literal) {
    out[n++] = exponent < 0 ? '-' : '+';
  }
  const unsigned value = (unsigned)(exponent < 0 ? -exponent : exponent);
  if (literal && value < 10) {
    out[n++] = '0';
  }
  n += jb_u64_digits(value, out + n);
  return n;
}

static size_t scientific(const char *digits, size_t count, int exponent, bool negative,
                         bool literal, char *out) {
  size_t n = 0;
  if (negative) {
    out[n++] = '-';
  }
  out[n++] = digits[0];
  if (count > 1) {
    out[n++] = '.';
    memcpy(out + n, digits + 1, count - 1);
    n += count - 1;
  }
  n += exponent_text(exponent, literal, out + n);
  return n;
}

static void round_digits(const char *exact, size_t length, unsigned precision, char rounded[17],
                         int *exponent) {
  for (unsigned i = 0; i < precision; ++i) {
    rounded[i] = i < length ? exact[i] : '0';
  }
  if (length <= precision) {
    return;
  }
  bool sticky = false;
  for (size_t i = (size_t)precision + 1; i < length; ++i) {
    sticky = sticky || exact[i] != '0';
  }
  if (exact[precision] < '5' ||
      (exact[precision] == '5' && !sticky && ((rounded[precision - 1] - '0') & 1) == 0)) {
    return;
  }
  for (unsigned i = precision; i != 0; --i) {
    if (rounded[i - 1] != '9') {
      ++rounded[i - 1];
      return;
    }
    rounded[i - 1] = '0';
  }
  rounded[0] = '1';
  ++*exponent;
}

static size_t compact(const char *digits, size_t count, int exponent, bool negative, char *out) {
  const size_t scientific_size = scientific(digits, count, exponent, negative, false, out);
  const size_t sign = negative ? 1u : 0u;
  size_t fixed_size;
  if (exponent < 0) {
    fixed_size = sign + 2 + (size_t)(-exponent - 1) + count;
  } else if ((size_t)exponent + 1 >= count) {
    fixed_size = sign + (size_t)exponent + 1;
  } else {
    fixed_size = sign + count + 1;
  }
  if (fixed_size >= scientific_size) {
    return scientific_size;
  }
  size_t n = 0;
  if (negative) {
    out[n++] = '-';
  }
  if (exponent < 0) {
    out[n++] = '0';
    out[n++] = '.';
    for (int i = -1; i > exponent; --i) {
      out[n++] = '0';
    }
    memcpy(out + n, digits, count);
    n += count;
  } else {
    for (size_t i = 0; i < count || i <= (size_t)exponent; ++i) {
      if (i == (size_t)exponent + 1) {
        out[n++] = '.';
      }
      out[n++] = i < count ? digits[i] : '0';
    }
  }
  return n;
}

hal_status_t jb_float_shortest(uint64_t bits, unsigned width, char *out, size_t *length) {
  const unsigned fraction_bits = width == 32 ? 23u : 52u;
  const uint64_t mask = width == 32 ? UINT64_C(0xff) : UINT64_C(0x7ff);
  if (((bits >> fraction_bits) & mask) == mask) {
    return HAL_EINVAL;
  }
  char text[32];
  int count;
  if (width == 32) {
    const uint32_t small = (uint32_t)bits;
    float value;
    memcpy(&value, &small, sizeof(value));
    count = f2s_buffered_n(value, text);
  } else {
    double value;
    memcpy(&value, &bits, sizeof(value));
    count = d2s_buffered_n(value, text);
  }
  const bool negative = text[0] == '-';
  size_t position = negative ? 1u : 0u, used = 0;
  char digits[17];
  while (position < (size_t)count && text[position] != 'E') {
    if (text[position] != '.') {
      if (used == sizeof(digits)) {
        return HAL_EINTERNAL;
      }
      digits[used++] = text[position];
    }
    ++position;
  }
  if (used == 0 || position == (size_t)count) {
    return HAL_EINTERNAL;
  }
  ++position;
  const bool exponent_negative = text[position] == '-';
  if (text[position] == '-' || text[position] == '+') {
    ++position;
  }
  int exponent = 0;
  while (position < (size_t)count) {
    exponent = exponent * 10 + text[position++] - '0';
  }
  if (exponent_negative) {
    exponent = -exponent;
  }
  *length = compact(digits, used, exponent, negative, out);
  return HAL_OK;
}

hal_status_t jb_float_format(uint64_t bits, unsigned width, unsigned precision,
                             jb_json_workspace_t work, jb_budget_t *budget, char *out,
                             size_t *length) {
  if (work.data == NULL || work.size < JB_FLOAT_FORMAT_SCRATCH) {
    return HAL_EOVERFLOW;
  }
  const unsigned p = width == 32 ? 24u : 53u;
  const unsigned exp_width = width == 32 ? 8u : 11u;
  const uint64_t exp_mask = (UINT64_C(1) << exp_width) - 1;
  const uint64_t exp_field = (bits >> (p - 1)) & exp_mask;
  if (exp_field == exp_mask) {
    return HAL_EINVAL;
  }
  const bool negative = (bits & (UINT64_C(1) << (width - 1))) != 0;
  const uint64_t fraction = bits & ((UINT64_C(1) << (p - 1)) - 1);
  const uint64_t significand = fraction | (exp_field != 0 ? UINT64_C(1) << (p - 1) : 0);
  const unsigned maximum = width == 32 ? 9u : 17u;
  if (precision != maximum) {
    return HAL_EINVAL;
  }
  if (significand == 0) {
    if (!jb_charge(budget, 1)) {
      return HAL_EOVERFLOW;
    }
    char zeroes[17];
    memset(zeroes, '0', sizeof(zeroes));
    *length = scientific(zeroes, precision == 0 ? 1u : precision, 0, negative, precision != 0, out);
    return HAL_OK;
  }
  const int bias = width == 32 ? 127 : 1023;
  const int binary_exponent = (exp_field == 0 ? 1 - bias : (int)exp_field - bias) - (int)p + 1;
  jb_big_t a = {(unsigned char *)work.data, 0, 128, budget};
  jb_big_t temp = {(unsigned char *)work.data + 512, 0, 128, budget};
  if (!jb_big_small(&a, significand)) {
    return HAL_EOVERFLOW;
  }
  if (binary_exponent >= 0) {
    if (!jb_big_shift(&temp, &a, (size_t)binary_exponent) || !jb_big_shift(&a, &temp, 0)) {
      return HAL_EOVERFLOW;
    }
  } else {
    for (int i = 0; i < -binary_exponent; ++i) {
      if (!jb_big_mul_add(&a, 5, 0)) {
        return HAL_EOVERFLOW;
      }
    }
  }
  char *exact = (char *)work.data + 1024;
  size_t count = 0;
  do {
    uint32_t digit;
    if (!jb_big_div_small(&a, 10, &digit)) {
      return HAL_EOVERFLOW;
    }
    exact[count++] = (char)('0' + digit);
  } while (a.used != 0);
  for (size_t i = 0; i < count / 2; ++i) {
    const char c = exact[i];
    exact[i] = exact[count - 1 - i];
    exact[count - 1 - i] = c;
  }
  const int exponent =
      significand == 0 ? 0 : (int)count - 1 + (binary_exponent < 0 ? binary_exponent : 0);
  if (!jb_charge(budget, count + precision)) {
    return HAL_EOVERFLOW;
  }
  char rounded[17];
  int rounded_exponent = exponent;
  round_digits(exact, count, precision, rounded, &rounded_exponent);
  *length = scientific(rounded, precision, rounded_exponent, negative, true, out);
  return HAL_OK;
}
