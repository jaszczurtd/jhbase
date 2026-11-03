#ifndef JB_ALLOCATOR_H
#define JB_ALLOCATOR_H

/** @file allocator.h
 * Explicit allocation for owned C containers. The portable library never
 * chooses a heap allocator implicitly.
 */
#include "jb/status.h"
#ifdef __cplusplus
#include <cstddef>
#endif

/** Alignment carrier for allocations holding ordinary scalar C/C++ types.
 * Uses the standard max_align_t where available. MSVC's C headers omit that
 * type, so MSVC uses the same scalar union in C and C++ instead. Extended
 * over-aligned types require a separate allocator and are not covered here.
 */
#ifdef _MSC_VER
typedef union {
  bool boolean;
  char character;
  short short_integer;
  int integer;
  long long_integer;
  long long wide_integer;
  float single;
  double real;
  long double extended;
  void *object_pointer;
  void (*function_pointer)(void);
} jb_max_align_t;
#ifdef __cplusplus
JB_STATIC_ASSERT(JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(std::max_align_t),
                 "jb requires the full fundamental allocation alignment");
#endif
#else
typedef max_align_t jb_max_align_t;
#endif
JB_STATIC_ASSERT(JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(long double) &&
                     JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(double) &&
                     JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(long long) &&
                     JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(void *) &&
                     JB_ALIGNOF(jb_max_align_t) >= JB_ALIGNOF(void (*)(void)),
                 "jb requires natural scalar allocation alignment");

#ifdef __cplusplus
extern "C" {
#endif

/** Allocation callbacks copied into each owner. Both callbacks are required;
 * context may be NULL and must outlive every allocation. allocate receives a
 * nonzero byte count and returns distinct storage aligned for jb_max_align_t, or
 * NULL on failure. release consumes exactly a pointer returned by allocate;
 * the containers never call it with NULL. Callbacks must not reenter the owner
 * or throw C++ exceptions. Thread safety follows the supplied allocator.
 */
typedef struct {
  void *context;
  void *(*allocate)(void *context, size_t bytes);
  void (*release)(void *context, void *data);
} jb_allocator_t;

/** Borrow the process heap allocator (malloc/free), linked with jb::filesystem
 * (also provided by jb::host).
 * The returned immutable object has static lifetime and never returns NULL.
 * Memory detached from a container using this allocator may be passed to free.
 */
const jb_allocator_t *jb_heap_allocator(void);

#ifdef __cplusplus
}
#endif
JB_STATIC_ASSERT(JB_ALIGNOF(jb_allocator_t) == sizeof(size_t) &&
                     sizeof(jb_allocator_t) == 3 * sizeof(size_t) &&
                     offsetof(jb_allocator_t, allocate) == sizeof(size_t) &&
                     offsetof(jb_allocator_t, release) == 2 * sizeof(size_t),
                 "jb requires a naturally aligned allocator");
#endif
