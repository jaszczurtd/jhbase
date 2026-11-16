#ifndef JB_TEST_HEAP_H
#define JB_TEST_HEAP_H
/* Counting replacement of jb_heap_allocator for one C host test program.
 * The test object defines the symbol, so the linker skips the library member.
 * Setting fail_allocation to n fails the n-th following allocation; setUp and
 * tearDown reset it and require every allocation to be released. */
#include "jb/allocator.h"
#include "unity.h"
#include <stdlib.h>

static size_t fail_allocation, live_allocations;

static void *test_allocate(void *context, size_t bytes) {
  (void)context;
  if (fail_allocation != 0 && --fail_allocation == 0)
    return NULL;
  void *data = malloc(bytes);
  if (data != NULL)
    ++live_allocations;
  return data;
}

static void test_release(void *context, void *data) {
  (void)context;
  if (live_allocations == 0)
    abort();
  --live_allocations;
  free(data);
}

const jb_allocator_t *jb_heap_allocator(void) {
  static const jb_allocator_t allocator = {NULL, test_allocate, test_release};
  return &allocator;
}

void setUp(void) { fail_allocation = 0; }
void tearDown(void) {
  fail_allocation = 0;
  TEST_ASSERT_EQUAL_size_t(0, live_allocations);
}
#endif
