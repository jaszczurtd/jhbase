#include "library_exercise.h"
#include <stdlib.h>

static volatile bool guarded;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
void __real_free(void *pointer);
void *__wrap_malloc(size_t size);
void *__wrap_calloc(size_t count, size_t size);
void *__wrap_realloc(void *pointer, size_t size);
void __wrap_free(void *pointer);

void *__wrap_malloc(size_t size) {
  if (guarded) {
    abort();
  }
  return __real_malloc(size);
}
void *__wrap_calloc(size_t count, size_t size) {
  if (guarded) {
    abort();
  }
  return __real_calloc(count, size);
}
void *__wrap_realloc(void *pointer, size_t size) {
  if (guarded) {
    abort();
  }
  return __real_realloc(pointer, size);
}
void __wrap_free(void *pointer) {
  if (guarded) {
    abort();
  }
  __real_free(pointer);
}
int main(void) {
  guarded = true;
  const int result = library_exercise();
  guarded = false;
  return result;
}
