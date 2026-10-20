#ifndef JB_TEST_FILE_H
#define JB_TEST_FILE_H
#include <stdio.h>

/* Use the supported CRT entry point without suppressing MSVC diagnostics. */
static inline FILE *jb_test_file_open(const char *path, const char *mode) {
#ifdef _MSC_VER
  FILE *file = NULL;
  return fopen_s(&file, path, mode) == 0 ? file : NULL;
#else
  return fopen(path, mode);
#endif
}
#endif
