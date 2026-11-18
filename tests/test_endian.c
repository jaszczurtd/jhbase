#include "jb/endian.h"
#include "unity.h"
#include <jb/array.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static const uint8_t ascending[8] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08};

static void test_loads_follow_byte_order_not_host(void) {
  TEST_ASSERT_EQUAL_HEX16(0x0201u, jb_load_le16(ascending));
  TEST_ASSERT_EQUAL_HEX32(0x04030201u, jb_load_le32(ascending));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0807060504030201), jb_load_le64(ascending));
  TEST_ASSERT_EQUAL_HEX16(0x0102u, jb_load_be16(ascending));
  TEST_ASSERT_EQUAL_HEX32(0x01020304u, jb_load_be32(ascending));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0102030405060708), jb_load_be64(ascending));
}

static void test_stores_write_exactly_the_width(void) {
  uint8_t bytes[10];
  memset(bytes, 0xA5, sizeof(bytes));
  jb_store_le16(bytes + 1, 0x0201u);
  const uint8_t little16[10] = {0xA5, 0x01, 0x02, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(little16, bytes, sizeof(bytes));
  jb_store_be16(bytes + 3, 0x0102u);
  const uint8_t both16[10] = {0xA5, 0x01, 0x02, 0x01, 0x02, 0xA5, 0xA5, 0xA5, 0xA5, 0xA5};
  TEST_ASSERT_EQUAL_HEX8_ARRAY(both16, bytes, sizeof(bytes));

  memset(bytes, 0xA5, sizeof(bytes));
  jb_store_le32(bytes + 1, 0x04030201u);
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ascending, bytes + 1, 4);
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[5]);
  jb_store_be32(bytes + 5, 0x01020304u);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ascending, bytes + 5, 4);
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[9]);

  memset(bytes, 0xA5, sizeof(bytes));
  jb_store_le64(bytes + 1, UINT64_C(0x0807060504030201));
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ascending, bytes + 1, 8);
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[9]);
  jb_store_be64(bytes + 1, UINT64_C(0x0102030405060708));
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[0]);
  TEST_ASSERT_EQUAL_HEX8_ARRAY(ascending, bytes + 1, 8);
  TEST_ASSERT_EQUAL_UINT8(0xA5, bytes[9]);
}

static void test_extreme_values_keep_every_bit(void) {
  static const uint64_t values[] = {0u,
                                    1u,
                                    UINT64_C(0x8000000000000001),
                                    UINT64_C(0x7FFFFFFFFFFFFFFE),
                                    UINT64_C(0x00000000FFFFFFFF),
                                    UINT64_C(0xFFFFFFFF00000000),
                                    UINT64_MAX};
  for (size_t i = 0u; i < COUNTOF(values); ++i) {
    const uint64_t value = values[i];
    uint8_t le[8], be[8];
    jb_store_le64(le, value);
    jb_store_be64(be, value);
    for (unsigned byte = 0u; byte < 8u; ++byte) {
      const uint8_t expected = (uint8_t)(value >> (8u * byte));
      TEST_ASSERT_EQUAL_UINT8(expected, le[byte]);
      TEST_ASSERT_EQUAL_UINT8(expected, be[7u - byte]);
    }
    TEST_ASSERT_EQUAL_HEX64(value, jb_load_le64(le));
    TEST_ASSERT_EQUAL_HEX64(value, jb_load_be64(be));

    const uint32_t value32 = (uint32_t)(value >> 16u);
    jb_store_le32(le, value32);
    jb_store_be32(be, value32);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)value32, le[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(value32 >> 24u), le[3]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(value32 >> 24u), be[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)value32, be[3]);
    TEST_ASSERT_EQUAL_HEX32(value32, jb_load_le32(le));
    TEST_ASSERT_EQUAL_HEX32(value32, jb_load_be32(be));

    const uint16_t value16 = (uint16_t)(value >> 8u);
    jb_store_le16(le, value16);
    jb_store_be16(be, value16);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)value16, le[0]);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(value16 >> 8u), be[0]);
    TEST_ASSERT_EQUAL_HEX16(value16, jb_load_le16(le));
    TEST_ASSERT_EQUAL_HEX16(value16, jb_load_be16(be));
  }
}

static void test_unaligned_buffers_at_every_offset(void) {
  uint8_t storage[24];
  for (size_t offset = 0u; offset < 8u; ++offset) {
    memset(storage, 0xA5, sizeof(storage));
    uint8_t *at = storage + offset;
    jb_store_be64(at, UINT64_C(0x0102030405060708));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(ascending, at, 8);
    TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0807060504030201), jb_load_le64(at));
    TEST_ASSERT_EQUAL_HEX32(0x05060708u, jb_load_be32(at + 4));
    TEST_ASSERT_EQUAL_HEX16(0x0403u, jb_load_le16(at + 2));
    jb_store_le32(at + 8, 0xDDCCBBAAu);
    TEST_ASSERT_EQUAL_HEX32(0xAABBCCDDu, jb_load_be32(at + 8));
    jb_store_be16(at + 12, 0xEEFFu);
    TEST_ASSERT_EQUAL_HEX16(0xFFEEu, jb_load_le16(at + 12));
    for (size_t i = 0u; i < sizeof(storage); ++i) {
      if (i < offset || i >= offset + 14u) {
        TEST_ASSERT_EQUAL_UINT8(0xA5, storage[i]);
      }
    }
  }
}

static void test_byte_swaps(void) {
  TEST_ASSERT_EQUAL_HEX16(0x3412u, jb_bswap16(0x1234u));
  TEST_ASSERT_EQUAL_HEX16(0x00FFu, jb_bswap16(0xFF00u));
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, jb_bswap16(0xFFFFu));
  TEST_ASSERT_EQUAL_HEX32(0x78563412u, jb_bswap32(0x12345678u));
  TEST_ASSERT_EQUAL_HEX32(0x000000FFu, jb_bswap32(0xFF000000u));
  TEST_ASSERT_EQUAL_HEX32(0u, jb_bswap32(0u));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x0807060504030201), jb_bswap64(UINT64_C(0x0102030405060708)));
  TEST_ASSERT_EQUAL_HEX64(UINT64_C(0x01000000000000FF), jb_bswap64(UINT64_C(0xFF00000000000001)));
  TEST_ASSERT_EQUAL_HEX64(UINT64_MAX, jb_bswap64(UINT64_MAX));
  TEST_ASSERT_EQUAL_HEX64(jb_load_le64(ascending), jb_bswap64(jb_load_be64(ascending)));
  TEST_ASSERT_EQUAL_HEX32(jb_load_le32(ascending), jb_bswap32(jb_load_be32(ascending)));
  TEST_ASSERT_EQUAL_HEX16(jb_load_le16(ascending), jb_bswap16(jb_load_be16(ascending)));
}

static void test_sixteen_bit_byte_helpers(void) {
  TEST_ASSERT_EQUAL_UINT8(0xA5, jb_u16_msb(0xA55Au));
  TEST_ASSERT_EQUAL_UINT8(0x5A, jb_u16_lsb(0xA55Au));
  TEST_ASSERT_EQUAL_UINT8(0x00, jb_u16_msb(0x00FFu));
  TEST_ASSERT_EQUAL_UINT8(0xFF, jb_u16_lsb(0x00FFu));
  TEST_ASSERT_EQUAL_UINT8(0xFF, jb_u16_msb(0xFF00u));
  TEST_ASSERT_EQUAL_UINT8(0x00, jb_u16_lsb(0xFF00u));
  TEST_ASSERT_EQUAL_HEX16(0xA55Au, jb_u16_from_bytes(0xA5, 0x5A));
  TEST_ASSERT_EQUAL_HEX16(0x0000u, jb_u16_from_bytes(0x00, 0x00));
  TEST_ASSERT_EQUAL_HEX16(0xFFFFu, jb_u16_from_bytes(0xFF, 0xFF));
  TEST_ASSERT_EQUAL_HEX16(0x00FFu, jb_u16_from_bytes(0x00, 0xFF));
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_loads_follow_byte_order_not_host);
  RUN_TEST(test_stores_write_exactly_the_width);
  RUN_TEST(test_extreme_values_keep_every_bit);
  RUN_TEST(test_unaligned_buffers_at_every_offset);
  RUN_TEST(test_byte_swaps);
  RUN_TEST(test_sixteen_bit_byte_helpers);
  return UNITY_END();
}
