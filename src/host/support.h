#ifndef JB_HOST_SUPPORT_H
#define JB_HOST_SUPPORT_H
#include "jb/utf8.h"
#include <cstring>
#include <filesystem>
#include <new>
#include <stdexcept>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#endif

namespace jb_host {
namespace fs = std::filesystem;
struct failure {
  hal_status_t status;
  uint32_t native_error = 0;
};

inline hal_status_t status_for_generic(int value) {
  switch (static_cast<std::errc>(value)) {
  case std::errc::no_such_file_or_directory:
  case std::errc::not_a_directory:
    return HAL_ENOENT;
  case std::errc::permission_denied:
  case std::errc::operation_not_permitted:
    return HAL_EPERM;
  case std::errc::file_exists:
    return HAL_EEXIST;
  case std::errc::device_or_resource_busy:
  case std::errc::text_file_busy:
    return HAL_EBUSY;
  case std::errc::not_enough_memory:
  case std::errc::too_many_files_open:
  case std::errc::too_many_files_open_in_system:
    return HAL_ENOMEM;
  case std::errc::resource_unavailable_try_again:
    return HAL_EAGAIN;
  case std::errc::executable_format_error:
    return HAL_ETYPE;
  case std::errc::filename_too_long:
  case std::errc::value_too_large:
  case std::errc::argument_list_too_long:
    return HAL_EOVERFLOW;
  default:
    return HAL_EIO;
  }
}

inline hal_status_t status_for(const std::error_code &error) {
#ifdef _WIN32
  if (error.category() == std::system_category()) {
    switch (error.value()) {
    case ERROR_DIRECTORY:
      return HAL_ENOENT;
    case ERROR_BAD_EXE_FORMAT:
    case ERROR_EXE_MACHINE_TYPE_MISMATCH:
      return HAL_ETYPE;
    case ERROR_TOO_MANY_OPEN_FILES:
    case ERROR_NOT_ENOUGH_QUOTA:
      return HAL_ENOMEM;
    case ERROR_RETRY:
      return HAL_EAGAIN;
    default:
      break;
    }
  }
#endif
  const auto condition = error.default_error_condition();
  return condition.category() == std::generic_category() ? status_for_generic(condition.value())
                                                         : HAL_EIO;
}

[[noreturn]] inline void io_error() {
#ifdef _WIN32
  const DWORD code = GetLastError();
  if (code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION)
    throw failure{HAL_EBUSY, code};
  throw std::system_error(static_cast<int>(code), std::system_category());
#else
  const int code = errno;
  // Resource exhaustion must not require error-category virtual calls or
  // message allocation while the process has no descriptors left.
  throw failure{status_for_generic(code), static_cast<uint32_t>(code)};
#endif
}

template <typename F>
hal_status_t boundary(F operation, uint32_t *native_error = nullptr) noexcept {
  try {
    operation();
    return HAL_OK;
  } catch (const failure &error) {
    if (native_error != nullptr)
      *native_error = error.native_error;
    return error.status;
  } catch (const std::bad_alloc &) {
    return HAL_ENOMEM;
  } catch (const std::length_error &) {
    return HAL_EOVERFLOW;
  } catch (const std::system_error &error) {
    if (native_error != nullptr)
      *native_error = static_cast<uint32_t>(error.code().value());
    return status_for(error.code());
  } catch (...) {
    return HAL_EINTERNAL;
  }
}

inline void valid_text(const char *text) {
  if (text == nullptr)
    throw failure{HAL_EINVAL};
  size_t bad;
  const hal_status_t status = jb_utf8_validate(text, std::strlen(text), &bad);
  if (status != HAL_OK)
    throw failure{status};
}

inline fs::path path_from(const char *text) {
  valid_text(text);
  if (text[0] == '\0')
    throw failure{HAL_EINVAL};
  return fs::u8path(text);
}

} // namespace jb_host
#endif
