#ifndef _WIN32
#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L
#define _XOPEN_SOURCE 700
#endif

#include "jb/filesystem.h"
#include "jb/file.h"
#include "jb/string.h"
#include "jb/utf8.h"
#include "jb/vector.h"
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#define FS_SEPARATOR '\\'
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#define FS_SEPARATOR '/'
#endif

typedef struct {
  size_t name, end, volume_end;
  bool directory, device, removable_namespace;
} fs_root_t;

static void *fs_allocate(size_t bytes) {
  const jb_allocator_t *allocator = jb_heap_allocator();
  return allocator->allocate(allocator->context, bytes);
}

static void fs_release(void *data) {
  if (data != NULL) {
    const jb_allocator_t *allocator = jb_heap_allocator();
    allocator->release(allocator->context, data);
  }
}

static bool fs_separator(char value) {
#ifdef _WIN32
  return value == '/' || value == '\\';
#else
  return value == '/';
#endif
}

static hal_status_t fs_text(const char *text) {
  if (text == NULL || text[0] == '\0')
    return HAL_EINVAL;
  return jb_utf8_validate(text, strlen(text), NULL);
}

static bool fs_overlap(const void *left, size_t left_size, const void *right, size_t right_size) {
  const uintptr_t a = (uintptr_t)left, b = (uintptr_t)right;
  return left != NULL && right != NULL && left_size != 0 && right_size != 0 &&
         (a <= b ? b - a < left_size : a - b < right_size);
}

static bool fs_output_valid(const char *buffer, size_t capacity, const size_t *required) {
  return required != NULL && (buffer != NULL || capacity == 0) &&
         !fs_overlap(buffer, capacity, required, sizeof(*required));
}

static bool fs_path_output_valid(const char *path, const char *buffer, size_t capacity,
                                 const size_t *required) {
  return fs_output_valid(buffer, capacity, required) &&
         (path == NULL || !fs_overlap(path, strlen(path) + 1, required, sizeof(*required)));
}

static hal_status_t fs_emit(const char *text, size_t length, char *buffer, size_t capacity,
                            size_t *required) {
  if (length == SIZE_MAX)
    return HAL_EOVERFLOW;
  *required = length + 1;
  if (buffer == NULL)
    return HAL_OK;
  if (capacity < length + 1)
    return HAL_EOVERFLOW;
  memmove(buffer, text, length);
  buffer[length] = '\0';
  return HAL_OK;
}

static hal_status_t fs_string(jb_string_t *string) {
  return jb_string_init(string, jb_heap_allocator());
}

static hal_status_t fs_native_append(jb_string_t *out, const char *text, size_t length) {
  size_t begin = 0;
  for (size_t i = 0; i < length; ++i) {
    if (fs_separator(text[i])) {
      hal_status_t status = jb_string_append(out, text + begin, i - begin);
      if (status == HAL_OK) {
        const char separator = FS_SEPARATOR;
        status = jb_string_append(out, &separator, 1);
      }
      if (status != HAL_OK)
        return status;
      begin = i + 1;
    }
  }
  return jb_string_append(out, text + begin, length - begin);
}

static hal_status_t fs_root(const char *path, fs_root_t *root) {
  hal_status_t status = fs_text(path);
  if (status != HAL_OK)
    return status;
  const size_t length = strlen(path);
  fs_root_t result = {0};
#ifdef _WIN32
  bool unc = false;
  if (length >= 2 && path[1] == ':' &&
      ((path[0] >= 'A' && path[0] <= 'Z') || (path[0] >= 'a' && path[0] <= 'z'))) {
    result.name = 2;
  } else if (length >= 4 && fs_separator(path[0]) && fs_separator(path[3]) &&
             (length == 4 || !fs_separator(path[4])) &&
             ((fs_separator(path[1]) && (path[2] == '?' || path[2] == '.')) ||
              (path[1] == '?' && path[2] == '?'))) {
    result.name = 3;
    result.device = true;
  } else if (length > 2 && fs_separator(path[0]) && fs_separator(path[1]) &&
             !fs_separator(path[2])) {
    unc = true;
    size_t end = 2;
    while (end < length && !fs_separator(path[end]))
      ++end;
    result.name = end;
  }
#endif
  result.end = result.name;
  if (result.end < length && fs_separator(path[result.end])) {
    result.directory = true;
    while (result.end < length && fs_separator(path[result.end]))
      ++result.end;
  }
  result.volume_end = result.end;
#ifdef _WIN32
  if (unc && result.directory) {
    while (result.volume_end < length && !fs_separator(path[result.volume_end]))
      ++result.volume_end;
  } else if (result.device && length >= 6 && path[5] == ':' &&
             (length == 6 || fs_separator(path[6]))) {
    result.removable_namespace = true;
    result.volume_end = length == 6 ? 6 : 7;
  } else if (result.device && length >= 11 && strncmp(path + 4, "Volume{", 7) == 0) {
    size_t end = 11;
    while (end < length && path[end] != '}' && !fs_separator(path[end]))
      ++end;
    if (end < length && path[end] == '}' && (end + 1 == length || fs_separator(path[end + 1]))) {
      result.removable_namespace = true;
      result.volume_end = end + 1;
    }
  } else if (result.device && length >= 8 && (path[4] == 'U' || path[4] == 'u') &&
             (path[5] == 'N' || path[5] == 'n') && (path[6] == 'C' || path[6] == 'c') &&
             fs_separator(path[7])) {
    size_t end = 8;
    for (unsigned part = 0; part < 2; ++part) {
      while (end < length && fs_separator(path[end]))
        ++end;
      while (end < length && !fs_separator(path[end]))
        ++end;
    }
    result.removable_namespace = true;
    result.volume_end = end;
  }
#endif
  *root = result;
  return HAL_OK;
}

static bool fs_absolute(fs_root_t root) {
#ifdef _WIN32
  return root.name >= 3 || (root.name == 2 && root.directory);
#else
  return root.directory;
#endif
}

static hal_status_t fs_normalized(const char *path, jb_string_t *result) {
  fs_root_t root;
  hal_status_t status = fs_root(path, &root);
  if (status != HAL_OK)
    return status;
#ifndef _WIN32
  if (root.directory && root.end == strlen(path))
    return jb_string_assign(result, path, root.end);
#endif
  jb_vector_t starts = {0};
  status = jb_vector_init(&starts, sizeof(size_t), jb_heap_allocator());
  if (status == HAL_OK)
    status = fs_native_append(result, path, root.name);
  if (status == HAL_OK && root.directory) {
    const char separator = FS_SEPARATOR;
    status = jb_string_append(result, &separator, 1);
  }
  const size_t prefix = jb_string_size(result);
  const size_t length = strlen(path);
  size_t offset = root.end;
  bool directory_tail = false;
  while (status == HAL_OK && offset < length) {
    const size_t begin = offset;
    while (offset < length && !fs_separator(path[offset]))
      ++offset;
    const size_t count = offset - begin;
    const bool dot = count == 1 && path[begin] == '.';
    const bool dotdot = count == 2 && path[begin] == '.' && path[begin + 1] == '.';
    const size_t components = jb_vector_size(&starts);
    if (dot) {
      directory_tail = true;
    } else if (dotdot && components != 0) {
      const size_t last = *(const size_t *)jb_vector_const_at(&starts, components - 1);
      const char *text = jb_string_data(result);
      const bool previous_dotdot =
          jb_string_size(result) - last == 2 && text[last] == '.' && text[last + 1] == '.';
      if (!previous_dotdot) {
        status = jb_string_resize(result, last == prefix ? prefix : last - 1, '\0');
        if (status == HAL_OK)
          status = jb_vector_erase(&starts, components - 1, 1);
        directory_tail = true;
      } else {
        directory_tail = false;
        const char separator = FS_SEPARATOR;
        status = jb_string_append(result, &separator, 1);
        const size_t start = jb_string_size(result);
        if (status == HAL_OK)
          status = jb_vector_append(&starts, &start, 1);
        if (status == HAL_OK)
          status = jb_string_append(result, path + begin, count);
      }
    } else if (dotdot && root.directory) {
      directory_tail = true;
    } else if (count != 0) {
      if (components != 0) {
        const char separator = FS_SEPARATOR;
        status = jb_string_append(result, &separator, 1);
      }
      const size_t start = jb_string_size(result);
      if (status == HAL_OK)
        status = jb_vector_append(&starts, &start, 1);
      if (status == HAL_OK)
        status = jb_string_append(result, path + begin, count);
      directory_tail = false;
    }
    if (offset < length) {
      while (offset < length && fs_separator(path[offset]))
        ++offset;
      if (offset == length)
        directory_tail = true;
    }
  }
  if (status == HAL_OK && jb_string_size(result) == 0)
    status = jb_string_append(result, ".", 1);
  else if (status == HAL_OK && directory_tail && jb_vector_size(&starts) != 0) {
    const size_t last = *(const size_t *)jb_vector_const_at(&starts, jb_vector_size(&starts) - 1);
    const char *text = jb_string_data(result);
    if (jb_string_size(result) - last != 2 || text[last] != '.' || text[last + 1] != '.') {
      const char separator = FS_SEPARATOR;
      status = jb_string_append(result, &separator, 1);
    }
  }
  jb_vector_destroy(&starts);
  return status;
}

static hal_status_t fs_joined(const char *base, const char *path, jb_string_t *out) {
  fs_root_t left = {0}, right = {0};
  hal_status_t status = fs_root(base, &left);
  if (status == HAL_OK)
    status = fs_root(path, &right);
  if (status != HAL_OK)
    return status;
  const bool different_root =
      right.name != 0 && (left.name != right.name || memcmp(base, path, left.name) != 0);
  if (fs_absolute(right) || different_root)
    return jb_string_assign(out, path, strlen(path));
  if (right.directory) {
    status = jb_string_assign(out, base, left.name);
  } else {
    const size_t length = strlen(base);
    status = jb_string_assign(out, base, length);
    if (status == HAL_OK && !fs_separator(base[length - 1]) &&
        (length != left.name || fs_absolute(left))) {
      const char separator = FS_SEPARATOR;
      status = jb_string_append(out, &separator, 1);
    }
  }
  if (status == HAL_OK)
    status = jb_string_append(out, path + right.name, strlen(path) - right.name);
  return status;
}

hal_status_t jb_fs_path_is_absolute(const char *path, bool *absolute) {
  if (absolute == NULL)
    return HAL_EINVAL;
  fs_root_t root;
  const hal_status_t status = fs_root(path, &root);
  if (status == HAL_OK)
    *absolute = fs_absolute(root);
  return status;
}

hal_status_t jb_fs_path_normalize(const char *path, char *buffer, size_t capacity,
                                  size_t *required) {
  if (!fs_path_output_valid(path, buffer, capacity, required))
    return HAL_EINVAL;
  jb_string_t text = {0};
  hal_status_t status = fs_string(&text);
  if (status == HAL_OK)
    status = fs_normalized(path, &text);
  if (status == HAL_OK)
    status = fs_emit(jb_string_data(&text), jb_string_size(&text), buffer, capacity, required);
  jb_string_destroy(&text);
  return status;
}

hal_status_t jb_fs_path_join(const char *base, const char *path, char *buffer, size_t capacity,
                             size_t *required) {
  if (!fs_path_output_valid(base, buffer, capacity, required) ||
      !fs_path_output_valid(path, buffer, capacity, required))
    return HAL_EINVAL;
  jb_string_t joined = {0};
  hal_status_t status = fs_string(&joined);
  if (status == HAL_OK)
    status = fs_joined(base, path, &joined);
  if (status == HAL_OK)
    status = jb_fs_path_normalize(jb_string_data(&joined), buffer, capacity, required);
  jb_string_destroy(&joined);
  return status;
}

hal_status_t jb_path_resolve(const char *base, const char *path, char *buffer, size_t capacity,
                             size_t *required) {
  if (!fs_path_output_valid(base, buffer, capacity, required) ||
      !fs_path_output_valid(path, buffer, capacity, required))
    return HAL_EINVAL;
  fs_root_t left = {0}, right = {0};
  hal_status_t status = fs_root(base, &left);
  if (status == HAL_OK)
    status = fs_root(path, &right);
  if (status != HAL_OK)
    return status;
  if (!fs_absolute(left) || (right.name != 0 && !fs_absolute(right)))
    return HAL_EINVAL;
#ifndef _WIN32
  if (left.end > 1 && left.end == strlen(base) && !right.directory) {
    jb_string_t joined = {0}, normalized = {0};
    status = fs_string(&joined);
    if (status == HAL_OK)
      status = fs_string(&normalized);
    if (status == HAL_OK)
      status = fs_joined(base, path, &joined);
    if (status == HAL_OK)
      status = fs_normalized(jb_string_data(&joined), &normalized);
    if (status == HAL_OK)
      status = jb_string_insert(&normalized, 0, base, left.end - 1);
    if (status == HAL_OK)
      status = fs_emit(jb_string_data(&normalized), jb_string_size(&normalized), buffer, capacity,
                       required);
    jb_string_destroy(&normalized);
    jb_string_destroy(&joined);
    return status;
  }
#endif
  return jb_fs_path_join(base, path, buffer, capacity, required);
}

static hal_status_t fs_component(const char *path, bool parent, char *buffer, size_t capacity,
                                 size_t *required) {
  if (!fs_path_output_valid(path, buffer, capacity, required))
    return HAL_EINVAL;
  fs_root_t root;
  hal_status_t status = fs_root(path, &root);
  if (status != HAL_OK)
    return status;
  const size_t length = strlen(path);
  size_t begin = length;
  while (begin > root.end && !fs_separator(path[begin - 1]))
    --begin;
  if (!parent)
    return fs_emit(path + begin, length - begin, buffer, capacity, required);
  size_t end = begin;
  while (end > root.end && fs_separator(path[end - 1]))
    --end;
#ifndef _WIN32
  if (end == root.end && root.directory && length > root.end)
    end = 1;
#endif
  jb_string_t text = {0};
  status = fs_string(&text);
  if (status == HAL_OK)
    status = fs_native_append(&text, path, end);
  if (status == HAL_OK)
    status = fs_emit(jb_string_data(&text), jb_string_size(&text), buffer, capacity, required);
  jb_string_destroy(&text);
  return status;
}

hal_status_t jb_fs_path_filename(const char *path, char *buffer, size_t capacity,
                                 size_t *required) {
  return fs_component(path, false, buffer, capacity, required);
}

hal_status_t jb_fs_path_parent(const char *path, char *buffer, size_t capacity, size_t *required) {
  return fs_component(path, true, buffer, capacity, required);
}

#ifdef _WIN32
static hal_status_t fs_error(DWORD code) {
  switch (code) {
  case ERROR_FILE_NOT_FOUND:
  case ERROR_PATH_NOT_FOUND:
  case ERROR_INVALID_DRIVE:
  case ERROR_DIRECTORY:
    return HAL_ENOENT;
  case ERROR_ACCESS_DENIED:
  case ERROR_PRIVILEGE_NOT_HELD:
  case ERROR_WRITE_PROTECT:
    return HAL_EPERM;
  case ERROR_ALREADY_EXISTS:
  case ERROR_FILE_EXISTS:
    return HAL_EEXIST;
  case ERROR_SHARING_VIOLATION:
  case ERROR_LOCK_VIOLATION:
  case ERROR_DIR_NOT_EMPTY:
  case ERROR_BUSY:
    return HAL_EBUSY;
  case ERROR_NOT_ENOUGH_MEMORY:
  case ERROR_OUTOFMEMORY:
  case ERROR_TOO_MANY_OPEN_FILES:
  case ERROR_NOT_ENOUGH_QUOTA:
    return HAL_ENOMEM;
  case ERROR_FILENAME_EXCED_RANGE:
  case ERROR_BUFFER_OVERFLOW:
    return HAL_EOVERFLOW;
  case ERROR_NO_UNICODE_TRANSLATION:
    return HAL_EPROTO;
  case ERROR_NOT_SAME_DEVICE:
  case ERROR_NOT_SUPPORTED:
    return HAL_EUNSUPPORTED;
  case ERROR_INVALID_NAME:
  case ERROR_INVALID_PARAMETER:
    return HAL_EINVAL;
  default:
    return HAL_EIO;
  }
}

static hal_status_t fs_wide(const char *path, wchar_t **out) {
  hal_status_t status = fs_text(path);
  if (status != HAL_OK)
    return status;
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, NULL, 0);
  if (count == 0)
    return fs_error(GetLastError());
  if ((size_t)count > SIZE_MAX / sizeof(wchar_t))
    return HAL_EOVERFLOW;
  wchar_t *wide = fs_allocate((size_t)count * sizeof(wchar_t));
  if (wide == NULL)
    return HAL_ENOMEM;
  if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, count) == 0) {
    status = fs_error(GetLastError());
    fs_release(wide);
    return status;
  }
  *out = wide;
  return HAL_OK;
}

static hal_status_t fs_utf8(const wchar_t *wide, char **out) {
  const int count =
      WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, NULL, 0, NULL, NULL);
  if (count == 0)
    return fs_error(GetLastError());
  char *text = fs_allocate((size_t)count);
  if (text == NULL)
    return HAL_ENOMEM;
  if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide, -1, text, count, NULL, NULL) == 0) {
    const hal_status_t status = fs_error(GetLastError());
    fs_release(text);
    return status;
  }
  *out = text;
  return HAL_OK;
}

static jb_fs_info_t fs_windows_info(DWORD attributes, DWORD high, DWORD low, bool links) {
  jb_fs_info_t info = {0};
  if (links && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
    info.kind = JB_FS_LINK;
  else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    info.kind = JB_FS_DIRECTORY;
  else if ((attributes & FILE_ATTRIBUTE_DEVICE) != 0)
    info.kind = JB_FS_OTHER;
  else {
    info.kind = JB_FS_REGULAR;
    info.size = ((uint64_t)high << 32u) | (uint64_t)low;
  }
  return info;
}

static hal_status_t fs_handle(const char *path, bool follow, HANDLE *out) {
  wchar_t *wide = NULL;
  hal_status_t status = fs_wide(path, &wide);
  if (status != HAL_OK)
    return status;
  HANDLE handle = CreateFileW(
      wide, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
      FILE_FLAG_BACKUP_SEMANTICS | (follow ? 0u : FILE_FLAG_OPEN_REPARSE_POINT), NULL);
  if (handle == INVALID_HANDLE_VALUE)
    status = fs_error(GetLastError());
  else
    *out = handle;
  fs_release(wide);
  return status;
}

static hal_status_t fs_close(HANDLE handle, hal_status_t status) {
  if (!CloseHandle(handle) && status == HAL_OK)
    return fs_error(GetLastError());
  return status;
}

static hal_status_t fs_handle_name(HANDLE handle, wchar_t **out) {
  DWORD count = GetFinalPathNameByHandleW(handle, NULL, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
  if (count == 0)
    return fs_error(GetLastError());
  for (;;) {
    if ((size_t)count > SIZE_MAX / sizeof(wchar_t))
      return HAL_EOVERFLOW;
    wchar_t *wide = fs_allocate((size_t)count * sizeof(wchar_t));
    if (wide == NULL)
      return HAL_ENOMEM;
    const DWORD written =
        GetFinalPathNameByHandleW(handle, wide, count, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (written != 0 && written < count) {
      *out = wide;
      return HAL_OK;
    }
    const DWORD error = GetLastError();
    fs_release(wide);
    if (written == 0)
      return fs_error(error);
    count = written;
  }
}
#else
static hal_status_t fs_error(int code) {
  switch (code) {
  case ENOENT:
  case ENOTDIR:
    return HAL_ENOENT;
  case EACCES:
  case EPERM:
  case EROFS:
    return HAL_EPERM;
  case EEXIST:
    return HAL_EEXIST;
  case EBUSY:
  case ETXTBSY:
  case ENOTEMPTY:
    return HAL_EBUSY;
  case ENOMEM:
  case EMFILE:
  case ENFILE:
    return HAL_ENOMEM;
  case EOVERFLOW:
  case ENAMETOOLONG:
    return HAL_EOVERFLOW;
  case EXDEV:
  case ENOTSUP:
    return HAL_EUNSUPPORTED;
  case EINVAL:
    return HAL_EINVAL;
  case EAGAIN:
    return HAL_EAGAIN;
  default:
    return HAL_EIO;
  }
}

static jb_fs_info_t fs_posix_info(const struct stat *metadata) {
  jb_fs_info_t info = {0};
  if (S_ISREG(metadata->st_mode)) {
    info.kind = JB_FS_REGULAR;
    info.size = (uint64_t)metadata->st_size;
  } else if (S_ISDIR(metadata->st_mode))
    info.kind = JB_FS_DIRECTORY;
  else if (S_ISLNK(metadata->st_mode))
    info.kind = JB_FS_LINK;
  else
    info.kind = JB_FS_OTHER;
  return info;
}
#endif

hal_status_t jb_fs_status(const char *path, bool follow_symlinks, jb_fs_info_t *out) {
  if (out == NULL)
    return HAL_EINVAL;
  hal_status_t status = fs_text(path);
  if (status != HAL_OK)
    return status;
  jb_string_t trimmed = {0};
  if (!follow_symlinks) {
    fs_root_t root;
    status = fs_root(path, &root);
    if (status == HAL_OK)
      status = fs_string(&trimmed);
    if (status == HAL_OK) {
      const size_t original_length = strlen(path);
      size_t length = original_length;
      while (length > root.volume_end && fs_separator(path[length - 1]))
        --length;
      if (root.device && root.removable_namespace && length == root.volume_end &&
          length < original_length && !fs_separator(path[length - 1]))
        ++length;
      status = jb_string_assign(&trimmed, path, length);
      if (status == HAL_OK)
        path = jb_string_data(&trimmed);
    }
    if (status != HAL_OK) {
      jb_string_destroy(&trimmed);
      return status;
    }
  }
  jb_fs_info_t info = {0};
#ifdef _WIN32
  if (follow_symlinks) {
    HANDLE handle;
    status = fs_handle(path, true, &handle);
    if (status == HAL_OK) {
      BY_HANDLE_FILE_INFORMATION metadata;
      if (!GetFileInformationByHandle(handle, &metadata))
        status = fs_error(GetLastError());
      else
        info = fs_windows_info(metadata.dwFileAttributes, metadata.nFileSizeHigh,
                               metadata.nFileSizeLow, false);
      status = fs_close(handle, status);
    }
  } else {
    wchar_t *wide = NULL;
    status = fs_wide(path, &wide);
    if (status == HAL_OK) {
      WIN32_FILE_ATTRIBUTE_DATA metadata;
      if (!GetFileAttributesExW(wide, GetFileExInfoStandard, &metadata))
        status = fs_error(GetLastError());
      else
        info = fs_windows_info(metadata.dwFileAttributes, metadata.nFileSizeHigh,
                               metadata.nFileSizeLow, true);
    }
    fs_release(wide);
  }
#else
  struct stat metadata;
  if ((follow_symlinks ? stat(path, &metadata) : lstat(path, &metadata)) != 0)
    status = fs_error(errno);
  else
    info = fs_posix_info(&metadata);
#endif
  if (status == HAL_OK)
    *out = info;
  jb_string_destroy(&trimmed);
  return status;
}

hal_status_t jb_fs_cwd(char *buffer, size_t capacity, size_t *required) {
  if (!fs_output_valid(buffer, capacity, required))
    return HAL_EINVAL;
  char *text = NULL;
  hal_status_t status = HAL_OK;
#ifdef _WIN32
  DWORD count = GetCurrentDirectoryW(0, NULL);
  if (count == 0)
    return fs_error(GetLastError());
  for (;;) {
    if ((size_t)count > SIZE_MAX / sizeof(wchar_t)) {
      status = HAL_EOVERFLOW;
      break;
    }
    wchar_t *wide = fs_allocate((size_t)count * sizeof(wchar_t));
    if (wide == NULL) {
      status = HAL_ENOMEM;
      break;
    }
    const DWORD written = GetCurrentDirectoryW(count, wide);
    if (written == 0)
      status = fs_error(GetLastError());
    else if (written < count)
      status = fs_utf8(wide, &text);
    fs_release(wide);
    if (written < count || status != HAL_OK)
      break;
    count = written;
  }
#else
  size_t count = 256;
  for (;;) {
    text = fs_allocate(count);
    if (text == NULL) {
      status = HAL_ENOMEM;
      break;
    }
    if (getcwd(text, count) != NULL)
      break;
    const int error = errno;
    fs_release(text);
    text = NULL;
    if (error != ERANGE) {
      status = fs_error(error);
      break;
    }
    if (count > SIZE_MAX / 2) {
      status = HAL_EOVERFLOW;
      break;
    }
    count *= 2;
  }
  if (status == HAL_OK)
    status = fs_text(text);
#endif
  if (status == HAL_OK)
    status = fs_emit(text, strlen(text), buffer, capacity, required);
  fs_release(text);
  return status;
}

hal_status_t jb_fs_realpath(const char *path, char *buffer, size_t capacity, size_t *required) {
  if (!fs_path_output_valid(path, buffer, capacity, required))
    return HAL_EINVAL;
  hal_status_t status = fs_text(path);
  if (status != HAL_OK)
    return status;
  char *text = NULL;
#ifdef _WIN32
  HANDLE handle;
  status = fs_handle(path, true, &handle);
  if (status != HAL_OK) {
    if (status == HAL_EOVERFLOW)
      *required = 0;
    return status;
  }
  wchar_t *wide = NULL;
  status = fs_handle_name(handle, &wide);
  if (status == HAL_OK)
    status = fs_utf8(wide, &text);
  fs_release(wide);
  status = fs_close(handle, status);
#else
  text = realpath(path, NULL);
  if (text == NULL)
    status = fs_error(errno);
  else
    status = fs_text(text);
#endif
  if (status == HAL_EOVERFLOW)
    *required = 0;
  if (status == HAL_OK)
    status = fs_emit(text, strlen(text), buffer, capacity, required);
#ifdef _WIN32
  fs_release(text);
#else
  /* realpath allocated this buffer through libc, not jb_heap_allocator. */
  free(text);
#endif
  return status;
}

static hal_status_t fs_equivalent(const char *left, const char *right, bool follow,
                                  bool *equivalent, bool *same_entry) {
  if (equivalent == NULL)
    return HAL_EINVAL;
  hal_status_t status = fs_text(left);
  if (status == HAL_OK)
    status = fs_text(right);
  if (status != HAL_OK)
    return status;
  bool result = false;
#ifdef _WIN32
  HANDLE a, b;
  status = fs_handle(left, follow, &a);
  if (status != HAL_OK)
    return status;
  status = fs_handle(right, follow, &b);
  if (status == HAL_OK) {
    FILE_ID_INFO ai, bi;
    if (!GetFileInformationByHandleEx(a, FileIdInfo, &ai, sizeof(ai)) ||
        !GetFileInformationByHandleEx(b, FileIdInfo, &bi, sizeof(bi)))
      status = fs_error(GetLastError());
    else
      result =
          ai.VolumeSerialNumber == bi.VolumeSerialNumber &&
          memcmp(ai.FileId.Identifier, bi.FileId.Identifier, sizeof(ai.FileId.Identifier)) == 0;
    if (status == HAL_OK && result && same_entry != NULL) {
      wchar_t *an = NULL, *bn = NULL;
      status = fs_handle_name(a, &an);
      if (status == HAL_OK)
        status = fs_handle_name(b, &bn);
      if (status == HAL_OK)
        *same_entry = wcscmp(an, bn) == 0;
      fs_release(bn);
      fs_release(an);
    }
    status = fs_close(b, status);
  }
  status = fs_close(a, status);
#else
  (void)same_entry;
  struct stat a, b;
  if ((follow ? stat(left, &a) : lstat(left, &a)) != 0 ||
      (follow ? stat(right, &b) : lstat(right, &b)) != 0)
    status = fs_error(errno);
  else
    result = a.st_dev == b.st_dev && a.st_ino == b.st_ino;
#endif
  if (status == HAL_OK)
    *equivalent = result;
  return status;
}

hal_status_t jb_fs_equivalent(const char *left, const char *right, bool *equivalent) {
  return fs_equivalent(left, right, true, equivalent, NULL);
}

hal_status_t jb_fs_list(const char *path, jb_fs_entry_fn entry, void *context) {
  if (entry == NULL)
    return HAL_EINVAL;
  hal_status_t status = fs_text(path);
  if (status != HAL_OK)
    return status;
#ifdef _WIN32
  jb_fs_info_t directory;
  status = jb_fs_status(path, true, &directory);
  if (status != HAL_OK)
    return status;
  if (directory.kind != JB_FS_DIRECTORY)
    return HAL_ETYPE;
  jb_string_t pattern = {0};
  wchar_t *wide = NULL;
  status = fs_string(&pattern);
  if (status == HAL_OK)
    status = fs_joined(path, "*", &pattern);
  if (status == HAL_OK)
    status = fs_wide(jb_string_data(&pattern), &wide);
  jb_string_destroy(&pattern);
  if (status != HAL_OK)
    return status;
  WIN32_FIND_DATAW found;
  HANDLE search = FindFirstFileW(wide, &found);
  const DWORD find_error = search == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
  fs_release(wide);
  if (search == INVALID_HANDLE_VALUE) {
    return find_error == ERROR_FILE_NOT_FOUND ? HAL_OK : fs_error(find_error);
  }
  for (;;) {
    if (wcscmp(found.cFileName, L".") != 0 && wcscmp(found.cFileName, L"..") != 0) {
      char *name = NULL;
      status = fs_utf8(found.cFileName, &name);
      if (status == HAL_OK) {
        const jb_fs_info_t info =
            fs_windows_info(found.dwFileAttributes, found.nFileSizeHigh, found.nFileSizeLow, true);
        status = entry(context, name, &info);
      }
      fs_release(name);
      if (status != HAL_OK)
        break;
    }
    if (!FindNextFileW(search, &found)) {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_FILES)
        status = fs_error(error);
      break;
    }
  }
  if (!FindClose(search) && status == HAL_OK)
    status = fs_error(GetLastError());
#else
  DIR *directory = opendir(path);
  if (directory == NULL)
    return errno == ENOTDIR ? HAL_ETYPE : fs_error(errno);
  for (;;) {
    errno = 0;
    const struct dirent *found = readdir(directory);
    if (found == NULL) {
      if (errno != 0)
        status = fs_error(errno);
      break;
    }
    if (strcmp(found->d_name, ".") == 0 || strcmp(found->d_name, "..") == 0)
      continue;
    status = fs_text(found->d_name);
    struct stat metadata;
    if (status == HAL_OK &&
        fstatat(dirfd(directory), found->d_name, &metadata, AT_SYMLINK_NOFOLLOW) != 0)
      status = fs_error(errno);
    if (status == HAL_OK) {
      const jb_fs_info_t info = fs_posix_info(&metadata);
      status = entry(context, found->d_name, &info);
    }
    if (status != HAL_OK)
      break;
  }
  if (closedir(directory) != 0 && status == HAL_OK)
    status = fs_error(errno);
#endif
  return status;
}

static hal_status_t fs_mkdir_one(const char *path) {
  jb_fs_info_t info;
  hal_status_t status = jb_fs_status(path, true, &info);
  if (status == HAL_OK)
    return info.kind == JB_FS_DIRECTORY ? HAL_OK : HAL_EEXIST;
  if (status != HAL_ENOENT)
    return status;
#ifdef _WIN32
  wchar_t *wide = NULL;
  status = fs_wide(path, &wide);
  if (status != HAL_OK)
    return status;
  if (CreateDirectoryW(wide, NULL))
    status = HAL_OK;
  else
    status = fs_error(GetLastError());
  fs_release(wide);
#else
  status = mkdir(path, 0777) == 0 ? HAL_OK : fs_error(errno);
#endif
  if (status == HAL_EEXIST) {
    const hal_status_t observed = jb_fs_status(path, true, &info);
    if (observed == HAL_OK && info.kind == JB_FS_DIRECTORY)
      return HAL_OK;
    if (observed != HAL_OK && observed != HAL_ENOENT)
      return observed;
  }
  return status;
}

hal_status_t jb_fs_mkdir(const char *path, bool parents) {
  fs_root_t root;
  hal_status_t status = fs_root(path, &root);
  if (status != HAL_OK)
    return status;
  if (!parents)
    return fs_mkdir_one(path);
  jb_string_t prefix = {0};
  status = fs_string(&prefix);
  const size_t length = strlen(path);
  size_t offset = root.volume_end;
  while (status == HAL_OK && offset < length) {
    while (offset < length && fs_separator(path[offset]))
      ++offset;
    while (offset < length && !fs_separator(path[offset]))
      ++offset;
    status = jb_string_assign(&prefix, path, offset);
    if (status == HAL_OK)
      status = fs_mkdir_one(jb_string_data(&prefix));
  }
  if (status == HAL_OK && offset == root.volume_end)
    status = fs_mkdir_one(path);
  jb_string_destroy(&prefix);
  return status;
}

#ifdef _WIN32
typedef struct {
  HANDLE handle, search;
  wchar_t *path;
  DWORD attributes;
  bool started;
} fs_remove_frame_t;

static void fs_remove_close(fs_remove_frame_t *frame) {
  if (frame->search != NULL)
    (void)FindClose(frame->search);
  if (frame->handle != NULL)
    (void)CloseHandle(frame->handle);
  fs_release(frame->path);
  *frame = (fs_remove_frame_t){0};
}

static hal_status_t fs_remove_open(wchar_t *path, bool trailing, fs_remove_frame_t *frame) {
  frame->path = path;
  /* Deny replacement and write handles that could change a reparse point. */
  frame->handle = CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES | FILE_WRITE_ATTRIBUTES,
                              FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (frame->handle == INVALID_HANDLE_VALUE && GetLastError() == ERROR_ACCESS_DENIED)
    frame->handle =
        CreateFileW(path, DELETE | FILE_READ_ATTRIBUTES, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                    FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, NULL);
  if (frame->handle == INVALID_HANDLE_VALUE) {
    frame->handle = NULL;
    return fs_error(GetLastError());
  }
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(frame->handle, &info))
    return fs_error(GetLastError());
  frame->attributes = info.dwFileAttributes;
  if (trailing &&
      (frame->attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0)
    return HAL_ETYPE;
  return HAL_OK;
}

static hal_status_t fs_wide_child(const wchar_t *parent, const wchar_t *name, wchar_t **out) {
  const size_t prefix = wcslen(parent), suffix = wcslen(name);
  if (prefix > SIZE_MAX / sizeof(wchar_t) - 2 || suffix > SIZE_MAX / sizeof(wchar_t) - 2 - prefix)
    return HAL_EOVERFLOW;
  wchar_t *path = fs_allocate((prefix + suffix + 2) * sizeof(wchar_t));
  if (path == NULL)
    return HAL_ENOMEM;
  memcpy(path, parent, prefix * sizeof(wchar_t));
  path[prefix] = L'\\';
  memcpy(path + prefix + 1, name, (suffix + 1) * sizeof(wchar_t));
  *out = path;
  return HAL_OK;
}

static hal_status_t fs_remove_next(fs_remove_frame_t *frame, fs_remove_frame_t *child) {
  if ((frame->attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) !=
      FILE_ATTRIBUTE_DIRECTORY)
    return HAL_NONE;
  WIN32_FIND_DATAW found;
  for (;;) {
    if (!frame->started) {
      wchar_t *pattern = NULL;
      hal_status_t status = fs_wide_child(frame->path, L"*", &pattern);
      if (status != HAL_OK)
        return status;
      frame->search = FindFirstFileW(pattern, &found);
      const DWORD error = GetLastError();
      fs_release(pattern);
      frame->started = true;
      if (frame->search == INVALID_HANDLE_VALUE) {
        frame->search = NULL;
        return error == ERROR_FILE_NOT_FOUND ? HAL_NONE : fs_error(error);
      }
    } else if (!FindNextFileW(frame->search, &found)) {
      const DWORD error = GetLastError();
      if (error != ERROR_NO_MORE_FILES)
        return fs_error(error);
      if (!FindClose(frame->search))
        return fs_error(GetLastError());
      frame->search = NULL;
      return HAL_NONE;
    }
    if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0)
      continue;
    wchar_t *path = NULL;
    hal_status_t status = fs_wide_child(frame->path, found.cFileName, &path);
    return status == HAL_OK ? fs_remove_open(path, false, child) : status;
  }
}

static hal_status_t fs_remove_finish(fs_remove_frame_t *frame) {
  FILE_BASIC_INFO attributes = {0};
  const bool readonly = (frame->attributes & FILE_ATTRIBUTE_READONLY) != 0;
  if (readonly) {
    attributes.FileAttributes = frame->attributes & ~(DWORD)FILE_ATTRIBUTE_READONLY;
    if (attributes.FileAttributes == 0)
      attributes.FileAttributes = FILE_ATTRIBUTE_NORMAL;
    if (!SetFileInformationByHandle(frame->handle, FileBasicInfo, &attributes, sizeof(attributes)))
      return fs_error(GetLastError());
  }
  FILE_DISPOSITION_INFO disposition = {TRUE};
  if (SetFileInformationByHandle(frame->handle, FileDispositionInfo, &disposition,
                                 sizeof(disposition)))
    return HAL_OK;
  const hal_status_t status = fs_error(GetLastError());
  if (readonly) {
    attributes.FileAttributes = frame->attributes;
    if (!SetFileInformationByHandle(frame->handle, FileBasicInfo, &attributes, sizeof(attributes)))
      return fs_error(GetLastError());
  }
  return status;
}
#else
typedef struct {
  DIR *directory;
  int parent;
  char *name;
} fs_remove_frame_t;

static void fs_remove_close(fs_remove_frame_t *frame) {
  if (frame->directory != NULL)
    (void)closedir(frame->directory);
  fs_release(frame->name);
  *frame = (fs_remove_frame_t){0};
}

static hal_status_t fs_remove_open(int parent, const char *name, bool trailing,
                                   fs_remove_frame_t *frame) {
  frame->parent = parent;
  const size_t length = strlen(name) + 1;
  frame->name = fs_allocate(length);
  if (frame->name == NULL)
    return HAL_ENOMEM;
  memcpy(frame->name, name, length);
  const int fd = openat(parent, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) {
    if (errno != ENOTDIR && errno != ELOOP)
      return fs_error(errno);
    struct stat info;
    if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
      return fs_error(errno);
    if (S_ISDIR(info.st_mode))
      return HAL_EAGAIN;
    return trailing && !S_ISLNK(info.st_mode) ? HAL_ETYPE : HAL_OK;
  }
  frame->directory = fdopendir(fd);
  if (frame->directory == NULL) {
    const hal_status_t status = fs_error(errno);
    (void)close(fd);
    return status;
  }
  return HAL_OK;
}

static hal_status_t fs_remove_next(fs_remove_frame_t *frame, fs_remove_frame_t *child) {
  if (frame->directory == NULL)
    return HAL_NONE;
  for (;;) {
    errno = 0;
    const struct dirent *entry = readdir(frame->directory);
    if (entry == NULL)
      return errno == 0 ? HAL_NONE : fs_error(errno);
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
      continue;
    return fs_remove_open(dirfd(frame->directory), entry->d_name, false, child);
  }
}

static hal_status_t fs_remove_finish(fs_remove_frame_t *frame) {
  int flags = 0;
  if (frame->directory != NULL) {
    struct stat opened, current;
    if (fstat(dirfd(frame->directory), &opened) != 0 ||
        fstatat(frame->parent, frame->name, &current, AT_SYMLINK_NOFOLLOW) != 0)
      return fs_error(errno);
    if (opened.st_dev != current.st_dev || opened.st_ino != current.st_ino)
      return HAL_EBUSY;
    flags = AT_REMOVEDIR;
  }
  return unlinkat(frame->parent, frame->name, flags) == 0 ? HAL_OK : fs_error(errno);
}

static hal_status_t fs_remove_single(int parent, const char *name, bool trailing) {
  struct stat info;
  if (fstatat(parent, name, &info, AT_SYMLINK_NOFOLLOW) != 0)
    return fs_error(errno);
  if (trailing && !S_ISDIR(info.st_mode) && !S_ISLNK(info.st_mode))
    return HAL_ETYPE;
  return unlinkat(parent, name, S_ISDIR(info.st_mode) ? AT_REMOVEDIR : 0) == 0 ? HAL_OK
                                                                               : fs_error(errno);
}
#endif

/* Owners in frames contain only handles/pointers, never embedded containers. */
static hal_status_t fs_remove_walk(fs_remove_frame_t *first, bool recursive) {
  jb_vector_t frames = {0};
  hal_status_t status = jb_vector_init(&frames, sizeof(*first), jb_heap_allocator());
  if (status == HAL_OK)
    status = jb_vector_append(&frames, first, 1);
  if (status == HAL_OK)
    *first = (fs_remove_frame_t){0};
  fs_remove_close(first);
  while (status == HAL_OK && jb_vector_size(&frames) != 0) {
    const size_t index = jb_vector_size(&frames) - 1;
    fs_remove_frame_t *frame = jb_vector_at(&frames, index);
    fs_remove_frame_t child = {0};
    status = recursive ? fs_remove_next(frame, &child) : HAL_NONE;
    if (status == HAL_NONE) {
      status = fs_remove_finish(frame);
      fs_remove_close(frame);
      (void)jb_vector_erase(&frames, index, 1);
    } else if (status == HAL_OK) {
      status = jb_vector_append(&frames, &child, 1);
      if (status == HAL_OK)
        child = (fs_remove_frame_t){0};
    }
    fs_remove_close(&child);
  }
  for (size_t count = jb_vector_size(&frames); count != 0; --count)
    fs_remove_close(jb_vector_at(&frames, count - 1));
  jb_vector_destroy(&frames);
  return status;
}

hal_status_t jb_fs_remove(const char *path, bool recursive) {
  fs_root_t root;
  hal_status_t status = fs_root(path, &root);
  if (status != HAL_OK)
    return status;
  if (root.device && !root.removable_namespace)
    return HAL_EUNSUPPORTED;
  size_t length = strlen(path);
#ifdef _WIN32
  for (size_t i = 0; i < length; ++i) {
    const bool drive_colon = (root.name == 2 && i == 1) ||
                             (root.device && root.volume_end >= 6 && i == 5 && path[5] == ':');
    if (path[i] == ':' && !drive_colon)
      return HAL_EUNSUPPORTED;
  }
#endif
  while (length > root.volume_end && fs_separator(path[length - 1]))
    --length;
  if (length <= root.volume_end)
    return HAL_EINVAL;
  size_t last = length;
  while (last > root.end && !fs_separator(path[last - 1]))
    --last;
#ifdef _WIN32
  const bool literal_namespace =
      length >= 4 && path[0] == '\\' && path[1] == '\\' && path[2] == '?' && path[3] == '\\';
  if (!literal_namespace) {
    bool dots_or_spaces = true;
    for (size_t i = last; i < length; ++i) {
      if (path[i] != '.' && path[i] != ' ') {
        dots_or_spaces = false;
        break;
      }
    }
    if (dots_or_spaces)
      return HAL_EINVAL;
  }
#endif
  if ((length - last == 1 && path[last] == '.') ||
      (length - last == 2 && path[last] == '.' && path[last + 1] == '.'))
    return HAL_EINVAL;
  jb_string_t trimmed = {0};
  status = fs_string(&trimmed);
  if (status == HAL_OK)
    status = jb_string_assign(&trimmed, path, length);
  if (status != HAL_OK) {
    jb_string_destroy(&trimmed);
    return status;
  }
  const bool trailing = path[length] != '\0';
  fs_remove_frame_t first = {0};
#ifdef _WIN32
  const bool walk = true;
  wchar_t *wide = NULL;
  status = fs_wide(jb_string_data(&trimmed), &wide);
  if (status == HAL_OK)
    status = fs_remove_open(wide, trailing, &first);
  if (status == HAL_OK) {
    wchar_t *literal = NULL;
    status = fs_handle_name(first.handle, &literal);
    if (status == HAL_OK) {
      fs_release(first.path);
      first.path = literal;
    }
  }
#else
  const bool walk = recursive;
  /* Only the caller's parent path is resolved by name. Descendants use dirfds. */
  int parent = -1;
  const char *name = jb_string_data(&trimmed) + last;
  if (last == 0)
    parent = open(".", O_PATH | O_DIRECTORY | O_CLOEXEC);
  else {
    char *parent_path = jb_string_mutable_data(&trimmed);
    parent_path[last - 1] = '\0';
    parent = open(last == 1 ? "/" : parent_path, O_PATH | O_DIRECTORY | O_CLOEXEC);
  }
  if (parent < 0)
    status = fs_error(errno);
  else if (!recursive)
    status = fs_remove_single(parent, name, trailing);
  else
    status = fs_remove_open(parent, name, trailing, &first);
#endif
  if (status == HAL_OK && walk)
    status = fs_remove_walk(&first, recursive);
  else
    fs_remove_close(&first);
#ifndef _WIN32
  if (parent >= 0 && close(parent) != 0 && status == HAL_OK)
    status = fs_error(errno);
#endif
  jb_string_destroy(&trimmed);
  return status;
}

hal_status_t jb_fs_rename(const char *from, const char *to) {
  jb_fs_info_t source, destination;
  hal_status_t status = jb_fs_status(from, false, &source);
  if (status != HAL_OK)
    return status;
  status = jb_fs_status(to, false, &destination);
  if (status == HAL_OK) {
    if (destination.kind == JB_FS_LINK && source.kind != JB_FS_LINK)
      return HAL_ETYPE;
    bool same, same_entry = false;
    status = fs_equivalent(from, to, false, &same, &same_entry);
    if (status != HAL_OK)
      return status;
#ifdef _WIN32
    /* Distinct hard links remain a no-op; one entry may still change case. */
    if (same && !same_entry)
      return HAL_OK;
#endif
    if (!same && destination.kind != JB_FS_REGULAR)
      return destination.kind == JB_FS_DIRECTORY ? HAL_EEXIST : HAL_ETYPE;
    if (!same && source.kind == JB_FS_DIRECTORY)
      return HAL_ETYPE;
  } else if (status != HAL_ENOENT) {
    return status;
  }
#ifdef _WIN32
  wchar_t *a = NULL, *b = NULL;
  status = fs_wide(from, &a);
  if (status == HAL_OK)
    status = fs_wide(to, &b);
  if (status == HAL_OK && !MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING))
    status = fs_error(GetLastError());
  fs_release(b);
  fs_release(a);
#else
  status = rename(from, to) == 0 ? HAL_OK : fs_error(errno);
#endif
  return status;
}
