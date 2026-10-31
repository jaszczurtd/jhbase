// Include C++ math before Unity introduces its C math fallback macros.
#include <cmath>

#include "../src/host/support.h"
#include "jb/file.h"
#include "jb/json_tree.h"
#include "jb/process.h"
#include "unity.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <jb/array.h>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef _WIN32
// Inject a denied group cleanup without requiring a privileged uid change.
static bool deny_group_kill = false;
static pid_t denied_group = -1;
static bool wait_for_natural_cleanup = false;
extern "C" int __real_kill(pid_t pid, int signal);
extern "C" int __wrap_kill(pid_t pid, int signal) {
  if (wait_for_natural_cleanup && pid > 0 && signal == SIGKILL) {
    // Delay cleanup until the child has exited naturally, without reaping it.
    siginfo_t info{};
    for (unsigned attempt = 0; attempt < 500; ++attempt) {
      if (::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOHANG | WNOWAIT) == 0 &&
          info.si_pid != 0)
        return 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  }
  if (deny_group_kill && pid < 0 && signal == SIGKILL) {
    denied_group = -pid;
    errno = EPERM;
    return -1;
  }
  return __real_kill(pid, signal);
}
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
// STL temporary buffers also use nothrow new. Route them through the same
// allocator so fault injection and ASan see matching allocation/deallocation.
void *operator new(std::size_t size, const std::nothrow_t &) noexcept {
  try {
    return ::operator new(size);
  } catch (const std::bad_alloc &) {
    return nullptr;
  }
}
void *operator new[](std::size_t size, const std::nothrow_t &tag) noexcept {
  return ::operator new(size, tag);
}
void operator delete(void *memory, const std::nothrow_t &) noexcept { ::operator delete(memory); }
void operator delete[](void *memory, const std::nothrow_t &) noexcept { ::operator delete(memory); }
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

static std::string self;
static const char *const tricky[] = {
    "",           "white space",    "quote\"inside",
    "trailing\\", "slash\\\"quote", "\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87",
    "$();&|*",    nullptr};
static void pause_ms(unsigned ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

static std::string environment_value(const char *name) {
#ifdef _WIN32
  const auto key = fs::u8path(name).wstring();
  const DWORD size = GetEnvironmentVariableW(key.c_str(), nullptr, 0);
  if (size == 0)
    return {};
  std::wstring value(size, L'\0');
  const DWORD count = GetEnvironmentVariableW(key.c_str(), value.data(), size);
  if (count == 0 || count >= size)
    return {};
  value.resize(count);
  return fs::path(value).u8string();
#else
  const char *value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string(value);
#endif
}

static int missing_exec_check();
static int busy_exec_check();
static int process_child(int argc, char **argv) {
  const std::string mode = argv[2];
  if (mode == "missing-exec-check")
    return missing_exec_check();
  if (mode == "busy-exec-check")
    return busy_exec_check();

  if (mode == "completion-gate") {
#ifdef _WIN32
    const auto pid = GetCurrentProcessId();
#else
    const auto pid = ::getpid();
#endif
    const auto text = std::to_string(pid);
    bool changed;
    if (jb_file_write_atomic("completion.pid", text.data(), text.size(), false, &changed) != HAL_OK)
      return 101;
    unsigned char bytes[4096];
    std::memset(bytes, 17, sizeof(bytes));
    if (std::fwrite(bytes, 1, 1, stdout) != 1 || std::fflush(stdout) != 0)
      return 102;
    // Keep the child alive until the callback starts observing completion.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!fs::exists("completion.allow")) {
      if (std::chrono::steady_clock::now() >= deadline)
        return 103;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (std::fwrite(bytes, 1, sizeof(bytes), stdout) != sizeof(bytes) ||
        std::fwrite(bytes, 1, sizeof(bytes), stderr) != sizeof(bytes))
      return 102;
    return 37;
  }
#ifndef _WIN32
  if (mode == "mask-check") {
    sigset_t mask;
    return ::sigprocmask(SIG_BLOCK, nullptr, &mask) == 0 && ::sigismember(&mask, SIGUSR1) == 1
               ? 0
               : 103;
  }
  if (mode == "oversize-exec-check") {
    const long page = ::sysconf(_SC_PAGESIZE);
    if (page <= 0)
      return 114;
    std::string huge(static_cast<size_t>(page) * 33, 'x');
    const char *args[] = {huge.c_str(), nullptr};
    const auto cwd = fs::current_path().u8string();
    jb_process_options_t options{};
    options.executable = self.c_str();
    options.arguments = args;
    options.working_directory = cwd.c_str();
    options.timeout_ms = 10000;
    jb_process_result_t result{};
    // A failed exec cannot be resumed by Valgrind 3.22. Run the real OS
    // fault in an exec'd helper on every configuration, without a skip.
    return jb_process_run(&options, &result) == HAL_EOVERFLOW && result.native_error == E2BIG ? 0
                                                                                              : 115;
  }
  if (mode == "file-limit-check") {
    struct rlimit previous;
    if (::getrlimit(RLIMIT_NOFILE, &previous) != 0)
      return 104;
    const struct rlimit limit {
      3, previous.rlim_max
    };
    if (::setrlimit(RLIMIT_NOFILE, &limit) != 0)
      return 104;
    const auto cwd = fs::current_path().u8string();
    jb_process_options_t options{};
    options.executable = self.c_str();
    options.working_directory = cwd.c_str();
    jb_process_result_t result{};
    const auto status = jb_process_run(&options, &result);
    if (::setrlimit(RLIMIT_NOFILE, &previous) != 0)
      return 113;
    return status == HAL_ENOMEM && result.native_error == EMFILE && !result.started ? 0 : 105;
  }
  if (mode == "wait-for-parent-death") {
    int death_signal;
    if (::prctl(PR_GET_PDEATHSIG, &death_signal) != 0 || death_signal != SIGKILL)
      return 106;
    const auto text = std::to_string(::getpid());
    bool changed;
    if (jb_file_write_atomic("orphan.pid", text.data(), text.size(), false, &changed) != HAL_OK)
      return 107;
    for (;;)
      ::pause();
  }
  if (mode == "parent-death-test") {
    if (::prctl(PR_SET_CHILD_SUBREAPER, 1) != 0)
      return 108;
    const pid_t launcher = ::fork();
    if (launcher < 0)
      return 109;
    if (launcher == 0) {
      const char *args[] = {"--child", "wait-for-parent-death", nullptr};
      const auto cwd = fs::current_path().u8string();
      jb_process_options_t options{};
      options.executable = self.c_str();
      options.arguments = args;
      options.working_directory = cwd.c_str();
      options.timeout_ms = 15000;
      jb_process_result_t result{};
      (void)jb_process_run(&options, &result);
      _exit(110);
    }
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!fs::exists("orphan.pid") && std::chrono::steady_clock::now() < until)
      pause_ms(5);
    jb_file_data_t data{};
    const auto loaded = jb_file_read("orphan.pid", 32, &data);
    const pid_t target = loaded == HAL_OK ? static_cast<pid_t>(std::stol(std::string(
                                                reinterpret_cast<char *>(data.data), data.length)))
                                          : -1;
    jb_file_release(&data);
    ::kill(launcher, SIGKILL);
    int status;
    while (::waitpid(launcher, &status, 0) < 0 && errno == EINTR) {
    }
    if (target <= 0)
      return 111;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    pid_t ended;
    do {
      ended = ::waitpid(target, &status, WNOHANG);
      if (ended != 0)
        break;
      pause_ms(5);
    } while (std::chrono::steady_clock::now() < deadline);
    const bool killed = ended == target && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
    if (ended == 0) {
      ::kill(target, SIGKILL);
      while (::waitpid(target, &status, 0) < 0 && errno == EINTR) {
      }
    }
    return killed ? 0 : 112;
  }
#endif
  if (mode == "lock-try" || mode == "lock-own") {
    const auto path = fs::absolute("process.lock").u8string();
    jb_file_lock_t *lock = nullptr;
    const auto status = jb_file_lock_acquire(path.c_str(), 0, nullptr, nullptr, &lock);
    if (mode == "lock-try") {
      if (lock != nullptr)
        jb_file_lock_release(&lock);
      return status == HAL_EBUSY ? 0 : 94;
    }
    if (status != HAL_OK)
      return 95;
    std::fputs("READY", stdout);
    std::fflush(stdout);
    pause_ms(10000);
    return 96;
  }
#ifndef _WIN32
  if (mode == "writer-fsize") {
    const auto path = fs::absolute("limited-output").u8string();
    struct rlimit limit {
      512, 512
    };
    if (::setrlimit(RLIMIT_FSIZE, &limit) != 0)
      return 97;
    ::signal(SIGXFSZ, SIG_IGN);
    jb_file_writer_t *writer = nullptr;
    if (jb_file_writer_begin(path.c_str(), 2048, &writer) != HAL_OK)
      return 98;
    unsigned char bytes[1024]{};
    const auto status = jb_file_writer_write(writer, bytes, sizeof(bytes));
    bool changed = false;
    return status == HAL_EIO && jb_file_writer_commit(&writer, &changed) == HAL_ESTATE && !changed
               ? 0
               : 99;
  }
#endif
  if (mode == "environment-empty") {
    return environment_value("PATH").empty() ? 0 : 90;
  }
#ifndef _WIN32
  if (mode == "natural-exit") {
    pause_ms(1000);
    return 37;
  }
  if (mode == "signal") {
    ::raise(SIGTERM);
    return 91;
  }
  if (mode == "closed-stdio") {
    ::close(0);
    ::close(1);
    ::close(2);
    const char *args[] = {"--child", "environment-empty", nullptr};
    const char *env[] = {nullptr};
    const auto cwd = fs::current_path().u8string();
    jb_process_options_t options{};
    options.executable = self.c_str();
    options.arguments = args;
    options.working_directory = cwd.c_str();
    options.environment = env;
    options.timeout_ms = 10000;
    jb_process_result_t result{};
    return jb_process_run(&options, &result) == HAL_OK && result.exited && result.exit_code == 0
               ? 0
               : 92;
  }
#endif
  if (mode == "report") {
    if (argc != 10)
      return 80;
    for (size_t i = 0; tricky[i] != nullptr; ++i)
      if (std::strcmp(argv[i + 3], tricky[i]) != 0)
        return 81;
    const auto value = environment_value("JB_PROCESS_TEST");
    if (value != "value = \xc5\xbc" || !fs::exists("working-directory-marker"))
      return 82;
    if (std::getchar() != EOF)
      return 83;
    return 37;
  }
  if (mode == "flood") {
#ifdef _WIN32
    if (_setmode(_fileno(stdout), _O_BINARY) == -1 || _setmode(_fileno(stderr), _O_BINARY) == -1)
      return 93;
#endif
    unsigned char bytes[4096];
    for (size_t i = 0; i < sizeof(bytes); ++i)
      bytes[i] = static_cast<unsigned char>(i % 256);
    for (unsigned i = 0; i < 512; ++i) {
      if (std::fwrite(bytes, 1, sizeof(bytes), stdout) != sizeof(bytes) ||
          std::fwrite(bytes, 1, sizeof(bytes), stderr) != sizeof(bytes))
        return 84;
    }
    return 0;
  }
  if (mode == "sleeper") {
    bool changed;
#ifdef _WIN32
    const auto text = std::to_string(GetCurrentProcessId());
#else
    const auto text = std::to_string(::getpid());
#endif
    if (jb_file_write_atomic("descendant-ready", text.data(), text.size(), false, &changed) !=
        HAL_OK)
      return 85;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(25);
    while (!fs::exists("allow-descendant-exit") && std::chrono::steady_clock::now() < deadline)
      pause_ms(5);
    (void)jb_file_write_atomic("descendant-survived", "bad", 3, false, &changed);
    return 0;
  }
  if (mode == "tree" || mode == "tree-exit") {
#ifdef _WIN32
    const auto exe = fs::u8path(self).wstring();
    std::wstring command = L"\"" + exe + L"\" --child sleeper";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr,
                        &startup, &info))
      return 86;
    CloseHandle(info.hThread);
    CloseHandle(info.hProcess);
#else
    const pid_t pid = ::fork();
    if (pid < 0)
      return 86;
    if (pid == 0) {
      ::execl(self.c_str(), self.c_str(), "--child", "sleeper", static_cast<char *>(nullptr));
      _exit(87);
    }
#endif
    for (unsigned i = 0; i < 500 && !fs::exists("descendant-ready"); ++i)
      pause_ms(5);
    if (!fs::exists("descendant-ready"))
      return 88;
    std::fputs("READY", stdout);
    std::fflush(stdout);
    if (mode == "tree-exit")
      return 0;
    pause_ms(10000);
    return 0;
  }
  return 89;
}

struct process_capture {
  size_t count[2]{};
  bool invalid = false, cancel = false, stop_on_output = false;
  bool check_bytes = false;
};
static hal_status_t capture_output(void *context, jb_process_stream_t stream,
                                   const unsigned char *bytes, size_t length) {
  auto &capture = *static_cast<process_capture *>(context);
  const size_t index = static_cast<size_t>(stream);
  for (size_t i = 0; capture.check_bytes && i < length; ++i)
    if (bytes[i] != static_cast<unsigned char>((capture.count[index] + i) % 256))
      capture.invalid = true;
  capture.count[index] += length;
  capture.cancel = true;
  return capture.stop_on_output ? HAL_EBUSY : HAL_OK;
}
static bool cancel_process(void *context) {
  return static_cast<process_capture *>(context)->cancel;
}
static jb_process_options_t process_options(const char *const *args, const std::string &cwd) {
  jb_process_options_t options{};
  options.executable = self.c_str();
  options.arguments = args;
  options.working_directory = cwd.c_str();
  options.timeout_ms = 15000;
  return options;
}

static void test_process_arguments_environment_cwd_and_exit() {
  const auto before = fs::current_path();
  const fs::path directory = root / fs::u8path("process # \xc5\xbc");
  fs::create_directories(directory);
  const std::string cwd = directory.u8string();
  const auto executable = (directory / fs::u8path("program \xc5\xbc.exe")).u8string();
  fs::copy_file(fs::u8path(self), fs::u8path(executable));
  bool changed;
  const auto marker = (directory / "working-directory-marker").u8string();
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(marker.c_str(), "x", 1, false, &changed));
  std::vector<const char *> args = {"--child", "report"};
  for (const char *arg : tricky)
    args.push_back(arg);
  const char *environment[] = {"JB_PROCESS_TEST=value = \xc5\xbc", nullptr};
  auto options = process_options(args.data(), cwd);
  options.executable = executable.c_str();
  options.environment = environment;
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_TRUE(result.started && result.exited);
  TEST_ASSERT_EQUAL(37, result.exit_code);
  TEST_ASSERT_EQUAL(0, result.term_signal);
  TEST_ASSERT_EQUAL(0, result.native_error);
  TEST_ASSERT_TRUE(fs::current_path() == before);
  TEST_ASSERT_TRUE(environment_value("JB_PROCESS_TEST").empty());
}

static void test_process_drains_both_streams_without_deadlock() {
  const std::string cwd = root.u8string();
  const char *args[] = {"--child", "flood", nullptr};
  auto options = process_options(args, cwd);
  process_capture capture;
  capture.check_bytes = true;
  options.output = capture_output;
  options.context = &capture;
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
  TEST_ASSERT_FALSE(capture.invalid);
  TEST_ASSERT_EQUAL(2097152, capture.count[0]);
  TEST_ASSERT_EQUAL(2097152, capture.count[1]);
  options.output = nullptr;
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
}

// Failed execs need a native helper: Valgrind 3.22 can replace the child
// termination itself. The parent still checks the helper on every run.
static int missing_exec_check() {
  const auto cwd = fs::current_path().u8string();
  const auto missing = (fs::current_path() / "missing-process").u8string();
  auto options = process_options(nullptr, cwd);
  options.executable = missing.c_str();
  jb_process_result_t result{};
  const auto status = jb_process_run(&options, &result);
#ifdef _WIN32
  const bool lifecycle = !result.started && !result.exited && result.exit_code == 0;
#else
  const bool lifecycle = result.started && result.exited && result.exit_code == 127;
#endif
  return status == HAL_ENOENT && lifecycle && result.native_error != 0 && result.term_signal == 0
             ? 0
             : 116;
}

static void test_process_failures_and_prelaunch_cancellation() {
  const std::string cwd = root.u8string();
  auto options = process_options(nullptr, cwd);
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_process_run(nullptr, &result));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_process_run(&options, nullptr));
  options.executable = "relative";
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_process_run(&options, &result));
  options.executable = "\xff";
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_process_run(&options, &result));
  options.executable = self.c_str();
  const char *duplicate[] = {"JB_TEST=a", "JB_TEST=b", nullptr};
  options.environment = duplicate;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_process_run(&options, &result));
  const char *invalid_args[] = {"\xff", nullptr};
  options.environment = nullptr;
  options.arguments = invalid_args;
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_process_run(&options, &result));
  TEST_ASSERT_FALSE(result.started || result.exited);
  options.arguments = nullptr;
  const char *invalid_env[] = {"JB_TEST=\xff", nullptr};
  options.environment = invalid_env;
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_process_run(&options, &result));
  TEST_ASSERT_FALSE(result.started || result.exited);
  const char *no_equal[] = {"JB_TEST", nullptr};
  options.environment = no_equal;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_process_run(&options, &result));
  TEST_ASSERT_FALSE(result.started || result.exited);
  options.environment = nullptr;
  const std::string missing = (root / "missing-process").u8string();
  const char *missing_args[] = {"--child", "missing-exec-check", nullptr};
  options.arguments = missing_args;
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_TRUE(result.started && result.exited);
  TEST_ASSERT_EQUAL(0, result.exit_code);
  options.arguments = nullptr;
  options.working_directory = missing.c_str();
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_process_run(&options, &result));
  options.working_directory = cwd.c_str();
  process_capture capture;
  capture.cancel = true;
  options.context = &capture;
  options.canceled = cancel_process;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_process_run(&options, &result));
  TEST_ASSERT_FALSE(result.started);
}

static void test_process_collects_after_group_cleanup_permission_error() {
#ifndef _WIN32
  const auto cwd = root.u8string();
  const char *args[] = {"--child", "environment-empty", nullptr};
  const char *env[] = {nullptr};
  auto options = process_options(args, cwd);
  options.environment = env;
  jb_process_result_t result{};
  denied_group = -1;
  deny_group_kill = true;
  const auto status = jb_process_run(&options, &result);
  deny_group_kill = false;
  int state;
  const pid_t waited = denied_group > 0 ? ::waitpid(denied_group, &state, WNOHANG) : 0;
  const int wait_error = errno;
  TEST_ASSERT_EQUAL(HAL_EPERM, status);
  TEST_ASSERT_TRUE(result.started && result.exited);
  TEST_ASSERT_EQUAL(0, result.exit_code);
  TEST_ASSERT_EQUAL(EPERM, result.native_error);
  TEST_ASSERT_TRUE(denied_group > 0);
  TEST_ASSERT_EQUAL(-1, waited);
  TEST_ASSERT_EQUAL(ECHILD, wait_error);
#endif
}

static void test_process_stop_status_survives_natural_exit_during_cleanup() {
#ifndef _WIN32
  const auto cwd = root.u8string();
  const char *args[] = {"--child", "natural-exit", nullptr};
  auto options = process_options(args, cwd);
  options.timeout_ms = 250;
  jb_process_result_t result{};
  wait_for_natural_cleanup = true;
  const auto status = jb_process_run(&options, &result);
  wait_for_natural_cleanup = false;
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, status);
  TEST_ASSERT_TRUE(result.started && result.exited);
  TEST_ASSERT_EQUAL(0, result.term_signal);
  TEST_ASSERT_EQUAL(37, result.exit_code);
  TEST_ASSERT_EQUAL(0, result.native_error);
#endif
}

static void test_process_parallel_calls_empty_environment_and_signals() {
  const std::string cwd = root.u8string();
  const char *args[] = {"--child", "environment-empty", nullptr};
  const char *env[] = {nullptr};
  auto options = process_options(args, cwd);
  options.environment = env;
  jb_process_result_t results[2]{};
  hal_status_t statuses[2]{};
  std::thread first([&] { statuses[0] = jb_process_run(&options, &results[0]); });
  statuses[1] = jb_process_run(&options, &results[1]);
  first.join();
  for (unsigned i = 0; i < 2; ++i) {
    TEST_ASSERT_EQUAL(HAL_OK, statuses[i]);
    TEST_ASSERT_TRUE(results[i].exited);
    TEST_ASSERT_EQUAL(0, results[i].exit_code);
  }
#ifndef _WIN32
  args[1] = "signal";
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &results[0]));
  TEST_ASSERT_EQUAL(SIGTERM, results[0].term_signal);
  args[1] = "closed-stdio";
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &results[0]));
  TEST_ASSERT_EQUAL(0, results[0].exit_code);
#endif
}

static void test_process_stops_descendants_and_handles_inherited_pipes() {
  const std::string cwd = root.u8string();
  for (unsigned scenario = 0; scenario < 4; ++scenario) {
    fs::remove(root / "descendant-ready");
    fs::remove(root / "descendant-survived");
    fs::remove(root / "allow-descendant-exit");
    const char *args[] = {"--child", scenario == 3 ? "tree-exit" : "tree", nullptr};
    auto options = process_options(args, cwd);
    process_capture capture;
    capture.stop_on_output = scenario == 2;
    options.context = &capture;
    options.output = capture_output;
    if (scenario == 0)
      options.timeout_ms = 5000;
    if (scenario == 1)
      options.canceled = cancel_process;
    jb_process_result_t result{};
    const auto status = jb_process_run(&options, &result);
    TEST_ASSERT_EQUAL(scenario == 0   ? HAL_ETIMEOUT
                      : scenario == 1 ? HAL_ECANCELED
                      : scenario == 2 ? HAL_EBUSY
                                      : HAL_OK,
                      status);
    TEST_ASSERT_TRUE(result.started && result.exited);
    TEST_ASSERT_TRUE(fs::exists(root / "descendant-ready"));

    jb_file_data_t pid_bytes{};
    const auto pid_path = (root / "descendant-ready").u8string();
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(pid_path.c_str(), 32, &pid_bytes));
    const auto pid =
        std::stoul(std::string(reinterpret_cast<char *>(pid_bytes.data), pid_bytes.length));
    jb_file_release(&pid_bytes);
#ifdef _WIN32
    HANDLE held = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
#endif
    bool changed;
    const auto gate = (root / "allow-descendant-exit").u8string();
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(gate.c_str(), "go", 2, false, &changed));
#ifdef _WIN32
    const auto exited = held == nullptr ? WAIT_OBJECT_0 : WaitForSingleObject(held, 5000);
    if (held != nullptr)
      CloseHandle(held);
    TEST_ASSERT_EQUAL(WAIT_OBJECT_0, exited);
#else
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    bool running;
    do {
      const auto stat_path = "/proc/" + std::to_string(pid) + "/stat";
      jb_file_data_t stat{};
      const auto read = jb_file_read(stat_path.c_str(), 4096, &stat);
      std::string text;
      if (read == HAL_OK)
        text.assign(reinterpret_cast<char *>(stat.data), stat.length);
      jb_file_release(&stat);
      const auto end = text.rfind(')');
      running = read == HAL_OK &&
                (end == std::string::npos || text.size() <= end + 2 || text[end + 2] != 'Z');
      if (running)
        pause_ms(5);
    } while (running && std::chrono::steady_clock::now() < deadline);
    TEST_ASSERT_FALSE(running);
#endif
    TEST_ASSERT_FALSE(fs::exists(root / "descendant-survived"));
  }
}

static void assert_no_temporary_siblings() {
  for (const auto &entry : fs::directory_iterator(root)) {
    const auto name = entry.path().filename().u8string();
    TEST_ASSERT_TRUE(name.find(".tmp-") == std::string::npos);
    TEST_ASSERT_TRUE(name.find(".jb-dir-") == std::string::npos);
  }
}

static void test_streaming_commit_abort_limits_and_unchanged_mtime() {
  const auto path = (root / fs::u8path("stream \xc5\xbc.bin")).u8string();
  const unsigned char bytes[] = {'a', 0, '\r', '\n', 255};
  bool changed = false;
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_file_write_atomic(path.c_str(), bytes, sizeof(bytes), false, &changed));
  const auto stamp = fs::file_time_type::clock::now() - std::chrono::hours(24);
  fs::last_write_time(fs::u8path(path), stamp);
  const auto before = fs::last_write_time(fs::u8path(path));
  jb_file_writer_t *writer = nullptr;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), sizeof(bytes), &writer));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_writer_begin(path.c_str(), 0, &writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, bytes, 2));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, bytes + 2, sizeof(bytes) - 2));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_NULL(writer);
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_TRUE(before == fs::last_write_time(fs::u8path(path)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), 4, &writer));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_file_writer_write(writer, bytes, sizeof(bytes)));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_writer_write(writer, bytes, 1));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_NULL(writer);
  TEST_ASSERT_TRUE(before == fs::last_write_time(fs::u8path(path)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), 4, &writer));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_writer_write(writer, nullptr, 1));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), UINT64_MAX, &writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, "replacement", 11));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_writer_commit(&writer, nullptr));
  TEST_ASSERT_NOT_NULL(writer);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_abort(&writer));
  TEST_ASSERT_NULL(writer);
  TEST_ASSERT_TRUE(before == fs::last_write_time(fs::u8path(path)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_abort(&writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), 0, &writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, nullptr, 0));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_EQUAL(0, fs::file_size(fs::u8path(path)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), 2097152, &writer));
  unsigned char chunk[4096]{};
  chunk[0] = 0xff;
  for (unsigned i = 0; i < 512; ++i)
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, chunk, sizeof(chunk)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_TRUE(changed);
  TEST_ASSERT_EQUAL(2097152, fs::file_size(fs::u8path(path)));
  jb_file_data_t data{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), 2097152, &data));
  for (size_t i = 0; i < data.length; ++i)
    TEST_ASSERT_EQUAL_UINT8(i % sizeof(chunk) == 0 ? 255 : 0, data.data[i]);
  jb_file_release(&data);
  assert_no_temporary_siblings();
}

static void test_streaming_refuses_changed_destination_and_partial_io() {
  const auto destination = root / "changing-output";
  const auto path = destination.u8string();
  bool changed = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "old", 3, false, &changed));
  jb_file_writer_t *writer = nullptr;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_begin(path.c_str(), 3, &writer));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_write(writer, "new", 3));
  fs::rename(destination, root / "saved-output");
  fs::create_directory(destination);
  changed = false;
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_writer_commit(&writer, &changed));
  TEST_ASSERT_FALSE(changed);
  TEST_ASSERT_NULL(writer);
  TEST_ASSERT_TRUE(fs::is_directory(destination));
  fs::remove(destination);
#ifndef _WIN32
  const auto limited = (root / "limited-output").u8string();
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(limited.c_str(), "old", 3, false, &changed));
  const std::string cwd = root.u8string();
  const char *args[] = {"--child", "writer-fsize", nullptr};
  auto options = process_options(args, cwd);
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
  jb_file_data_t bytes{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(limited.c_str(), 3, &bytes));
  TEST_ASSERT_EQUAL_MEMORY("old", bytes.data, 3);
  jb_file_release(&bytes);
#endif
  assert_no_temporary_siblings();
}

static bool create_test_link(const fs::path &target, const fs::path &link, bool directory) {
  std::error_code error;
  if (directory)
    fs::create_directory_symlink(target, link, error);
  else
    fs::create_symlink(target, link, error);
#ifdef _WIN32
  if (error.value() == ERROR_PRIVILEGE_NOT_HELD) {
    std::fputs("CAPABILITY: symlink subcase unavailable (Windows privilege missing)\n", stderr);
    return false;
  }
#endif
  TEST_ASSERT_FALSE_MESSAGE(static_cast<bool>(error), error.message().c_str());
  return true;
}

static void test_private_temporary_directories_and_link_cleanup() {
  const auto parent = root / fs::u8path("temporary # \xc5\xbc");
  fs::create_directories(parent);
  const auto base = parent.u8string();
  jb_temp_dir_t *first = nullptr, *second = nullptr;
#ifndef _WIN32
  const mode_t previous_mask = ::umask(0777);
  const auto created = jb_temp_dir_create(base.c_str(), &first);
  ::umask(previous_mask);
  TEST_ASSERT_EQUAL(HAL_OK, created);
#else
  TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_create(base.c_str(), &first));
#endif
  TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_create(base.c_str(), &second));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_temp_dir_create(base.c_str(), &first));
  const auto a = fs::u8path(jb_temp_dir_path(first)), b = fs::u8path(jb_temp_dir_path(second));
  TEST_ASSERT_TRUE(a != b);
  TEST_ASSERT_TRUE(fs::is_directory(a) && fs::is_directory(b));
#ifndef _WIN32
  struct stat info;
  TEST_ASSERT_EQUAL(0, ::stat(a.c_str(), &info));
  TEST_ASSERT_EQUAL(0700, info.st_mode & 0777);
#else
  DWORD required = 0;
  GetFileSecurityW(a.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &required);
  TEST_ASSERT_TRUE(required > 0);
  std::vector<unsigned char> security(required);
  TEST_ASSERT_TRUE(GetFileSecurityW(a.c_str(), DACL_SECURITY_INFORMATION, security.data(), required,
                                    &required) != 0);
  SECURITY_DESCRIPTOR_CONTROL control;
  DWORD revision;
  TEST_ASSERT_TRUE(GetSecurityDescriptorControl(security.data(), &control, &revision) != 0);
  TEST_ASSERT_TRUE((control & SE_DACL_PROTECTED) != 0);
  PACL acl = nullptr;
  BOOL present, defaulted;
  TEST_ASSERT_TRUE(GetSecurityDescriptorDacl(security.data(), &present, &acl, &defaulted) != 0);
  TEST_ASSERT_TRUE(present && acl != nullptr);
  TEST_ASSERT_EQUAL(1, acl->AceCount);
  void *raw_ace = nullptr;
  TEST_ASSERT_TRUE(GetAce(acl, 0, &raw_ace) != 0);
  const auto *ace = static_cast<ACCESS_ALLOWED_ACE *>(raw_ace);
  TEST_ASSERT_EQUAL(ACCESS_ALLOWED_ACE_TYPE, ace->Header.AceType);
  TEST_ASSERT_EQUAL(OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE, ace->Header.AceFlags);
  TEST_ASSERT_EQUAL(FILE_ALL_ACCESS, ace->Mask);
  HANDLE token = INVALID_HANDLE_VALUE;
  TEST_ASSERT_TRUE(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token) != 0);
  GetTokenInformation(token, TokenUser, nullptr, 0, &required);
  std::vector<unsigned char> user(required);
  const auto queried = GetTokenInformation(token, TokenUser, user.data(), required, &required);
  CloseHandle(token);
  TEST_ASSERT_TRUE(queried != 0);
  TEST_ASSERT_TRUE(EqualSid(const_cast<DWORD *>(&ace->SidStart),
                            reinterpret_cast<TOKEN_USER *>(user.data())->User.Sid) != 0);
#endif
  fs::create_directories(a / "nested");
  (void)create_test_link(b, a / "nested" / "outside", true);
  bool changed;
  const auto sentinel = (b / "keep").u8string();
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(sentinel.c_str(), "keep", 4, false, &changed));
  const auto file_link = a / "file-link";
  const bool file_link_available = create_test_link(fs::u8path(sentinel), file_link, false);
  const auto link_path = file_link.u8string();
  jb_file_writer_t *writer = nullptr;
  jb_file_lock_t *lock = nullptr;
  if (file_link_available) {
    TEST_ASSERT_EQUAL(HAL_ETYPE, jb_file_writer_begin(link_path.c_str(), 1, &writer));
    TEST_ASSERT_EQUAL(HAL_ETYPE,
                      jb_file_lock_acquire(link_path.c_str(), 0, nullptr, nullptr, &lock));
    TEST_ASSERT_NULL(writer);
    TEST_ASSERT_NULL(lock);
  }
  TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_remove(&first));
  TEST_ASSERT_NULL(first);
  TEST_ASSERT_FALSE(fs::exists(a));
  TEST_ASSERT_TRUE(fs::exists(fs::u8path(sentinel)));
#ifdef _WIN32
  HANDLE held = CreateFileW(fs::u8path(sentinel).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                            OPEN_EXISTING, 0, nullptr);
  TEST_ASSERT_TRUE(held != INVALID_HANDLE_VALUE);
  const auto cleanup_status = jb_temp_dir_remove(&second);
  CloseHandle(held);
  TEST_ASSERT_TRUE(cleanup_status == HAL_EBUSY || cleanup_status == HAL_EPERM);
  TEST_ASSERT_NOT_NULL(second);
#endif
  TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_remove(&second));
  TEST_ASSERT_NULL(second);
  TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_remove(nullptr));
  TEST_ASSERT_NULL(jb_temp_dir_path(nullptr));
  assert_no_temporary_siblings();
}

struct release_waiting_lock {
  jb_file_lock_t **owner;
  unsigned polls = 0;
  hal_status_t status = HAL_NONE;
};
static bool release_after_contention(void *context) {
  auto &state = *static_cast<release_waiting_lock *>(context);
  if (++state.polls == 3)
    state.status = jb_file_lock_release(state.owner);
  return false;
}

struct lock_cancel_state {
  unsigned polls = 0;
};
static bool cancel_lock(void *context) {
  return ++static_cast<lock_cancel_state *>(context)->polls >= 3;
}

static bool delay_lock_attempt(void *context) {
  auto &calls = *static_cast<unsigned *>(context);
  if (++calls == 2)
    pause_ms(20);
  return false;
}

static void test_file_lock_contention_wait_cancellation_and_process_death() {
  const auto cwd = root.u8string();
  const auto path = (root / "process.lock").u8string();
  jb_file_lock_t *owner = nullptr, *other = nullptr;
  bool changed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "sentinel", 8, false, &changed));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_acquire(path.c_str(), 0, nullptr, nullptr, &owner));
  TEST_ASSERT_EQUAL(HAL_ESTATE, jb_file_lock_acquire(path.c_str(), 0, nullptr, nullptr, &owner));
  TEST_ASSERT_EQUAL(HAL_EBUSY, jb_file_lock_acquire(path.c_str(), 0, nullptr, nullptr, &other));
  TEST_ASSERT_NULL(other);
  TEST_ASSERT_EQUAL(HAL_ETIMEOUT, jb_file_lock_acquire(path.c_str(), 25, nullptr, nullptr, &other));
  lock_cancel_state canceled;
  TEST_ASSERT_EQUAL(HAL_ECANCELED,
                    jb_file_lock_acquire(path.c_str(), UINT32_MAX, cancel_lock, &canceled, &other));
  const char *args[] = {"--child", "lock-try", nullptr};
  auto options = process_options(args, cwd);
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
  release_waiting_lock release{&owner};
  const auto acquired =
      jb_file_lock_acquire(path.c_str(), 2000, release_after_contention, &release, &other);
  TEST_ASSERT_EQUAL(HAL_OK, release.status);
  TEST_ASSERT_NULL(owner);
  TEST_ASSERT_EQUAL(HAL_OK, acquired);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_release(&other));
  args[1] = "lock-own";
  process_capture capture;
  options.output = capture_output;
  options.canceled = cancel_process;
  options.context = &capture;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_process_run(&options, &result));
  TEST_ASSERT_TRUE(result.exited);
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_acquire(path.c_str(), 0, nullptr, nullptr, &owner));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_release(&owner));
  TEST_ASSERT_TRUE(fs::exists(fs::u8path(path)));
  jb_file_data_t bytes{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_read(path.c_str(), 8, &bytes));
  TEST_ASSERT_EQUAL_MEMORY("sentinel", bytes.data, 8);
  jb_file_release(&bytes);
  unsigned delayed_polls = 0;
  TEST_ASSERT_EQUAL(
      HAL_OK, jb_file_lock_acquire(path.c_str(), 1, delay_lock_attempt, &delayed_polls, &owner));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_release(&owner));
  const auto unicode = (root / fs::u8path("lock # \xc5\xbc")).u8string();
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_acquire(unicode.c_str(), 0, nullptr, nullptr, &owner));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_release(&owner));
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_lock_release(nullptr));
  const auto pre_canceled = (root / "canceled.lock").u8string();
  capture.cancel = true;
  TEST_ASSERT_EQUAL(HAL_ECANCELED, jb_file_lock_acquire(pre_canceled.c_str(), 0, cancel_process,
                                                        &capture, &owner));
  TEST_ASSERT_FALSE(fs::exists(fs::u8path(pre_canceled)));
}

static void test_new_file_owners_validate_inputs_and_cleanup_on_oom() {
  jb_file_writer_t *writer = nullptr;
  jb_temp_dir_t *directory = nullptr;
  jb_file_lock_t *lock = nullptr;
  const auto base = root.u8string(), path = (root / "oom-stream").u8string();
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_writer_begin("relative", 0, &writer));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_temp_dir_create("relative", &directory));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_lock_acquire("relative", 0, nullptr, nullptr, &lock));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_file_writer_begin("\xff", 0, &writer));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_writer_write(nullptr, nullptr, 0));
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_file_writer_commit(&writer, nullptr));
#ifndef JB_NO_ALLOCATION_FAULTS
  size_t caught = 0;
  for (size_t fail_at = 1; fail_at <= 32; ++fail_at) {
    fail_allocation = fail_at;
    auto status = jb_temp_dir_create(base.c_str(), &directory);
    fail_allocation = 0;
    TEST_ASSERT_TRUE(status == HAL_OK || status == HAL_ENOMEM);
    if (status == HAL_ENOMEM)
      ++caught;
    TEST_ASSERT_EQUAL(HAL_OK, jb_temp_dir_remove(&directory));
    fail_allocation = fail_at;
    status = jb_file_writer_begin(path.c_str(), 10, &writer);
    fail_allocation = 0;
    TEST_ASSERT_TRUE(status == HAL_OK || status == HAL_ENOMEM);
    TEST_ASSERT_EQUAL(HAL_OK, jb_file_writer_abort(&writer));
    assert_no_temporary_siblings();
  }
  TEST_ASSERT_TRUE(caught > 0);
#endif
}

struct completed_output {
  std::string pid_path, gate_path;
  std::chrono::steady_clock::time_point deadline;
  size_t bytes[2]{};
  bool observed = false, cancel = false;
};
static hal_status_t completed_output_callback(void *context, jb_process_stream_t stream,
                                              const unsigned char *data, size_t length) {
  auto &state = *static_cast<completed_output *>(context);
  for (size_t i = 0; i < length; ++i)
    if (data[i] != 17)
      return HAL_EPROTO;
  state.bytes[stream] += length;
  if (!state.observed) {
    jb_file_data_t bytes{};
    if (jb_file_read(state.pid_path.c_str(), 32, &bytes) != HAL_OK)
      return HAL_EIO;
    const auto pid = std::stoul(std::string(reinterpret_cast<char *>(bytes.data), bytes.length));
    jb_file_release(&bytes);
    bool changed = false;
    if (jb_file_write_atomic(state.gate_path.c_str(), "go", 2, false, &changed) != HAL_OK)
      return HAL_EIO;
#ifdef _WIN32
    HANDLE handle = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
      return HAL_EIO;
    const auto wait = WaitForSingleObject(handle, 10000);
    CloseHandle(handle);
    if (wait != WAIT_OBJECT_0)
      return HAL_EIO;
#else
    siginfo_t info{};
    int wait;
    do {
      wait = ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT);
    } while (wait < 0 && errno == EINTR);
    if (wait < 0)
      return HAL_EIO;
#endif
    // The child is known to have exited. Deliberately return control after
    // the deadline, rather than hoping a fast child beats the scheduler.
    std::this_thread::sleep_until(state.deadline);
    state.observed = true;
    state.cancel = true;
  }
  return HAL_OK;
}
static bool cancel_completed(void *context) {
  return static_cast<completed_output *>(context)->cancel;
}
static void test_completed_process_wins_deadline_and_drains_queued_output() {
  const auto cwd = root.u8string();
  const char *args[] = {"--child", "completion-gate", nullptr};
  for (bool cancellation : {false, true}) {
    completed_output state;
    state.pid_path = (root / "completion.pid").u8string();
    state.gate_path = (root / "completion.allow").u8string();
    fs::remove(fs::u8path(state.pid_path));
    fs::remove(fs::u8path(state.gate_path));
    auto options = process_options(args, cwd);
    options.timeout_ms = cancellation ? 0 : 2000;
    state.deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(cancellation ? 0 : 2100);
    options.output = completed_output_callback;
    options.context = &state;
    options.canceled = cancellation ? cancel_completed : nullptr;
    jb_process_result_t result{};
    TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
    TEST_ASSERT_TRUE(state.observed && result.exited);
    TEST_ASSERT_EQUAL(37, result.exit_code);
    TEST_ASSERT_EQUAL(4097, state.bytes[0]);
    TEST_ASSERT_EQUAL(4096, state.bytes[1]);
  }
}

static int busy_exec_check() {
  const auto cwd = fs::current_path().u8string();
  const auto executable = (fs::current_path() / "busy-program.exe").u8string();
  fs::copy_file(fs::u8path(self), fs::u8path(executable));
  auto options = process_options(nullptr, cwd);
  options.executable = executable.c_str();
#ifdef _WIN32
  const HANDLE file = CreateFileW(fs::u8path(executable).c_str(), GENERIC_WRITE, 0, nullptr,
                                  OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE)
    return 117;
#else
  FILE *const file = std::fopen(executable.c_str(), "r+b");
  if (file == nullptr)
    return 117;
#endif
  jb_process_result_t result{};
  const auto status = jb_process_run(&options, &result);
#ifdef _WIN32
  CloseHandle(file);
#else
  std::fclose(file);
#endif
  return status == HAL_EBUSY && result.native_error != 0 ? 0 : 118;
}

static void test_busy_executable_reports_native_error() {
  const auto cwd = root.u8string();
  const char *args[] = {"--child", "busy-exec-check", nullptr};
  auto options = process_options(args, cwd);
  jb_process_result_t result{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_TRUE(result.exited);
  TEST_ASSERT_EQUAL(0, result.exit_code);
}

static void test_host_error_mapping_and_process_start_errors() {
  using condition = std::errc;
  const std::pair<condition, hal_status_t> mappings[] = {
      {condition::argument_list_too_long, HAL_EOVERFLOW},
      {condition::not_a_directory, HAL_ENOENT},
      {condition::executable_format_error, HAL_ETYPE},
      {condition::resource_unavailable_try_again, HAL_EAGAIN},
      {condition::too_many_files_open, HAL_ENOMEM},
      {condition::too_many_files_open_in_system, HAL_ENOMEM}};
  for (const auto &entry : mappings)
    TEST_ASSERT_EQUAL(entry.second, jb_host::status_for(std::make_error_code(entry.first)));
  const auto cwd = root.u8string();
  const auto path = (root / "bad-format.exe").u8string();
  bool changed;
  TEST_ASSERT_EQUAL(HAL_OK, jb_file_write_atomic(path.c_str(), "bad", 3, false, &changed));
#ifndef _WIN32
  TEST_ASSERT_EQUAL(0, ::chmod(path.c_str(), 0700));
#endif
  auto options = process_options(nullptr, cwd);
  options.executable = path.c_str();
  jb_process_result_t result{};
#ifdef _WIN32
  const DWORD error_mode = GetThreadErrorMode();
#endif
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_process_run(&options, &result));
#ifdef _WIN32
  TEST_ASSERT_EQUAL(error_mode, GetThreadErrorMode());
#endif
  TEST_ASSERT_TRUE(result.native_error != 0);
  options.executable = self.c_str();
  options.working_directory = path.c_str();
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_process_run(&options, &result));
#ifndef _WIN32
  options.working_directory = cwd.c_str();
  const char *oversize_args[] = {"--child", "oversize-exec-check", nullptr};
  options.arguments = oversize_args;
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
  const char *child_args[] = {"--child", "file-limit-check", nullptr};
  options.arguments = child_args;
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
  const char *mask_args[] = {"--child", "mask-check", nullptr};
  options.arguments = mask_args;
  sigset_t blocked, previous;
  ::sigemptyset(&blocked);
  ::sigaddset(&blocked, SIGUSR1);
  TEST_ASSERT_EQUAL(0, ::sigprocmask(SIG_BLOCK, &blocked, &previous));
  const auto status = jb_process_run(&options, &result);
  const int restored = ::sigprocmask(SIG_SETMASK, &previous, nullptr);
  TEST_ASSERT_EQUAL(0, restored);
  TEST_ASSERT_EQUAL(HAL_OK, status);
  TEST_ASSERT_EQUAL(0, result.exit_code);
  fs::remove(root / "orphan.pid");
  const char *death_args[] = {"--child", "parent-death-test", nullptr};
  options.arguments = death_args;
  TEST_ASSERT_EQUAL(HAL_OK, jb_process_run(&options, &result));
  TEST_ASSERT_EQUAL(0, result.exit_code);
#endif
}

using tree_owner = std::unique_ptr<jb_json_tree_t, decltype(&jb_json_tree_destroy)>;
static const jb_json_tree_limits_t tree_limits{4096, 256, 1024 * 1024, 1024 * 1024};

static tree_owner configuration_tree(const std::string &input,
                                     const jb_json_tree_limits_t &limits = tree_limits) {
  const jb_json_limits_t parsing{input.size(), 4096, 256,        input.size(),
                                 input.size(), 4096, 1024 * 1024};
  size_t bytes = 0;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse_workspace_size(&parsing, &bytes));
  std::vector<unsigned char> scratch(bytes);
  jb_json_requirements_t measured{};
  const jb_json_span_t source{input.data(), input.size()};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_measure(source, &parsing, {scratch.data(), scratch.size()},
                                            &measured, nullptr));
  std::vector<jb_json_token_t> tokens(measured.node_count);
  jb_json_doc_t document{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_parse(source, &parsing, tokens.data(), tokens.size(),
                                          {scratch.data(), scratch.size()}, &document, nullptr));
  jb_json_node_t value{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_root(&document, &value));
  jb_json_tree_t *owned = nullptr;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_create(value, &limits, &owned));
  return tree_owner(owned, jb_json_tree_destroy);
}

static jb_json_tree_view_t tree_root(const tree_owner &tree) {
  jb_json_tree_view_t value{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_root(tree.get(), &value));
  return value;
}

static hal_status_t append_tree_text(void *user, const char *bytes, size_t length) {
  static_cast<std::string *>(user)->append(bytes, length);
  return HAL_OK;
}

static std::string formatted_tree(jb_json_tree_view_t value, uint32_t indent = 0,
                                  bool sorted = false, bool ascii = false) {
  std::string output;
  size_t written = SIZE_MAX;
  const jb_json_tree_format_t options{1024 * 1024, indent, sorted, ascii};
  TEST_ASSERT_EQUAL(HAL_OK,
                    jb_json_tree_format(value, &options, append_tree_text, &output, &written));
  TEST_ASSERT_EQUAL(output.size(), written);
  return output;
}

// Manifest overlays must replace definition arrays and retain untouched defaults,
// field order and exact numeric tokens used in generated-artifact hashes.
static void test_configuration_merge_owns_values_and_preserves_manifest_semantics() {
  auto base = configuration_tree(
      R"({"target":"rp2040","cmake":{"cache":{"n":18446744073709551616,"keep":true},"defines":["A","B"]},"zero":-0.00e+0})");
  auto overlay =
      configuration_tree(R"({"cmake":{"cache":{"more":"ż"},"defines":["C"]},"new":null})");
  jb_json_tree_t *raw = nullptr;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_copy(tree_root(base), &tree_limits, &raw));
  tree_owner copy(raw, jb_json_tree_destroy);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_merge(base.get(), tree_root(overlay)));
  overlay.reset();
  const std::string merged = formatted_tree(tree_root(base));
  TEST_ASSERT_EQUAL_STRING(
      R"({"target":"rp2040","cmake":{"cache":{"n":18446744073709551616,"keep":true,"more":"ż"},"defines":["C"]},"zero":-0.00e+0,"new":null})",
      merged.c_str());
  TEST_ASSERT_EQUAL_STRING(
      R"({"target":"rp2040","cmake":{"cache":{"n":18446744073709551616,"keep":true},"defines":["A","B"]},"zero":-0.00e+0})",
      formatted_tree(tree_root(copy)).c_str());
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_merge(base.get(), tree_root(base)));
  TEST_ASSERT_EQUAL_STRING(merged.c_str(), formatted_tree(tree_root(base)).c_str());
  jb_json_tree_view_t member{}, number{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_at(tree_root(base), 2, &member));
  jb_json_span_t text{};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_key(member, &text));
  TEST_ASSERT_EQUAL(4, text.length);
  TEST_ASSERT_EQUAL_MEMORY("zero", text.data, text.length);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_text(member, &text));
  TEST_ASSERT_EQUAL(8, text.length);
  TEST_ASSERT_EQUAL_MEMORY("-0.00e+0", text.data, text.length);
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_find(tree_root(base), {"cmake", 5}, &member));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_find(member, {"cache", 5}, &member));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_find(member, {"n", 1}, &number));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_text(number, &text));
  TEST_ASSERT_EQUAL(20, text.length);
  TEST_ASSERT_EQUAL_MEMORY("18446744073709551616", text.data, text.length);
  auto replacement = configuration_tree("[false]");
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_merge(base.get(), tree_root(replacement)));
  TEST_ASSERT_EQUAL_STRING("[false]", formatted_tree(tree_root(base)).c_str());
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_at(tree_root(base), 0, &member));
  bool boolean = true;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_get_bool(member, &boolean));
  TEST_ASSERT_FALSE(boolean);
}

static void test_configuration_mutations_copy_self_values_and_preserve_empty_keys() {
  auto tree = configuration_tree(R"({"a":{"old":1},"keep":2,"":null,"a\u0000b":true})");
  auto replacement = configuration_tree(R"({"new":3})");
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_set(tree.get(), {"a", 1}, tree_root(replacement)));
  TEST_ASSERT_EQUAL_STRING(R"({"a":{"new":3},"keep":2,"":null,"a\u0000b":true})",
                           formatted_tree(tree_root(tree)).c_str());
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_set(tree.get(), {"copy", 4}, tree_root(tree)));
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_erase(tree.get(), {nullptr, 0}));
  TEST_ASSERT_EQUAL_STRING(
      R"({"a":{"new":3},"keep":2,"a\u0000b":true,"copy":{"a":{"new":3},"keep":2,"":null,"a\u0000b":true}})",
      formatted_tree(tree_root(tree)).c_str());
  jb_json_tree_view_t value{};
  const char key[] = {'a', 0, 'b'};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_find(tree_root(tree), {key, sizeof(key)}, &value));
  bool boolean = false;
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_get_bool(value, &boolean));
  TEST_ASSERT_TRUE(boolean);
  auto array = configuration_tree("[1]");
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_append(array.get(), tree_root(array)));
  TEST_ASSERT_EQUAL_STRING("[1,[1]]", formatted_tree(tree_root(array)).c_str());
  const auto old = formatted_tree(tree_root(tree));
  TEST_ASSERT_EQUAL(HAL_ENOENT, jb_json_tree_erase(tree.get(), {"absent", 6}));
  TEST_ASSERT_EQUAL(HAL_ETYPE, jb_json_tree_append(tree.get(), tree_root(array)));
  TEST_ASSERT_EQUAL(HAL_EPROTO, jb_json_tree_set(tree.get(), {"\xff", 1}, tree_root(array)));
  TEST_ASSERT_EQUAL_STRING(old.c_str(), formatted_tree(tree_root(tree)).c_str());
}

// These are the byte formats used by Python json.dumps for repository files:
// pretty indentation, scalar key order, ASCII escapes and short controls.
static void test_configuration_formatting_matches_repository_json_bytes() {
  auto tree =
      configuration_tree(R"({"ż":"😀\n\b\f\r\t\u0000\u007f","a":[1e+03,{},[]],"\\u000a":"\\n"})");
  const std::string pretty = formatted_tree(tree_root(tree), 2, true, true);
  TEST_ASSERT_EQUAL_STRING(
      "{\n  \"\\\\u000a\": \"\\\\n\",\n  \"a\": [\n    1e+03,\n    {},\n    []\n  ],\n  "
      "\"\\u017c\": \"\\ud83d\\ude00\\n\\b\\f\\r\\t\\u0000\\u007f\"\n}",
      pretty.c_str());
  const std::string wide = formatted_tree(tree_root(tree), 4, false, false);
  TEST_ASSERT_TRUE(wide.find("\n    \"ż\": \"😀\\n\\b\\f\\r\\t\\u0000\x7f\"") != std::string::npos);
  TEST_ASSERT_TRUE(wide.find("\n        1e+03,") != std::string::npos);
  const std::string ordered = formatted_tree(tree_root(tree));
  TEST_ASSERT_TRUE(ordered.find("{\"ż\":") == 0);
  auto empty = configuration_tree("{}");
  TEST_ASSERT_EQUAL_STRING("{}", formatted_tree(tree_root(empty), 4).c_str());
  size_t written = 123;
  std::string sink;
  jb_json_tree_format_t options{4, 0, false, false};
  auto linefeed = configuration_tree(R"("\n")");
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_format(tree_root(linefeed), &options, append_tree_text,
                                                &sink, &written));
  TEST_ASSERT_EQUAL_STRING("\"\\n\"", sink.c_str());
  options.indent = 3;
  written = 99;
  TEST_ASSERT_EQUAL(HAL_EINVAL, jb_json_tree_format(tree_root(tree), &options, append_tree_text,
                                                    &sink, &written));
  TEST_ASSERT_EQUAL(99, written);
}

static hal_status_t reject_tree_output(void *, const char *, size_t) { return HAL_EIO; }

static void test_configuration_limits_and_sink_errors_do_not_publish_partial_mutations() {
  const jb_json_tree_limits_t limits{3, 1, 5, 100};
  auto tree = configuration_tree(R"({"a":1})", limits);
  auto big = configuration_tree("[1,2,3]");
  const auto before = formatted_tree(tree_root(tree));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_tree_set(tree.get(), {"a", 1}, tree_root(big)));
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_tree_set(tree.get(), {"longkey", 7}, tree_root(big)));
  TEST_ASSERT_EQUAL_STRING(before.c_str(), formatted_tree(tree_root(tree)).c_str());
  auto simple = configuration_tree("0");
  jb_json_tree_t *out = nullptr;
  const jb_json_tree_limits_t zero{};
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_tree_copy(tree_root(simple), &zero, &out));
  TEST_ASSERT_NULL(out);
  const jb_json_tree_limits_t scalar{1, 0, 1, 0};
  TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_copy(tree_root(simple), &scalar, &out));
  jb_json_tree_destroy(out);
  const jb_json_tree_limits_t no_work{5, 2, 100, 0};
  auto constrained = configuration_tree(R"({"a":1})", no_work);
  jb_json_tree_view_t sentinel{simple.get(), 0};
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_tree_find(tree_root(constrained), {"a", 1}, &sentinel));
  TEST_ASSERT_TRUE(sentinel.tree == simple.get());
  size_t written = SIZE_MAX;
  const jb_json_tree_format_t format{100, 0, false, false};
  TEST_ASSERT_EQUAL(HAL_EIO, jb_json_tree_format(tree_root(tree), &format, reject_tree_output,
                                                 nullptr, &written));
  TEST_ASSERT_EQUAL(0, written);
  const jb_json_tree_format_t tiny{1, 0, false, false};
  std::string partial;
  TEST_ASSERT_EQUAL(HAL_EOVERFLOW, jb_json_tree_format(tree_root(tree), &tiny, append_tree_text,
                                                       &partial, &written));
  TEST_ASSERT_EQUAL(partial.size(), written);
  TEST_ASSERT_EQUAL_STRING(before.c_str(), formatted_tree(tree_root(tree)).c_str());
  std::string nested(256, '[');
  nested += "0";
  nested.append(256, ']');
  auto deep = configuration_tree(nested);
  TEST_ASSERT_EQUAL_STRING(nested.c_str(), formatted_tree(tree_root(deep)).c_str());
}

#ifndef JB_NO_ALLOCATION_FAULTS
static void test_configuration_allocation_failures_preserve_owners_and_values() {
  auto base = configuration_tree(R"({"a":{"x":[1,2]},"b":"ż"})");
  auto overlay = configuration_tree(R"({"a":{"y":[3,4]},"new":true})");
  const auto original = formatted_tree(tree_root(base));
  bool completed = false;
  for (size_t failure = 1; failure < 256; ++failure) {
    jb_json_tree_t *raw = nullptr;
    TEST_ASSERT_EQUAL(HAL_OK, jb_json_tree_copy(tree_root(base), &tree_limits, &raw));
    tree_owner copy(raw, jb_json_tree_destroy);
    fail_allocation = failure;
    const auto status = jb_json_tree_merge(copy.get(), tree_root(overlay));
    fail_allocation = 0;
    if (status == HAL_OK) {
      completed = true;
      break;
    }
    TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
    TEST_ASSERT_EQUAL_STRING(original.c_str(), formatted_tree(tree_root(copy)).c_str());
  }
  TEST_ASSERT_TRUE(completed);
  jb_json_tree_t *out = nullptr;
  fail_allocation = 1;
  const auto status = jb_json_tree_copy(tree_root(base), &tree_limits, &out);
  fail_allocation = 0;
  TEST_ASSERT_EQUAL(HAL_ENOMEM, status);
  TEST_ASSERT_NULL(out);
}
#endif

static int host_main(int argc, char **argv) {
  self = fs::absolute(fs::u8path(argv[0])).u8string();
  if (argc >= 3 && std::strcmp(argv[1], "--child") == 0)
    return process_child(argc, argv);
  std::setvbuf(stdout, nullptr, _IONBF, 0);
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
  RUN_TEST(test_process_arguments_environment_cwd_and_exit);
  RUN_TEST(test_process_drains_both_streams_without_deadlock);
  RUN_TEST(test_process_failures_and_prelaunch_cancellation);
  RUN_TEST(test_process_collects_after_group_cleanup_permission_error);
  RUN_TEST(test_process_stop_status_survives_natural_exit_during_cleanup);
  RUN_TEST(test_process_parallel_calls_empty_environment_and_signals);
  RUN_TEST(test_process_stops_descendants_and_handles_inherited_pipes);
  RUN_TEST(test_streaming_commit_abort_limits_and_unchanged_mtime);
  RUN_TEST(test_streaming_refuses_changed_destination_and_partial_io);
  RUN_TEST(test_private_temporary_directories_and_link_cleanup);
  RUN_TEST(test_file_lock_contention_wait_cancellation_and_process_death);
  RUN_TEST(test_new_file_owners_validate_inputs_and_cleanup_on_oom);
  RUN_TEST(test_completed_process_wins_deadline_and_drains_queued_output);
  RUN_TEST(test_busy_executable_reports_native_error);
  RUN_TEST(test_host_error_mapping_and_process_start_errors);
  RUN_TEST(test_configuration_merge_owns_values_and_preserves_manifest_semantics);
  RUN_TEST(test_configuration_mutations_copy_self_values_and_preserve_empty_keys);
  RUN_TEST(test_configuration_formatting_matches_repository_json_bytes);
  RUN_TEST(test_configuration_limits_and_sink_errors_do_not_publish_partial_mutations);
#ifndef JB_NO_ALLOCATION_FAULTS
  RUN_TEST(test_configuration_allocation_failures_preserve_owners_and_values);
#endif
  const int result = UNITY_END();
  fs::remove_all(root);
  return result;
}

#ifdef _WIN32
int wmain(int argc, wchar_t **argv) {
  std::vector<std::string> values;
  for (int i = 0; i < argc; ++i)
    values.push_back(fs::path(argv[i]).u8string());
  std::vector<char *> args;
  for (auto &value : values)
    args.push_back(value.data());
  return host_main(argc, args.data());
}
#else
int main(int argc, char **argv) { return host_main(argc, argv); }
#endif
