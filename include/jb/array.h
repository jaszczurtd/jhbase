#ifndef JB_ARRAY_H
#define JB_ARRAY_H

/** @file array.h
 * Compile-time helpers for fixed-size C and C++ arrays. JaszczurHAL exposes
 * the same macro through hal/core/hal_array.h.
 */

/**
 * @def COUNTOF(arr)
 * @brief Number of elements in an actual array, including a local array.
 *
 * This is the primary JaszczurHAL spelling used by firmware and downstream
 * applications.
 *
 * @note Works only for actual arrays. Passing a pointer yields an incorrect
 *       result.
 */
#ifndef COUNTOF
#define COUNTOF(arr) (sizeof(arr) / sizeof((arr)[0]))
#endif

#endif
