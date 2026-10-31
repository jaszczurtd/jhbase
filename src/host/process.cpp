#include "jb/process.h"
#include "support.h"
#include <algorithm>
#include <chrono>
#include <climits>
#include <iterator>
#include <string>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace {
using namespace jb_host;
using clock_type = std::chrono::steady_clock;

struct request {
  fs::path executable, directory;
  std::vector<std::string> args, env;
  bool inherit;
  explicit request(const jb_process_options_t &options)
      : executable(path_from(options.executable)), directory(path_from(options.working_directory)),
        inherit(options.environment == nullptr) {
    if (!executable.is_absolute() || !directory.is_absolute())
      throw failure{HAL_EINVAL};
    args.push_back(executable.u8string());
    if (options.arguments != nullptr)
      for (auto p = options.arguments; *p != nullptr; ++p) {
        valid_text(*p);
        args.emplace_back(*p);
      }
    if (!inherit)
      for (auto p = options.environment; *p != nullptr; ++p) {
        valid_text(*p);
        const char *equal = std::strchr(*p, '=');
        if (equal == nullptr || equal == *p)
          throw failure{HAL_EINVAL};
        env.emplace_back(*p);
      }
    for (size_t i = 0; i < env.size(); ++i) {
      const auto name = env[i].substr(0, env[i].find('='));
      for (size_t j = 0; j < i; ++j) {
        const auto previous = env[j].substr(0, env[j].find('='));
#ifdef _WIN32
        const auto a = fs::u8path(name).wstring(), b = fs::u8path(previous).wstring();
        const bool equal = CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
#else
        const bool equal = name == previous;
#endif
        if (equal)
          throw failure{HAL_EINVAL};
      }
    }
  }
};

struct control {
  const jb_process_options_t &options;
  clock_type::time_point start = clock_type::now();
  hal_status_t status() const {
    if (options.canceled != nullptr && options.canceled(options.context))
      return HAL_ECANCELED;
    if (options.timeout_ms != 0 &&
        std::chrono::duration_cast<std::chrono::milliseconds>(clock_type::now() - start).count() >=
            options.timeout_ms)
      return HAL_ETIMEOUT;
    return HAL_OK;
  }
  void check() const {
    const auto stopped = status();
    if (stopped != HAL_OK)
      throw failure{stopped};
  }
  void output(unsigned stream, const unsigned char *bytes, size_t size) const {
    if (options.output != nullptr) {
      const auto delivered =
          options.output(options.context, static_cast<jb_process_stream_t>(stream), bytes, size);
      if (delivered != HAL_OK)
        throw failure{delivered};
    }
  }
};

#ifdef _WIN32
struct handle {
  HANDLE value = INVALID_HANDLE_VALUE;
  void close() {
    if (value != INVALID_HANDLE_VALUE && value != nullptr)
      CloseHandle(value);
    value = INVALID_HANDLE_VALUE;
  }
  ~handle() { close(); }
  handle() = default;
  handle(const handle &) = delete;
  handle &operator=(const handle &) = delete;
};

// The Microsoft C runtime splits backslashes only before quotes/end quotes.
std::wstring quoted(const std::wstring &arg) {
  std::wstring out(1, L'"');
  size_t slashes = 0;
  for (wchar_t c : arg) {
    if (c == L'\\') {
      ++slashes;
      continue;
    }
    out.append(slashes * (c == L'"' ? 2u : 1u), L'\\');
    if (c == L'"')
      out.push_back(L'\\');
    out.push_back(c);
    slashes = 0;
  }
  out.append(slashes * 2u, L'\\');
  out.push_back(L'"');
  return out;
}

struct attributes {
  std::vector<unsigned char> storage;
  LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
  ~attributes() {
    if (value != nullptr)
      DeleteProcThreadAttributeList(value);
  }
  void init(HANDLE *handles, size_t count) {
    SIZE_T bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    storage.resize(bytes);
    auto list = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!InitializeProcThreadAttributeList(list, 1, 0, &bytes))
      io_error();
    value = list;
    if (!UpdateProcThreadAttribute(value, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles,
                                   count * sizeof(HANDLE), nullptr, nullptr))
      io_error();
  }
};

struct child {
  handle process, thread, job;
  jb_process_result_t &result;
  explicit child(jb_process_result_t &out) : result(out) {}
  void collect() {
    if (process.value == INVALID_HANDLE_VALUE || result.exited)
      return;
    const DWORD wait = WaitForSingleObject(process.value, INFINITE);
    if (wait != WAIT_OBJECT_0)
      io_error();
    DWORD code;
    if (!GetExitCodeProcess(process.value, &code))
      io_error();
    result.exited = true;
    result.exit_code = code;
  }
  ~child() {
    // Closing the job kills even descendants of a main process already exited.
    job.close();
    if (process.value != INVALID_HANDLE_VALUE && !result.exited) {
      TerminateProcess(process.value, 1);
      try {
        collect();
      } catch (...) {
      }
    }
  }
};

struct launch_error_mode {
  DWORD previous = 0;
  launch_error_mode() {
    const DWORD flags = GetThreadErrorMode() | SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX |
                        SEM_NOOPENFILEERRORBOX;
    if (!SetThreadErrorMode(flags, &previous))
      io_error();
  }
  ~launch_error_mode() { SetThreadErrorMode(previous, nullptr); }
};

void run(const request &req, const control &ctl, jb_process_result_t &result) {
  std::wstring command;
  for (const auto &arg : req.args) {
    if (!command.empty())
      command.push_back(L' ');
    command += quoted(fs::u8path(arg).wstring());
  }
  if (command.size() >= 32767u)
    throw failure{HAL_EOVERFLOW};
  std::vector<std::wstring> variables;
  std::transform(req.env.begin(), req.env.end(), std::back_inserter(variables),
                 [](const auto &entry) { return fs::u8path(entry).wstring(); });
  std::sort(variables.begin(), variables.end(), [](const auto &a, const auto &b) {
    const auto a_name = a.substr(0, a.find(L'=')), b_name = b.substr(0, b.find(L'='));
    return CompareStringOrdinal(a_name.c_str(), -1, b_name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
  });
  std::vector<wchar_t> environment;
  for (const auto &entry : variables) {
    environment.insert(environment.end(), entry.begin(), entry.end());
    environment.push_back(0);
  }
  environment.push_back(0);
  if (variables.empty())
    environment.push_back(0);

  SECURITY_ATTRIBUTES security{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
  handle input, reads[2], writes[2];
  input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (input.value == INVALID_HANDLE_VALUE)
    io_error();
  for (unsigned i = 0; i < 2; ++i) {
    if (!CreatePipe(&reads[i].value, &writes[i].value, &security, 0) ||
        !SetHandleInformation(reads[i].value, HANDLE_FLAG_INHERIT, 0))
      io_error();
  }
  HANDLE inherited[] = {input.value, writes[0].value, writes[1].value};
  attributes attrs;
  attrs.init(inherited, 3);
  STARTUPINFOEXW startup{};
  startup.StartupInfo.cb = sizeof(startup);
  startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
  startup.StartupInfo.hStdInput = input.value;
  startup.StartupInfo.hStdOutput = writes[0].value;
  startup.StartupInfo.hStdError = writes[1].value;
  startup.lpAttributeList = attrs.value;
  child owned(result);
  owned.job.value = CreateJobObjectW(nullptr, nullptr);
  if (owned.job.value == nullptr)
    io_error();
  JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
  limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
  if (!SetInformationJobObject(owned.job.value, JobObjectExtendedLimitInformation, &limits,
                               sizeof(limits)))
    io_error();
  PROCESS_INFORMATION info{};
  ctl.check();
  {
    launch_error_mode no_dialog;
    if (!CreateProcessW(req.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                            EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW,
                        req.inherit ? nullptr : environment.data(), req.directory.c_str(),
                        &startup.StartupInfo, &info))
      io_error();
  }
  owned.process.value = info.hProcess;
  owned.thread.value = info.hThread;
  result.started = true;
  if (!AssignProcessToJobObject(owned.job.value, info.hProcess) ||
      ResumeThread(info.hThread) == static_cast<DWORD>(-1))
    io_error();
  owned.thread.close();
  input.close();
  for (auto &write : writes)
    write.close();
  bool alive[2] = {true, true};
  DWORD remaining[2]{};
  auto available = [&](unsigned i) -> DWORD {
    DWORD bytes;
    if (PeekNamedPipe(reads[i].value, nullptr, 0, nullptr, &bytes, nullptr))
      return bytes;
    if (GetLastError() != ERROR_BROKEN_PIPE)
      io_error();
    alive[i] = false;
    return 0;
  };
  auto observe = [&] {
    if (result.exited)
      return;
    const DWORD wait = WaitForSingleObject(owned.process.value, 0);
    if (wait == WAIT_FAILED)
      io_error();
    if (wait == WAIT_OBJECT_0) {
      owned.collect();
      owned.job.close();
      // Freeze the queued output. Escaped/external writers cannot extend the
      // drain indefinitely after the main process has finished.
      for (unsigned i = 0; i < 2; ++i)
        if (alive[i])
          remaining[i] = available(i);
    }
  };
  while (!result.exited || alive[0] || alive[1]) {
    observe();
    bool progress = false;
    for (unsigned i = 0; i < 2; ++i) {
      if (!alive[i])
        continue;
      const DWORD bytes = result.exited ? remaining[i] : available(i);
      if (result.exited && bytes == 0) {
        alive[i] = false;
        reads[i].close();
        continue;
      }
      if (bytes != 0) {
        unsigned char buffer[4096];
        DWORD count;
        if (!ReadFile(reads[i].value, buffer, std::min<DWORD>(bytes, sizeof(buffer)), &count,
                      nullptr))
          io_error();
        ctl.output(i, buffer, count);
        if (result.exited)
          remaining[i] -= count;
        progress = true;
      }
    }
    if (result.exited)
      continue;
    const auto stopped = ctl.status();
    if (stopped != HAL_OK) {
      observe();
      if (!result.exited)
        throw failure{stopped};
    } else if (!progress) {
      Sleep(5);
    }
  }
}
#else
struct descriptor {
  int value = -1;
  void close() {
    if (value >= 0)
      ::close(value);
    value = -1;
  }
  ~descriptor() { close(); }
  descriptor() = default;
  descriptor(const descriptor &) = delete;
  descriptor &operator=(const descriptor &) = delete;
};

void make_pipe(descriptor (&fds)[2]) {
  int values[2];
  if (::pipe2(values, O_CLOEXEC) != 0)
    io_error();
  fds[0].value = values[0];
  fds[1].value = values[1];
  // Preserve callers which have closed one or more standard descriptors.
  for (auto &fd : fds)
    if (fd.value < 3) {
      const int high = ::fcntl(fd.value, F_DUPFD_CLOEXEC, 3);
      if (high < 0)
        io_error();
      fd.close();
      fd.value = high;
    }
}

bool child_exited(pid_t pid, bool block) {
  siginfo_t info{};
  int status;
  do {
    status =
        ::waitid(P_PID, static_cast<id_t>(pid), &info, WEXITED | WNOWAIT | (block ? 0 : WNOHANG));
  } while (status < 0 && errno == EINTR);
  if (status < 0)
    io_error();
  return info.si_pid != 0;
}

struct child {
  pid_t pid = -1;
  jb_process_result_t &result;
  explicit child(jb_process_result_t &out) : result(out) {}
  void collect() {
    if (pid < 0)
      return;
    // The main process has not been reaped yet: its pid/group cannot be reused.
    const int cleanup_error = (::kill(-pid, SIGKILL) != 0 && errno != ESRCH) ? errno : 0;
    int state;
    pid_t waited;
    do {
      waited = ::waitpid(pid, &state, 0);
    } while (waited < 0 && errno == EINTR);
    if (waited < 0)
      io_error();
    pid = -1;
    result.exited = true;
    if (WIFEXITED(state))
      result.exit_code = static_cast<uint32_t>(WEXITSTATUS(state));
    else if (WIFSIGNALED(state))
      result.term_signal = WTERMSIG(state);
    // A failed group cleanup must not prevent reaping the main child.
    if (cleanup_error != 0)
      throw failure{status_for_generic(cleanup_error), static_cast<uint32_t>(cleanup_error)};
  }
  ~child() {
    if (pid >= 0) {
      // Also kill the main process if startup failed before setpgid.
      ::kill(pid, SIGKILL);
      try {
        collect();
      } catch (...) {
      }
    }
  }
};

[[noreturn]] void child_error(int fd) {
  const int error = errno;
  ssize_t count;
  do {
    count = ::write(fd, &error, sizeof(error));
  } while (count < 0 && errno == EINTR);
  _exit(127);
}

void run(const request &req, const control &ctl, jb_process_result_t &result) {
  std::vector<char *> args, env;
  std::transform(req.args.begin(), req.args.end(), std::back_inserter(args),
                 [](const auto &arg) { return const_cast<char *>(arg.c_str()); });
  args.push_back(nullptr);
  std::transform(req.env.begin(), req.env.end(), std::back_inserter(env),
                 [](const auto &entry) { return const_cast<char *>(entry.c_str()); });
  env.push_back(nullptr);
  char **environment = req.inherit ? environ : env.data();
  descriptor output[2][2], errors[2], input;
  for (auto &pipe : output)
    make_pipe(pipe);
  make_pipe(errors);
  input.value = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  if (input.value < 0)
    io_error();
  // fcntl must not run on a child standard fd when parent's stdin was closed.
  if (input.value < 3) {
    const int high = ::fcntl(input.value, F_DUPFD_CLOEXEC, 3);
    if (high < 0)
      io_error();
    input.close();
    input.value = high;
  }
  for (auto &pipe : output)
    if (::fcntl(pipe[0].value, F_SETFL, O_NONBLOCK) != 0)
      io_error();
  if (::fcntl(errors[0].value, F_SETFL, O_NONBLOCK) != 0)
    io_error();
  const long max_fd = ::sysconf(_SC_OPEN_MAX);
  if (max_fd < 0 || max_fd > INT_MAX)
    throw failure{HAL_EOVERFLOW};
  child owned(result);
  ctl.check();
  const pid_t parent = ::getpid();
  const pid_t pid = ::fork();
  if (pid < 0)
    io_error();
  if (pid == 0) {
    // Only async-signal-safe operations after fork, including failure reporting.
    int error_fd = errors[1].value;
    if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0)
      child_error(error_fd);
    if (::getppid() != parent)
      _exit(127);
    if (::setpgid(0, 0) != 0 || ::chdir(req.directory.c_str()) != 0 || ::dup2(input.value, 0) < 0 ||
        ::dup2(output[0][1].value, 1) < 0 || ::dup2(output[1][1].value, 2) < 0)
      child_error(error_fd);
    if (::dup2(error_fd, 3) < 0)
      child_error(error_fd);
    error_fd = 3;
    if (::fcntl(error_fd, F_SETFD, FD_CLOEXEC) != 0)
      child_error(error_fd);
#ifdef SYS_close_range
    if (::syscall(SYS_close_range, 4u, UINT_MAX, 0u) != 0)
#endif
      for (int fd = 4; fd < max_fd; ++fd)
        ::close(fd);
    ::execve(req.executable.c_str(), args.data(), environment);
    child_error(error_fd);
  }
  owned.pid = pid;
  result.started = true;
  input.close();
  errors[1].close();
  for (auto &pipe : output)
    pipe[1].close();
  bool alive[2] = {true, true};
  bool launch_pending = true;
  size_t remaining[2]{};
  auto observe = [&] {
    if (result.exited)
      return;
    if (child_exited(pid, false)) {
      owned.collect();
      for (unsigned i = 0; i < 2; ++i) {
        if (!alive[i])
          continue;
        int queued;
        if (::ioctl(output[i][0].value, FIONREAD, &queued) != 0)
          io_error();
        remaining[i] = static_cast<size_t>(queued);
      }
    }
  };
  while (!result.exited || alive[0] || alive[1] || launch_pending) {
    if (launch_pending) {
      int error;
      const ssize_t count = ::read(errors[0].value, &error, sizeof(error));
      if (count > 0) {
        if (count != sizeof(error))
          throw failure{HAL_EIO};
        // The fork-side reporter will immediately _exit(127). Let it finish
        // before cleanup signals, retaining its exit code even if preempted
        // after writing errno. No executable has started on this path.
        if (!result.exited)
          (void)child_exited(pid, true);
        errno = error;
        io_error();
      }
      if (count == 0) {
        launch_pending = false;
        errors[0].close();
      } else if (errno != EAGAIN && errno != EINTR) {
        io_error();
      }
    }
    observe();
    for (unsigned i = 0; i < 2; ++i) {
      if (!alive[i])
        continue;
      if (result.exited && remaining[i] == 0) {
        alive[i] = false;
        output[i][0].close();
        continue;
      }
      unsigned char buffer[4096];
      const auto capacity = result.exited ? std::min(sizeof(buffer), remaining[i]) : sizeof(buffer);
      const ssize_t count = ::read(output[i][0].value, buffer, capacity);
      if (count > 0) {
        ctl.output(i, buffer, static_cast<size_t>(count));
        if (result.exited)
          remaining[i] -= static_cast<size_t>(count);
      } else if (count == 0) {
        alive[i] = false;
        output[i][0].close();
      } else if (errno != EINTR && errno != EAGAIN) {
        io_error();
      }
    }
    if (result.exited)
      continue;
    const auto stopped = ctl.status();
    if (stopped != HAL_OK) {
      observe();
      if (!result.exited)
        throw failure{stopped};
      continue;
    }
    pollfd pending[3] = {{alive[0] ? output[0][0].value : -1, POLLIN, 0},
                         {alive[1] ? output[1][0].value : -1, POLLIN, 0},
                         {launch_pending ? errors[0].value : -1, POLLIN, 0}};
    if (::poll(pending, 3, 5) < 0 && errno != EINTR)
      io_error();
  }
}
#endif
} // namespace

extern "C" hal_status_t jb_process_run(const jb_process_options_t *options,
                                       jb_process_result_t *result) {
  if (result == nullptr)
    return HAL_EINVAL;
  *result = {};
  if (options == nullptr)
    return HAL_EINVAL;
  return boundary(
      [&] {
        control ctl{*options};
        request req(*options);
        ctl.check();
        run(req, ctl, *result);
      },
      &result->native_error);
}
