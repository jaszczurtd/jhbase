#include "jb/file.h"
#include "jb/utf8.h"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
namespace fs = std::filesystem;
struct failure {
  hal_status_t status;
};

hal_status_t status_for(const std::error_code &error) {
  if (error == std::errc::no_such_file_or_directory)
    return HAL_ENOENT;
  if (error == std::errc::permission_denied || error == std::errc::operation_not_permitted)
    return HAL_EPERM;
  if (error == std::errc::file_exists)
    return HAL_EEXIST;
  if (error == std::errc::device_or_resource_busy || error == std::errc::text_file_busy)
    return HAL_EBUSY;
  if (error == std::errc::not_enough_memory)
    return HAL_ENOMEM;
  if (error == std::errc::filename_too_long || error == std::errc::value_too_large)
    return HAL_EOVERFLOW;
  return HAL_EIO;
}

[[noreturn]] void io_error() {
#ifdef _WIN32
  const DWORD code = GetLastError();
  if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
    throw failure{HAL_EBUSY};
  throw std::system_error(static_cast<int>(code), std::system_category());
#else
  throw std::system_error(errno, std::generic_category());
#endif
}

template <typename F> hal_status_t boundary(F operation) noexcept {
  try {
    operation();
    return HAL_OK;
  } catch (const failure &error) {
    return error.status;
  } catch (const std::bad_alloc &) {
    return HAL_ENOMEM;
  } catch (const std::length_error &) {
    return HAL_EOVERFLOW;
  } catch (const std::system_error &error) {
    return status_for(error.code());
  } catch (...) {
    return HAL_EINTERNAL;
  }
}

fs::path path_from(const char *text) {
  if (text == nullptr || text[0] == '\0')
    throw failure{HAL_EINVAL};
  size_t bad;
  const hal_status_t status = jb_utf8_validate(text, std::strlen(text), &bad);
  if (status != HAL_OK)
    throw failure{status};
  return fs::u8path(text);
}

struct native_file {
#ifdef _WIN32
  HANDLE value = INVALID_HANDLE_VALUE;
  bool close() {
    const HANDLE old = value;
    value = INVALID_HANDLE_VALUE;
    return old == INVALID_HANDLE_VALUE || CloseHandle(old) != 0;
  }
#else
  int value = -1;
  bool close() {
    const int old = value;
    value = -1;
    // Do not retry close after EINTR: the descriptor may already be reused.
    return old == -1 || ::close(old) == 0;
  }
#endif
  ~native_file() { (void)close(); }
  native_file() = default;
  native_file(const native_file &) = delete;
  native_file &operator=(const native_file &) = delete;
};

size_t open_read(const fs::path &path, native_file &file) {
#ifdef _WIN32
  file.value = CreateFileW(
      path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
      OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
  if (file.value == INVALID_HANDLE_VALUE)
    io_error();
  if (GetFileType(file.value) != FILE_TYPE_DISK)
    throw failure{HAL_ETYPE};
  BY_HANDLE_FILE_INFORMATION info;
  if (!GetFileInformationByHandle(file.value, &info))
    io_error();
  if (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
    throw failure{HAL_ETYPE};
  const uint64_t size = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
#else
  file.value = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK);
  if (file.value == -1)
    io_error();
  struct stat info;
  if (::fstat(file.value, &info) != 0)
    io_error();
  if (!S_ISREG(info.st_mode))
    throw failure{HAL_ETYPE};
  if (info.st_size < 0)
    throw failure{HAL_EIO};
  const uint64_t size = static_cast<uint64_t>(info.st_size);
#endif
  if (size > SIZE_MAX)
    throw failure{HAL_EOVERFLOW};
  return static_cast<size_t>(size);
}

size_t read_chunk(native_file &file, unsigned char *data, size_t length) {
#ifdef _WIN32
  DWORD count = 0;
  if (!ReadFile(file.value, data, static_cast<DWORD>(length), &count, nullptr))
    io_error();
  return count;
#else
  ssize_t count;
  do {
    count = ::read(file.value, data, length);
  } while (count == -1 && errno == EINTR);
  if (count == -1)
    io_error();
  return static_cast<size_t>(count);
#endif
}

struct temporary_file {
  fs::path path;
  native_file file;
  ~temporary_file() {
    (void)file.close();
    if (!path.empty()) {
#ifdef _WIN32
      (void)DeleteFileW(path.c_str());
#else
      (void)::unlink(path.c_str());
#endif
    }
  }
};

void create_temporary(const fs::path &destination, temporary_file &temp) {
#ifdef _WIN32
  static std::atomic<unsigned long long> sequence{0};
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    const auto name = destination.filename().wstring() + L".tmp-" +
                      std::to_wstring(GetCurrentProcessId()) + L"-" +
                      std::to_wstring(sequence.fetch_add(1));
    const fs::path candidate = destination.parent_path() / name;
    temp.path = candidate;
    temp.file.value = CreateFileW(candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
    if (temp.file.value != INVALID_HANDLE_VALUE) {
      return;
    }
    const DWORD error = GetLastError();
    temp.path.clear();
    if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
      SetLastError(error);
      io_error();
    }
  }
  throw failure{HAL_EEXIST};
#else
  std::string name = destination.string() + ".tmp-XXXXXX";
  // Keep the generated name available if assigning the owned path fails.
  std::vector<char> writable(name.begin(), name.end());
  writable.push_back('\0');
  temp.file.value = ::mkostemp(writable.data(), O_CLOEXEC);
  if (temp.file.value == -1)
    io_error();
  try {
    temp.path = writable.data();
  } catch (...) {
    ::unlink(writable.data());
    throw;
  }
#endif
}

void write_complete(temporary_file &temp, const void *data, size_t length) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  size_t at = 0;
  while (at < length) {
    const size_t chunk = std::min(length - at, size_t{65536});
#ifdef _WIN32
    DWORD count;
    if (!WriteFile(temp.file.value, bytes + at, static_cast<DWORD>(chunk), &count, nullptr))
      io_error();
#else
    ssize_t count;
    do {
      count = ::write(temp.file.value, bytes + at, chunk);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
      io_error();
#endif
    if (count == 0)
      throw failure{HAL_EIO};
    at += static_cast<size_t>(count);
  }
#ifdef _WIN32
  if (!FlushFileBuffers(temp.file.value))
    io_error();
#else
  if (::fsync(temp.file.value) != 0)
    io_error();
#endif
  if (!temp.file.close())
    io_error();
}

bool same_file(const fs::path &path, const void *data, size_t length) {
  const auto kind = fs::symlink_status(path);
  if (!fs::exists(kind))
    return false;
#ifdef _WIN32
  const DWORD attributes = GetFileAttributesW(path.c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES)
    io_error();
  if (attributes & FILE_ATTRIBUTE_REPARSE_POINT)
    throw failure{HAL_ETYPE};
#endif
  if (!fs::is_regular_file(kind))
    throw failure{HAL_ETYPE};
  jb_file_data_t previous{};
  const std::string text = path.u8string();
  const hal_status_t status = jb_file_read(text.c_str(), length, &previous);
  if (status == HAL_EOVERFLOW)
    return false;
  if (status != HAL_OK)
    throw failure{status};
  const bool equal =
      previous.length == length && (length == 0 || std::memcmp(previous.data, data, length) == 0);
  jb_file_release(&previous);
  return equal;
}
} // namespace

extern "C" hal_status_t jb_file_read(const char *path, size_t max_bytes, jb_file_data_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  if (out->data != nullptr || out->length != 0)
    return HAL_ESTATE;
  return boundary([&] {
    native_file file;
    const size_t initial = open_read(path_from(path), file);
    if (initial > max_bytes)
      throw failure{HAL_EOVERFLOW};
    std::vector<unsigned char> bytes;
    bytes.reserve(initial);
    unsigned char chunk[4096];
    for (;;) {
      const size_t count = read_chunk(file, chunk, sizeof(chunk));
      if (count == 0)
        break;
      if (count > max_bytes - bytes.size())
        throw failure{HAL_EOVERFLOW};
      bytes.insert(bytes.end(), chunk, chunk + count);
    }
    if (!file.close())
      io_error();
    auto *data = static_cast<unsigned char *>(std::malloc(bytes.size()));
    if (data == nullptr && !bytes.empty())
      throw failure{HAL_ENOMEM};
    if (!bytes.empty())
      std::memcpy(data, bytes.data(), bytes.size());
    *out = {data, bytes.size()};
  });
}

extern "C" void jb_file_release(jb_file_data_t *data) {
  if (data != nullptr) {
    std::free(data->data);
    *data = {};
  }
}

extern "C" hal_status_t jb_file_write_atomic(const char *path, const void *data, size_t length,
                                             bool check_only, bool *changed) {
  if (changed == nullptr || (data == nullptr && length != 0))
    return HAL_EINVAL;
  return boundary([&] {
    const fs::path destination = path_from(path);
    if (same_file(destination, data, length)) {
      *changed = false;
      return;
    }
    if (check_only) {
      *changed = true;
      return;
    }
    const fs::path parent =
        destination.has_parent_path() ? destination.parent_path() : fs::path(".");
    fs::create_directories(parent);
    temporary_file temp;
    create_temporary(destination, temp);
#ifndef _WIN32
    struct stat info;
    if (::stat(destination.c_str(), &info) == 0) {
      if (::fchmod(temp.file.value, info.st_mode & 0777) != 0)
        io_error();
    } else if (errno != ENOENT) {
      io_error();
    }
#endif
    write_complete(temp, data, length);
#ifdef _WIN32
    if (!MoveFileExW(temp.path.c_str(), destination.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
      io_error();
#else
    native_file directory;
    directory.value = ::open(parent.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY);
    if (directory.value == -1)
      io_error();
    if (::rename(temp.path.c_str(), destination.c_str()) != 0)
      io_error();
#endif
    temp.path.clear();
    *changed = true;
#ifndef _WIN32
    if (::fsync(directory.value) != 0)
      io_error();
#endif
  });
}

extern "C" hal_status_t jb_path_resolve(const char *base, const char *path, char *buffer,
                                        size_t capacity, size_t *required) {
  if (required == nullptr || (buffer == nullptr && capacity != 0))
    return HAL_EINVAL;
  return boundary([&] {
    const fs::path root = path_from(base), part = path_from(path);
    if (!root.is_absolute() || (part.has_root_name() && !part.is_absolute()))
      throw failure{HAL_EINVAL};
    const std::string result = (root / part).lexically_normal().u8string();
    if (result.size() == SIZE_MAX)
      throw failure{HAL_EOVERFLOW};
    *required = result.size() + 1;
    if (buffer == nullptr)
      return;
    if (capacity < *required)
      throw failure{HAL_EOVERFLOW};
    std::memcpy(buffer, result.c_str(), *required);
  });
}
