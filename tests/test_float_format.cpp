#include "jb/json_writer.h"
#include <charconv>
#include <cstdio>
#include <cstring>
#include <system_error>

static unsigned char scratch[8192];

static size_t significant(const char *text, size_t length) {
  size_t digits = 0, last = 0;
  bool begun = false;
  for (size_t i = 0; i < length && text[i] != 'e' && text[i] != 'E'; ++i) {
    if (text[i] < '0' || text[i] > '9') {
      continue;
    }
    if (!begun && text[i] == '0') {
      continue;
    }
    begun = true;
    ++digits;
    if (text[i] != '0') {
      last = digits;
    }
  }
  return last == 0 ? 1 : last;
}

template <typename Number, typename Bits> static bool check(Bits bits) {
  Number value;
  std::memcpy(&value, &bits, sizeof(value));
  char actual[64], reference[64];
  jb_json_buffer_t buffer = {actual, sizeof(actual), 0};
  const jb_json_writer_limits_t limits = {sizeof(actual), 0, 0, 0, 0};
  jb_json_writer_t writer;
  if (jb_json_writer_init(&writer, &limits, {scratch, sizeof(scratch)}, jb_json_buffer_sink,
                          &buffer) != HAL_OK) {
    return false;
  }
  const hal_status_t status = sizeof(Number) == 4
                                  ? jb_json_writer_f32(&writer, static_cast<float>(value))
                                  : jb_json_writer_f64(&writer, static_cast<double>(value));
  size_t length;
  if (status != HAL_OK || jb_json_writer_finish(&writer, &length) != HAL_OK) {
    return false;
  }
  const auto result =
      std::to_chars(reference, reference + sizeof(reference), value, std::chars_format::scientific);
  if (result.ec != std::errc()) {
    return false;
  }
  Number parsed{};
  const auto read = std::from_chars(actual, actual + length, parsed, std::chars_format::general);
  Bits read_bits;
  std::memcpy(&read_bits, &parsed, sizeof(read_bits));
  const size_t reference_length = static_cast<size_t>(result.ptr - reference);
  const bool okay = read.ec == std::errc() && read.ptr == actual + length && read_bits == bits &&
                    significant(actual, length) == significant(reference, reference_length);
  if (!okay) {
    std::fprintf(stderr, "shortest round-trip mismatch: %.*s / %.*s\n", static_cast<int>(length),
                 actual, static_cast<int>(reference_length), reference);
  }
  return okay;
}
int main() {
  const uint64_t boundaries[] = {0,
                                 UINT64_C(0x8000000000000000),
                                 1,
                                 UINT64_C(0x000fffffffffffff),
                                 UINT64_C(0x0010000000000000),
                                 UINT64_C(0x7fefffffffffffff),
                                 UINT64_C(0x3ff0000000000000),
                                 UINT64_C(0x3fefffffffffffff),
                                 UINT64_C(0x3ff0000000000001)};
  for (uint64_t bits : boundaries) {
    if (!check<double>(bits)) {
      return 1;
    }
  }
  const uint32_t small_boundaries[] = {0,
                                       UINT32_C(0x80000000),
                                       1,
                                       UINT32_C(0x007fffff),
                                       UINT32_C(0x00800000),
                                       UINT32_C(0x7f7fffff),
                                       UINT32_C(0x3f800000),
                                       UINT32_C(0x3f7fffff)};
  for (uint32_t bits : small_boundaries) {
    if (!check<float>(bits)) {
      return 2;
    }
  }
  uint64_t random = 8259;
  for (size_t i = 0; i < 4096; ++i) {
    random = random * UINT64_C(6364136223846793005) + 1;
    uint64_t wide = random;
    if ((wide & UINT64_C(0x7ff0000000000000)) == UINT64_C(0x7ff0000000000000)) {
      wide ^= UINT64_C(0x0010000000000000);
    }
    uint32_t small = static_cast<uint32_t>(random);
    if ((small & UINT32_C(0x7f800000)) == UINT32_C(0x7f800000)) {
      small ^= UINT32_C(0x00800000);
    }
    if (!check<double>(wide) || !check<float>(small)) {
      return 3;
    }
  }
  return 0;
}
