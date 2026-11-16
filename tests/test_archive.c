#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif
#include "jb/allocator.h"
#include "jb/archive.h"
#include "jb/array.h"
#include "jb/filesystem.h"
#include "jb/utf8.h"
#include "test_file.h"
#include "test_heap.h"
#include "unity.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define ZLIB_CONST
#include <zlib.h>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#else
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

/* Writer-only formats: uncompressed TAR as a wrong input, ustar for raw names. */
enum { TEST_TAR = 99, TEST_USTAR_GZIP = 98 };

static char test_root[8192];
static char case_root[8192];
static char archive_path[8192];
static char destination[8192];
static char paths[4][8192];
static unsigned char fixture[1u << 20];
static size_t fixture_size;
static bool symlinks_supported = true;
#ifndef _WIN32
static locale_t writer_locale, caller_locale;
#endif

#ifdef __linux__
ssize_t __real_write(int descriptor, const void *data, size_t size);
static int fail_write_errno;
static size_t partial_write; /* bytes the first write still stores before the failures */
ssize_t __wrap_write(int descriptor, const void *data, size_t size);
ssize_t __wrap_write(int descriptor, const void *data, size_t size) {
  if (fail_write_errno != 0 && partial_write != 0) {
    const size_t count = size < partial_write ? size : partial_write;
    partial_write = 0;
    return __real_write(descriptor, data, count);
  }
  if (fail_write_errno != 0) {
    errno = fail_write_errno;
    return -1;
  }
  return __real_write(descriptor, data, size);
}
#endif

static const char *join(char *out, size_t capacity, const char *base, const char *relative) {
  size_t required = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_path_join(base, relative, out, capacity, &required));
  return out;
}

static const char *in_destination(size_t slot, const char *relative) {
  TEST_ASSERT_LESS_THAN(COUNTOF(paths), slot);
  return join(paths[slot], sizeof(paths[slot]), destination, relative);
}

/* A fresh case directory with an empty destination and a sibling "outside". */
static void begin_case(const char *name) {
  join(case_root, sizeof(case_root), test_root, name);
  jb_fs_info_t info;
  if (jb_fs_status(case_root, false, &info) == HAL_OK)
    TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(case_root, true));
  join(destination, sizeof(destination), case_root, "destination");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(destination, true));
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_fs_mkdir(join(paths[0], sizeof(paths[0]), case_root, "outside"), false));
  join(archive_path, sizeof(archive_path), case_root, "fixture.bin");
}

static void reset_destination(void) {
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_remove(destination, true));
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(destination, false));
}

static hal_status_t count_entry(void *context, const char *name, const jb_fs_info_t *info) {
  (void)name;
  (void)info;
  ++*(size_t *)context;
  return HAL_OK;
}

static size_t count_children(const char *path) {
  size_t count = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_list(path, count_entry, &count));
  return count;
}

static void expect_outside_untouched(void) {
  TEST_ASSERT_EQUAL_size_t(0,
                           count_children(join(paths[3], sizeof(paths[3]), case_root, "outside")));
  TEST_ASSERT_EQUAL_size_t(3, count_children(case_root)); /* destination, outside, fixture */
}

static jb_fs_kind_t kind_of(const char *relative) {
  jb_fs_info_t info;
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, jb_fs_status(in_destination(2, relative), false, &info),
                            relative);
  return info.kind;
}

static bool exists(const char *relative) {
  jb_fs_info_t info;
  return jb_fs_status(in_destination(2, relative), false, &info) == HAL_OK;
}

/* Windows CRT narrow paths use the ANSI code page; open UTF-8 names as UTF-16. */
static FILE *open_utf8(const char *path) {
#ifdef _WIN32
  wchar_t wide[8192];
  FILE *file = NULL;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, (int)COUNTOF(wide)) == 0)
    return NULL;
  return _wfopen_s(&file, wide, L"rb") == 0 ? file : NULL;
#else
  return jb_test_file_open(path, "rb");
#endif
}

static void expect_content(const char *relative, const char *expected) {
  FILE *file = open_utf8(in_destination(1, relative));
  TEST_ASSERT_NOT_NULL_MESSAGE(file, relative);
  char buffer[256];
  const size_t length = fread(buffer, 1, sizeof(buffer), file);
  TEST_ASSERT_EQUAL_INT(0, fclose(file));
  TEST_ASSERT_EQUAL_size_t(strlen(expected), length);
  TEST_ASSERT_EQUAL_MEMORY(expected, buffer, length);
}

/* Linux writers run in C.UTF-8 to store UTF-8 names; extraction runs in the
 * process locale ("C"), like a caller that never calls setlocale. */
static struct archive *writer_begin(jb_archive_format_t format) {
#ifndef _WIN32
  caller_locale = uselocale(writer_locale);
  TEST_ASSERT_TRUE(caller_locale != (locale_t)0);
#endif
  struct archive *writer = archive_write_new();
  TEST_ASSERT_NOT_NULL(writer);
  if (format == JB_ARCHIVE_ZIP) {
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_set_format_zip(writer));
  } else {
    TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, format == TEST_USTAR_GZIP
                                          ? archive_write_set_format_ustar(writer)
                                          : archive_write_set_format_pax_restricted(writer));
    if (format != TEST_TAR)
      TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_add_filter_gzip(writer));
  }
  TEST_ASSERT_EQUAL_INT(ARCHIVE_OK,
                        archive_write_open_memory(writer, fixture, sizeof(fixture), &fixture_size));
  return writer;
}

static void writer_entry(struct archive *writer, const char *name, unsigned type, unsigned perm,
                         const void *data, size_t size, const char *link, bool hard) {
  struct archive_entry *entry = archive_entry_new();
  TEST_ASSERT_NOT_NULL(entry);
  /* Invalid UTF-8 is written raw on purpose. Valid names go through UTF-16 on
   * Windows: update_pathname_utf8 also converts to the ANSI code page there
   * and fails for characters outside it. */
  size_t invalid = 0;
  const bool raw = jb_utf8_validate(name, strlen(name), &invalid) != HAL_OK;
  if (raw) {
    archive_entry_copy_pathname(entry, name);
  } else {
#ifdef _WIN32
    wchar_t wide[1024];
    TEST_ASSERT_NOT_EQUAL_INT(
        0, MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wide, (int)COUNTOF(wide)));
    archive_entry_copy_pathname_w(entry, wide);
#else
    TEST_ASSERT_NOT_EQUAL_INT(0, archive_entry_update_pathname_utf8(entry, name));
#endif
  }
  archive_entry_set_filetype(entry, type);
  archive_entry_set_perm(entry, (unsigned short)perm);
  archive_entry_set_size(entry, (la_int64_t)size);
  if (link != NULL && hard)
    TEST_ASSERT_NOT_EQUAL_INT(0, archive_entry_update_hardlink_utf8(entry, link));
  else if (link != NULL)
    TEST_ASSERT_NOT_EQUAL_INT(0, archive_entry_update_symlink_utf8(entry, link));
  /* pax warns about a raw name and still writes it */
  const int written = archive_write_header(writer, entry);
  TEST_ASSERT_TRUE_MESSAGE(written == ARCHIVE_OK || (raw && written == ARCHIVE_WARN),
                           archive_error_string(writer));
  if (size > 0)
    TEST_ASSERT_EQUAL_INT64((la_int64_t)size, archive_write_data(writer, data, size));
  archive_entry_free(entry);
}

static void add_file(struct archive *writer, const char *name, unsigned perm, const char *text) {
  writer_entry(writer, name, AE_IFREG, perm, text, strlen(text), NULL, false);
}

static void add_directory(struct archive *writer, const char *name) {
  writer_entry(writer, name, AE_IFDIR, 0755, NULL, 0, NULL, false);
}

static void add_symlink(struct archive *writer, const char *name, const char *target) {
  writer_entry(writer, name, AE_IFLNK, 0777, NULL, 0, target, false);
}

static void add_hardlink(struct archive *writer, const char *name, const char *target) {
  writer_entry(writer, name, AE_IFREG, 0644, NULL, 0, target, true);
}

static void save_fixture(void) {
  FILE *file = jb_test_file_open(archive_path, "wb");
  TEST_ASSERT_NOT_NULL(file);
  TEST_ASSERT_EQUAL_size_t(fixture_size, fwrite(fixture, 1, fixture_size, file));
  TEST_ASSERT_EQUAL_INT(0, fclose(file));
}

static void writer_close(struct archive *writer) {
  TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_close(writer));
  TEST_ASSERT_EQUAL_INT(ARCHIVE_OK, archive_write_free(writer));
#ifndef _WIN32
  uselocale(caller_locale);
#endif
}

static void writer_end(struct archive *writer) {
  writer_close(writer);
  save_fixture();
}

static jb_archive_options_t default_options(jb_archive_format_t format) {
  jb_archive_options_t options;
  memset(&options, 0, sizeof(options));
  options.format = format;
  options.max_entries = 1000;
  options.max_total_bytes = 1u << 20;
  options.max_file_bytes = 1u << 20;
  return options;
}

static hal_status_t extract_with(const jb_archive_options_t *options, jb_archive_result_t *result) {
  return jb_archive_extract(archive_path, destination, options, result);
}

static hal_status_t extract(jb_archive_format_t format, jb_archive_result_t *result) {
  const jb_archive_options_t options = default_options(format);
  return extract_with(&options, result);
}

/* One entry, then the extraction status; result keeps the diagnostics. */
static hal_status_t single_entry(jb_archive_format_t format, const char *name, unsigned type,
                                 const char *link, jb_archive_result_t *result) {
  reset_destination();
  struct archive *writer = writer_begin(format);
  if (type == AE_IFREG)
    add_file(writer, name, 0644, "x");
  else
    writer_entry(writer, name, type, 0755, NULL, 0, link, false);
  writer_end(writer);
  return extract(format, result);
}

/* The diagnostics carry the name stored in the archive. The Windows writer
 * stores backslashes as '/', so either spelling names the same entry. */
static void expect_entry(const char *written, const char *reported) {
  char stored[256];
  const size_t length = strlen(written);
  TEST_ASSERT_LESS_THAN_size_t(sizeof(stored), length);
  for (size_t index = 0; index <= length; ++index)
    stored[index] = written[index] == '\\' ? '/' : written[index];
  if (strcmp(written, reported) != 0)
    TEST_ASSERT_EQUAL_STRING(stored, reported);
}

static void expect_failure(hal_status_t expected, hal_status_t actual,
                           const jb_archive_result_t *result) {
  TEST_ASSERT_EQUAL_MESSAGE(expected, actual, result->message);
  TEST_ASSERT_TRUE_MESSAGE(result->message[0] != '\0', "failure has a message");
}

static void expect_success(hal_status_t actual, const jb_archive_result_t *result) {
  TEST_ASSERT_EQUAL_MESSAGE(HAL_OK, actual, result->message);
  TEST_ASSERT_EQUAL_STRING("", result->entry);
  TEST_ASSERT_EQUAL_STRING("", result->message);
  TEST_ASSERT_EQUAL_UINT32(0, result->native_error);
}

#ifndef _WIN32
static unsigned permissions(const char *relative) {
  struct stat metadata;
  TEST_ASSERT_EQUAL_INT(0, lstat(in_destination(1, relative), &metadata));
  return (unsigned)metadata.st_mode & 0777u;
}

static void expect_link_target(const char *relative, const char *target) {
  char buffer[256];
  const ssize_t length = readlink(in_destination(1, relative), buffer, sizeof(buffer) - 1);
  TEST_ASSERT_GREATER_THAN(0, length);
  buffer[length] = '\0';
  TEST_ASSERT_EQUAL_STRING(target, buffer);
}
#else
static bool directory_link(const char *relative) {
  wchar_t wide[8192];
  TEST_ASSERT_NOT_EQUAL_INT(0, MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                                   in_destination(1, relative), -1, wide,
                                                   (int)COUNTOF(wide)));
  const DWORD attributes = GetFileAttributesW(wide);
  TEST_ASSERT_NOT_EQUAL_UINT32(INVALID_FILE_ATTRIBUTES, attributes);
  TEST_ASSERT_TRUE((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0);
  return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
#endif

/* Symbolic links fail with HAL_EPERM where Windows does not allow them. */
static bool links_expected(hal_status_t status, const jb_archive_result_t *result) {
  if (symlinks_supported)
    return true;
  TEST_ASSERT_EQUAL_MESSAGE(HAL_EPERM, status, result->message);
  TEST_MESSAGE("symbolic links unavailable: checked the HAL_EPERM path instead");
  return false;
}

static void test_tar_gzip_tree_links_and_modes(void) {
  begin_case("tar-tree");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_directory(writer, "./");
  add_directory(writer, "pkg/");
  add_file(writer, "pkg/bin/tool", 0755, "#!tool\n");
  add_file(writer, "pkg/readme.txt", 0600, "readme\n");
  add_file(writer, "pkg//deep/./name.txt", 0644, "deep\n");
  add_file(writer, "pkg\\win\\file.txt", 0644, "windows separators\n");
  add_file(writer, "pkg/za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.txt", 0644, "utf8\n");
  add_directory(writer, "pkg/bin/");
  add_hardlink(writer, "pkg/hard.txt", "pkg/readme.txt");
  add_symlink(writer, "pkg/lib/current", "../bin");
  add_symlink(writer, "pkg/lib/self", ".");
  add_symlink(writer, "pkg/tool-link", "bin\\tool");
  add_symlink(writer, "pkg/chain", "lib/current");
  writer_end(writer);
#ifndef _WIN32
  const locale_t before = uselocale((locale_t)0);
#endif
  jb_archive_result_t result;
  const hal_status_t status = extract(JB_ARCHIVE_TAR_GZIP, &result);
#ifndef _WIN32
  TEST_ASSERT_TRUE(before == uselocale((locale_t)0));
#endif
  if (!links_expected(status, &result))
    return;
  expect_success(status, &result);
  TEST_ASSERT_EQUAL_UINT64(13, result.entries);
  TEST_ASSERT_EQUAL_UINT64(7 + 7 + 5 + 19 + 5, result.bytes);
  expect_content("pkg/bin/tool", "#!tool\n");
  expect_content("pkg/readme.txt", "readme\n");
  expect_content("pkg/hard.txt", "readme\n");
  expect_content("pkg/deep/name.txt", "deep\n");
  expect_content("pkg/win/file.txt", "windows separators\n");
  expect_content("pkg/za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87.txt", "utf8\n");
  TEST_ASSERT_EQUAL_UINT32(JB_FS_DIRECTORY, kind_of("pkg/bin"));
  TEST_ASSERT_EQUAL_UINT32(JB_FS_LINK, kind_of("pkg/lib/current"));
  TEST_ASSERT_EQUAL_UINT32(JB_FS_LINK, kind_of("pkg/tool-link"));
  expect_content("pkg/lib/current/tool", "#!tool\n");
  expect_content("pkg/chain/tool", "#!tool\n");
  expect_content("pkg/tool-link", "#!tool\n");
  expect_content("pkg/lib/self/current/tool", "#!tool\n");
#ifndef _WIN32
  const mode_t mask = umask(022);
  umask(mask);
  TEST_ASSERT_EQUAL_HEX32(0777u & ~(unsigned)mask, permissions("pkg/bin/tool"));
  TEST_ASSERT_EQUAL_HEX32(0666u & ~(unsigned)mask, permissions("pkg/readme.txt"));
  expect_link_target("pkg/lib/current", "../bin");
  expect_link_target("pkg/tool-link", "bin/tool");
  expect_link_target("pkg/lib/self", ".");
  struct stat first, second;
  TEST_ASSERT_EQUAL_INT(0, stat(in_destination(1, "pkg/readme.txt"), &first));
  TEST_ASSERT_EQUAL_INT(0, stat(in_destination(1, "pkg/hard.txt"), &second));
  TEST_ASSERT_TRUE(first.st_ino == second.st_ino);
#else
  TEST_ASSERT_TRUE(directory_link("pkg/lib/current"));
  TEST_ASSERT_TRUE(directory_link("pkg/chain"));
  TEST_ASSERT_TRUE(directory_link("pkg/lib/self"));
  TEST_ASSERT_FALSE(directory_link("pkg/tool-link"));
#endif
  expect_outside_untouched();
}

static void test_zip_tree_exclusions_and_execute_bits(void) {
  begin_case("zip-tree");
  struct archive *writer = writer_begin(JB_ARCHIVE_ZIP);
  add_directory(writer, "docs/");
  add_file(writer, "docs/a.txt", 0644, "alpha\n");
  add_file(writer, "bin/run", 0755, "run\n");
  add_file(writer, "skip\\me.txt", 0644, "excluded\n");
  add_file(writer, "skip/other.txt", 0644, "kept\n");
  add_file(writer, "[Content_Types].xml", 0644, "<Types/>\n");
  writer_end(writer);
  const char *const exclusions[] = {"skip/me.txt", "[Content_Types].xml", NULL};
  jb_archive_options_t options = default_options(JB_ARCHIVE_ZIP);
  options.exclusions = exclusions;
  jb_archive_result_t result;
  expect_success(extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_UINT64(6, result.entries);
  TEST_ASSERT_EQUAL_UINT64(6 + 4 + 5, result.bytes);
  expect_content("docs/a.txt", "alpha\n");
  expect_content("bin/run", "run\n");
  expect_content("skip/other.txt", "kept\n");
  TEST_ASSERT_FALSE(exists("skip/me.txt"));
  TEST_ASSERT_FALSE(exists("[Content_Types].xml"));
#ifndef _WIN32
  TEST_ASSERT_TRUE((permissions("bin/run") & 0100u) != 0);
  TEST_ASSERT_TRUE((permissions("docs/a.txt") & 0111u) == 0);
#endif
  /* An exclusion that matches nothing names itself after the last entry. */
  reset_destination();
  const char *const missing[] = {"skip/me.txt", "not/there.txt", NULL};
  options.exclusions = missing;
  expect_failure(HAL_ENOENT, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_STRING("not/there.txt", result.entry);
  TEST_ASSERT_EQUAL_UINT64(6, result.entries);
}

static void test_entry_names_cannot_leave_destination(void) {
  begin_case("escape-names");
  static const char *const names[] = {"../outside/x.txt", "a/../../outside/x.txt", "a/../b.txt",
                                      "/absolute.txt",    "\\absolute.txt",        "C:drive.txt",
                                      "c:/drive.txt",     "\\\\server\\share.txt", ".."};
  for (size_t index = 0; index < COUNTOF(names); ++index) {
    jb_archive_result_t result;
    const hal_status_t status =
        single_entry(JB_ARCHIVE_TAR_GZIP, names[index], AE_IFREG, NULL, &result);
    expect_failure(HAL_EPERM, status, &result);
    expect_entry(names[index], result.entry);
    TEST_ASSERT_EQUAL_size_t(0, count_children(destination));
    expect_outside_untouched();
  }
  jb_archive_result_t result;
  expect_failure(HAL_EPERM,
                 single_entry(JB_ARCHIVE_ZIP, "../outside/zip.txt", AE_IFREG, NULL, &result),
                 &result);
  expect_failure(HAL_EPERM, single_entry(JB_ARCHIVE_TAR_GZIP, ".", AE_IFREG, NULL, &result),
                 &result);
  expect_outside_untouched();
}

static void test_links_cannot_leave_destination(void) {
  begin_case("escape-links");
  static const struct {
    const char *name, *target;
  } links[] = {{"top", ".."},
               {"a/up", "../../outside"},
               {"a/abs", "/etc/passwd"},
               {"a/drive", "C:/Windows"},
               {"a/unc", "\\\\server\\share"},
               {"a/b/back", "../x/../.."},
               {"a/b/name-up", "c/../../.."},
               {"a/b/sideways", "x/../.."}};
  for (size_t index = 0; index < COUNTOF(links); ++index) {
    jb_archive_result_t result;
    expect_failure(HAL_EPERM,
                   single_entry(JB_ARCHIVE_TAR_GZIP, links[index].name, AE_IFLNK,
                                links[index].target, &result),
                   &result);
    expect_entry(links[index].name, result.entry);
    expect_outside_untouched();
  }
  /* Writes never pass through a link, even one pointing inside. */
  reset_destination();
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_directory(writer, "inside/");
  add_symlink(writer, "link", "inside");
  add_file(writer, "link/file.txt", 0644, "through\n");
  writer_end(writer);
  jb_archive_result_t result;
  expect_failure(HAL_EPERM, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_EQUAL_STRING("link/file.txt", result.entry);
  TEST_ASSERT_EQUAL_size_t(0, count_children(in_destination(0, "inside")));
  /* Hard links: unsafe names, missing targets and directories. */
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_hardlink(writer, "hard", "../outside/x");
  writer_end(writer);
  expect_failure(HAL_EPERM, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_hardlink(writer, "hard", "missing.txt");
  writer_end(writer);
  expect_failure(HAL_ENOENT, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_directory(writer, "dir/");
  add_hardlink(writer, "hard", "dir");
  writer_end(writer);
  expect_failure(HAL_EUNSUPPORTED, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  /* The deepest permitted climb stays inside. */
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "c.txt", 0644, "top\n");
  add_symlink(writer, "a/b/top", "../../c.txt");
  writer_end(writer);
  const hal_status_t status = extract(JB_ARCHIVE_TAR_GZIP, &result);
  if (links_expected(status, &result)) {
    expect_success(status, &result);
    expect_content("a/b/top", "top\n");
  }
  expect_outside_untouched();
}

static void test_repeated_and_conflicting_names(void) {
  begin_case("collisions");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "same.txt", 0644, "first\n");
  add_file(writer, "same.txt", 0644, "second\n");
  writer_end(writer);
  jb_archive_result_t result;
  expect_failure(HAL_EEXIST, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_EQUAL_STRING("the archive repeats this name", result.message);
  expect_content("same.txt", "first\n");

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_directory(writer, "dir/");
  add_file(writer, "dir/a.txt", 0644, "a\n");
  add_directory(writer, "dir/");
  writer_end(writer);
  expect_success(extract(JB_ARCHIVE_TAR_GZIP, &result), &result);

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "node", 0644, "file\n");
  add_file(writer, "node/child.txt", 0644, "child\n");
  writer_end(writer);
  expect_failure(HAL_EEXIST, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "node", 0644, "file\n");
  add_directory(writer, "node/");
  writer_end(writer);
  expect_failure(HAL_EEXIST, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_symlink(writer, "link", "target");
  add_file(writer, "link", 0644, "file\n");
  writer_end(writer);
  expect_failure(HAL_EEXIST, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
#ifdef _WIN32
  /* Names differing in case are one name on NTFS. */
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "Dir/a.txt", 0644, "a\n");
  add_file(writer, "dir/b.txt", 0644, "b\n");
  writer_end(writer);
  expect_failure(HAL_EEXIST, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_EQUAL_STRING("dir/b.txt", result.entry);
  TEST_ASSERT_EQUAL_STRING("name collides with an existing entry", result.message);
#endif
}

/* Replace every recorded uncompressed size of the only ZIP entry. */
static void understate_zip_size(uint32_t actual, uint32_t declared) {
  size_t replaced = 0;
  for (size_t index = 0; index + 46 <= fixture_size; ++index) {
    const unsigned char *at = fixture + index;
    size_t offset = 0;
    if (memcmp(at, "PK\x03\x04", 4) == 0)
      offset = 22;
    else if (memcmp(at, "PK\x01\x02", 4) == 0)
      offset = 24;
    else if (memcmp(at, "PK\x07\x08", 4) == 0)
      offset = 12;
    if (offset == 0)
      continue;
    unsigned char *size = fixture + index + offset;
    const uint32_t value = (uint32_t)size[0] | (uint32_t)size[1] << 8 | (uint32_t)size[2] << 16 |
                           (uint32_t)size[3] << 24;
    if (value != actual)
      continue;
    for (unsigned byte = 0; byte < 4; ++byte)
      size[byte] = (unsigned char)(declared >> (8u * byte));
    ++replaced;
  }
  TEST_ASSERT_GREATER_THAN_size_t(0, replaced);
}

static void test_limits_count_entries_and_bytes_while_reading(void) {
  begin_case("limits");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "one.txt", 0644, "0123456789");
  add_file(writer, "two.txt", 0644, "0123456789");
  add_file(writer, "three.txt", 0644, "0123456789");
  writer_end(writer);
  jb_archive_result_t result;
  jb_archive_options_t options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.max_entries = 2;
  expect_failure(HAL_EOVERFLOW, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_UINT64(3, result.entries);
  TEST_ASSERT_EQUAL_STRING("three.txt", result.entry); /* the header over the limit */
  TEST_ASSERT_FALSE(exists("three.txt"));

  reset_destination();
  options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.max_file_bytes = 9;
  expect_failure(HAL_EOVERFLOW, extract_with(&options, &result), &result);
  TEST_ASSERT_FALSE(exists("one.txt"));
  TEST_ASSERT_EQUAL_UINT64(0, result.bytes);

  reset_destination();
  options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.max_total_bytes = 25;
  expect_failure(HAL_EOVERFLOW, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_UINT64(20, result.bytes);
  TEST_ASSERT_EQUAL_STRING("three.txt", result.entry);

  reset_destination();
  options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.max_entries = 0;
  expect_failure(HAL_EOVERFLOW, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_STRING("one.txt", result.entry);

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_ZIP);
  add_file(writer, "first", 0644, "1");
  add_file(writer, "second", 0644, "2");
  writer_end(writer);
  options = default_options(JB_ARCHIVE_ZIP);
  options.max_entries = 1;
  expect_failure(HAL_EOVERFLOW, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_UINT64(2, result.entries);
  TEST_ASSERT_EQUAL_STRING("second", result.entry);
  TEST_ASSERT_FALSE(exists("second"));

  /* A ZIP header that understates its size cannot bypass the limit: the pinned
   * libarchive refuses the first larger block before any byte is written. */
  static char zeros[65536];
  reset_destination();
  writer = writer_begin(JB_ARCHIVE_ZIP);
  writer_entry(writer, "bomb.bin", AE_IFREG, 0644, zeros, sizeof(zeros), NULL, false);
  writer_close(writer);
  understate_zip_size((uint32_t)sizeof(zeros), 4);
  save_fixture();
  options = default_options(JB_ARCHIVE_ZIP);
  options.max_file_bytes = 1024;
  expect_failure(HAL_EPROTO, extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL_STRING("bomb.bin", result.entry);
  TEST_ASSERT_EQUAL_UINT64(0, result.bytes);
}

static void test_damaged_wrong_and_unsupported_input(void) {
  begin_case("damaged");
  jb_archive_result_t result;
  struct archive *writer = writer_begin(JB_ARCHIVE_ZIP);
  static char text[4096];
  for (size_t index = 0; index < sizeof(text); ++index)
    text[index] = (char)('a' + index % 23u);
  writer_entry(writer, "text.txt", AE_IFREG, 0644, text, sizeof(text), NULL, false);
  writer_close(writer);
  /* Flip one byte of the compressed data after the local header, name and extra. */
  TEST_ASSERT_EQUAL_MEMORY("PK\x03\x04", fixture, 4);
  const size_t data = 30u + ((size_t)fixture[26] | (size_t)fixture[27] << 8) +
                      ((size_t)fixture[28] | (size_t)fixture[29] << 8);
  TEST_ASSERT_LESS_THAN_size_t(fixture_size, data + 10u);
  fixture[data + 10u] ^= 0x55;
  save_fixture();
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_ZIP, &result), &result);

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "file.txt", 0644, "data\n");
  writer_end(writer);
  const size_t complete = fixture_size;
  fixture_size = complete / 2;
  save_fixture();
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  fixture_size = complete;
  save_fixture();
  reset_destination();
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_ZIP, &result), &result); /* tar.gz as ZIP */

  reset_destination();
  writer = writer_begin(TEST_TAR);
  add_file(writer, "plain.txt", 0644, "plain\n");
  writer_end(writer);
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_EQUAL_STRING("archive is not gzip-compressed", result.message);
  TEST_ASSERT_EQUAL_size_t(0, count_children(destination));

  reset_destination();
  writer = writer_begin(JB_ARCHIVE_ZIP);
  add_file(writer, "zip.txt", 0644, "zip\n");
  writer_end(writer);
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);

  reset_destination();
  writer = writer_begin(TEST_USTAR_GZIP);
  add_file(writer, "bad\xff name.txt", 0644, "x");
  writer_end(writer);
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_EQUAL_STRING("", result.entry); /* libarchive drops a ustar name it cannot read */
  TEST_ASSERT_NOT_EQUAL_UINT32(0, result.native_error); /* the header warning fails the call */
  TEST_ASSERT_EQUAL_size_t(0, count_children(destination));
#ifndef _WIN32
  /* A pax name keeps its raw bytes; diagnostics show the valid prefix. Windows
   * writers convert raw names from the ANSI code page, so this case is POSIX. */
  expect_failure(HAL_EPROTO,
                 single_entry(JB_ARCHIVE_TAR_GZIP, "bad\xff name.txt", AE_IFREG, NULL, &result),
                 &result);
  TEST_ASSERT_EQUAL_STRING("bad", result.entry);
  TEST_ASSERT_NOT_EQUAL_UINT32(0, result.native_error);
  TEST_ASSERT_EQUAL_size_t(0, count_children(destination));
#endif
  expect_failure(HAL_EUNSUPPORTED,
                 single_entry(JB_ARCHIVE_TAR_GZIP, "fifo", AE_IFIFO, NULL, &result), &result);
  TEST_ASSERT_FALSE(exists("fifo"));
}

/* Appends data to the fixture as one complete gzip member. */
static size_t gzip_append(const unsigned char *data, size_t size) {
  z_stream stream;
  memset(&stream, 0, sizeof(stream));
  TEST_ASSERT_EQUAL_INT(
      Z_OK, deflateInit2(&stream, Z_BEST_COMPRESSION, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY));
  stream.next_in = data;
  stream.avail_in = (uInt)size;
  stream.next_out = fixture + fixture_size;
  stream.avail_out = (uInt)(sizeof(fixture) - fixture_size);
  TEST_ASSERT_EQUAL_INT(Z_STREAM_END, deflate(&stream, Z_FINISH));
  fixture_size += (size_t)stream.total_out;
  TEST_ASSERT_EQUAL_INT(Z_OK, deflateEnd(&stream));
  return fixture_size;
}

static void expect_damaged_gzip(const char *message) {
  reset_destination();
  save_fixture();
  jb_archive_result_t result;
  expect_failure(HAL_EPROTO, extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  TEST_ASSERT_NOT_NULL_MESSAGE(strstr(result.message, message), result.message);
}

/* The pinned libarchive skips gzip trailers; jhbase checks every member. */
static void test_gzip_members_are_checked(void) {
  begin_case("gzip-check");
  jb_archive_result_t result;
  /* A small stream is checked while libarchive reads it. */
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "file.txt", 0644, "data\n");
  writer_end(writer);
  size_t size = fixture_size;
  fixture[size - 8u] ^= 0x01u; /* CRC-32 */
  expect_damaged_gzip("incorrect data check");
  fixture[size - 8u] ^= 0x01u;
  fixture[size - 4u] ^= 0x01u; /* ISIZE */
  expect_damaged_gzip("incorrect length check");
  fixture[size - 4u] ^= 0x01u;
  fixture[size] = 0;
  fixture_size = size + 1u;
  expect_damaged_gzip("data follows the last gzip member");

  /* libarchive stops at the TAR end and never reads this padding or the
   * trailer behind it; jhbase reads the rest of the file to check them. */
  static unsigned char tar[1u << 18];
  writer = writer_begin(TEST_TAR);
  add_file(writer, "a.txt", 0644, "first\n");
  add_file(writer, "b.txt", 0644, "second\n");
  writer_close(writer);
  const size_t tar_size = fixture_size, padded = tar_size + (1u << 17);
  TEST_ASSERT_LESS_OR_EQUAL_size_t(sizeof(tar), padded);
  memcpy(tar, fixture, tar_size);
  memset(tar + tar_size, 0, padded - tar_size);

  /* A member boundary inside the second header: libarchive and the check
   * both continue with the next member. */
  fixture_size = 0;
  gzip_append(tar, 1280u);
  gzip_append(tar + 1280u, tar_size - 1280u);
  reset_destination();
  save_fixture();
  expect_success(extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  expect_content("b.txt", "second\n");

  fixture_size = 0;
  size = gzip_append(tar, padded);
  reset_destination();
  save_fixture();
  expect_success(extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  expect_content("b.txt", "second\n");
  fixture[size - 8u] ^= 0x01u;
  expect_damaged_gzip("incorrect data check");
  fixture[size - 8u] ^= 0x01u;
  fixture_size = size - 4u;
  expect_damaged_gzip("gzip data is truncated");

  /* Two members: the TAR split in the middle, each half compressed alone. */
  const size_t half = padded / 2u;
  fixture_size = 0;
  const size_t first_end = gzip_append(tar, half);
  gzip_append(tar + half, padded - half);
  reset_destination();
  save_fixture();
  expect_success(extract(JB_ARCHIVE_TAR_GZIP, &result), &result);
  expect_content("a.txt", "first\n");
  expect_content("b.txt", "second\n");
  fixture[first_end - 8u] ^= 0x01u; /* CRC-32 of the first member */
  expect_damaged_gzip("incorrect data check");
}

static void test_destination_and_arguments(void) {
  begin_case("arguments");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "file.txt", 0644, "data\n");
  writer_end(writer);
  jb_archive_options_t options = default_options(JB_ARCHIVE_TAR_GZIP);
  jb_archive_result_t result;
  memset(&result, 0x5a, sizeof(result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract(NULL, destination, &options, &result));
  TEST_ASSERT_EQUAL_UINT64(0, result.entries);
  TEST_ASSERT_EQUAL_STRING("", result.entry);
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract(archive_path, NULL, &options, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract(archive_path, destination, NULL, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract(archive_path, destination, &options, NULL));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract("", destination, &options, &result));
  options.format = 3;
  TEST_ASSERT_EQUAL(HAL_EINVAL, extract_with(&options, &result));
  options = default_options(JB_ARCHIVE_TAR_GZIP);
  const char *const empty[] = {"", NULL};
  options.exclusions = empty;
  TEST_ASSERT_EQUAL(HAL_EINVAL, extract_with(&options, &result));
  options.exclusions = NULL;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_archive_extract(archive_path, "relative", &options, &result));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_archive_extract(archive_path, in_destination(0, "missing"),
                                                   &options, &result));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_archive_extract(archive_path, archive_path, &options, &result));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_archive_extract(in_destination(0, "missing.tgz"), destination,
                                                   &options, &result));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_archive_extract(case_root, destination, &options, &result));
  expect_success(extract_with(&options, &result), &result);
  TEST_ASSERT_EQUAL(HAL_EEXIST, extract_with(&options, &result));
  TEST_ASSERT_EQUAL_STRING("destination is not empty", result.message);
}

static size_t cancel_after;
static bool cancel_poll(void *context) {
  ++*(size_t *)context;
  return cancel_after != 0 && --cancel_after == 0;
}

static void test_cancellation_stops_between_entries_and_blocks(void) {
  begin_case("cancel");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "a.txt", 0644, "a\n");
  add_file(writer, "b.txt", 0644, "b\n");
  writer_end(writer);
  size_t polls = 0;
  jb_archive_options_t options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.canceled = cancel_poll;
  options.context = &polls;
  jb_archive_result_t result;
  cancel_after = 0;
  expect_success(extract_with(&options, &result), &result);
  const size_t all_polls = polls;
  TEST_ASSERT_GREATER_OR_EQUAL_size_t(3, all_polls);
  for (size_t stop = 1; stop <= all_polls; ++stop) {
    reset_destination();
    cancel_after = stop;
    polls = 0;
    expect_failure(HAL_ECANCELED, extract_with(&options, &result), &result);
    TEST_ASSERT_EQUAL_size_t(stop, polls);
  }
  cancel_after = 0;
}

static void test_allocation_failures_release_everything(void) {
  begin_case("allocation");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "dir/a.txt", 0644, "a\n");
  add_hardlink(writer, "dir/hard.txt", "dir/a.txt");
  add_file(writer, "skip.txt", 0644, "skip\n");
  add_symlink(writer, "dir/link", "a.txt");
  writer_end(writer);
  const char *const exclusions[] = {"skip.txt", NULL};
  jb_archive_options_t options = default_options(JB_ARCHIVE_TAR_GZIP);
  options.exclusions = exclusions;
  jb_archive_result_t result;
  hal_status_t status = HAL_ENOMEM;
  size_t failures = 0;
  for (size_t attempt = 1; status == HAL_ENOMEM && attempt < 1000; ++attempt) {
    reset_destination();
    fail_allocation = attempt;
    status = extract_with(&options, &result);
    const bool injected = fail_allocation == 0;
    fail_allocation = 0;
    TEST_ASSERT_EQUAL_size_t(0, live_allocations);
    if (injected) {
      TEST_ASSERT_EQUAL_MESSAGE(HAL_ENOMEM, status, result.message);
      ++failures;
    }
  }
  TEST_ASSERT_GREATER_THAN_size_t(10, failures);
  if (links_expected(status, &result))
    expect_success(status, &result);
}

#ifdef __linux__
static void test_write_failure_reports_the_os_error(void) {
  begin_case("write-failure");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_file(writer, "file.txt", 0644, "data\n");
  writer_end(writer);
  jb_archive_result_t result;
  fail_write_errno = ENOSPC;
  const hal_status_t status = extract(JB_ARCHIVE_TAR_GZIP, &result);
  fail_write_errno = 0;
  expect_failure(HAL_EIO, status, &result);
  TEST_ASSERT_EQUAL_UINT32(ENOSPC, result.native_error);
  TEST_ASSERT_EQUAL_STRING("file.txt", result.entry);
  TEST_ASSERT_EQUAL_UINT64(0, result.bytes);

  /* Bytes stored by a short write before the failure are counted. */
  reset_destination();
  fail_write_errno = ENOSPC;
  partial_write = 3;
  const hal_status_t partial = extract(JB_ARCHIVE_TAR_GZIP, &result);
  fail_write_errno = 0;
  partial_write = 0;
  expect_failure(HAL_EIO, partial, &result);
  TEST_ASSERT_EQUAL_UINT32(ENOSPC, result.native_error);
  TEST_ASSERT_EQUAL_UINT64(3, result.bytes);
  expect_content("file.txt", "dat");
}
#endif

#ifndef _WIN32
/* A POSIX name may end with a backslash; entries still go below it. */
static void test_destination_ending_with_backslash(void) {
  begin_case("backslash-destination");
  struct archive *writer = writer_begin(JB_ARCHIVE_ZIP);
  add_file(writer, "x", 0644, "inside\n");
  writer_end(writer);
  char odd[8192], inside[8192], sibling[8192];
  join(odd, sizeof(odd), case_root, "dest\\");
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_mkdir(odd, false));
  const jb_archive_options_t options = default_options(JB_ARCHIVE_ZIP);
  jb_archive_result_t result;
  expect_success(jb_archive_extract(archive_path, odd, &options, &result), &result);
  jb_fs_info_t info;
  TEST_ASSERT_EQUAL(HAL_OK, jb_fs_status(join(inside, sizeof(inside), odd, "x"), false, &info));
  TEST_ASSERT_EQUAL_UINT32(JB_FS_REGULAR, info.kind);
  TEST_ASSERT_EQUAL(
      HAL_ENOENT, jb_fs_status(join(sibling, sizeof(sibling), case_root, "dest\\x"), false, &info));
  TEST_ASSERT_EQUAL_size_t(4, count_children(case_root)); /* destination, outside, fixture, odd */
}
#endif

#ifdef _WIN32
static void test_windows_names_are_refused(void) {
  begin_case("windows-names");
  static const struct {
    const char *name;
    hal_status_t status;
  } names[] = {{"aux.txt", HAL_EUNSUPPORTED},  {"dir/CON", HAL_EUNSUPPORTED},
               {"com1.log", HAL_EUNSUPPORTED}, {"LPT\xc2\xb9", HAL_EUNSUPPORTED},
               {"nul .txt", HAL_EUNSUPPORTED}, {"name.", HAL_EUNSUPPORTED},
               {"name ", HAL_EUNSUPPORTED},    {"bad|name", HAL_EUNSUPPORTED},
               {"bad?name", HAL_EUNSUPPORTED}, {"stream.txt:hidden", HAL_EPERM},
               {"dir/a:b", HAL_EPERM}};
  for (size_t index = 0; index < COUNTOF(names); ++index) {
    jb_archive_result_t result;
    expect_failure(names[index].status,
                   single_entry(JB_ARCHIVE_TAR_GZIP, names[index].name, AE_IFREG, NULL, &result),
                   &result);
    TEST_ASSERT_EQUAL_size_t(0, count_children(destination));
  }
  jb_archive_result_t result;
  TEST_ASSERT_EQUAL(HAL_OK,
                    single_entry(JB_ARCHIVE_TAR_GZIP, "com10.txt", AE_IFREG, NULL, &result));
  TEST_ASSERT_EQUAL(HAL_OK,
                    single_entry(JB_ARCHIVE_TAR_GZIP, "auxiliary.txt", AE_IFREG, NULL, &result));
  expect_failure(HAL_EPERM, single_entry(JB_ARCHIVE_TAR_GZIP, "link", AE_IFLNK, "a:b", &result),
                 &result);
}

/* NTFS compares names without case: the link flag follows what the
 * filesystem resolves, not the spelling in the archive. */
static void test_windows_link_kind_follows_the_filesystem(void) {
  begin_case("windows-link-kind");
  struct archive *writer = writer_begin(JB_ARCHIVE_TAR_GZIP);
  add_directory(writer, "Dir/");
  add_file(writer, "Dir/x.txt", 0644, "x\n");
  add_symlink(writer, "exact", "Dir");
  add_symlink(writer, "alias", "dir");
  add_symlink(writer, "chain", "ALIAS");
  add_symlink(writer, "file", "DIR/X.TXT");
  add_symlink(writer, "dangling", "missing");
  writer_end(writer);
  jb_archive_result_t result;
  const hal_status_t status = extract(JB_ARCHIVE_TAR_GZIP, &result);
  if (!links_expected(status, &result))
    return;
  expect_success(status, &result);
  TEST_ASSERT_TRUE(directory_link("exact"));
  TEST_ASSERT_TRUE(directory_link("alias"));
  TEST_ASSERT_TRUE(directory_link("chain"));
  TEST_ASSERT_FALSE(directory_link("file"));
  TEST_ASSERT_FALSE(directory_link("dangling"));
  TEST_ASSERT_EQUAL_size_t(1, count_children(in_destination(0, "alias")));
  TEST_ASSERT_EQUAL_size_t(1, count_children(in_destination(0, "chain")));
  expect_content("chain/x.txt", "x\n");
  expect_content("file", "x\n");
}

static void probe_symlinks(void) {
  wchar_t link[8192], target[] = L"probe-target";
  char path[8192];
  join(path, sizeof(path), test_root, "symlink-probe");
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, link, (int)COUNTOF(link)) == 0)
    return;
  symlinks_supported =
      CreateSymbolicLinkW(link, target, SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE) != 0;
  if (symlinks_supported)
    DeleteFileW(link);
  printf("Windows symbolic links: %s\n", symlinks_supported ? "available" : "unavailable");
}
#endif

int main(int argc, char **argv) {
  char cwd[8192];
  size_t required = 0;
  if (jb_fs_cwd(cwd, sizeof(cwd), &required) != HAL_OK)
    return 2;
#ifdef _WIN32
  const unsigned long process = GetCurrentProcessId();
#else
  const unsigned long process = (unsigned long)getpid();
#endif
  char name[8192];
  const int count =
      snprintf(name, sizeof(name), "%s-%lu", argc > 1 ? argv[1] : "archive-check", process);
  if (count < 0 || (size_t)count >= sizeof(name) ||
      jb_fs_path_join(cwd, name, test_root, sizeof(test_root), &required) != HAL_OK ||
      jb_fs_mkdir(test_root, true) != HAL_OK) {
    fprintf(stderr, "Cannot create the archive fixture below %s\n", cwd);
    return 2;
  }
#ifdef _WIN32
  probe_symlinks();
#else
  writer_locale = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (writer_locale == (locale_t)0) {
    fprintf(stderr, "The C.UTF-8 locale is unavailable\n");
    return 2;
  }
#endif
  UNITY_BEGIN();
  RUN_TEST(test_tar_gzip_tree_links_and_modes);
  RUN_TEST(test_zip_tree_exclusions_and_execute_bits);
  RUN_TEST(test_entry_names_cannot_leave_destination);
  RUN_TEST(test_links_cannot_leave_destination);
  RUN_TEST(test_repeated_and_conflicting_names);
  RUN_TEST(test_limits_count_entries_and_bytes_while_reading);
  RUN_TEST(test_damaged_wrong_and_unsupported_input);
  RUN_TEST(test_gzip_members_are_checked);
  RUN_TEST(test_destination_and_arguments);
  RUN_TEST(test_cancellation_stops_between_entries_and_blocks);
  RUN_TEST(test_allocation_failures_release_everything);
#ifdef __linux__
  RUN_TEST(test_write_failure_reports_the_os_error);
#endif
#ifndef _WIN32
  RUN_TEST(test_destination_ending_with_backslash);
#endif
#ifdef _WIN32
  RUN_TEST(test_windows_names_are_refused);
  RUN_TEST(test_windows_link_kind_follows_the_filesystem);
#endif
  const int failures = UNITY_END();
#ifndef _WIN32
  freelocale(writer_locale);
#endif
  if (failures == 0 && jb_fs_remove(test_root, true) != HAL_OK)
    return 3;
  return failures;
}
