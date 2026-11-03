#include "jb/map.h"
#include "unity.h"
#include <jb/array.h>
#include <stdlib.h>
#include <string.h>

static jb_map_t map;
static size_t allocations, releases, live, comparisons;
static bool fail_allocation;

static void *allocate(void *context, size_t bytes) {
  (void)context;
  ++allocations;
  if (fail_allocation)
    return NULL;
  void *result = malloc(bytes);
  if (result != NULL)
    ++live;
  return result;
}

static void release(void *context, void *pointer) {
  (void)context;
  if (pointer != NULL) {
    ++releases;
    --live;
  }
  free(pointer);
}

static const jb_allocator_t allocator = {NULL, allocate, release};

static int compare_int(const void *left, const void *right, void *context) {
  (void)context;
  ++comparisons;
  const int a = *(const int *)left, b = *(const int *)right;
  return (a > b) - (a < b);
}

void setUp(void) {
  map = (jb_map_t){0};
  allocations = releases = live = comparisons = 0;
  fail_allocation = false;
}

void tearDown(void) {
  jb_map_destroy(&map);
  TEST_ASSERT_EQUAL(0, live);
}

static void init_int_map(void) {
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_map_init(&map, sizeof(int), sizeof(int), compare_int, NULL, &allocator));
}

static void put_int(int key) {
  const int value = key * 3;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_insert(&map, &key, &value));
}

static void check_order(const int *keys, size_t count) {
  TEST_ASSERT_EQUAL(count, jb_map_size(&map));
  jb_map_iter_t iterator = {0};
  TEST_ASSERT_EQUAL(count == 0 ? HAL_ENOENT : HAL_OK, jb_map_first(&map, &iterator));
  for (size_t i = 0; i < count; ++i) {
    const void *key = NULL, *value = NULL;
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_iter_get(&iterator, &key, &value));
    TEST_ASSERT_EQUAL_INT(keys[i], *(const int *)key);
    TEST_ASSERT_EQUAL_INT(keys[i] * 3, *(const int *)value);
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &keys[i], &value));
    TEST_ASSERT_EQUAL_INT(keys[i] * 3, *(const int *)value);
    TEST_ASSERT_EQUAL(i + 1 == count ? HAL_ENOENT : HAL_OK, jb_map_next(&iterator));
  }
  if (count != 0)
    TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_next(&iterator));
}

static void test_key_order_and_duplicate_preserve_generator_output(void) {
  init_int_map();
  const int input[] = {9, 1, 6, 3, 8, 2, 7, 4, 5};
  const int expected[] = {1, 2, 3, 4, 5, 6, 7, 8, 9};
  for (size_t i = 0; i < COUNTOF(input); ++i)
    put_int(input[i]);
  const int replacement = 99;
  const size_t calls = allocations;
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_map_insert(&map, &input[0], &replacement));
  TEST_ASSERT_EQUAL(calls, allocations);
  check_order(expected, COUNTOF(expected));
}

static void test_erasing_two_child_nodes_keeps_other_addresses(void) {
  init_int_map();
  static const void *values[128];
  static bool present[128];
  static int expected[127];
  memset(present, 0, sizeof(present));
  for (int key = 1; key <= 127; ++key) {
    put_int(key);
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &key, &values[key]));
    present[key] = true;
  }
  /* Removing the middle of ordered input exercises successor transplantation;
   * retained consumers must still see the same storage after every removal. */
  const int erased[] = {64, 32, 96, 16, 48, 80, 112, 65, 33, 97};
  for (size_t i = 0; i < COUNTOF(erased); ++i) {
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_erase(&map, &erased[i]));
    present[erased[i]] = false;
    size_t count = 0;
    for (int key = 1; key <= 127; ++key) {
      if (present[key]) {
        const void *value = NULL;
        TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &key, &value));
        TEST_ASSERT_EQUAL_PTR(values[key], value);
        expected[count++] = key;
      }
    }
    check_order(expected, count);
  }
}

static void test_mixed_erase_order_keeps_lookup_and_iteration_bounded(void) {
  init_int_map();
  static bool present[257];
  static int expected[257];
  memset(present, 1, sizeof(present));
  for (int i = 0; i < 257; ++i)
    put_int((i * 73) % 257);
  for (int i = 0; i < 257; ++i) {
    const int key = (i * 97) % 257;
    const void *value = NULL;
    comparisons = 0;
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &key, &value));
    TEST_ASSERT_LESS_THAN(20, comparisons);
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_erase(&map, &key));
    present[key] = false;
    TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_find_const(&map, &key, &value));
    size_t count = 0;
    for (int j = 0; j < 257; ++j)
      if (present[j])
        expected[count++] = j;
    check_order(expected, count);
  }
  TEST_ASSERT_EQUAL(0, live);
}

static void test_allocation_failure_preserves_entries_and_replacement_works(void) {
  init_int_map();
  const int first = 1, second = 2, absent = 3, updated = 100;
  put_int(first);
  put_int(second);
  const void *before = NULL;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &first, &before));
  fail_allocation = true;
  TEST_ASSERT_EQUAL(HAL_ENOMEM, jb_map_insert(&map, &absent, &updated));
  TEST_ASSERT_EQUAL(HAL_ENOMEM, jb_map_upsert(&map, &absent, &updated));
  TEST_ASSERT_EQUAL(2, jb_map_size(&map));
  const int expected[] = {1, 2};
  check_order(expected, COUNTOF(expected));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_upsert(&map, &first, before));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_upsert(&map, &first, &updated));
  void *after = NULL;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_find(&map, &first, &after));
  TEST_ASSERT_EQUAL_PTR(before, after);
  TEST_ASSERT_EQUAL_INT(updated, *(const int *)after);
  *(int *)after = 200;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &first, &before));
  TEST_ASSERT_EQUAL_INT(200, *(const int *)before);
}

static int compare_byte(const void *left, const void *right, void *context) {
  (void)context;
  return (int)*(const unsigned char *)left - (int)*(const unsigned char *)right;
}

static void test_overlapping_upsert_and_maximum_natural_alignment(void) {
  unsigned char key[JB_ALIGNOF(jb_max_align_t)];
  unsigned char bytes[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  memset(key, 0x42, sizeof(key));
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_map_init(&map, sizeof(key), sizeof(bytes), compare_byte, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_insert(&map, key, bytes));
  jb_map_iter_t iterator;
  const void *stored_key = NULL, *value = NULL;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_first(&map, &iterator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_iter_get(&iterator, &stored_key, &value));
  TEST_ASSERT_EQUAL(0, (uintptr_t)stored_key % JB_ALIGNOF(jb_max_align_t));
  TEST_ASSERT_EQUAL(0, (uintptr_t)value % JB_ALIGNOF(jb_max_align_t));
  TEST_ASSERT_EQUAL_PTR((const unsigned char *)stored_key + sizeof(key), value);
  unsigned char expected[8];
  memcpy(expected, (const unsigned char *)value - 4, sizeof(expected));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_upsert(&map, stored_key, (const unsigned char *)value - 4));
  TEST_ASSERT_EQUAL_MEMORY(expected, value, sizeof(expected));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_erase(&map, stored_key));
  TEST_ASSERT_EQUAL(0, live);
}

static int compare_text(const void *left, const void *right, void *context) {
  const int direction = *(const int *)context;
  const int order = strcmp(*(const char *const *)left, *(const char *const *)right);
  return direction * ((order > 0) - (order < 0));
}

static void test_equivalent_text_key_preserves_original_key_and_context(void) {
  const char first_text[] = "module", second_text[] = "module";
  const char *first = first_text, *second = second_text;
  const int direction = -1, initial = 3, updated = 9;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_init(&map, sizeof(first), sizeof(initial), compare_text,
                                        (void *)&direction, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_insert(&map, &first, &initial));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_upsert(&map, &second, &updated));
  TEST_ASSERT_EQUAL(1, jb_map_size(&map));
  jb_map_iter_t iterator;
  const void *key = NULL, *value = NULL;
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_first(&map, &iterator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_iter_get(&iterator, &key, &value));
  TEST_ASSERT_EQUAL_PTR(first, *(const char *const *)key);
  TEST_ASSERT_EQUAL_INT(updated, *(const int *)value);
  jb_map_destroy(&map);
}

static void test_reuse_after_clear_and_destroy(void) {
  init_int_map();
  for (int key = 0; key < 64; ++key)
    put_int(key);
  jb_map_clear(&map);
  TEST_ASSERT_EQUAL(0, live);
  TEST_ASSERT_EQUAL(allocations, releases);
  TEST_ASSERT_EQUAL(0, jb_map_size(&map));
  put_int(12);
  jb_map_destroy(&map);
  TEST_ASSERT_EQUAL(0, live);
  jb_map_destroy(&map);
  init_int_map();
  put_int(21);
}

static void test_invalid_arguments_overflow_and_failure_outputs(void) {
  const int key = 1;
  const void *result = &key;
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_map_find_const(&map, &key, &result));
  TEST_ASSERT_EQUAL_PTR(&key, result);
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_map_init(NULL, sizeof(key), sizeof(key), compare_int, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_map_init(&map, 0, 1, compare_int, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_map_init(&map, 1, 1, NULL, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_map_init(&map, SIZE_MAX, 1, compare_int, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_map_init(&map, 1, SIZE_MAX, compare_int, NULL, &allocator));
  TEST_ASSERT_EQUAL(0, allocations);
  init_int_map();
  TEST_ASSERT_EQUAL(HAL_ESTATE,
                    jb_map_init(&map, sizeof(key), sizeof(key), compare_int, NULL, &allocator));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_map_insert(&map, NULL, &key));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_map_upsert(&map, &key, NULL));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_find_const(&map, &key, &result));
  TEST_ASSERT_EQUAL_PTR(&key, result);
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_erase(&map, &key));
  jb_map_iter_t iterator = {0};
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_first(&map, &iterator));
  TEST_ASSERT_NULL(iterator.owner);
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_map_next(&iterator));
  put_int(key);
  TEST_ASSERT_EQUAL(HAL_OK, jb_map_first(&map, &iterator));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_next(&iterator));
  const void *out_key = &key;
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_map_iter_get(&iterator, &out_key, &result));
  TEST_ASSERT_EQUAL_PTR(&key, out_key);
  TEST_ASSERT_EQUAL_PTR(&key, result);
  jb_map_clear(NULL);
  jb_map_destroy(NULL);
  TEST_ASSERT_EQUAL(0, jb_map_size(NULL));
}

static void check_search_height(int first, int end) {
  const size_t count = (size_t)(end - first);
  size_t smaller = 0, nodes = 1, maximum_height = 0;
  /* An AVL tree of height h needs N(h) = 1 + N(h-1) + N(h-2) nodes. */
  while (nodes <= count) {
    const size_t next = 1 + nodes + smaller;
    smaller = nodes;
    nodes = next;
    ++maximum_height;
  }
  for (int key = first; key < end; ++key) {
    const void *value = NULL;
    comparisons = 0;
    TEST_ASSERT_EQUAL(HAL_OK, jb_map_find_const(&map, &key, &value));
    TEST_ASSERT_TRUE(comparisons <= maximum_height);
    TEST_ASSERT_EQUAL_INT(key * 3, *(const int *)value);
  }
}

static void test_sorted_updates_keep_avl_search_height(void) {
  init_int_map();
  for (unsigned reverse = 0; reverse < 2; ++reverse) {
    for (int i = 0; i < 1024; ++i)
      put_int(reverse != 0 ? 1023 - i : i);
    check_search_height(0, 1024);
    for (int i = 0; i < 1024; ++i) {
      const int key = reverse != 0 ? 1023 - i : i;
      TEST_ASSERT_EQUAL(HAL_OK, jb_map_erase(&map, &key));
      check_search_height(reverse != 0 ? 0 : i + 1, reverse != 0 ? key : 1024);
    }
  }
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_key_order_and_duplicate_preserve_generator_output);
  RUN_TEST(test_erasing_two_child_nodes_keeps_other_addresses);
  RUN_TEST(test_mixed_erase_order_keeps_lookup_and_iteration_bounded);
  RUN_TEST(test_sorted_updates_keep_avl_search_height);
  RUN_TEST(test_allocation_failure_preserves_entries_and_replacement_works);
  RUN_TEST(test_overlapping_upsert_and_maximum_natural_alignment);
  RUN_TEST(test_equivalent_text_key_preserves_original_key_and_context);
  RUN_TEST(test_reuse_after_clear_and_destroy);
  RUN_TEST(test_invalid_arguments_overflow_and_failure_outputs);
  return UNITY_END();
}
