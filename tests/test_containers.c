#include "jb/array.h"
#include "jb/json.h"
#include "jb/string.h"
#include "jb/vector.h"
#include "unity.h"
#include <stdlib.h>
#include <string.h>

typedef struct {
  size_t calls, live, fail_at, last_bytes;
} allocation_state_t;

static allocation_state_t allocations;

static void *allocate(void *context, size_t bytes) {
  allocation_state_t *state = context;
  TEST_ASSERT_TRUE(bytes != 0);
  state->last_bytes = bytes;
  if (state->calls++ == state->fail_at)
    return NULL;
  void *data = malloc(bytes);
  if (data != NULL)
    ++state->live;
  return data;
}

static void release(void *context, void *data) {
  allocation_state_t *state = context;
  TEST_ASSERT_NOT_NULL(data);
  --state->live;
  free(data);
}

static const jb_allocator_t allocator = {&allocations, allocate, release};

void setUp(void) { allocations = (allocation_state_t){0, 0, SIZE_MAX, 0}; }

void tearDown(void) { TEST_ASSERT_EQUAL(0, allocations.live); }

static void test_string_embedded_nul_and_self_append(void) {
  jb_string_t string = {0};
  const char source[] = {'A', '\0', 'B'};
  const char expected[] = {'A', '\0', 'B', 'A', '\0', 'B', '\0'};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL_STRING("", jb_string_data(&string));
  TEST_ASSERT_EQUAL(0, allocations.calls);
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, source, sizeof(source)));
  const char *before = jb_string_data(&string);
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_append(&string, before, sizeof(source)));
  TEST_ASSERT_EQUAL(6, jb_string_size(&string));
  TEST_ASSERT_EQUAL_MEMORY(expected, jb_string_data(&string), sizeof(expected));
  TEST_ASSERT_EQUAL(1, allocations.live);
  jb_string_destroy(&string);
  TEST_ASSERT_NULL(jb_string_data(&string));
  jb_string_destroy(&string);
}

static void test_string_overlapping_insert_assign_and_zero_fill(void) {
  jb_string_t string = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, "abcd", 4));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_reserve(&string, 32));
  TEST_ASSERT_EQUAL_size_t(32, jb_string_capacity(&string));
  TEST_ASSERT_EQUAL_size_t(33, allocations.last_bytes);
  const char *reserved = jb_string_data(&string);
  const size_t calls = allocations.calls;
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_reserve(&string, 32));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_reserve(&string, 0));
  TEST_ASSERT_EQUAL_PTR(reserved, jb_string_data(&string));
  TEST_ASSERT_EQUAL_size_t(calls, allocations.calls);
  TEST_ASSERT_EQUAL_size_t(32, jb_string_capacity(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_reserve(&string, 33));
  TEST_ASSERT_EQUAL_size_t(33, jb_string_capacity(&string));
  TEST_ASSERT_EQUAL_size_t(34, allocations.last_bytes);
  TEST_ASSERT_EQUAL_STRING("abcd", jb_string_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_insert(&string, 2, jb_string_data(&string) + 1, 2));
  TEST_ASSERT_EQUAL_STRING("abbccd", jb_string_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, jb_string_data(&string) + 2, 3));
  TEST_ASSERT_EQUAL_STRING("bcc", jb_string_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_erase(&string, 1, 1));
  TEST_ASSERT_EQUAL_STRING("bc", jb_string_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_resize(&string, 4, '!'));
  TEST_ASSERT_EQUAL_STRING("bc!!", jb_string_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_resize(&string, 2, '?'));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_resize(&string, 5, '\0'));
  const char expected[] = {'b', 'c', '\0', '\0', '\0', '\0'};
  TEST_ASSERT_EQUAL_MEMORY(expected, jb_string_data(&string), sizeof(expected));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_append(&string, jb_string_data(&string) + 5, 1));
  TEST_ASSERT_EQUAL(6, jb_string_size(&string));
  TEST_ASSERT_EQUAL_CHAR('\0', jb_string_data(&string)[6]);
  const size_t capacity = jb_string_capacity(&string);
  jb_string_clear(&string);
  TEST_ASSERT_EQUAL_STRING("", jb_string_data(&string));
  TEST_ASSERT_EQUAL(capacity, jb_string_capacity(&string));
  jb_string_destroy(&string);
}

static void test_string_failed_mutations_keep_bytes_and_owner(void) {
  jb_string_t string = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, "keep", 4));
  const char *before = jb_string_data(&string);
  const size_t capacity = jb_string_capacity(&string);
  for (unsigned operation = 0; operation < 5; ++operation) {
    allocations.fail_at = allocations.calls;
    hal_status_t status;
    switch (operation) {
    case 0:
      status = jb_string_reserve(&string, capacity + 1);
      break;
    case 1:
      status = jb_string_assign(&string, "replacement exceeds current space", 33);
      break;
    case 2:
      status = jb_string_append(&string, before, 4);
      break;
    case 3:
      status = jb_string_insert(&string, 1, before + 1, 2);
      break;
    default:
      status = jb_string_resize(&string, capacity + 1, '!');
      break;
    }
    TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
    TEST_ASSERT_EQUAL_PTR(before, jb_string_data(&string));
    TEST_ASSERT_EQUAL(capacity, jb_string_capacity(&string));
    TEST_ASSERT_EQUAL(4, jb_string_size(&string));
    TEST_ASSERT_EQUAL_STRING("keep", jb_string_data(&string));
  }
  jb_string_destroy(&string);
}

static void test_string_overflow_and_invalid_internal_span(void) {
  jb_string_t string = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, "abc", 3));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_reserve(&string, 32));
  const size_t calls = allocations.calls;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_string_reserve(&string, SIZE_MAX));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_string_append(&string, "x", SIZE_MAX));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_assign(&string, jb_string_data(&string) + 4, 1));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_insert(&string, 1, jb_string_data(&string) + 2, 3));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_erase(&string, SIZE_MAX, 1));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_erase(&string, 2, SIZE_MAX));
  TEST_ASSERT_EQUAL(calls, allocations.calls);
  TEST_ASSERT_EQUAL_STRING("abc", jb_string_data(&string));
  jb_string_destroy(&string);
}

static void test_decode_json_into_resized_string(void) {
  static unsigned char scratch[256];
  static const char input[] = "\"a\\u0000b\"";
  const jb_json_limits_t limits = {sizeof(input) - 1, 1, 1, 16, 0, 0, 0};
  jb_json_token_t token;
  jb_json_doc_t document = {0};
  TEST_ASSERT_EQUAL(
      HAL_OK, jb_json_parse((jb_json_span_t){input, sizeof(input) - 1}, &limits, &token, 1,
                            (jb_json_workspace_t){scratch, sizeof(scratch)}, &document, NULL));
  jb_json_node_t value;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_root(&document, &value));
  size_t length = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_string_copy(value, NULL, 0, &length));
  jb_string_t string = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_NULL(jb_string_mutable_data(&string));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_resize(&string, length, '?'));
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_string_copy(value, jb_string_mutable_data(&string), length, &length));
  const char expected[] = {'a', '\0', 'b', '\0'};
  TEST_ASSERT_EQUAL(3, jb_string_size(&string));
  TEST_ASSERT_EQUAL_MEMORY(expected, jb_string_data(&string), sizeof(expected));
  jb_string_destroy(&string);
}

static void test_string_comparison_uses_all_bytes_and_preserves_error_output(void) {
  jb_string_t left = {0}, right = {0}, inactive = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&left, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&right, &allocator));
  int ordering = 99;
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_compare(&left, &right, &ordering));
  TEST_ASSERT_EQUAL(0, ordering);
  const unsigned char a[] = {'x', '\0', 0x80};
  const unsigned char b[] = {'x', '\0', 0x7f};
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&left, (const char *)a, sizeof(a)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&right, (const char *)b, sizeof(b)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_compare(&left, &right, &ordering));
  TEST_ASSERT_TRUE(ordering > 0);
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_compare(&right, &left, &ordering));
  TEST_ASSERT_TRUE(ordering < 0);
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&right, (const char *)a, sizeof(a)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_compare(&left, &right, &ordering));
  TEST_ASSERT_EQUAL(0, ordering);
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_resize(&right, 2, '\0'));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_compare(&left, &right, &ordering));
  TEST_ASSERT_TRUE(ordering > 0);
  ordering = 99;
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_string_compare(&left, &inactive, &ordering));
  TEST_ASSERT_EQUAL(99, ordering);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_compare(NULL, &right, &ordering));
  TEST_ASSERT_EQUAL(99, ordering);
  jb_string_destroy(&left);
  jb_string_destroy(&right);
}

static void test_vector_aliasing_and_resize(void) {
  jb_vector_t vector = {0};
  const uint32_t source[] = {11, 22, 33, 44};
  const uint32_t appended[] = {11, 22, 33, 44, 11, 22, 33, 44};
  const uint32_t inserted[] = {11, 22, 33, 44, 22, 33, 44, 11, 22, 33, 44};
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_init(&vector, sizeof(source[0]), &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, source, COUNTOF(source)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_append(&vector, jb_vector_const_data(&vector), 4));
  TEST_ASSERT_EQUAL_MEMORY(appended, jb_vector_const_data(&vector), sizeof(appended));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_reserve(&vector, 32));
  TEST_ASSERT_EQUAL_size_t(32, jb_vector_capacity(&vector));
  TEST_ASSERT_EQUAL_size_t(32 * sizeof(source[0]), allocations.last_bytes);
  const void *reserved = jb_vector_const_data(&vector);
  const size_t calls = allocations.calls;
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_reserve(&vector, 32));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_reserve(&vector, 0));
  TEST_ASSERT_EQUAL_PTR(reserved, jb_vector_const_data(&vector));
  TEST_ASSERT_EQUAL_size_t(calls, allocations.calls);
  TEST_ASSERT_EQUAL_size_t(32, jb_vector_capacity(&vector));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_reserve(&vector, 33));
  TEST_ASSERT_EQUAL_size_t(33, jb_vector_capacity(&vector));
  TEST_ASSERT_EQUAL_size_t(33 * sizeof(source[0]), allocations.last_bytes);
  TEST_ASSERT_EQUAL_MEMORY(appended, jb_vector_const_data(&vector), sizeof(appended));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_insert(&vector, 1, jb_vector_const_at(&vector, 1), 3));
  TEST_ASSERT_EQUAL_MEMORY(inserted, jb_vector_const_data(&vector), sizeof(inserted));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, jb_vector_const_at(&vector, 4), 3));
  TEST_ASSERT_EQUAL_MEMORY(source + 1, jb_vector_const_data(&vector), 3 * sizeof(source[0]));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_erase(&vector, 1, 1));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_resize(&vector, 4));
  const uint32_t resized[] = {22, 44, 0, 0};
  TEST_ASSERT_EQUAL_MEMORY(resized, jb_vector_const_data(&vector), sizeof(resized));
  TEST_ASSERT_NULL(jb_vector_at(&vector, 4));
  TEST_ASSERT_EQUAL(sizeof(uint32_t), jb_vector_element_size(&vector));
  jb_vector_clear(&vector);
  TEST_ASSERT_EQUAL(0, jb_vector_size(&vector));
  TEST_ASSERT_NOT_NULL(jb_vector_data(&vector));
  jb_vector_destroy(&vector);
}

static void test_vector_failed_mutations_preserve_contents(void) {
  jb_vector_t vector = {0};
  const uint32_t source[] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_init(&vector, sizeof(source[0]), &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, source, COUNTOF(source)));
  const void *before = jb_vector_const_data(&vector);
  const size_t capacity = jb_vector_capacity(&vector);
  for (unsigned operation = 0; operation < 4; ++operation) {
    allocations.fail_at = allocations.calls;
    hal_status_t status;
    switch (operation) {
    case 0:
      status = jb_vector_reserve(&vector, capacity + 1);
      break;
    case 1:
      status = jb_vector_append(&vector, before, 4);
      break;
    case 2:
      status = jb_vector_insert(&vector, 1, jb_vector_const_at(&vector, 1), 2);
      break;
    default:
      status = jb_vector_resize(&vector, capacity + 1);
      break;
    }
    TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
    TEST_ASSERT_EQUAL_PTR(before, jb_vector_const_data(&vector));
    TEST_ASSERT_EQUAL(capacity, jb_vector_capacity(&vector));
    TEST_ASSERT_EQUAL(4, jb_vector_size(&vector));
    TEST_ASSERT_EQUAL_MEMORY(source, jb_vector_const_data(&vector), sizeof(source));
  }
  jb_vector_destroy(&vector);
}

static void test_vector_overflow_and_invalid_internal_span(void) {
  jb_vector_t vector = {0};
  const uint32_t source[] = {1, 2};
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_init(&vector, sizeof(source[0]), &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, source, COUNTOF(source)));
  const size_t calls = allocations.calls;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_vector_reserve(&vector, SIZE_MAX));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_vector_assign(&vector, source, SIZE_MAX));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_vector_append(&vector, source, SIZE_MAX));
  const uint32_t *inside = jb_vector_const_data(&vector);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_assign(&vector, inside + 2, 1));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_insert(&vector, 1, inside + 1, 2));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_erase(&vector, 1, SIZE_MAX));
  TEST_ASSERT_EQUAL(calls, allocations.calls);
  TEST_ASSERT_EQUAL_MEMORY(source, jb_vector_const_data(&vector), sizeof(source));
  jb_vector_destroy(&vector);
}

static void test_vector_detach_preserves_allocator_and_rejects_output_aliases(void) {
  jb_vector_t vector = {0};
  const uint32_t source[] = {1, 2, 3, 4};
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_init(&vector, sizeof(source[0]), &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, source, COUNTOF(source)));
  void *data = NULL;
  size_t count = 99, capacity = 99;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_detach(&vector, &vector.data, &count, &capacity));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_detach(&vector, &data, &count, &count));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_detach(&vector, &data, vector.data, &capacity));
  TEST_ASSERT_NULL(data);
  TEST_ASSERT_EQUAL(99, count);
  TEST_ASSERT_EQUAL(99, capacity);
  TEST_ASSERT_EQUAL_MEMORY(source, jb_vector_const_data(&vector), sizeof(source));
  const void *before = jb_vector_const_data(&vector);
  allocations.fail_at = allocations.calls;
  TEST_ASSERT_EQUAL(HAL_ENOMEM, jb_vector_shrink_to_fit(&vector));
  TEST_ASSERT_EQUAL_PTR(before, jb_vector_const_data(&vector));
  TEST_ASSERT_EQUAL_MEMORY(source, before, sizeof(source));
  allocations.fail_at = SIZE_MAX;
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_shrink_to_fit(&vector));
  TEST_ASSERT_EQUAL_size_t(COUNTOF(source), jb_vector_capacity(&vector));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_detach(&vector, &data, &count, &capacity));
  TEST_ASSERT_EQUAL(4, count);
  TEST_ASSERT_TRUE(capacity >= count);
  TEST_ASSERT_EQUAL_MEMORY(source, data, sizeof(source));
  TEST_ASSERT_NULL(jb_vector_data(&vector));
  TEST_ASSERT_EQUAL(0, jb_vector_size(&vector));
  TEST_ASSERT_EQUAL(sizeof(uint32_t), jb_vector_element_size(&vector));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_shrink_to_fit(&vector));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_append(&vector, source, 1));
  jb_vector_clear(&vector);
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_shrink_to_fit(&vector));
  TEST_ASSERT_NULL(jb_vector_data(&vector));
  jb_vector_destroy(&vector);
  allocator.release(allocator.context, data);
}

static void test_initialization_does_not_replace_existing_ownership(void) {
  jb_string_t string = {0};
  jb_vector_t vector = {0};
  const jb_allocator_t invalid = {NULL, NULL, release};
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_string_init(&string, &invalid));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_vector_init(&vector, 0, &allocator));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_string_reserve(&string, 1));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_vector_resize(&vector, 1));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_init(&vector, 1, &allocator));
  TEST_ASSERT_EQUAL(HAL_OK, jb_string_assign(&string, "x", 1));
  TEST_ASSERT_EQUAL(HAL_OK, jb_vector_assign(&vector, "y", 1));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_string_init(&string, &allocator));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_vector_init(&vector, 1, &allocator));
  TEST_ASSERT_EQUAL_STRING("x", jb_string_data(&string));
  TEST_ASSERT_EQUAL_MEMORY("y", jb_vector_const_data(&vector), 1);
  jb_string_destroy(&string);
  jb_vector_destroy(&vector);
}

int main(void) {
  UNITY_BEGIN();
  RUN_TEST(test_string_embedded_nul_and_self_append);
  RUN_TEST(test_string_overlapping_insert_assign_and_zero_fill);
  RUN_TEST(test_string_failed_mutations_keep_bytes_and_owner);
  RUN_TEST(test_string_overflow_and_invalid_internal_span);
  RUN_TEST(test_decode_json_into_resized_string);
  RUN_TEST(test_string_comparison_uses_all_bytes_and_preserves_error_output);
  RUN_TEST(test_vector_aliasing_and_resize);
  RUN_TEST(test_vector_failed_mutations_preserve_contents);
  RUN_TEST(test_vector_overflow_and_invalid_internal_span);
  RUN_TEST(test_vector_detach_preserves_allocator_and_rejects_output_aliases);
  RUN_TEST(test_initialization_does_not_replace_existing_ownership);
  return UNITY_END();
}
