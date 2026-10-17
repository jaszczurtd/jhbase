#include "number_internal.h"
#include <string.h>

bool jb_charge(jb_budget_t *budget, size_t amount) {
  if (amount > budget->remaining) {
    return false;
  }
  budget->remaining -= amount;
  return true;
}

uint32_t jb_big_get(const jb_big_t *a, size_t at) {
  uint32_t value = 0;
  if (at < a->used) {
    memcpy(&value, a->data + at * sizeof(value), sizeof(value));
  }
  return value;
}
void jb_big_set(jb_big_t *a, size_t at, uint32_t value) {
  memcpy(a->data + at * sizeof(value), &value, sizeof(value));
}
static void trim(jb_big_t *a) {
  while (a->used != 0 && jb_big_get(a, a->used - 1) == 0) {
    --a->used;
  }
}
bool jb_big_small(jb_big_t *a, uint64_t value) {
  a->used = 0;
  do {
    if (a->used == a->capacity || !jb_charge(a->budget, 1)) {
      return false;
    }
    jb_big_set(a, a->used++, (uint32_t)value);
    value >>= 32;
  } while (value != 0);
  trim(a);
  return true;
}
bool jb_big_mul_add(jb_big_t *a, uint32_t factor, uint32_t add) {
  uint64_t carry = add;
  if (!jb_charge(a->budget, a->used + 1)) {
    return false;
  }
  for (size_t i = 0; i < a->used; ++i) {
    const uint64_t next = (uint64_t)jb_big_get(a, i) * factor + carry;
    jb_big_set(a, i, (uint32_t)next);
    carry = next >> 32;
  }
  if (carry != 0) {
    if (a->used == a->capacity) {
      return false;
    }
    jb_big_set(a, a->used++, (uint32_t)carry);
  }
  trim(a);
  return true;
}
bool jb_big_shift(jb_big_t *out, const jb_big_t *a, size_t bits) {
  const size_t words = bits / 32;
  const unsigned shift = (unsigned)(bits % 32);
  if (words >= out->capacity || a->used >= out->capacity - words ||
      !jb_charge(out->budget, a->used + words + 1)) {
    return false;
  }
  uint32_t carry = 0;
  for (size_t i = 0; i < a->used; ++i) {
    const uint32_t value = jb_big_get(a, i);
    jb_big_set(out, i + words, (uint32_t)(value << shift) | carry);
    carry = shift == 0 ? 0 : value >> (32 - shift);
  }
  for (size_t i = 0; i < words; ++i) {
    jb_big_set(out, i, 0);
  }
  out->used = a->used + words;
  if (carry != 0) {
    jb_big_set(out, out->used++, carry);
  }
  trim(out);
  return true;
}
bool jb_big_compare(const jb_big_t *a, const jb_big_t *b, int *out) {
  if (!jb_charge(a->budget, 1)) {
    return false;
  }
  if (a->used != b->used) {
    *out = a->used > b->used ? 1 : -1;
    return true;
  }
  for (size_t i = a->used; i != 0; --i) {
    if (!jb_charge(a->budget, 1)) {
      return false;
    }
    const uint32_t av = jb_big_get(a, i - 1), bv = jb_big_get(b, i - 1);
    if (av != bv) {
      *out = av > bv ? 1 : -1;
      return true;
    }
  }
  *out = 0;
  return true;
}
bool jb_big_sub(jb_big_t *a, const jb_big_t *b) {
  if (!jb_charge(a->budget, a->used + 1)) {
    return false;
  }
  uint64_t borrow = 0;
  for (size_t i = 0; i < a->used; ++i) {
    const uint64_t av = jb_big_get(a, i), bv = (uint64_t)jb_big_get(b, i) + borrow;
    jb_big_set(a, i, (uint32_t)(av - bv));
    borrow = av < bv ? 1u : 0u;
  }
  trim(a);
  return borrow == 0;
}
size_t jb_big_bits(const jb_big_t *a) {
  if (a->used == 0) {
    return 0;
  }
  uint32_t top = jb_big_get(a, a->used - 1);
  size_t bits = (a->used - 1) * 32;
  while (top != 0) {
    ++bits;
    top >>= 1;
  }
  return bits;
}
bool jb_big_div_small(jb_big_t *a, uint32_t divisor, uint32_t *remainder) {
  if (!jb_charge(a->budget, a->used + 1)) {
    return false;
  }
  uint64_t carry = 0;
  for (size_t i = a->used; i != 0; --i) {
    const uint64_t value = (carry << 32) | jb_big_get(a, i - 1);
    jb_big_set(a, i - 1, (uint32_t)(value / divisor));
    carry = value % divisor;
  }
  trim(a);
  *remainder = (uint32_t)carry;
  return true;
}

size_t jb_u64_digits(uint64_t value, char *out) {
  size_t count = 0;
  do {
    out[count++] = (char)('0' + value % 10);
    value /= 10;
  } while (value != 0);
  for (size_t i = 0; i < count / 2; ++i) {
    const char c = out[i];
    out[i] = out[count - 1 - i];
    out[count - 1 - i] = c;
  }
  return count;
}
