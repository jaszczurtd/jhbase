#include "jb/map.h"
#include "jb/string.h"
#include "jb/vector.h"
#include <jb/array.h>
#include <stdio.h>
#include <string.h>

struct sample {
  const char *name;
  int amount;
};

static int compare_names(const void *left, const void *right, void *context) {
  (void)context;
  return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static hal_status_t aggregate(const jb_vector_t *samples, jb_map_t *totals) {
  for (size_t i = 0; i < jb_vector_size(samples); ++i) {
    const struct sample *sample = (const struct sample *)jb_vector_const_at(samples, i);
    void *value = NULL;
    hal_status_t status = jb_map_find(totals, &sample->name, &value);
    if (status == HAL_ENOENT)
      status = jb_map_insert(totals, &sample->name, &sample->amount);
    else if (status == HAL_OK)
      *(int *)value += sample->amount;
    if (status != HAL_OK)
      return status;
  }
  return HAL_OK;
}

static hal_status_t report(const jb_map_t *totals, jb_string_t *text) {
  jb_map_iter_t iterator;
  hal_status_t status = jb_map_first(totals, &iterator);
  while (status == HAL_OK) {
    const void *key = NULL, *value = NULL;
    status = jb_map_iter_get(&iterator, &key, &value);
    if (status != HAL_OK)
      return status;
    const char *name = *(const char *const *)key;
    char number[32];
    const int length = snprintf(number, sizeof(number), "=%d\n", *(const int *)value);
    if (length < 0 || (size_t)length >= sizeof(number))
      return HAL_EOVERFLOW;
    status = jb_string_append(text, name, strlen(name));
    if (status == HAL_OK)
      status = jb_string_append(text, number, (size_t)length);
    if (status != HAL_OK)
      return status;
    status = jb_map_next(&iterator);
  }
  return status == HAL_ENOENT ? HAL_OK : status;
}

int main(void) {
  static const struct sample input[] = {{"beta", 2}, {"alpha", 1}, {"alpha", 2}};
  static jb_vector_t samples;
  static jb_map_t totals;
  static jb_string_t text;
  const jb_allocator_t *allocator = jb_heap_allocator();
  hal_status_t status = jb_vector_init(&samples, sizeof(struct sample), allocator);
  if (status == HAL_OK)
    status =
        jb_map_init(&totals, sizeof(const char *), sizeof(int), compare_names, NULL, allocator);
  if (status == HAL_OK)
    status = jb_string_init(&text, allocator);
  if (status == HAL_OK)
    status = jb_vector_append(&samples, input, COUNTOF(input));
  if (status == HAL_OK)
    status = aggregate(&samples, &totals);
  if (status == HAL_OK)
    status = report(&totals, &text);
  if (status == HAL_OK &&
      fwrite(jb_string_data(&text), 1, jb_string_size(&text), stdout) != jb_string_size(&text))
    status = HAL_EIO;
  jb_string_destroy(&text);
  jb_map_destroy(&totals);
  jb_vector_destroy(&samples);
  return status == HAL_OK ? 0 : 1;
}
