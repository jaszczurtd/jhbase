#ifndef JB_ABI_H
#define JB_ABI_H

/** @file abi.h
 * Natural 32/64-bit layouts used by this prototype. Packing is rejected.
 */
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** Compile-time assertion usable from C11 and C++17. */
#ifdef __cplusplus
#define JB_STATIC_ASSERT(condition, message) static_assert(condition, message)
/** Natural alignment of a scalar or complete object type. */
#define JB_ALIGNOF(type) alignof(type)
#else
#define JB_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
/** Natural alignment of a scalar or complete object type. */
#define JB_ALIGNOF(type) _Alignof(type)
#endif

JB_STATIC_ASSERT(CHAR_BIT == 8, "jb requires 8-bit bytes");
JB_STATIC_ASSERT(sizeof(size_t) == 4 || sizeof(size_t) == 8, "jb requires 32-bit or 64-bit size_t");
JB_STATIC_ASSERT(sizeof(void *) == sizeof(size_t), "jb pointer/size_t mismatch");
JB_STATIC_ASSERT(JB_ALIGNOF(void *) == sizeof(size_t) && JB_ALIGNOF(size_t) == sizeof(size_t),
                 "jb requires natural pointer/size_t alignment");
JB_STATIC_ASSERT(sizeof(uint32_t) == 4 && JB_ALIGNOF(uint32_t) == 4,
                 "jb requires natural uint32_t alignment");
JB_STATIC_ASSERT(sizeof(bool) == 1 && JB_ALIGNOF(bool) == 1, "jb requires one-byte bool");

#endif
