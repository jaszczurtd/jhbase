#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif
#include "jb/allocator.h"
#include "jb/array.h"
#include "jb/file.h"
#include "jb/filesystem.h"
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

static char test_root[8192];
static char paths[8][8192];
static char output[8192];
static char cwd_before[8192];
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

/* Replace the separate archive member for allocation-failure coverage. */
const jb_allocator_t *jb_heap_allocator(void) {
  static const jb_allocator_t allocator = {NULL, test_allocate, test_release};
  return &allocator;
}

void setUp(void) { fail_allocation = 0; }
void tearDown(void) {
  fail_allocation = 0;
  TEST_ASSERT_EQUAL_size_t(0, live_allocations);
}

static const char *test_path(size_t slot, const char *relative) {
  size_t required = 0;
  TEST_ASSERT_LESS_THAN(COUNTOF(paths), slot);
  TEST_ASSERT_EQUAL(
      HAL_OK, jb_path_resolve(test_root, relative, paths[slot], sizeof(paths[slot]), &required));
  return paths[slot];
}

#ifdef _WIN32
static wchar_t native_paths[2][8192];
static const wchar_t *wide_path(size_t slot, const char *path) {
  TEST_ASSERT_LESS_THAN(COUNTOF(native_paths), slot);
  TEST_ASSERT_TRUE(MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, native_paths[slot],
                                       (int)COUNTOF(native_paths[slot])) != 0);
  return native_paths[slot];
}
#endif

static void write_bytes(const char *path, const char *bytes) {
#ifdef _WIN32
  FILE *file = _wfopen(wide_path(0, path), L"wb");
#else
  FILE *file = fopen(path, "wb");
#endif
  TEST_ASSERT_NOT_NULL(file);
  const size_t length = strlen(bytes);
  const size_t written = fwrite(bytes, 1, length, file);
  const int closed = fclose(file);
  TEST_ASSERT_EQUAL_size_t(length, written);
  TEST_ASSERT_EQUAL_INT(0, closed);
}

static void assert_kind(const char *path, bool follow, jb_fs_kind_t expected, uint64_t size) {
  jb_fs_info_t info = {0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_status(path, follow, &info));
  TEST_ASSERT_EQUAL_UINT32(expected, info.kind);
  TEST_ASSERT_EQUAL_UINT64(size, info.size);
}

static void assert_normalized(const char *path, const char *expected) {
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_normalize(path, output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING(expected, output);
  TEST_ASSERT_EQUAL_size_t(strlen(expected) + 1, required);
}

static void test_lexical_normalization_and_components(void) {
  static const struct {
    const char *path, *expected;
  } common[] = {{".", "."},    {"a/..", "."},          {"a/../.", "."},
                {"../", ".."}, {"a/../../b/..", ".."}, {"./a/../", "."}};
  for (size_t i = 0; i < COUNTOF(common); ++i)
    assert_normalized(common[i].path, common[i].expected);
#ifdef _WIN32
  assert_normalized("C:/one//two/.././three/", "C:\\one\\three\\");
  assert_normalized("C:one/../two", "C:two");
  assert_normalized("\\\\server\\share\\a\\..\\b", "\\\\server\\share\\b");
  assert_normalized("\\\\?\\C:\\one\\..\\two", "\\\\?\\C:\\two");
  assert_normalized("\\\\?\\UNC\\server\\share\\one\\..", "\\\\?\\UNC\\server\\share\\");
  assert_normalized("C:/../../one", "C:\\one");
  assert_normalized("a/b/..", "a\\");
  assert_normalized("../a", "..\\a");
#else
  assert_normalized("////", "////");
  assert_normalized("/one//two/.././three/", "/one/three/");
  assert_normalized("/../../one", "/one");
  assert_normalized("a/b/..", "a/");
  assert_normalized("../a", "../a");
  assert_normalized("back\\slash", "back\\slash");
#endif
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_filename("some/path/", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_filename("some/path/..", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("..", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_parent("one/", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("one", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_parent("one", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_filename("/", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("", output);
#ifdef _WIN32
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_parent("C://one", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("C:\\\\", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_parent("\\//one", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("\\\\\\", output);
#else
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_parent("///a", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("/", output);
#endif
}

static void test_join_and_resolve_native_roots(void) {
  size_t required = 0;
  bool absolute = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute(test_root, &absolute));
  TEST_ASSERT_TRUE(absolute);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute("one/two", &absolute));
  TEST_ASSERT_FALSE(absolute);
#ifdef _WIN32
  static const struct {
    const char *base, *path, *expected;
  } cases[] = {{"C:\\one", "\\two", "C:\\two"},
               {"C:\\one", "C:two", "C:\\one\\two"},
               {"C:\\one", "D:two", "D:two"},
               {"C:\\one", "D:\\two", "D:\\two"},
               {"C:", "one", "C:one"},
               {"\\\\server", "share", "\\\\server\\share"},
               {"\\\\server\\share\\one", "\\other", "\\\\server\\other"}};
  for (size_t i = 0; i < COUNTOF(cases); ++i) {
    TEST_ASSERT_EQUAL(
        HAL_OK, jb_fs_path_join(cases[i].base, cases[i].path, output, sizeof(output), &required));
    TEST_ASSERT_EQUAL_STRING(cases[i].expected, output);
  }
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute("C:relative", &absolute));
  TEST_ASSERT_FALSE(absolute);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute("\\relative", &absolute));
  TEST_ASSERT_FALSE(absolute);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute("\\\\server", &absolute));
  TEST_ASSERT_TRUE(absolute);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_path_resolve(test_root, "C:relative", NULL, 0, &required));
  static const char *const rejected_roots[] = {
      "\\\\server\\share",
      "\\\\server\\share\\",
      "\\\\?\\UNC\\server\\share\\",
      "\\\\?\\C:",
      "\\\\?\\C:\\",
      "\\\\?\\Volume{00000000-0000-0000-0000-000000000000}\\"};
  for (size_t i = 0; i < COUNTOF(rejected_roots); ++i)
    TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_remove(rejected_roots[i], false));
  TEST_ASSERT_EQUAL(HAL_EUNSUPPORTED,
                    jb_fs_remove("\\\\?\\GLOBALROOT\\Device\\not-a-device", false));
#else
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_fs_path_join("relative/a", "/b/../c", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("/c", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_path_resolve(test_root, "//", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("//", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_path_resolve("//", "a", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("//a", output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_path_resolve("///", "a/../b", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING("///b", output);
#endif
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_path_resolve("relative", test_root, NULL, 0, &required));
  const char *expected = test_path(0, "b/\xc5\xbc.json");
  TEST_ASSERT_EQUAL(HAL_OK, jb_path_resolve(test_root, "a/../b/\xc5\xbc.json", output,
                                            sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING(expected, output);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_cwd(output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_STRING(cwd_before, output);
}

static void test_buffers_and_invalid_inputs_are_transactional(void) {
  size_t required = 71;
  memset(output, 'Z', sizeof(output));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_normalize("abc", NULL, 0, &required));
  TEST_ASSERT_EQUAL_size_t(4, required);
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_fs_path_normalize("abc", output, 3, &required));
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', output, sizeof(output));
  union {
    size_t required;
    char bytes[32];
  } overlap;
  memset(&overlap, 'Z', sizeof(overlap));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_path_normalize("abc", overlap.bytes, sizeof(overlap.bytes),
                                                     &overlap.required));
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', &overlap, sizeof(overlap));
  memcpy(overlap.bytes, "abc", sizeof("abc"));
  TEST_ASSERT_EQUAL(HAL_EINVAL,
                    jb_fs_path_filename(overlap.bytes, output, sizeof(output), &overlap.required));
  TEST_ASSERT_EQUAL_STRING("abc", overlap.bytes);
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', output, sizeof(output));
  TEST_ASSERT_EQUAL_size_t(4, required);
  required = 71;
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_fs_path_normalize("bad\xff", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_size_t(71, required);
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', output, sizeof(output));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_path_normalize(NULL, output, sizeof(output), &required));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_path_normalize("", output, sizeof(output), &required));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_cwd(NULL, 1, &required));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_cwd(output, sizeof(output), NULL));
  jb_fs_info_t info = {JB_FS_OTHER, UINT64_C(1234)};
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_status(test_path(0, "does-not-exist"), true, &info));
  TEST_ASSERT_EQUAL_UINT32(JB_FS_OTHER, info.kind);
  TEST_ASSERT_EQUAL_UINT64(1234, info.size);
  bool same = true;
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_equivalent(test_root, paths[0], &same));
  TEST_ASSERT_TRUE(same);
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_fs_status("\xff", true, &info));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_status(test_root, true, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_list(test_root, NULL, NULL));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_realpath(paths[0], output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_size_t(71, required);
  TEST_ASSERT_EACH_EQUAL_UINT8('Z', output, sizeof(output));
}

static void test_allocation_failure_preserves_outputs_and_releases_owners(void) {
  bool succeeded = false;
  for (size_t failure = 1; failure < 64; ++failure) {
    memset(output, 'Z', sizeof(output));
    size_t required = 313;
    fail_allocation = failure;
    const hal_status_t status =
        jb_path_resolve(test_root, "one/two/../three/file", output, sizeof(output), &required);
    fail_allocation = 0;
    TEST_ASSERT_EQUAL_size_t(0, live_allocations);
    if (status == HAL_OK) {
      succeeded = true;
      break;
    }
    TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
    TEST_ASSERT_EQUAL_size_t(313, required);
    TEST_ASSERT_EACH_EQUAL_UINT8('Z', output, sizeof(output));
  }
  TEST_ASSERT_TRUE(succeeded);
  const char *directory = test_path(0, "oom-remove");
  const char *nested = test_path(1, "oom-remove/a/b");
  const char *file = test_path(2, "oom-remove/a/b/file");
  succeeded = false;
  for (size_t failure = 1; failure < 128; ++failure) {
    TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(nested, true));
    write_bytes(file, "owned");
    fail_allocation = failure;
    const hal_status_t status = jb_fs_remove(directory, true);
    fail_allocation = 0;
    TEST_ASSERT_EQUAL_size_t(0, live_allocations);
    if (status == HAL_OK) {
      succeeded = true;
      break;
    }
    TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
  }
  TEST_ASSERT_TRUE(succeeded);
}

typedef struct {
  size_t files, directories, links, calls;
  bool unicode;
  hal_status_t stop;
} entry_counts_t;

static hal_status_t collect_entry(void *context, const char *name, const jb_fs_info_t *info) {
  entry_counts_t *counts = context;
  ++counts->calls;
  if (info->kind == JB_FS_REGULAR)
    ++counts->files;
  else if (info->kind == JB_FS_DIRECTORY)
    ++counts->directories;
  else if (info->kind == JB_FS_LINK)
    ++counts->links;
  if (strcmp(name, "\xc5\xbc\xf0\x9f\xa6\x8e.txt") == 0)
    counts->unicode = true;
  return counts->stop;
}

static void test_directory_listing_unicode_and_callback_error(void) {
  const char *directory = test_path(0, "listing");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(test_path(1, "listing/nested/deep"), true));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(directory, false));
  const char *file = test_path(2, "listing/\xc5\xbc\xf0\x9f\xa6\x8e.txt");
  write_bytes(file, "abc");
  assert_kind(file, true, JB_FS_REGULAR, 3);
  assert_kind(directory, true, JB_FS_DIRECTORY, 0);
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_fs_mkdir(file, false));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_mkdir(test_path(3, "missing-parent/child"), false));
  entry_counts_t counts = {0, 0, 0, 0, false, HAL_OK};
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_list(directory, collect_entry, &counts));
  TEST_ASSERT_EQUAL_size_t(1, counts.files);
  TEST_ASSERT_EQUAL_size_t(1, counts.directories);
  TEST_ASSERT_EQUAL_size_t(2, counts.calls);
  TEST_ASSERT_TRUE(counts.unicode);
  counts.calls = 0;
  counts.stop = HAL_ECANCELED;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_fs_list(directory, collect_entry, &counts));
  TEST_ASSERT_EQUAL_size_t(1, counts.calls);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_fs_list(file, collect_entry, &counts));
  TEST_ASSERT_EQUAL(HAL_EBUSY, jb_fs_remove(directory, false));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(directory, true));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_remove(directory, true));
}

static void test_identity_canonical_and_rename(void) {
  const char *source = test_path(0, "source.txt");
  const char *hardlink = test_path(1, "hard.txt");
  const char *destination = test_path(2, "destination.txt");
  write_bytes(source, "original");
#ifdef _WIN32
  TEST_ASSERT_TRUE(CreateHardLinkW(wide_path(0, hardlink), wide_path(1, source), NULL));
#else
  TEST_ASSERT_EQUAL_INT(0, link(source, hardlink));
#endif
  bool same = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_equivalent(source, hardlink, &same));
  TEST_ASSERT_TRUE(same);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_rename(source, hardlink));
  assert_kind(source, false, JB_FS_REGULAR, 8);
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_realpath(source, output, sizeof(output), &required));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_equivalent(output, source, &same));
  TEST_ASSERT_TRUE(same);
  bool absolute = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_is_absolute(output, &absolute));
  TEST_ASSERT_TRUE(absolute);
  write_bytes(destination, "old");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_rename(source, destination));
  assert_kind(destination, true, JB_FS_REGULAR, 8);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_equivalent(destination, hardlink, &same));
  TEST_ASSERT_TRUE(same);
  jb_fs_info_t info;
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_status(source, true, &info));
  const char *directory = test_path(3, "rename-directory");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(directory, false));
  TEST_ASSERT_EQUAL(HAL_EEXIST, jb_fs_rename(destination, directory));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_fs_rename(directory, destination));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_rename(directory, test_path(4, "renamed-directory")));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_rename(paths[4], paths[4]));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(destination, false));
  assert_kind(hardlink, true, JB_FS_REGULAR, 8);
}

static bool create_directory_link(const char *path, const char *target) {
#ifdef _WIN32
  const wchar_t *link_name = wide_path(0, path), *destination = wide_path(1, target);
  if (CreateSymbolicLinkW(link_name, destination, SYMBOLIC_LINK_FLAG_DIRECTORY | 0x2u))
    return true;
  if (GetLastError() == ERROR_INVALID_PARAMETER &&
      CreateSymbolicLinkW(link_name, destination, SYMBOLIC_LINK_FLAG_DIRECTORY))
    return true;
  if (GetLastError() == ERROR_PRIVILEGE_NOT_HELD)
    return false;
  TEST_FAIL_MESSAGE("CreateSymbolicLinkW failed for a reason other than unavailable privilege");
#else
  TEST_ASSERT_EQUAL_INT(0, symlink(target, path));
  return true;
#endif
}

static void test_link_status_and_removal_do_not_follow_targets(void) {
  const char *kept = test_path(0, "kept");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(kept, false));
  const char *file = test_path(1, "kept/payload");
  write_bytes(file, "keep");
  const char *container = test_path(2, "links");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(container, false));
  const char *link_name = test_path(3, "links/link");
  if (!create_directory_link(link_name, kept))
    TEST_IGNORE_MESSAGE("Windows symbolic-link privilege/developer mode is unavailable");
  assert_kind(link_name, false, JB_FS_LINK, 0);
  assert_kind(link_name, true, JB_FS_DIRECTORY, 0);
  bool same = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_equivalent(link_name, kept, &same));
  TEST_ASSERT_TRUE(same);
  const int written = snprintf(output, sizeof(output), "%s/", link_name);
  TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(output));
  assert_kind(output, false, JB_FS_LINK, 0);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_fs_rename(kept, output));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(output, true));
  assert_kind(file, true, JB_FS_REGULAR, 4);
  TEST_ASSERT_TRUE(create_directory_link(link_name, kept));
  entry_counts_t counts = {0, 0, 0, 0, false, HAL_OK};
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_list(container, collect_entry, &counts));
  TEST_ASSERT_EQUAL_size_t(1, counts.links);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(container, true));
  assert_kind(file, true, JB_FS_REGULAR, 4);
  const char *dangling = test_path(4, "dangling");
  TEST_ASSERT_TRUE(create_directory_link(dangling, test_path(5, "absent-target")));
  assert_kind(dangling, false, JB_FS_LINK, 0);
  jb_fs_info_t info;
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_fs_status(dangling, true, &info));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(dangling, true));
}

#ifdef _WIN32
static void test_windows_remove_aliases_and_extended_root_metadata(void) {
  const char *directory = test_path(0, "alias-guard");
  const char *payload = test_path(1, "alias-guard/payload");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(directory, false));
  write_bytes(payload, "keep");
  static const char *const aliases[] = {". ", ".. ", " ", "...", "... "};
  for (size_t i = 0; i < COUNTOF(aliases); ++i) {
    const int written = snprintf(output, sizeof(output), "%s\\%s", directory, aliases[i]);
    TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(output));
    /* A failed guard can only attempt rmdir on our nonempty fixture. */
    TEST_ASSERT_EQUAL(HAL_EINVAL, jb_fs_remove(output, false));
    assert_kind(payload, true, JB_FS_REGULAR, 4);
  }
  if (strlen(cwd_before) >= 3 && cwd_before[1] == ':') {
    char extended_root[] = "\\\\?\\C:\\";
    extended_root[4] = cwd_before[0];
    assert_kind(extended_root, false, JB_FS_DIRECTORY, 0);
    extended_root[2] = '.';
    assert_kind(extended_root, false, JB_FS_DIRECTORY, 0);
  }
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_realpath(directory, output, sizeof(output), &required));
  static const char *const streams[] = {"::$INDEX_ALLOCATION", ":$I30:$INDEX_ALLOCATION",
                                        ":stream"};
  for (size_t i = 0; i < COUNTOF(streams); ++i) {
    for (unsigned extended = 0; extended < 2; ++extended) {
      const int length = snprintf(paths[3], sizeof(paths[3]), "%s%s",
                                  extended != 0 ? output : directory, streams[i]);
      TEST_ASSERT_TRUE(length > 0 && (size_t)length < sizeof(paths[3]));
      TEST_ASSERT_EQUAL(HAL_EUNSUPPORTED, jb_fs_remove(paths[3], false));
    }
  }
  const int written = snprintf(paths[2], sizeof(paths[2]), "%s\\...", output);
  TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(paths[2]));
  TEST_ASSERT_TRUE(CreateDirectoryW(wide_path(0, paths[2]), NULL));
  assert_kind(paths[2], false, JB_FS_DIRECTORY, 0);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(paths[2], true));
  assert_kind(payload, true, JB_FS_REGULAR, 4);
}
#endif

#ifndef _WIN32
static void test_non_utf8_directory_entry_is_reported(void) {
  const char *directory = test_path(0, "invalid-entry");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(directory, false));
  const int written = snprintf(output, sizeof(output), "%s/invalid\xff", directory);
  TEST_ASSERT_TRUE(written > 0 && (size_t)written < sizeof(output));
  write_bytes(output, "x");
  entry_counts_t counts = {0, 0, 0, 0, false, HAL_OK};
  const hal_status_t listed = jb_fs_list(directory, collect_entry, &counts);
  TEST_ASSERT_EQUAL(HAL_EPROTO, listed);
  TEST_ASSERT_EQUAL_size_t(0, counts.calls);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(directory, true));
}
#endif

#ifndef _WIN32
static bool replace_directory;
static const char *race_source, *race_saved, *race_target;

static void replace_before_open(const char *path) {
  const char *name = strrchr(path, '/');
  name = name == NULL ? path : name + 1;
  if (replace_directory && strcmp(name, "race-child") == 0) {
    replace_directory = false;
    if (rename(race_source, race_saved) != 0 || symlink(race_target, race_source) != 0)
      abort();
  }
}

extern int __real_openat(int directory, const char *path, int flags, ...);
int __wrap_openat(int directory, const char *path, int flags, ...) {
  mode_t mode = 0;
  if ((flags & O_CREAT) != 0) {
    va_list args;
    va_start(args, flags);
    mode = (mode_t)va_arg(args, int);
    va_end(args);
  }
  replace_before_open(path);
  return __real_openat(directory, path, flags, mode);
}

extern DIR *__real_opendir(const char *path);
DIR *__wrap_opendir(const char *path) {
  replace_before_open(path);
  return __real_opendir(path);
}

static void test_remove_does_not_follow_replaced_directory(void) {
  const char *tree = test_path(0, "race-tree");
  race_source = test_path(1, "race-tree/race-child");
  race_saved = test_path(2, "race-tree/saved");
  race_target = test_path(3, "race-outside");
  const char *precious = test_path(4, "race-outside/precious");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(race_source, true));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(race_target, false));
  write_bytes(precious, "keep");
  replace_directory = true;
  const hal_status_t removed = jb_fs_remove(tree, true);
  TEST_ASSERT_FALSE(replace_directory);
  assert_kind(precious, false, JB_FS_REGULAR, 4);
  TEST_ASSERT_TRUE(removed == HAL_OK || removed == HAL_EBUSY);
}

static void test_remove_deep_tree_without_full_child_paths(void) {
  const char *tree = test_path(0, "deep-tree");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(tree, false));
  int directory = open(tree, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  TEST_ASSERT_TRUE(directory >= 0);
  /* Exceed PATH_MAX with few open descriptors and bounded fixture creation. */
  static const char segment[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (unsigned i = 0; i < 128; ++i) {
    TEST_ASSERT_EQUAL_INT(0, mkdirat(directory, segment, 0700));
    const int next = openat(directory, segment, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    TEST_ASSERT_TRUE(next >= 0);
    TEST_ASSERT_EQUAL_INT(0, close(directory));
    directory = next;
  }
  const int file = openat(directory, "payload", O_WRONLY | O_CREAT | O_EXCL, 0600);
  TEST_ASSERT_TRUE(file >= 0);
  TEST_ASSERT_EQUAL_INT(0, close(file));
  TEST_ASSERT_EQUAL_INT(0, close(directory));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(tree, true));
  const char *restricted = test_path(1, "restricted-parent");
  const char *empty = test_path(2, "restricted-parent/empty");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(empty, true));
  TEST_ASSERT_EQUAL_INT(0, chmod(empty, 0000));
  TEST_ASSERT_EQUAL_INT(0, chmod(restricted, 0300));
  const hal_status_t removed = jb_fs_remove(empty, false);
  TEST_ASSERT_EQUAL_INT(0, chmod(restricted, 0700));
  TEST_ASSERT_EQUAL(HAL_OK, removed);
}
#endif

static void test_regular_file_trailing_separator_and_path_overflow(void) {
  const char *file = test_path(0, "trailing-file");
  write_bytes(file, "keep");
  TEST_ASSERT_TRUE(snprintf(output, sizeof(output), "%s/", file) > 0);
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_fs_remove(output, true));
  assert_kind(file, false, JB_FS_REGULAR, 4);
  static char too_long[40000];
  memset(too_long, 'a', sizeof(too_long) - 1);
  size_t required = 777;
  memcpy(output, "untouched", sizeof("untouched"));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_fs_realpath(too_long, output, sizeof(output), &required));
  TEST_ASSERT_EQUAL_size_t(0, required);
  TEST_ASSERT_EQUAL_STRING("untouched", output);
}

#ifdef _WIN32
static void create_junction(const char *path, const char *target) {
  static struct {
    DWORD tag;
    WORD bytes, reserved, substitute_offset, substitute_bytes, print_offset, print_bytes;
    wchar_t names[8192];
  } data;
  memset(&data, 0, sizeof(data));
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_realpath(target, output, sizeof(output), &required));
  const wchar_t *wide = wide_path(0, output);
  const size_t count = wcslen(wide);
  TEST_ASSERT_TRUE(count + 2 < COUNTOF(data.names));
  TEST_ASSERT_EQUAL_MEMORY(L"\\\\?\\", wide, 4 * sizeof(wchar_t));
  memcpy(data.names, wide, (count + 1) * sizeof(wchar_t));
  data.names[1] = L'?';
  data.names[2] = L'?';
  data.tag = IO_REPARSE_TAG_MOUNT_POINT;
  data.substitute_bytes = (WORD)(count * sizeof(wchar_t));
  data.print_offset = (WORD)((count + 1) * sizeof(wchar_t));
  data.bytes = (WORD)(8 + (count + 2) * sizeof(wchar_t));
  wide = wide_path(0, path);
  TEST_ASSERT_TRUE(CreateDirectoryW(wide, NULL));
  HANDLE directory = CreateFileW(wide, GENERIC_WRITE, 0, NULL, OPEN_EXISTING,
                                 FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_BACKUP_SEMANTICS, NULL);
  TEST_ASSERT_TRUE(directory != INVALID_HANDLE_VALUE);
  DWORD returned = 0;
  const BOOL created = DeviceIoControl(directory, FSCTL_SET_REPARSE_POINT, &data,
                                       (DWORD)data.bytes + 8, NULL, 0, &returned, NULL);
  const DWORD error = created ? ERROR_SUCCESS : GetLastError();
  TEST_ASSERT_TRUE(CloseHandle(directory));
  TEST_ASSERT_EQUAL_UINT32(ERROR_SUCCESS, error);
}

static void test_windows_junction_literal_names_preserve_outside_tree(void) {
  const char *outside = test_path(0, "junction-outside");
  const char *precious = test_path(1, "junction-outside/precious.txt");
  const char *nested = test_path(2, "junction-outside/sub");
  const char *deep = test_path(3, "junction-outside/sub/deep.txt");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(nested, true));
  write_bytes(precious, "keep");
  write_bytes(deep, "keep");
  static const char *const names[] = {"x. ", "x.", "x ", "junction"};
  for (size_t i = 0; i < COUNTOF(names); ++i) {
    const char *tree = test_path(4, "junction-tree");
    const char *ordinary = test_path(5, "junction-tree/x");
    TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(ordinary, true));
    size_t required = 0;
    TEST_ASSERT_EQUAL(HAL_OK, jb_fs_realpath(tree, output, sizeof(output), &required));
    const int count = snprintf(paths[6], sizeof(paths[6]), "%s\\%s", output, names[i]);
    TEST_ASSERT_TRUE(count > 0 && (size_t)count < sizeof(paths[6]));
    create_junction(paths[6], outside);
    TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(tree, true));
    assert_kind(precious, false, JB_FS_REGULAR, 4);
    assert_kind(deep, false, JB_FS_REGULAR, 4);
  }
}

static void test_windows_readonly_and_case_only_rename(void) {
  const char *directory = test_path(0, "readonly-tree");
  const char *file = test_path(1, "readonly-tree/File.txt");
  const char *renamed = test_path(2, "readonly-tree/file.txt");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(directory, false));
  write_bytes(file, "keep");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_rename(file, renamed));
  WIN32_FIND_DATAW info;
  HANDLE search = FindFirstFileW(wide_path(0, renamed), &info);
  TEST_ASSERT_TRUE(search != INVALID_HANDLE_VALUE);
  const bool correct_case = wcscmp(info.cFileName, L"file.txt") == 0;
  TEST_ASSERT_TRUE(FindClose(search));
  TEST_ASSERT_TRUE(correct_case);
  TEST_ASSERT_TRUE(SetFileAttributesW(wide_path(0, renamed), FILE_ATTRIBUTE_READONLY));
  HANDLE writer = CreateFileW(wide_path(0, directory), GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                              OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  TEST_ASSERT_TRUE(writer != INVALID_HANDLE_VALUE);
  const hal_status_t busy = jb_fs_remove(directory, true);
  TEST_ASSERT_TRUE(CloseHandle(writer));
  TEST_ASSERT_EQUAL(HAL_EBUSY, busy);
  assert_kind(renamed, false, JB_FS_REGULAR, 4);
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(directory, true));
}
#endif

int main(int argc, char **argv) {
  size_t required = 0;
  if (jb_fs_cwd(cwd_before, sizeof(cwd_before), &required) != HAL_OK) {
    fprintf(stderr, "Cannot read filesystem test working directory\n");
    return 2;
  }
#ifdef _WIN32
  const unsigned long process = GetCurrentProcessId();
#else
  const unsigned long process = (unsigned long)getpid();
#endif
  bool created = false;
  for (unsigned attempt = 0; attempt < 100 && !created; ++attempt) {
    const int count = snprintf(output, sizeof(output), "%s-%lu-%u",
                               argc > 1 ? argv[1] : "filesystem-check", process, attempt);
    if (count < 0 || (size_t)count >= sizeof(output) ||
        jb_path_resolve(cwd_before, output, test_root, sizeof(test_root), &required) != HAL_OK)
      break;
    jb_fs_info_t info;
    const hal_status_t observed = jb_fs_status(test_root, false, &info);
    if (observed == HAL_OK)
      continue;
    if (observed != HAL_ENOENT || jb_fs_mkdir(test_root, true) != HAL_OK)
      break;
    created = true;
  }
  if (!created) {
    fprintf(stderr, "Cannot create a fresh filesystem fixture below %s\n", cwd_before);
    return 2;
  }
  UNITY_BEGIN();
  RUN_TEST(test_lexical_normalization_and_components);
  RUN_TEST(test_join_and_resolve_native_roots);
  RUN_TEST(test_buffers_and_invalid_inputs_are_transactional);
  RUN_TEST(test_allocation_failure_preserves_outputs_and_releases_owners);
  RUN_TEST(test_directory_listing_unicode_and_callback_error);
  RUN_TEST(test_identity_canonical_and_rename);
  RUN_TEST(test_link_status_and_removal_do_not_follow_targets);
  RUN_TEST(test_regular_file_trailing_separator_and_path_overflow);
#ifdef _WIN32
  RUN_TEST(test_windows_remove_aliases_and_extended_root_metadata);
  RUN_TEST(test_windows_junction_literal_names_preserve_outside_tree);
  RUN_TEST(test_windows_readonly_and_case_only_rename);
#endif
#ifndef _WIN32
  RUN_TEST(test_non_utf8_directory_entry_is_reported);
  RUN_TEST(test_remove_does_not_follow_replaced_directory);
  RUN_TEST(test_remove_deep_tree_without_full_child_paths);
#endif
  const int result = UNITY_END();
  const hal_status_t cleanup = jb_fs_remove(test_root, true);
  return result != 0 || cleanup != HAL_OK ? 1 : 0;
}
