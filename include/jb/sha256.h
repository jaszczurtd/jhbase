#ifndef JB_SHA256_H
#define JB_SHA256_H

/** @file sha256.h
 * Incremental SHA-256 (FIPS 180-4) without heap. The caller owns the context
 * and the functions do not validate it; wrappers that need argument checks
 * (JaszczurHAL hal_sha256_*_ex) check before calling them.
 */
#include "jb/abi.h"

#ifdef __cplusplus
extern "C" {
#endif

/** SHA-256 block size in bytes. */
#define JB_SHA256_BLOCK_BYTES 64u

/** SHA-256 digest size in bytes. */
#define JB_SHA256_DIGEST_BYTES 32u

/** Incremental SHA-256 state. The fields are private; the fixed layout lets
 * firmware place the context statically. initialized is true between
 * jb_sha256_init() and jb_sha256_final().
 */
typedef struct {
  uint32_t state[8];
  uint64_t bit_count;
  uint8_t buffer[JB_SHA256_BLOCK_BYTES];
  size_t buffer_len;
  bool initialized;
} jb_sha256_context_t;

/* The alignment is checked as well: on 32-bit targets #pragma pack(4) keeps
 * every offset and the size but halves the alignment of bit_count. */
JB_STATIC_ASSERT(JB_ALIGNOF(jb_sha256_context_t) == JB_ALIGNOF(uint64_t) &&
                     offsetof(jb_sha256_context_t, bit_count) == 32 &&
                     offsetof(jb_sha256_context_t, buffer) == 40 &&
                     offsetof(jb_sha256_context_t, buffer_len) == 104 &&
                     offsetof(jb_sha256_context_t, initialized) == 104 + sizeof(size_t) &&
                     sizeof(jb_sha256_context_t) == 104 + 2 * sizeof(size_t),
                 "jb requires natural jb_sha256_context_t layout");

/** Start a digest; any previous state of context is replaced.
 * @param context Required context.
 */
void jb_sha256_init(jb_sha256_context_t *context);

/** Add bytes to a digest started by jb_sha256_init().
 * @param context Required started context.
 * @param data Input bytes; NULL only when length is zero.
 * @param length Input size in bytes; zero leaves the context unchanged.
 */
void jb_sha256_update(jb_sha256_context_t *context, const uint8_t *data, size_t length);

/** Finish a digest started by jb_sha256_init() and clear initialized.
 * Start the context again before the next digest.
 * @param context Required started context.
 * @param digest Required output of JB_SHA256_DIGEST_BYTES bytes.
 */
void jb_sha256_final(jb_sha256_context_t *context, uint8_t digest[JB_SHA256_DIGEST_BYTES]);

#ifdef __cplusplus
}
#endif

#endif
