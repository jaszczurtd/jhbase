#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#define _FILE_OFFSET_BITS 64
#endif

#include "jb/archive.h"
#include "filesystem_internal.h"
#include "jb/allocator.h"
#include "jb/array.h"
#include "jb/filesystem.h"
#include "jb/map.h"
#include "jb/string.h"
#include "jb/utf8.h"
#include "jb/vector.h"
#include <archive.h>
#include <archive_entry.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#define ZLIB_CONST
#include <zlib.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define JA_SEPARATOR '\\'
#ifndef SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE
#define SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE 0x2
#endif
#else
#include <fcntl.h>
#include <locale.h>
#include <sys/stat.h>
#include <unistd.h>
#define JA_SEPARATOR '/'
#endif

enum { JA_BLOCK = 64 * 1024, JA_MAX_TEXT = 4096 };
enum { JA_DIRECTORY = 1, JA_FILE = 2, JA_LINK = 3 };

/* Entries created by this call: relative path with '/' separators. */
typedef struct {
  const char *data;
  size_t length;
} ja_key_t;

typedef struct {
  uint32_t kind;
} ja_node_t;

/* Symbolic links are created after every other entry. */
typedef struct {
  char *path;
  char *target;
} ja_link_t;

typedef struct {
  const jb_archive_options_t *options;
  jb_archive_result_t *result;
  struct archive *archive;
  jb_map_t entries;
  jb_vector_t links;
  jb_vector_t found; /* bool per exclusion */
  jb_string_t destination;
  jb_string_t full;
  jb_string_t path;
  jb_string_t other;
  unsigned char *block;
  unsigned char *data;
#ifdef _WIN32
  HANDLE input;
#else
  int input;
#endif
  hal_status_t input_status; /* failure inside a libarchive callback, message already set */
  size_t exclusions;
  /* libarchive skips the gzip trailer, so every compressed byte read for a
   * tar.gz also passes through zlib, which checks CRC-32 and length. */
  z_stream gzip;
  unsigned char *scratch; /* discarded output of that check */
  uint64_t gzip_members;  /* members whose trailer matched */
  unsigned gzip_magic;    /* magic bytes seen of the current member */
  bool gzip_open;
  bool gzip_member_end; /* the last byte fed ended a member */
} ja_state_t;

static void *ja_allocate(size_t bytes) {
  const jb_allocator_t *allocator = jb_heap_allocator();
  return allocator->allocate(allocator->context, bytes);
}

static void ja_release(void *data) {
  if (data != NULL) {
    const jb_allocator_t *allocator = jb_heap_allocator();
    allocator->release(allocator->context, data);
  }
}

static char *ja_duplicate(const char *text, size_t length) {
  char *copy = ja_allocate(length + 1u);
  if (copy != NULL) {
    memcpy(copy, text, length);
    copy[length] = '\0';
  }
  return copy;
}

/* Name the entry being processed: its valid UTF-8 prefix that fits. */
static void ja_set_entry(ja_state_t *state, const char *text) {
  (void)jb_utf8_copy_valid_prefix(text, strlen(text), state->result->entry,
                                  sizeof(state->result->entry), NULL);
}

static hal_status_t ja_fail(ja_state_t *state, hal_status_t status, uint32_t native,
                            const char *format, ...) {
  char text[JB_ARCHIVE_MESSAGE_CAPACITY];
  va_list arguments;
  va_start(arguments, format);
  const int written = vsnprintf(text, sizeof(text), format, arguments);
  va_end(arguments);
  if (written < 0)
    text[0] = '\0';
  state->result->native_error = native;
  (void)jb_utf8_copy_valid_prefix(text, strlen(text), state->result->message,
                                  sizeof(state->result->message), NULL);
  return status;
}

static hal_status_t ja_os_fail(ja_state_t *state, uint32_t native, const char *what) {
  return ja_fail(state, jb_fs_native_status(native), native, "%s", what);
}

static uint32_t ja_last_error(void) {
#ifdef _WIN32
  return (uint32_t)GetLastError();
#else
  return (uint32_t)errno;
#endif
}

static hal_status_t ja_archive_fail(ja_state_t *state) {
  if (state->input_status != HAL_OK)
    return state->input_status;
  const int number = archive_errno(state->archive);
  const char *text = archive_error_string(state->archive);
  return ja_fail(state, number == ENOMEM ? HAL_ENOMEM : HAL_EPROTO,
                 number > 0 ? (uint32_t)number : 0u, "%s",
                 text != NULL ? text : "malformed archive");
}

static bool ja_canceled(const ja_state_t *state) {
  return state->options->canceled != NULL && state->options->canceled(state->options->context);
}

static int ja_compare(const void *left, const void *right, void *context) {
  const ja_key_t *first = left, *second = right;
  (void)context;
  const size_t common = first->length < second->length ? first->length : second->length;
  const int order = common == 0u ? 0 : memcmp(first->data, second->data, common);
  if (order != 0)
    return order;
  return first->length < second->length ? -1 : (first->length > second->length ? 1 : 0);
}

static const ja_node_t *ja_find(const ja_state_t *state, const char *path, size_t length) {
  const ja_key_t key = {path, length};
  const void *value = NULL;
  return jb_map_find_const(&state->entries, &key, &value) == HAL_OK ? value : NULL;
}

static hal_status_t ja_insert(ja_state_t *state, const char *path, size_t length, uint32_t kind) {
  char *copy = ja_duplicate(path, length);
  if (copy == NULL)
    return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  const ja_key_t key = {copy, length};
  const ja_node_t node = {kind};
  const hal_status_t status = jb_map_insert(&state->entries, &key, &node);
  if (status != HAL_OK) {
    ja_release(copy);
    return ja_fail(state, status, 0u, "cannot record the entry");
  }
  return HAL_OK;
}

static hal_status_t ja_memory(ja_state_t *state, hal_status_t status) {
  return status == HAL_OK ? HAL_OK : ja_fail(state, status, 0u, "out of memory");
}

/* Text from the archive: nonempty, bounded and valid UTF-8. */
static hal_status_t ja_text(ja_state_t *state, const char *text, const char *what) {
  if (text == NULL)
    return ja_fail(state, HAL_EPROTO, 0u, "%s cannot be read as UTF-8", what);
  const size_t length = strlen(text);
  if (length == 0u)
    return ja_fail(state, HAL_EPROTO, 0u, "%s is empty", what);
  if (length > JA_MAX_TEXT)
    return ja_fail(state, HAL_EOVERFLOW, 0u, "%s is longer than %d bytes", what, JA_MAX_TEXT);
  size_t invalid = 0;
  if (jb_utf8_validate(text, length, &invalid) != HAL_OK)
    return ja_fail(state, HAL_EPROTO, 0u, "%s is not valid UTF-8", what);
  return HAL_OK;
}

/* Entry names and link targets separate components with either character. */
static bool ja_separator(char value) { return value == '/' || value == '\\'; }

/* Host paths follow the OS: a POSIX name may contain a backslash. */
static bool ja_native_separator(char value) {
#ifdef _WIN32
  return ja_separator(value);
#else
  return value == '/';
#endif
}

static bool ja_rooted(const char *text) {
  const char first = text[0];
  return ja_separator(first) ||
         (((first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z')) && text[1] == ':');
}

/* Next nonempty component; false at the end. */
static bool ja_component(const char **cursor, const char **start, size_t *length) {
  const char *position = *cursor;
  while (*position != '\0' && ja_separator(*position))
    ++position;
  if (*position == '\0') {
    *cursor = position;
    return false;
  }
  *start = position;
  while (*position != '\0' && !ja_separator(*position))
    ++position;
  *length = (size_t)(position - *start);
  *cursor = position;
  return true;
}

static bool ja_dot(const char *part, size_t length) { return length == 1u && part[0] == '.'; }

static bool ja_dot_dot(const char *part, size_t length) {
  return length == 2u && part[0] == '.' && part[1] == '.';
}

#ifdef _WIN32
static bool ja_same_ascii(const char *text, size_t length, const char *name) {
  size_t index = 0;
  for (; index < length && name[index] != '\0'; ++index) {
    char value = text[index];
    if (value >= 'a' && value <= 'z')
      value = (char)(value - 'a' + 'A');
    if (value != name[index])
      return false;
  }
  return index == length && name[index] == '\0';
}

/* Win32 rejects or folds these names; ':' also selects an alternate data stream. */
static hal_status_t ja_windows_component(ja_state_t *state, const char *part, size_t length) {
  for (size_t index = 0; index < length; ++index) {
    const unsigned char value = (unsigned char)part[index];
    if (value == ':')
      return ja_fail(state, HAL_EPERM, 0u, "name contains ':' (drive or data stream)");
    if (value < 0x20u || strchr("<>\"|?*", value) != NULL)
      return ja_fail(state, HAL_EUNSUPPORTED, 0u, "name has a character Windows rejects");
  }
  if (part[length - 1u] == '.' || part[length - 1u] == ' ')
    return ja_fail(state, HAL_EUNSUPPORTED, 0u, "name ends with a dot or space");
  size_t base = 0;
  while (base < length && part[base] != '.')
    ++base;
  while (base > 0u && part[base - 1u] == ' ')
    --base;
  static const char *const reserved[] = {"CON", "PRN", "AUX", "NUL", "CONIN$", "CONOUT$"};
  bool device = false;
  for (size_t index = 0; index < COUNTOF(reserved); ++index)
    device = device || ja_same_ascii(part, base, reserved[index]);
  if (!device && base >= 4u && (ja_same_ascii(part, 3u, "COM") || ja_same_ascii(part, 3u, "LPT"))) {
    const unsigned char digit = (unsigned char)part[3];
    const unsigned char next = base == 5u ? (unsigned char)part[4] : 0u;
    device = (base == 4u && digit >= '0' && digit <= '9') ||
             (base == 5u && digit == 0xC2u && (next == 0xB9u || next == 0xB2u || next == 0xB3u));
  }
  if (device)
    return ja_fail(state, HAL_EUNSUPPORTED, 0u, "name is a Windows device name");
  return HAL_OK;
}
#endif

/* Entry name to a relative path with '/' separators; depth counts components. */
static hal_status_t ja_entry_path(ja_state_t *state, const char *name, jb_string_t *out,
                                  size_t *depth) {
  if (ja_rooted(name))
    return ja_fail(state, HAL_EPERM, 0u, "entry name is absolute or has a drive prefix");
  jb_string_clear(out);
  *depth = 0;
  const char *cursor = name, *part = NULL;
  size_t length = 0;
  while (ja_component(&cursor, &part, &length)) {
    if (ja_dot(part, length))
      continue;
    if (ja_dot_dot(part, length))
      return ja_fail(state, HAL_EPERM, 0u, "entry name contains ..");
#ifdef _WIN32
    const hal_status_t checked = ja_windows_component(state, part, length);
    if (checked != HAL_OK)
      return checked;
#endif
    hal_status_t status = *depth > 0u ? jb_string_append(out, "/", 1u) : HAL_OK;
    if (status == HAL_OK)
      status = jb_string_append(out, part, length);
    if (status != HAL_OK)
      return ja_memory(state, status);
    ++*depth;
  }
  return HAL_OK;
}

/* A link may climb with leading .. up to its own directory depth, then only
 * descend. Each link stays inside the tree; so does any chain of them. */
static hal_status_t ja_link_target(ja_state_t *state, const char *target, size_t parent_depth,
                                   jb_string_t *out) {
  hal_status_t status = ja_text(state, target, "link target");
  if (status != HAL_OK)
    return status;
  if (ja_rooted(target))
    return ja_fail(state, HAL_EPERM, 0u, "link target is absolute or has a drive prefix");
  jb_string_clear(out);
  const char *cursor = target, *part = NULL;
  size_t length = 0, ups = 0;
  bool named = false;
  while (ja_component(&cursor, &part, &length)) {
    if (ja_dot(part, length))
      continue;
    if (ja_dot_dot(part, length)) {
      if (named)
        return ja_fail(state, HAL_EPERM, 0u, "link target has .. after a name");
      if (++ups > parent_depth)
        return ja_fail(state, HAL_EPERM, 0u, "link target leaves the destination");
    } else {
#ifdef _WIN32
      status = ja_windows_component(state, part, length);
      if (status != HAL_OK)
        return status;
#endif
      named = true;
    }
    status = jb_string_size(out) > 0u ? jb_string_append(out, "/", 1u) : HAL_OK;
    if (status == HAL_OK)
      status = jb_string_append(out, part, length);
    if (status != HAL_OK)
      return ja_memory(state, status);
  }
  return jb_string_size(out) > 0u ? HAL_OK : ja_memory(state, jb_string_assign(out, ".", 1u));
}

/* destination + relative path with native separators, borrowed from state. */
static hal_status_t ja_full(ja_state_t *state, const char *path, size_t length, const char **out) {
  hal_status_t status = jb_string_assign(&state->full, jb_string_data(&state->destination),
                                         jb_string_size(&state->destination));
  if (status == HAL_OK)
    status = jb_string_append(&state->full, path, length);
  if (status != HAL_OK)
    return ja_memory(state, status);
#ifdef _WIN32
  char *text = jb_string_mutable_data(&state->full);
  for (size_t index = jb_string_size(&state->destination); index < jb_string_size(&state->full);
       ++index) {
    if (text[index] == '/')
      text[index] = JA_SEPARATOR;
  }
#endif
  *out = jb_string_data(&state->full);
  return HAL_OK;
}

static hal_status_t ja_mkdir(ja_state_t *state, const char *path, size_t length) {
  const char *full = NULL;
  hal_status_t status = ja_full(state, path, length, &full);
  if (status != HAL_OK)
    return status;
#ifdef _WIN32
  wchar_t *wide = NULL;
  status = jb_fs_native_path(full, &wide);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot convert the path");
  const BOOL created = CreateDirectoryW(wide, NULL);
  const uint32_t error = created ? 0u : ja_last_error();
  ja_release(wide);
  if (created)
    return HAL_OK;
#else
  if (mkdir(full, 0777) == 0)
    return HAL_OK;
  const uint32_t error = ja_last_error();
#endif
  status = jb_fs_native_status(error);
  return status == HAL_EEXIST
             ? ja_fail(state, status, error, "name collides with an existing entry")
             : ja_os_fail(state, error, "cannot create the directory");
}

/* Every parent of path must be a directory created by this call. */
static hal_status_t ja_parents(ja_state_t *state, const char *path, size_t length) {
  for (size_t end = 0; end < length; ++end) {
    if (path[end] != '/')
      continue;
    const ja_node_t *node = ja_find(state, path, end);
    if (node != NULL && node->kind == JA_DIRECTORY)
      continue;
    if (node != NULL)
      return node->kind == JA_LINK ? ja_fail(state, HAL_EPERM, 0u, "a parent component is a link")
                                   : ja_fail(state, HAL_EEXIST, 0u, "a parent component is a file");
    hal_status_t status = ja_mkdir(state, path, end);
    if (status == HAL_OK)
      status = ja_insert(state, path, end, JA_DIRECTORY);
    if (status != HAL_OK)
      return status;
  }
  return HAL_OK;
}

static hal_status_t ja_unique(ja_state_t *state, const char *path, size_t length) {
  return ja_find(state, path, length) == NULL
             ? HAL_OK
             : ja_fail(state, HAL_EEXIST, 0u, "the archive repeats this name");
}

static hal_status_t ja_directory(ja_state_t *state, const char *path, size_t length) {
  const ja_node_t *node = ja_find(state, path, length);
  if (node != NULL)
    return node->kind == JA_DIRECTORY
               ? HAL_OK
               : ja_fail(state, HAL_EEXIST, 0u, "the archive repeats this name");
  hal_status_t status = ja_parents(state, path, length);
  if (status == HAL_OK)
    status = ja_mkdir(state, path, length);
  return status == HAL_OK ? ja_insert(state, path, length, JA_DIRECTORY) : status;
}

#ifdef _WIN32
typedef HANDLE ja_output_t;
#define JA_NO_OUTPUT INVALID_HANDLE_VALUE
#else
typedef int ja_output_t;
#define JA_NO_OUTPUT (-1)
#endif

static hal_status_t ja_create(ja_state_t *state, const char *full, uint32_t mode,
                              ja_output_t *out) {
#ifdef _WIN32
  (void)mode;
  wchar_t *wide = NULL;
  const hal_status_t status = jb_fs_native_path(full, &wide);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot convert the path");
  *out = CreateFileW(wide, GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
  const uint32_t error = *out == INVALID_HANDLE_VALUE ? ja_last_error() : 0u;
  ja_release(wide);
  if (*out != INVALID_HANDLE_VALUE)
    return HAL_OK;
#else
  *out = open(full, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC | O_NOCTTY, (mode_t)mode);
  if (*out >= 0)
    return HAL_OK;
  const uint32_t error = ja_last_error();
#endif
  return jb_fs_native_status(error) == HAL_EEXIST
             ? ja_fail(state, HAL_EEXIST, error, "name collides with an existing entry")
             : ja_os_fail(state, error, "cannot create the file");
}

/* Every successful OS write is counted, also one before a failing write. */
static hal_status_t ja_write(ja_state_t *state, ja_output_t output, const unsigned char *data,
                             size_t length, uint64_t *file_bytes) {
  while (length > 0u) {
#ifdef _WIN32
    DWORD written = 0;
    if (!WriteFile(output, data, (DWORD)length, &written, NULL) || written == 0u)
      return ja_os_fail(state, ja_last_error(), "cannot write the file");
#else
    const ssize_t written = write(output, data, length);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0)
      return ja_os_fail(state, written < 0 ? ja_last_error() : (uint32_t)EIO,
                        "cannot write the file");
#endif
    data += (size_t)written;
    length -= (size_t)written;
    *file_bytes += (uint64_t)written;
    state->result->bytes += (uint64_t)written;
  }
  return HAL_OK;
}

static hal_status_t ja_close(ja_state_t *state, ja_output_t output, hal_status_t status) {
#ifdef _WIN32
  const bool closed = CloseHandle(output) != 0;
#else
  const bool closed = close(output) == 0;
#endif
  if (!closed && status == HAL_OK)
    return ja_os_fail(state, ja_last_error(), "cannot close the file");
  return status;
}

static hal_status_t ja_limit(ja_state_t *state, uint64_t file_bytes, uint64_t more) {
  const jb_archive_options_t *options = state->options;
  if (file_bytes > options->max_file_bytes || more > options->max_file_bytes - file_bytes)
    return ja_fail(state, HAL_EOVERFLOW, 0u, "file is larger than max_file_bytes");
  if (more > options->max_total_bytes - state->result->bytes)
    return ja_fail(state, HAL_EOVERFLOW, 0u, "archive is larger than max_total_bytes");
  return HAL_OK;
}

static hal_status_t ja_file(ja_state_t *state, struct archive_entry *entry, const char *path,
                            size_t length) {
  hal_status_t status = ja_unique(state, path, length);
  if (status == HAL_OK && archive_entry_size_is_set(entry) && archive_entry_size(entry) > 0)
    status = ja_limit(state, 0u, (uint64_t)archive_entry_size(entry));
  if (status == HAL_OK)
    status = ja_parents(state, path, length);
  const char *full = NULL;
  if (status == HAL_OK)
    status = ja_full(state, path, length, &full);
  ja_output_t output = JA_NO_OUTPUT;
  if (status == HAL_OK)
    status = ja_create(state, full, 0666u | ((uint32_t)archive_entry_perm(entry) & 0111u), &output);
  if (status != HAL_OK)
    return status;
  status = ja_insert(state, path, length, JA_FILE);
  uint64_t written = 0;
  while (status == HAL_OK) {
    if (ja_canceled(state)) {
      status = ja_fail(state, HAL_ECANCELED, 0u, "canceled");
      break;
    }
    const la_ssize_t count = archive_read_data(state->archive, state->data, JA_BLOCK);
    if (count == 0)
      break;
    if (count < 0) {
      status = ja_archive_fail(state);
      break;
    }
    status = ja_limit(state, written, (uint64_t)count);
    if (status == HAL_OK)
      status = ja_write(state, output, state->data, (size_t)count, &written);
  }
  return ja_close(state, output, status);
}

static hal_status_t ja_hard_link(ja_state_t *state, struct archive_entry *entry, const char *path,
                                 size_t length) {
  hal_status_t status = ja_text(state, archive_entry_hardlink_utf8(entry), "hard link target");
  size_t depth = 0;
  if (status == HAL_OK)
    status = ja_entry_path(state, archive_entry_hardlink_utf8(entry), &state->other, &depth);
  if (status != HAL_OK)
    return status;
  const ja_node_t *node =
      ja_find(state, jb_string_data(&state->other), jb_string_size(&state->other));
  if (node == NULL)
    return ja_fail(state, HAL_ENOENT, 0u, "hard link target was not extracted before");
  if (node->kind != JA_FILE)
    return ja_fail(state, HAL_EUNSUPPORTED, 0u, "hard link target is not a regular file");
  if (archive_entry_size_is_set(entry) && archive_entry_size(entry) != 0)
    return ja_fail(state, HAL_EUNSUPPORTED, 0u, "hard link carries data");
  status = ja_unique(state, path, length);
  if (status == HAL_OK)
    status = ja_parents(state, path, length);
  const char *full = NULL;
  if (status == HAL_OK)
    status = ja_full(state, path, length, &full);
  char *link = NULL;
  if (status == HAL_OK) {
    link = ja_duplicate(full, strlen(full));
    if (link == NULL)
      status = ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  }
  if (status == HAL_OK)
    status = ja_full(state, jb_string_data(&state->other), jb_string_size(&state->other), &full);
  uint32_t error = 0;
  if (status == HAL_OK) {
#ifdef _WIN32
    wchar_t *wide_link = NULL, *wide_target = NULL;
    status = jb_fs_native_path(link, &wide_link);
    if (status == HAL_OK)
      status = jb_fs_native_path(full, &wide_target);
    if (status != HAL_OK)
      status = ja_fail(state, status, 0u, "cannot convert the path");
    else if (!CreateHardLinkW(wide_link, wide_target, NULL))
      error = ja_last_error();
    ja_release(wide_link);
    ja_release(wide_target);
#else
    if (linkat(AT_FDCWD, full, AT_FDCWD, link, 0) != 0)
      error = ja_last_error();
#endif
  }
  ja_release(link);
  if (status == HAL_OK && error != 0u)
    status = jb_fs_native_status(error) == HAL_EEXIST
                 ? ja_fail(state, HAL_EEXIST, error, "name collides with an existing entry")
                 : ja_os_fail(state, error, "cannot create the hard link");
  return status == HAL_OK ? ja_insert(state, path, length, JA_FILE) : status;
}

static hal_status_t ja_symbolic_link(ja_state_t *state, struct archive_entry *entry,
                                     const char *path, size_t length, size_t depth) {
  hal_status_t status = ja_unique(state, path, length);
  if (status == HAL_OK)
    status = ja_link_target(state, archive_entry_symlink_utf8(entry), depth - 1u, &state->other);
  if (status == HAL_OK)
    status = ja_parents(state, path, length);
  if (status != HAL_OK)
    return status;
  ja_link_t link = {ja_duplicate(path, length),
                    ja_duplicate(jb_string_data(&state->other), jb_string_size(&state->other))};
  if (link.path != NULL && link.target != NULL)
    status = jb_vector_append(&state->links, &link, 1u);
  else
    status = HAL_ENOMEM;
  if (status != HAL_OK) {
    ja_release(link.path);
    ja_release(link.target);
    return ja_memory(state, status);
  }
  return ja_insert(state, path, length, JA_LINK);
}

static bool ja_excluded(ja_state_t *state, const char *name) {
  const char *const *exclusions = state->options->exclusions;
  bool excluded = false;
  for (size_t index = 0; index < state->exclusions; ++index) {
    const char *left = name, *right = exclusions[index];
    while (*left != '\0' && (*left == *right || (*left == '\\' && *right == '/'))) {
      ++left;
      ++right;
    }
    if (*left == '\0' && *right == '\0') {
      *(bool *)jb_vector_at(&state->found, index) = true;
      excluded = true;
    }
  }
  return excluded;
}

/* Diagnostics name each entry as soon as its header is read: UTF-8 when
 * libarchive converts the name, else the valid prefix of the raw bytes. */
static void ja_name_entry(ja_state_t *state, struct archive_entry *entry) {
  const char *name = archive_entry_pathname_utf8(entry);
  if (name == NULL)
    name = archive_entry_pathname(entry);
  ja_set_entry(state, name != NULL ? name : "");
}

static hal_status_t ja_entry(ja_state_t *state, struct archive_entry *entry) {
  const char *name = archive_entry_pathname_utf8(entry);
  if (name != NULL && ja_excluded(state, name))
    return HAL_OK;
  hal_status_t status = ja_text(state, name, "entry name");
  if (status != HAL_OK)
    return status;
  if (archive_entry_is_encrypted(entry))
    return ja_fail(state, HAL_EUNSUPPORTED, 0u, "entry is encrypted");
  size_t depth = 0;
  status = ja_entry_path(state, name, &state->path, &depth);
  if (status != HAL_OK)
    return status;
  const unsigned type = (unsigned)archive_entry_filetype(entry);
  if (depth == 0u)
    return type == (unsigned)AE_IFDIR && !archive_entry_hardlink_is_set(entry)
               ? HAL_OK
               : ja_fail(state, HAL_EPERM, 0u, "entry names the destination itself");
  /* Link handlers reuse the state strings; keep a stable copy of the path. */
  char *path = ja_duplicate(jb_string_data(&state->path), jb_string_size(&state->path));
  if (path == NULL)
    return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  const size_t length = jb_string_size(&state->path);
  if (archive_entry_hardlink_is_set(entry))
    status = ja_hard_link(state, entry, path, length);
  else if (type == (unsigned)AE_IFDIR)
    status = ja_directory(state, path, length);
  else if (type == (unsigned)AE_IFREG)
    status = ja_file(state, entry, path, length);
  else if (type == (unsigned)AE_IFLNK)
    status = ja_symbolic_link(state, entry, path, length, depth);
  else
    status = ja_fail(state, HAL_EUNSUPPORTED, 0u, "entry type is not supported");
  ja_release(path);
  return status;
}

/* Creates one symbolic link. On Windows, directory replaces the file link
 * made earlier for the same entry with a directory link. */
static hal_status_t ja_make_link(ja_state_t *state, const ja_link_t *link, bool directory) {
  const char *full = NULL;
  hal_status_t status = ja_full(state, link->path, strlen(link->path), &full);
  if (status != HAL_OK)
    return status;
  uint32_t error = 0;
#ifdef _WIN32
  char *target = ja_duplicate(link->target, strlen(link->target));
  if (target == NULL)
    status = ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  wchar_t *wide_link = NULL, *wide_target = NULL;
  if (status == HAL_OK) {
    for (char *cursor = target; *cursor != '\0'; ++cursor) {
      if (*cursor == '/')
        *cursor = JA_SEPARATOR;
    }
    status = jb_fs_native_path(full, &wide_link);
    if (status == HAL_OK)
      status = jb_fs_native_path(target, &wide_target);
    if (status != HAL_OK)
      status = ja_fail(state, status, 0u, "cannot convert the path");
  }
  if (status == HAL_OK && directory && !DeleteFileW(wide_link))
    status = ja_os_fail(state, ja_last_error(), "cannot replace the symbolic link");
  if (status == HAL_OK) {
    const DWORD flags = directory ? SYMBOLIC_LINK_FLAG_DIRECTORY : 0u;
    BOOL created = CreateSymbolicLinkW(wide_link, wide_target,
                                       flags | SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE);
    if (!created && GetLastError() == ERROR_INVALID_PARAMETER)
      created = CreateSymbolicLinkW(wide_link, wide_target, flags);
    error = created ? 0u : ja_last_error();
  }
  ja_release(wide_link);
  ja_release(wide_target);
  ja_release(target);
  if (status != HAL_OK)
    return status;
  if (error == (uint32_t)ERROR_PRIVILEGE_NOT_HELD)
    return ja_fail(state, HAL_EPERM, error,
                   "symbolic links need Developer Mode or the symbolic link privilege");
#else
  (void)directory;
  if (symlink(link->target, full) != 0)
    error = ja_last_error();
#endif
  if (error != 0u)
    return jb_fs_native_status(error) == HAL_EEXIST
               ? ja_fail(state, HAL_EEXIST, error, "name collides with an existing entry")
               : ja_os_fail(state, error, "cannot create the symbolic link");
  return HAL_OK;
}

#ifdef _WIN32
/* What a link names, relative to the destination with '/': its directory
 * less one component per leading "..", then the names. Validation allows
 * ".." only before the names and never above the destination. */
static hal_status_t ja_link_resolved(ja_state_t *state, const ja_link_t *link) {
  jb_string_t *out = &state->path;
  const char *slash = strrchr(link->path, '/');
  hal_status_t status =
      jb_string_assign(out, link->path, slash == NULL ? 0u : (size_t)(slash - link->path));
  const char *cursor = link->target, *part = NULL;
  size_t length = 0;
  while (status == HAL_OK && ja_component(&cursor, &part, &length)) {
    if (ja_dot(part, length))
      continue;
    if (ja_dot_dot(part, length)) {
      const char *text = jb_string_data(out), *last = strrchr(text, '/');
      status = jb_string_resize(out, last == NULL ? 0u : (size_t)(last - text), '\0');
      continue;
    }
    if (jb_string_size(out) > 0u)
      status = jb_string_append(out, "/", 1u);
    if (status == HAL_OK)
      status = jb_string_append(out, part, length);
  }
  return ja_memory(state, status);
}

/* Whether the link reaches a directory. The filesystem resolves the target
 * with its own name rules (case, short names) and follows links made by this
 * call; a missing target or a loop counts as a file. */
static hal_status_t ja_reaches_directory(ja_state_t *state, const ja_link_t *link,
                                         bool *directory) {
  *directory = false;
  hal_status_t status = ja_link_resolved(state, link);
  if (status != HAL_OK)
    return status;
  if (jb_string_size(&state->path) == 0u) {
    *directory = true; /* the destination itself */
    return HAL_OK;
  }
  const char *full = NULL;
  status = ja_full(state, jb_string_data(&state->path), jb_string_size(&state->path), &full);
  if (status != HAL_OK)
    return status;
  wchar_t *wide = NULL;
  status = jb_fs_native_path(full, &wide);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot convert the path");
  const HANDLE handle = CreateFileW(wide, FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, NULL);
  ja_release(wide);
  if (handle == INVALID_HANDLE_VALUE)
    return HAL_OK;
  BY_HANDLE_FILE_INFORMATION information;
  const BOOL known = GetFileInformationByHandle(handle, &information);
  CloseHandle(handle);
  *directory = known && (information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0u;
  return HAL_OK;
}
#endif

/* Windows needs the kind of the target when a link is created. Every link is
 * created as a file link first, so the filesystem can then resolve chains;
 * a link that reaches a directory is made again as a directory link. */
static hal_status_t ja_create_links(ja_state_t *state) {
  for (size_t index = 0; index < jb_vector_size(&state->links); ++index) {
    const ja_link_t *link = jb_vector_const_at(&state->links, index);
    ja_set_entry(state, link->path);
    const hal_status_t status = ja_make_link(state, link, false);
    if (status != HAL_OK)
      return status;
  }
#ifdef _WIN32
  for (size_t index = 0; index < jb_vector_size(&state->links); ++index) {
    const ja_link_t *link = jb_vector_const_at(&state->links, index);
    ja_set_entry(state, link->path);
    bool directory = false;
    hal_status_t status = ja_reaches_directory(state, link, &directory);
    if (status == HAL_OK && directory)
      status = ja_make_link(state, link, true);
    if (status != HAL_OK)
      return status;
  }
#endif
  return HAL_OK;
}

static voidpf ja_zalloc(voidpf opaque, uInt items, uInt size) {
  (void)opaque;
  if (size != 0u && items > SIZE_MAX / size)
    return Z_NULL;
  return ja_allocate((size_t)items * size);
}

static void ja_zfree(voidpf opaque, voidpf address) {
  (void)opaque;
  ja_release(address);
}

static hal_status_t ja_gzip_init(ja_state_t *state) {
  state->scratch = ja_allocate(JA_BLOCK);
  if (state->scratch == NULL)
    return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  state->gzip.zalloc = ja_zalloc;
  state->gzip.zfree = ja_zfree;
  state->gzip.opaque = Z_NULL;
  const int code = inflateInit2(&state->gzip, 15 + 16); /* gzip wrapper only */
  if (code == Z_MEM_ERROR)
    return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  if (code != Z_OK)
    return ja_fail(state, HAL_EINTERNAL, 0u, "cannot start the gzip check");
  state->gzip_open = true;
  return HAL_OK;
}

/* Feeds compressed bytes in file order. Each member must start with the gzip
 * magic, so data after the last member is refused; zlib compares CRC-32 and
 * length with the trailer when a member ends. */
static hal_status_t ja_gzip_feed(ja_state_t *state, const unsigned char *data, size_t length) {
  static const unsigned char magic[2] = {0x1Fu, 0x8Bu};
  z_stream *stream = &state->gzip;
  stream->next_in = data;
  stream->avail_in = (uInt)length; /* at most JA_BLOCK */
  while (stream->avail_in > 0u) {
    if (state->gzip_member_end) {
      if (inflateReset(stream) != Z_OK)
        return ja_fail(state, HAL_EINTERNAL, 0u, "cannot restart the gzip check");
      state->gzip_member_end = false;
      state->gzip_magic = 0;
    }
    for (size_t index = 0; state->gzip_magic < 2u && index < stream->avail_in; ++index) {
      if (stream->next_in[index] != magic[state->gzip_magic])
        return ja_fail(state, HAL_EPROTO, 0u,
                       state->gzip_members == 0u ? "archive is not gzip-compressed"
                                                 : "data follows the last gzip member");
      ++state->gzip_magic;
    }
    stream->next_out = state->scratch;
    stream->avail_out = JA_BLOCK;
    const int code = inflate(stream, Z_NO_FLUSH);
    if (code == Z_STREAM_END) {
      state->gzip_member_end = true;
      ++state->gzip_members;
    } else if (code == Z_MEM_ERROR) {
      return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
    } else if (code != Z_OK) {
      return ja_fail(state, HAL_EPROTO, 0u, "damaged gzip data: %s",
                     stream->msg != NULL ? stream->msg : "no progress");
    }
  }
  return HAL_OK;
}

/* One block of the archive file into state->block; count 0 at its end. */
static hal_status_t ja_read_block(ja_state_t *state, size_t *count) {
#ifdef _WIN32
  DWORD bytes = 0;
  if (!ReadFile(state->input, state->block, JA_BLOCK, &bytes, NULL))
    return ja_os_fail(state, ja_last_error(), "cannot read the archive file");
  *count = bytes;
  return HAL_OK;
#else
  for (;;) {
    const ssize_t bytes = read(state->input, state->block, JA_BLOCK);
    if (bytes >= 0) {
      *count = (size_t)bytes;
      return HAL_OK;
    }
    if (errno != EINTR)
      return ja_os_fail(state, ja_last_error(), "cannot read the archive file");
  }
#endif
}

/* libarchive may stop at the TAR end; the rest of the file must complete
 * the last gzip member. */
static hal_status_t ja_gzip_finish(ja_state_t *state) {
  for (;;) {
    if (ja_canceled(state))
      return ja_fail(state, HAL_ECANCELED, 0u, "canceled");
    size_t count = 0;
    hal_status_t status = ja_read_block(state, &count);
    if (status == HAL_OK && count == 0u)
      break;
    if (status == HAL_OK)
      status = ja_gzip_feed(state, state->block, count);
    if (status != HAL_OK)
      return status;
  }
  return state->gzip_member_end ? HAL_OK : ja_fail(state, HAL_EPROTO, 0u, "gzip data is truncated");
}

static la_ssize_t ja_read(struct archive *archive, void *context, const void **block) {
  ja_state_t *state = context;
  size_t count = 0;
  hal_status_t status = ja_read_block(state, &count);
  if (status == HAL_OK && state->gzip_open)
    status = ja_gzip_feed(state, state->block, count);
  if (status != HAL_OK) {
    state->input_status = status;
    archive_set_error(archive, EIO, "%s", state->result->message);
    return -1;
  }
  *block = state->block;
  return (la_ssize_t)count;
}

/* Only ZIP seeks; tar.gz is read once, in order, through the gzip check. */
static la_int64_t ja_seek(struct archive *archive, void *context, la_int64_t offset, int whence) {
  ja_state_t *state = context;
#ifdef _WIN32
  LARGE_INTEGER distance, position;
  distance.QuadPart = offset;
  const DWORD method =
      whence == SEEK_SET ? FILE_BEGIN : (whence == SEEK_CUR ? FILE_CURRENT : FILE_END);
  if (!SetFilePointerEx(state->input, distance, &position, method)) {
    state->input_status = ja_os_fail(state, ja_last_error(), "cannot seek in the archive file");
    archive_set_error(archive, EIO, "%s", state->result->message);
    return ARCHIVE_FATAL;
  }
  return (la_int64_t)position.QuadPart;
#else
  const off_t position = lseek(state->input, (off_t)offset, whence);
  if (position < 0) {
    state->input_status = ja_os_fail(state, ja_last_error(), "cannot seek in the archive file");
    archive_set_error(archive, EIO, "%s", state->result->message);
    return ARCHIVE_FATAL;
  }
  return (la_int64_t)position;
#endif
}

static hal_status_t ja_open_input(ja_state_t *state, const char *path) {
#ifdef _WIN32
  wchar_t *wide = NULL;
  const hal_status_t status = jb_fs_native_path(path, &wide);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot convert the archive path");
  state->input = CreateFileW(wide, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, NULL);
  const uint32_t error = state->input == INVALID_HANDLE_VALUE ? ja_last_error() : 0u;
  ja_release(wide);
  if (state->input == INVALID_HANDLE_VALUE)
    return ja_os_fail(state, error, "cannot open the archive file");
  if (GetFileType(state->input) != FILE_TYPE_DISK)
    return ja_fail(state, HAL_ETYPE, 0u, "archive is not a regular file");
#else
  state->input = open(path, O_RDONLY | O_CLOEXEC | O_NOCTTY | O_NONBLOCK);
  if (state->input < 0)
    return ja_os_fail(state, ja_last_error(), "cannot open the archive file");
  struct stat metadata;
  if (fstat(state->input, &metadata) != 0)
    return ja_os_fail(state, ja_last_error(), "cannot inspect the archive file");
  if (!S_ISREG(metadata.st_mode))
    return ja_fail(state, HAL_ETYPE, 0u, "archive is not a regular file");
#endif
  return HAL_OK;
}

static hal_status_t ja_open_archive(ja_state_t *state) {
  struct archive *archive = archive_read_new();
  state->archive = archive;
  if (archive == NULL)
    return ja_fail(state, HAL_ENOMEM, 0u, "out of memory");
  const bool zip = state->options->format == JB_ARCHIVE_ZIP;
  if (!zip) {
    const hal_status_t status = ja_gzip_init(state);
    if (status != HAL_OK)
      return status;
  }
  /* Only the requested reader and filter: no other format, no external program. */
  int code = zip ? archive_read_support_format_zip_seekable(archive)
                 : archive_read_support_format_tar(archive);
  if (code == ARCHIVE_OK && !zip)
    code = archive_read_support_filter_gzip(archive);
  if (code == ARCHIVE_OK)
    code = archive_read_set_format_option(archive, zip ? "zip" : "tar", "hdrcharset", "UTF-8");
  if (code == ARCHIVE_OK)
    code = archive_read_set_read_callback(archive, ja_read);
  if (code == ARCHIVE_OK && zip)
    code = archive_read_set_seek_callback(archive, ja_seek);
  if (code == ARCHIVE_OK)
    code = archive_read_set_callback_data(archive, state);
  if (code != ARCHIVE_OK)
    return ja_fail(state, HAL_EINTERNAL, 0u, "cannot configure the archive reader");
  /* The gzip check refuses any other input at its first bytes, so libarchive
   * never reads a TAR without the gzip filter. */
  return archive_read_open1(archive) == ARCHIVE_OK ? HAL_OK : ja_archive_fail(state);
}

static hal_status_t ja_count_entry(void *context, const char *name, const jb_fs_info_t *info) {
  (void)context;
  (void)name;
  (void)info;
  return HAL_EEXIST;
}

static hal_status_t ja_check_paths(ja_state_t *state, const char *archive,
                                   const char *destination) {
  bool absolute = false;
  hal_status_t status = jb_fs_path_is_absolute(destination, &absolute);
  if (status != HAL_OK || !absolute)
    return ja_fail(state, status != HAL_OK ? status : HAL_EINVAL, 0u,
                   "destination must be an absolute path");
  jb_fs_info_t info;
  status = jb_fs_status(destination, false, &info);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot inspect the destination");
  if (info.kind != JB_FS_DIRECTORY)
    return ja_fail(state, HAL_ETYPE, 0u, "destination is not a directory");
  status = jb_fs_list(destination, ja_count_entry, NULL);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u,
                   status == HAL_EEXIST ? "destination is not empty"
                                        : "cannot list the destination");
  status = jb_fs_status(archive, true, &info);
  if (status != HAL_OK)
    return ja_fail(state, status, 0u, "cannot inspect the archive file");
  if (info.kind != JB_FS_REGULAR)
    return ja_fail(state, HAL_ETYPE, 0u, "archive is not a regular file");
  status = jb_string_assign(&state->destination, destination, strlen(destination));
  if (status == HAL_OK && !ja_native_separator(destination[strlen(destination) - 1u])) {
    const char separator = JA_SEPARATOR;
    status = jb_string_append(&state->destination, &separator, 1u);
  }
  return ja_memory(state, status);
}

static hal_status_t ja_init(ja_state_t *state) {
  const jb_allocator_t *heap = jb_heap_allocator();
  hal_status_t status =
      jb_map_init(&state->entries, sizeof(ja_key_t), sizeof(ja_node_t), ja_compare, NULL, heap);
  if (status == HAL_OK)
    status = jb_vector_init(&state->links, sizeof(ja_link_t), heap);
  if (status == HAL_OK)
    status = jb_vector_init(&state->found, sizeof(bool), heap);
  if (status == HAL_OK)
    status = jb_vector_resize(&state->found, state->exclusions);
  if (status == HAL_OK)
    status = jb_string_init(&state->destination, heap);
  if (status == HAL_OK)
    status = jb_string_init(&state->full, heap);
  if (status == HAL_OK)
    status = jb_string_init(&state->path, heap);
  if (status == HAL_OK)
    status = jb_string_init(&state->other, heap);
  if (status == HAL_OK) {
    state->block = ja_allocate(JA_BLOCK);
    state->data = ja_allocate(JA_BLOCK);
    if (state->block == NULL || state->data == NULL)
      status = HAL_ENOMEM;
  }
  if (status == HAL_OK) {
    for (size_t index = 0; index < state->exclusions; ++index)
      *(bool *)jb_vector_at(&state->found, index) = false;
  }
  return ja_memory(state, status);
}

static void ja_destroy(ja_state_t *state) {
  if (state->archive != NULL)
    archive_read_free(state->archive);
#ifdef _WIN32
  if (state->input != INVALID_HANDLE_VALUE)
    CloseHandle(state->input);
#else
  if (state->input >= 0)
    close(state->input);
#endif
  jb_map_iter_t iterator;
  if (jb_map_first(&state->entries, &iterator) == HAL_OK) {
    do {
      const void *key = NULL, *value = NULL;
      if (jb_map_iter_get(&iterator, &key, &value) == HAL_OK)
        ja_release((void *)((const ja_key_t *)key)->data);
    } while (jb_map_next(&iterator) == HAL_OK);
  }
  jb_map_destroy(&state->entries);
  for (size_t index = 0; index < jb_vector_size(&state->links); ++index) {
    ja_link_t *link = jb_vector_at(&state->links, index);
    ja_release(link->path);
    ja_release(link->target);
  }
  jb_vector_destroy(&state->links);
  jb_vector_destroy(&state->found);
  jb_string_destroy(&state->destination);
  jb_string_destroy(&state->full);
  jb_string_destroy(&state->path);
  jb_string_destroy(&state->other);
  ja_release(state->block);
  ja_release(state->data);
  if (state->gzip_open)
    inflateEnd(&state->gzip);
  ja_release(state->scratch);
}

static hal_status_t ja_run(ja_state_t *state, const char *archive, const char *destination) {
  hal_status_t status = ja_init(state);
  if (status == HAL_OK)
    status = ja_check_paths(state, archive, destination);
  if (status == HAL_OK)
    status = ja_open_input(state, archive);
  if (status == HAL_OK)
    status = ja_open_archive(state);
  while (status == HAL_OK) {
    if (ja_canceled(state))
      return ja_fail(state, HAL_ECANCELED, 0u, "canceled");
    struct archive_entry *entry = NULL;
    const int code = archive_read_next_header(state->archive, &entry);
    if (code == ARCHIVE_EOF)
      break;
    /* A warning still returns the entry, e.g. a name that is not UTF-8. */
    if ((code == ARCHIVE_OK || code == ARCHIVE_WARN) && entry != NULL)
      ja_name_entry(state, entry);
    if (code != ARCHIVE_OK)
      return ja_archive_fail(state);
    if (++state->result->entries > state->options->max_entries)
      return ja_fail(state, HAL_EOVERFLOW, 0u, "archive has more than max_entries entries");
    status = ja_entry(state, entry);
  }
  if (status != HAL_OK)
    return status;
  state->result->entry[0] = '\0';
  if (state->gzip_open) {
    status = ja_gzip_finish(state);
    if (status != HAL_OK)
      return status;
  }
  for (size_t index = 0; index < state->exclusions; ++index) {
    if (!*(const bool *)jb_vector_const_at(&state->found, index)) {
      ja_set_entry(state, state->options->exclusions[index]);
      return ja_fail(state, HAL_ENOENT, 0u, "an exclusion matches no entry");
    }
  }
  status = ja_create_links(state);
  if (status == HAL_OK)
    state->result->entry[0] = '\0';
  return status;
}

hal_status_t jb_archive_extract(const char *archive, const char *destination,
                                const jb_archive_options_t *options, jb_archive_result_t *result) {
  if (result == NULL)
    return HAL_EINVAL;
  memset(result, 0, sizeof(*result));
  ja_state_t state;
  memset(&state, 0, sizeof(state));
  state.result = result;
  state.options = options;
#ifdef _WIN32
  state.input = INVALID_HANDLE_VALUE;
#else
  state.input = -1;
#endif
  state.input_status = HAL_OK; /* HAL_OK is not 0 */
  if (archive == NULL || destination == NULL || options == NULL || archive[0] == '\0' ||
      destination[0] == '\0' ||
      (options->format != JB_ARCHIVE_ZIP && options->format != JB_ARCHIVE_TAR_GZIP))
    return ja_fail(&state, HAL_EINVAL, 0u, "invalid argument");
  if (options->exclusions != NULL) {
    while (options->exclusions[state.exclusions] != NULL) {
      if (options->exclusions[state.exclusions][0] == '\0')
        return ja_fail(&state, HAL_EINVAL, 0u, "empty exclusion");
      ++state.exclusions;
    }
  }
#ifndef _WIN32
  locale_t utf8 = newlocale(LC_CTYPE_MASK, "C.UTF-8", (locale_t)0);
  if (utf8 == (locale_t)0)
    return ja_fail(&state, HAL_EUNSUPPORTED, ja_last_error(), "the C.UTF-8 locale is unavailable");
  const locale_t previous = uselocale(utf8);
#endif
  const hal_status_t status = ja_run(&state, archive, destination);
  ja_destroy(&state);
#ifndef _WIN32
  uselocale(previous);
  freelocale(utf8);
#endif
  return status;
}
