// Include C++ math before Unity introduces its C math fallback macros.
#include <cmath>

#include "jb/file.h"
#include "unity.h"
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <jb/array.h>
#include <new>
#include <string>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace fs = std::filesystem;
#ifndef JB_NO_ALLOCATION_FAULTS
static size_t fail_allocation = 0;
// Exercise bad_alloc translation at the C API, including temporary-file cleanup.
void *operator new(std::size_t size) {
  if (fail_allocation != 0 && --fail_allocation == 0)
    throw std::bad_alloc();
  if (void *memory = std::malloc(size == 0 ? 1 : size))
    return memory;
  throw std::bad_alloc();
}
void *operator new[](std::size_t size) { return ::operator new(size); }
void operator delete(void *memory) noexcept { std::free(memory); }
void operator delete[](void *memory) noexcept { ::operator delete(memory); }
void operator delete(void *memory, std::size_t) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, std::size_t) noexcept { ::operator delete(memory); }
#endif
static fs::path root;
void setUp() {}
void tearDown() {}

static void test_binary_files_limits_and_unchanged_results() {
  const std::string path = (root / fs::u8path("dane \xc5\xbc") / "binary.json").u8string();
  const unsigned char bytes[] = {'{', 0, '\r', '\n', '}', 0xff};
  bool changed = false;
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_file_write_atomic(path.c_str(), bytes, sizeof(bytes), true, &changed));
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_FALSE(fs::exists(root / fs::u8path("dane \xc5\xbc")));
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_file_write_atomic(path.c_str(), bytes, sizeof(bytes), false, &changed));
  TEST_ASSERT_TRUE(changed);
  jb_file_data_t data{};
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_file_read(path.c_str(), sizeof(bytes) - 1, &data));
  TEST_ASSERT_NULL(data.data);
  TEST_ASSERT_EQUAL(0, data.length);
#ifdef __linux__
  // procfs reports zero size; enforce the limit while reading its contents.
  TEST_ASSERT_TRUE(fs::file_size("/proc/self/status") == 0);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_file_read("/proc/self/status", 1, &data));
  TEST_ASSERT_NULL(data.data);
  TEST_ASSERT_EQUAL(0, data.length);
#endif
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), sizeof(bytes), &data));
  TEST_ASSERT_EQUAL(sizeof(bytes), data.length);
  TEST_ASSERT_EQUAL_MEMORY(bytes, data.data, sizeof(bytes));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_read(path.c_str(), sizeof(bytes), &data));
  jb_file_release(&data);
  TEST_ASSERT_NULL(data.data);
  TEST_ASSERT_EQUAL(0, data.length);
  jb_file_release(&data);
  jb_file_release(nullptr);
  const auto old_time = fs::file_time_type::clock::now() - std::chrono::hours(24);
  fs::last_write_time(fs::u8path(path), old_time);
  const auto before = fs::last_write_time(fs::u8path(path));
  for (bool check : {true, false}) {
    changed = true;
    TEST_ASSERT_EQUAL(HAL_OK,
                      jb_file_write_atomic(path.c_str(), bytes, sizeof(bytes), check, &changed));
    TEST_ASSERT_FALSE(changed);
    TEST_ASSERT_TRUE(fs::last_write_time(fs::u8path(path)) == before);
  }
  changed = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "new", 3, true, &changed));
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), sizeof(bytes), &data));
  TEST_ASSERT_EQUAL_MEMORY(bytes, data.data, sizeof(bytes));
  jb_file_release(&data);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), nullptr, 0, false, &changed));
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), 0, &data));
  TEST_ASSERT_EQUAL(0, data.length);
  jb_file_release(&data);
  for (const auto &entry : fs::directory_iterator(fs::u8path(path).parent_path())) {
    TEST_ASSERT_TRUE(entry.path().filename() == "binary.json");
  }
}

static void test_paths_use_explicit_base_and_transactional_capacity() {
  const std::string base = root.u8string();
  const std::string expected = (root / "b" / fs::u8path("\xc5\xbc.json")).u8string();
  const auto cwd = fs::current_path();
  size_t required = SIZE_MAX;
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_path_resolve(base.c_str(), "a/../b/\xc5\xbc.json", nullptr, 0, &required));
  TEST_ASSERT_EQUAL(expected.size() + 1, required);
  std::vector<char> buffer(required + 1, 'Z');
  required = SIZE_MAX;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_path_resolve(base.c_str(), "a/../b/\xc5\xbc.json",
                                                   buffer.data(), expected.size(), &required));
  TEST_ASSERT_EQUAL(expected.size() + 1, required);
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', buffer.data(), buffer.size());
  required = SIZE_MAX;
  TEST_ASSERT_EQUAL(HAL_OK, jb_path_resolve(base.c_str(), "a/../b/\xc5\xbc.json", buffer.data(),
                                            buffer.size() - 1, &required));
  TEST_ASSERT_EQUAL(expected.size() + 1, required);
  TEST_ASSERT_EQUAL_STRING(expected.c_str(), buffer.data());
  TEST_ASSERT_EQUAL_CHAR('Z', buffer.back());
  required = 42;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_path_resolve("relative", "file", nullptr, 0, &required));
  TEST_ASSERT_EQUAL(42, required);
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_path_resolve(base.c_str(), "\xff", nullptr, 0, &required));
  TEST_ASSERT_EQUAL(42, required);
  TEST_ASSERT_TRUE(fs::current_path() == cwd);
#ifdef _WIN32
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_path_resolve(base.c_str(), "C:relative", nullptr, 0, &required));
#endif
}

static void test_invalid_arguments_and_nonregular_destinations() {
  jb_file_data_t data{};
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_read(nullptr, 0, &data));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_read("", 0, &data));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_read("missing", 0, nullptr));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_file_read("\xff", 0, &data));
  const std::string missing = (root / "missing").u8string();
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_file_read(missing.c_str(), 100, &data));
  const std::string directory = root.u8string();
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_read(directory.c_str(), 100, &data));
#ifdef _WIN32
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_read("NUL", 100, &data));
#endif
  bool changed = false;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_write_atomic(missing.c_str(), nullptr, 1, false, &changed));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_write_atomic(missing.c_str(), "", 0, false, nullptr));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_write_atomic(directory.c_str(), "x", 1, false, &changed));
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_NULL(data.data);
  TEST_ASSERT_EQUAL(0, data.length);
}

static void test_links_permissions_and_failed_publication() {
  const fs::path destination = root / "protected.json";
  const std::string path = destination.u8string();
  bool changed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "old", 3, false, &changed));
#ifdef _WIN32
  HANDLE held = CreateFileW(destination.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  TEST_ASSERT_TRUE(held != INVALID_HANDLE_VALUE);
  changed = false;
  const hal_status_t result = jb_file_write_atomic(path.c_str(), "new", 3, false, &changed);
  CloseHandle(held);
  // MoveFileEx can report either sharing violation or access denied for a held destination.
  TEST_ASSERT_TRUE(result == HAL_EBUSY || result == HAL_EPERM);
  TEST_ASSERT_FALSE(changed);
  jb_file_data_t bytes{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), 3, &bytes));
  TEST_ASSERT_EQUAL_MEMORY("old", bytes.data, 3);
  jb_file_release(&bytes);
#else
  TEST_ASSERT_EQUAL(0, ::chmod(destination.c_str(), 0640));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "new", 3, false, &changed));
  struct stat info;
  TEST_ASSERT_EQUAL(0, ::stat(destination.c_str(), &info));
  TEST_ASSERT_EQUAL(0640, info.st_mode & 0777);
  const fs::path link = root / "link.json";
  fs::create_symlink(destination.filename(), link);
  const std::string link_path = link.u8string();
  changed = false;
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_write_atomic(link_path.c_str(), "bad", 3, false, &changed));
  TEST_ASSERT_FALSE(changed);
  jb_file_data_t bytes{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(link_path.c_str(), 3, &bytes));
  TEST_ASSERT_EQUAL_MEMORY("new", bytes.data, 3);
  jb_file_release(&bytes);
  const fs::path fifo = root / "fifo";
  TEST_ASSERT_EQUAL(0, ::mkfifo(fifo.c_str(), 0600));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_read(fifo.c_str(), 100, &bytes));
#endif
  for (const auto &entry : fs::directory_iterator(root)) {
    TEST_ASSERT_TRUE(entry.path().filename().u8string().find(".tmp-") == std::string::npos);
  }
}

#ifndef JB_NO_ALLOCATION_FAULTS
static void test_allocation_failure_preserves_outputs_and_cleans_temporary_files() {
  const std::string base = root.u8string();
  size_t required = 42;
  fail_allocation = 1;
  const hal_status_t path_status =
      jb_path_resolve(base.c_str(), "output.json", nullptr, 0, &required);
  fail_allocation = 0;
  TEST_ASSERT_EQUAL(HAL_ENOMEM, path_status);
  TEST_ASSERT_EQUAL(42, required);
  const std::string path = (root / "allocation.json").u8string();
  size_t caught = 0;
  for (size_t fail_at = 1; fail_at <= 48; ++fail_at) {
    bool changed = false;
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "old", 3, false, &changed));
    changed = false;
    fail_allocation = fail_at;
    const hal_status_t status = jb_file_write_atomic(path.c_str(), "new", 3, false, &changed);
    fail_allocation = 0;
    TEST_ASSERT_TRUE(status == HAL_ENOMEM || status == HAL_OK);
    jb_file_data_t bytes{};
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), 3, &bytes));
    TEST_ASSERT_EQUAL_MEMORY(status == HAL_OK ? "new" : "old", bytes.data, 3);
    jb_file_release(&bytes);
    TEST_ASSERT_EQUAL(status == HAL_OK, changed);
    if (status == HAL_ENOMEM)
      ++caught;
    for (const auto &entry : fs::directory_iterator(root)) {
      TEST_ASSERT_TRUE(entry.path().filename().u8string().find(".tmp-") == std::string::npos);
    }
  }
  TEST_ASSERT_TRUE(caught > 0);
}
#endif

int main(int argc, char **argv) {
  if (argc != 2)
    return 2;
  root = fs::absolute(fs::u8path(argv[1]));
  fs::remove_all(root);
  fs::create_directories(root);
  UNITY_BEGIN();
  RUN_TEST(test_binary_files_limits_and_unchanged_results);
  RUN_TEST(test_paths_use_explicit_base_and_transactional_capacity);
  RUN_TEST(test_invalid_arguments_and_nonregular_destinations);
  RUN_TEST(test_links_permissions_and_failed_publication);
#ifndef JB_NO_ALLOCATION_FAULTS
  RUN_TEST(test_allocation_failure_preserves_outputs_and_cleans_temporary_files);
#endif
  const int result = UNITY_END();
  fs::remove_all(root);
  return result;
}
