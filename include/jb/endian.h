#ifndef JB_ENDIAN_H
#define JB_ENDIAN_H

/** @file endian.h
 * Byte-order helpers for 16-, 32- and 64-bit integers in byte buffers.
 * Loads and stores read or write single bytes, so buffers need no alignment
 * and the result does not depend on the byte order of the host. They do not
 * validate pointers: the caller provides as many bytes as the width needs.
 * Everything is inline, without heap or other state.
 */
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Reverse the byte order of a 16-bit value.
 * @param value Input value.
 * @return value with its two bytes swapped.
 */
static inline uint16_t jb_bswap16(uint16_t value) {
  return (uint16_t)((uint16_t)(value << 8u) | (uint16_t)(value >> 8u));
}

/** Reverse the byte order of a 32-bit value.
 * @param value Input value.
 * @return value with its four bytes in reverse order.
 */
static inline uint32_t jb_bswap32(uint32_t value) {
  return ((value & UINT32_C(0x000000FF)) << 24u) | ((value & UINT32_C(0x0000FF00)) << 8u) |
         ((value & UINT32_C(0x00FF0000)) >> 8u) | ((value & UINT32_C(0xFF000000)) >> 24u);
}

/** Reverse the byte order of a 64-bit value.
 * @param value Input value.
 * @return value with its eight bytes in reverse order.
 */
static inline uint64_t jb_bswap64(uint64_t value) {
  return ((value & UINT64_C(0x00000000000000FF)) << 56u) |
         ((value & UINT64_C(0x000000000000FF00)) << 40u) |
         ((value & UINT64_C(0x0000000000FF0000)) << 24u) |
         ((value & UINT64_C(0x00000000FF000000)) << 8u) |
         ((value & UINT64_C(0x000000FF00000000)) >> 8u) |
         ((value & UINT64_C(0x0000FF0000000000)) >> 24u) |
         ((value & UINT64_C(0x00FF000000000000)) >> 40u) |
         ((value & UINT64_C(0xFF00000000000000)) >> 56u);
}

/** Most significant byte of a 16-bit value.
 * @param value Input value.
 * @return Bits 15..8.
 */
static inline uint8_t jb_u16_msb(uint16_t value) { return (uint8_t)(value >> 8u); }

/** Least significant byte of a 16-bit value.
 * @param value Input value.
 * @return Bits 7..0.
 */
static inline uint8_t jb_u16_lsb(uint16_t value) { return (uint8_t)value; }

/** Load a little-endian 16-bit value.
 * @param input At least two readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint16_t jb_load_le16(const uint8_t *input) {
  return (uint16_t)((uint16_t)input[0] | (uint16_t)((uint16_t)input[1] << 8u));
}

/** Load a little-endian 32-bit value.
 * @param input At least four readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint32_t jb_load_le32(const uint8_t *input) {
  return (uint32_t)input[0] | ((uint32_t)input[1] << 8u) | ((uint32_t)input[2] << 16u) |
         ((uint32_t)input[3] << 24u);
}

/** Load a little-endian 64-bit value.
 * @param input At least eight readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint64_t jb_load_le64(const uint8_t *input) {
  return (uint64_t)jb_load_le32(input) | ((uint64_t)jb_load_le32(input + 4u) << 32u);
}

/** Store a 16-bit value in little-endian order.
 * @param output At least two writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_le16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)value;
  output[1] = (uint8_t)(value >> 8u);
}

/** Store a 32-bit value in little-endian order.
 * @param output At least four writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_le32(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)value;
  output[1] = (uint8_t)(value >> 8u);
  output[2] = (uint8_t)(value >> 16u);
  output[3] = (uint8_t)(value >> 24u);
}

/** Store a 64-bit value in little-endian order.
 * @param output At least eight writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_le64(uint8_t *output, uint64_t value) {
  jb_store_le32(output, (uint32_t)value);
  jb_store_le32(output + 4u, (uint32_t)(value >> 32u));
}

/** Load a big-endian 16-bit value.
 * @param input At least two readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint16_t jb_load_be16(const uint8_t *input) {
  return (uint16_t)((uint16_t)((uint16_t)input[0] << 8u) | (uint16_t)input[1]);
}

/** Combine two bytes into a 16-bit value.
 * @param msb Bits 15..8 of the result.
 * @param lsb Bits 7..0 of the result.
 * @return The combined value.
 */
static inline uint16_t jb_u16_from_bytes(uint8_t msb, uint8_t lsb) {
  const uint8_t bytes[] = {msb, lsb};
  return jb_load_be16(bytes);
}

/** Load a big-endian 32-bit value.
 * @param input At least four readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint32_t jb_load_be32(const uint8_t *input) {
  return ((uint32_t)input[0] << 24u) | ((uint32_t)input[1] << 16u) | ((uint32_t)input[2] << 8u) |
         (uint32_t)input[3];
}

/** Load a big-endian 64-bit value.
 * @param input At least eight readable bytes, any alignment.
 * @return The decoded value.
 */
static inline uint64_t jb_load_be64(const uint8_t *input) {
  return ((uint64_t)jb_load_be32(input) << 32u) | (uint64_t)jb_load_be32(input + 4u);
}

/** Store a 16-bit value in big-endian order.
 * @param output At least two writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_be16(uint8_t *output, uint16_t value) {
  output[0] = (uint8_t)(value >> 8u);
  output[1] = (uint8_t)value;
}

/** Store a 32-bit value in big-endian order.
 * @param output At least four writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_be32(uint8_t *output, uint32_t value) {
  output[0] = (uint8_t)(value >> 24u);
  output[1] = (uint8_t)(value >> 16u);
  output[2] = (uint8_t)(value >> 8u);
  output[3] = (uint8_t)value;
}

/** Store a 64-bit value in big-endian order.
 * @param output At least eight writable bytes, any alignment.
 * @param value Value to encode.
 */
static inline void jb_store_be64(uint8_t *output, uint64_t value) {
  jb_store_be32(output, (uint32_t)(value >> 32u));
  jb_store_be32(output + 4u, (uint32_t)value);
}

#ifdef __cplusplus
}
#endif

#endif
