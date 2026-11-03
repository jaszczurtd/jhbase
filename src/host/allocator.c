#include "jb/allocator.h"
#include <stdlib.h>

static void *heap_allocate(void *context, size_t bytes) {
  (void)context;
  return malloc(bytes);
}

static void heap_release(void *context, void *data) {
  (void)context;
  free(data);
}

const jb_allocator_t *jb_heap_allocator(void) {
  static const jb_allocator_t allocator = {NULL, heap_allocate, heap_release};
  return &allocator;
}
