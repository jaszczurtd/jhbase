#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif
#include "jb/file.h"
#include "jb/utf8.h"
#include "support.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#include <sddl.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace {
using namespace jb_host;
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

uint64_t open_read(const fs::path &path, native_file &file) {
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
  return size;
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

#ifdef _WIN32
std::wstring temporary_suffix() {
  unsigned char random[16];
  if (BCryptGenRandom(nullptr, random, sizeof(random), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
    throw failure{HAL_EIO};
  constexpr wchar_t hex[] = L"0123456789abcdef";
  std::wstring result;
  result.reserve(2 * sizeof(random));
  for (unsigned char byte : random) {
    result.push_back(hex[byte >> 4]);
    result.push_back(hex[byte & 15]);
  }
  return result;
}
#endif

void create_temporary(const fs::path &destination, temporary_file &temp) {
#ifdef _WIN32
  for (unsigned attempt = 0; attempt < 64; ++attempt) {
    const auto name = destination.filename().wstring() + L".tmp-" + temporary_suffix();
    const fs::path candidate = destination.parent_path() / name;
    temp.path = candidate;
    temp.file.value = CreateFileW(candidate.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
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

void write_bytes(native_file &file, const void *data, size_t length) {
  const auto *bytes = static_cast<const unsigned char *>(data);
  size_t at = 0;
  while (at < length) {
    const size_t chunk = std::min(length - at, size_t{65536});
#ifdef _WIN32
    DWORD count;
    if (!WriteFile(file.value, bytes + at, static_cast<DWORD>(chunk), &count, nullptr))
      io_error();
#else
    ssize_t count;
    do {
      count = ::write(file.value, bytes + at, chunk);
    } while (count == -1 && errno == EINTR);
    if (count == -1)
      io_error();
#endif
    if (count == 0)
      throw failure{HAL_EIO};
    at += static_cast<size_t>(count);
  }
}

void flush_and_close(temporary_file &temp) {
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

void create_replacement(const fs::path &destination, temporary_file &temp) {
  const fs::path parent = destination.has_parent_path() ? destination.parent_path() : fs::path(".");
  fs::create_directories(parent);
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
}

void publish_temporary(temporary_file &temp, const fs::path &destination, bool *changed) {
#ifdef _WIN32
  if (!MoveFileExW(temp.path.c_str(), destination.c_str(),
                   MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
    io_error();
#else
  const auto parent = destination.has_parent_path() ? destination.parent_path() : fs::path(".");
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
}

bool regular_destination(const fs::path &path) {
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
  return true;
}

bool same_file(const fs::path &path, const void *data, size_t length) {
  if (!regular_destination(path))
    return false;
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
fs::path absolute_path(const char *path) {
  auto result = path_from(path);
  if (!result.is_absolute())
    throw failure{HAL_EINVAL};
  return result;
}

void remove_temporary(temporary_file &temp) {
  if (!temp.file.close())
    io_error();
  if (!temp.path.empty()) {
    fs::remove(temp.path);
    temp.path.clear();
  }
}

bool same_paths(const fs::path &destination, const fs::path &temporary) {
  if (!regular_destination(destination))
    return false;
  native_file left, right;
  if (open_read(destination, left) != open_read(temporary, right))
    return false;
  unsigned char a[4096], b[4096];
  for (;;) {
    const auto n = read_chunk(left, a, sizeof(a));
    size_t offset = 0;
    while (offset < n) {
      const auto m = read_chunk(right, b + offset, n - offset);
      if (m == 0)
        return false;
      offset += m;
    }
    if (n == 0)
      return read_chunk(right, b, 1) == 0;
    if (std::memcmp(a, b, n) != 0)
      return false;
  }
}

#ifdef _WIN32
using local_memory = std::unique_ptr<void, decltype(&LocalFree)>;
local_memory private_directory_security() {
  native_file token;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
    io_error();
  DWORD bytes = 0;
  GetTokenInformation(token.value, TokenUser, nullptr, 0, &bytes);
  if (bytes == 0)
    io_error();
  std::vector<unsigned char> buffer(bytes);
  if (!GetTokenInformation(token.value, TokenUser, buffer.data(), bytes, &bytes))
    io_error();
  LPWSTR sid_text = nullptr;
  if (!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER *>(buffer.data())->User.Sid, &sid_text))
    io_error();
  local_memory sid(sid_text, LocalFree);
  const std::wstring sddl = L"D:P(A;OICI;FA;;;" + std::wstring(sid_text) + L")";
  PSECURITY_DESCRIPTOR descriptor = nullptr;
  if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1,
                                                            &descriptor, nullptr))
    io_error();
  return local_memory(descriptor, LocalFree);
}
#endif
} // namespace

extern "C" hal_status_t jb_file_read(const char *path, size_t max_bytes, jb_file_data_t *out) {
  if (out == nullptr)
    return HAL_EINVAL;
  if (out->data != nullptr || out->length != 0)
    return HAL_ESTATE;
  return boundary([&] {
    native_file file;
    const uint64_t initial = open_read(path_from(path), file);
    if (initial > max_bytes)
      throw failure{HAL_EOVERFLOW};
    std::vector<unsigned char> bytes;
    bytes.reserve(static_cast<size_t>(initial));
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
    temporary_file temp;
    create_replacement(destination, temp);
    write_bytes(temp.file, data, length);
    flush_and_close(temp);
    publish_temporary(temp, destination, changed);
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

struct jb_file_writer {
  fs::path destination;
  temporary_file temp;
  uint64_t limit = 0, written = 0;
  bool failed = false;
};
struct jb_file_lock {
  native_file file;
};
struct jb_temp_dir {
  fs::path path;
  std::string text;
};

extern "C" hal_status_t jb_file_writer_begin(const char *path, uint64_t max_bytes,
                                             jb_file_writer_t **out) {
  if (out == nullptr)
    return HAL_EINVAL;
  if (*out != nullptr)
    return HAL_ESTATE;
  return boundary([&] {
    auto owner = std::make_unique<jb_file_writer>();
    owner->destination = absolute_path(path);
    (void)regular_destination(owner->destination);
    owner->limit = max_bytes;
    create_replacement(owner->destination, owner->temp);
    *out = owner.release();
  });
}

extern "C" hal_status_t jb_file_writer_write(jb_file_writer_t *writer, const void *data,
                                             size_t length) {
  if (writer == nullptr)
    return HAL_EINVAL;
  if (writer->failed)
    return HAL_ESTATE;
  const auto status = boundary([&] {
    if (data == nullptr && length != 0)
      throw failure{HAL_EINVAL};
    if (length > writer->limit - writer->written)
      throw failure{HAL_EOVERFLOW};
    write_bytes(writer->temp.file, data, length);
    writer->written += length;
  });
  if (status != HAL_OK)
    writer->failed = true;
  return status;
}

extern "C" hal_status_t jb_file_writer_commit(jb_file_writer_t **writer, bool *changed) {
  if (writer == nullptr || *writer == nullptr || changed == nullptr)
    return HAL_EINVAL;
  std::unique_ptr<jb_file_writer> owner(*writer);
  *writer = nullptr;
  const auto status = boundary([&] {
    if (owner->failed)
      throw failure{HAL_ESTATE};
    flush_and_close(owner->temp);
    if (same_paths(owner->destination, owner->temp.path)) {
      remove_temporary(owner->temp);
      *changed = false;
    } else {
      publish_temporary(owner->temp, owner->destination, changed);
    }
  });
  // Report cleanup failures even when publication was refused before rename.
  const auto cleanup = boundary([&] { remove_temporary(owner->temp); });
  return status != HAL_OK ? status : cleanup;
}

extern "C" hal_status_t jb_file_writer_abort(jb_file_writer_t **writer) {
  if (writer == nullptr || *writer == nullptr)
    return HAL_OK;
  std::unique_ptr<jb_file_writer> owner(*writer);
  *writer = nullptr;
  return boundary([&] { remove_temporary(owner->temp); });
}

extern "C" hal_status_t jb_file_lock_acquire(const char *path, uint32_t timeout_ms,
                                             jb_cancel_fn canceled, void *context,
                                             jb_file_lock_t **out) {
  if (out == nullptr)
    return HAL_EINVAL;
  if (*out != nullptr)
    return HAL_ESTATE;
  return boundary([&] {
    const auto started = std::chrono::steady_clock::now();
    if (canceled != nullptr && canceled(context))
      throw failure{HAL_ECANCELED};
    const auto name = absolute_path(path);
    (void)regular_destination(name);
    auto owner = std::make_unique<jb_file_lock>();
#ifdef _WIN32
    owner->file.value =
        CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                    nullptr, OPEN_ALWAYS, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (owner->file.value == INVALID_HANDLE_VALUE)
      io_error();
    BY_HANDLE_FILE_INFORMATION info;
    if (!GetFileInformationByHandle(owner->file.value, &info))
      io_error();
    if (GetFileType(owner->file.value) != FILE_TYPE_DISK ||
        (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)))
      throw failure{HAL_ETYPE};
#else
    owner->file.value =
        ::open(name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NONBLOCK | O_NOFOLLOW, 0600);
    if (owner->file.value < 0) {
      if (errno == ELOOP)
        throw failure{HAL_ETYPE};
      io_error();
    }
    struct stat info;
    if (::fstat(owner->file.value, &info) != 0)
      io_error();
    if (!S_ISREG(info.st_mode))
      throw failure{HAL_ETYPE};
#endif
    bool first_attempt = true;
    for (;;) {
      if (canceled != nullptr && canceled(context))
        throw failure{HAL_ECANCELED};
      if (!first_attempt && timeout_ms != 0 && timeout_ms != UINT32_MAX &&
          std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() -
                                                                started)
                  .count() >= timeout_ms)
        throw failure{HAL_ETIMEOUT};
      first_attempt = false;
#ifdef _WIN32
      OVERLAPPED position{};
      if (LockFileEx(owner->file.value, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1,
                     0, &position))
        break;
      if (GetLastError() != ERROR_LOCK_VIOLATION)
        io_error();
#else
      if (::flock(owner->file.value, LOCK_EX | LOCK_NB) == 0)
        break;
      if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        io_error();
#endif
      if (timeout_ms == 0)
        throw failure{HAL_EBUSY};
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    *out = owner.release();
  });
}

extern "C" hal_status_t jb_file_lock_release(jb_file_lock_t **lock) {
  if (lock == nullptr || *lock == nullptr)
    return HAL_OK;
  std::unique_ptr<jb_file_lock> owner(*lock);
  *lock = nullptr;
  return boundary([&] {
    if (!owner->file.close())
      io_error();
  });
}

extern "C" hal_status_t jb_temp_dir_create(const char *parent, jb_temp_dir_t **out) {
  if (out == nullptr)
    return HAL_EINVAL;
  if (*out != nullptr)
    return HAL_ESTATE;
  return boundary([&] {
    auto owner = std::make_unique<jb_temp_dir>();
    const auto base = absolute_path(parent);
#ifdef _WIN32
    auto descriptor = private_directory_security();
    SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), descriptor.get(), FALSE};
    for (unsigned attempt = 0; attempt < 64; ++attempt) {
      owner->path = base / (L".jb-dir-" + temporary_suffix());
      owner->text = owner->path.u8string();
      if (CreateDirectoryW(owner->path.c_str(), &security)) {
        *out = owner.release();
        return;
      }
      if (GetLastError() != ERROR_ALREADY_EXISTS)
        io_error();
    }
    throw failure{HAL_EEXIST};
#else
    const std::string pattern = (base / ".jb-dir-XXXXXX").string();
    std::vector<char> name(pattern.begin(), pattern.end());
    name.push_back(0);
    if (::mkdtemp(name.data()) == nullptr)
      io_error();
    try {
      if (::chmod(name.data(), 0700) != 0)
        io_error();
      owner->path = name.data();
      owner->text = owner->path.u8string();
    } catch (...) {
      ::rmdir(name.data());
      throw;
    }
    *out = owner.release();
#endif
  });
}

extern "C" const char *jb_temp_dir_path(const jb_temp_dir_t *directory) {
  return directory == nullptr ? nullptr : directory->text.c_str();
}

extern "C" hal_status_t jb_temp_dir_remove(jb_temp_dir_t **directory) {
  if (directory == nullptr || *directory == nullptr)
    return HAL_OK;
  const auto status = boundary([&] { fs::remove_all((*directory)->path); });
  if (status == HAL_OK) {
    delete *directory;
    *directory = nullptr;
  }
  return status;
}
