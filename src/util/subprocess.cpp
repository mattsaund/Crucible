// SPDX-License-Identifier: MIT
//
// fork/exec with a merged output pipe. See subprocess.hpp for why not popen.
#include "crucible/util/subprocess.hpp"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <utility>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace crucible::util {
namespace {

#if !defined(_WIN32)

/// A NULL-terminated char* array pointing into strings we keep alive.
/// execvp wants char* const*, and string::data() is writable since C++17.
std::vector<char*> to_argv(const std::vector<std::string>& args) {
    std::vector<char*> raw;
    raw.reserve(args.size() + 1);
    for (const std::string& arg : args) {
        raw.push_back(const_cast<char*>(arg.c_str()));
    }
    raw.push_back(nullptr);
    return raw;
}
#endif  // !_WIN32

}  // namespace

#if !defined(_WIN32)

Subprocess::~Subprocess() {
    if (running()) {
        terminate();
        wait();
    }
    close_pipe();
}

Subprocess::Subprocess(Subprocess&& other) noexcept { *this = std::move(other); }

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        close_pipe();
        pid_    = std::exchange(other.pid_, -1);
        fd_     = std::exchange(other.fd_, -1);
        status_ = std::exchange(other.status_, -1);
        reaped_ = std::exchange(other.reaped_, false);
        buffer_ = std::move(other.buffer_);
        eof_    = std::exchange(other.eof_, false);
    }
    return *this;
}

void Subprocess::close_pipe() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
}

bool Subprocess::start(const std::vector<std::string>& argv,
                       const std::filesystem::path& cwd,
                       const std::vector<std::string>& extra_env,
                       std::string& error) {
    if (argv.empty()) {
        error = "no command given";
        return false;
    }

    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(fds[0]);
        ::close(fds[1]);
        return false;
    }

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        // Nothing here may allocate or throw in a way that matters: on any
        // failure the child calls _exit directly rather than unwinding through
        // a parent's destructors.
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(fds[1], STDERR_FILENO);
        ::close(fds[1]);

        // Its own process group, so terminate() can signal the whole build
        // tree -- cmake spawns make, which spawns compilers, and killing only
        // cmake would leave those running.
        ::setpgid(0, 0);

        if (!cwd.empty() && ::chdir(cwd.c_str()) != 0) {
            ::_exit(127);
        }
        for (const std::string& entry : extra_env) {
            ::putenv(const_cast<char*>(entry.c_str()));
        }

        std::vector<char*> raw = to_argv(argv);
        ::execvp(raw[0], raw.data());
        ::_exit(127);  // only reached when exec failed
    }

    // --- parent ------------------------------------------------------------
    ::close(fds[1]);
    pid_    = pid;
    fd_     = fds[0];
    status_ = -1;
    reaped_ = false;
    eof_    = false;
    buffer_.clear();
    return true;
}

bool Subprocess::read_line(std::string& line) {
    for (;;) {
        // Serve a complete line out of what we already have before reading
        // more, so a chunk containing several lines is not lost.
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }

        if (eof_) {
            // Whatever is left is a final line with no terminator.
            if (buffer_.empty()) {
                return false;
            }
            line = std::move(buffer_);
            buffer_.clear();
            return true;
        }

        char chunk[4096];
        const ssize_t got = ::read(fd_, chunk, sizeof(chunk));
        if (got > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(got));
        } else if (got == 0) {
            eof_ = true;
        } else if (errno != EINTR) {
            eof_ = true;
        }
    }
}

int Subprocess::wait() {
    if (reaped_) {
        return status_;
    }
    if (pid_ <= 0) {
        return -1;
    }

    int raw = 0;
    while (::waitpid(pid_, &raw, 0) < 0) {
        if (errno != EINTR) {
            reaped_ = true;
            status_ = -1;
            pid_    = -1;
            return status_;
        }
    }

    if (WIFEXITED(raw)) {
        status_ = WEXITSTATUS(raw);
    } else if (WIFSIGNALED(raw)) {
        status_ = 128 + WTERMSIG(raw);
    } else {
        status_ = -1;
    }
    reaped_ = true;
    pid_    = -1;
    return status_;
}

void Subprocess::terminate() {
    if (pid_ <= 0) {
        return;
    }
    // Negative pid signals the whole process group, which is why the child put
    // itself in one: a build is cmake -> make -> a dozen compilers.
    ::kill(-pid_, SIGTERM);
    close_pipe();  // unblocks a read_line() waiting on output
}

void Subprocess::interrupt() {
    if (pid_ > 0) {
        ::kill(-pid_, SIGTERM);
    }
}

bool Subprocess::running() const {
    return pid_ > 0;
}

bool on_path(const std::string& program) {
    if (program.empty()) {
        return true;
    }
    if (program.find('/') != std::string::npos) {
        return ::access(program.c_str(), X_OK) == 0;
    }

    const char* path = std::getenv("PATH");
    if (path == nullptr) {
        return false;
    }

    const std::string haystack(path);
    std::size_t start = 0;
    while (start <= haystack.size()) {
        const std::size_t end = haystack.find(':', start);
        const std::string dir =
            haystack.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && ::access((dir + "/" + program).c_str(), X_OK) == 0) {
            return true;
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return false;
}

#endif  // !_WIN32

// ---------------------------------------------------------------------------
// Windows
//
// The same contract, built from CreateProcess and a pipe. Three things differ
// enough to be worth naming:
//
//   * a command line is one string here, not an argv, and the child re-parses
//     it -- so the quoting the parser expects has to be applied on the way out;
//   * there is no process group, so the child goes into a job object, which is
//     what lets terminate() take a whole build tree down;
//   * a broken pipe is how end-of-file arrives, and it is reported as a failure
//     with ERROR_BROKEN_PIPE rather than a zero-length read.
// ---------------------------------------------------------------------------
#if defined(_WIN32)

namespace {

/// One argument, quoted the way the C runtime's parser will read it back.
///
/// The rule that catches people: backslashes are only special immediately
/// before a quote, where each one has to be doubled, and the closing quote
/// needs the run before it doubled too.
std::string quote_argument(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) {
        return arg;
    }
    std::string out = "\"";
    for (std::size_t i = 0; i < arg.size(); ++i) {
        std::size_t slashes = 0;
        while (i < arg.size() && arg[i] == '\\') {
            ++slashes;
            ++i;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, '\\');
            break;
        }
        if (arg[i] == '"') {
            out.append(slashes * 2 + 1, '\\');
        } else {
            out.append(slashes, '\\');
        }
        out += arg[i];
    }
    out += '"';
    return out;
}

/// The command line for `argv`.
///
/// `cmd` is the exception and has to be, because it does not use the C
/// runtime's parser: `cmd /s /c "..."` means "strip the outer quotes and take
/// everything between them literally", which is the only form that survives a
/// command containing its own quotes. This shape is what util::shell_command
/// produces on Windows, and the two are meant to be read together.
std::wstring command_line(const std::vector<std::string>& argv) {
    std::string line;
    const bool shell = argv.size() == 3 && (argv[0] == "cmd" || argv[0] == "cmd.exe")
                    && argv[1] == "/c";
    if (shell) {
        line = "cmd.exe /s /c \"" + argv[2] + "\"";
    } else {
        for (const std::string& arg : argv) {
            if (!line.empty()) {
                line += ' ';
            }
            line += quote_argument(arg);
        }
    }

    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, line.c_str(),
                                           static_cast<int>(line.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                          out.data(), wide);
    return out;
}

std::string last_error() {
    const DWORD code = ::GetLastError();
    char* text = nullptr;
    const DWORD length = ::FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM
            | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length > 0 && text != nullptr ? std::string(text, length)
                                                        : "error " + std::to_string(code);
    if (text != nullptr) {
        ::LocalFree(text);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

}  // namespace

Subprocess::~Subprocess() {
    if (running()) {
        terminate();
        wait();
    }
    close_pipe();
    if (process_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(process_));
        process_ = nullptr;
    }
    if (job_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(job_));
        job_ = nullptr;
    }
}

Subprocess::Subprocess(Subprocess&& other) noexcept { *this = std::move(other); }

Subprocess& Subprocess::operator=(Subprocess&& other) noexcept {
    if (this != &other) {
        close_pipe();
        process_ = std::exchange(other.process_, nullptr);
        job_     = std::exchange(other.job_, nullptr);
        read_    = std::exchange(other.read_, nullptr);
        status_  = std::exchange(other.status_, -1);
        reaped_  = std::exchange(other.reaped_, false);
        buffer_  = std::move(other.buffer_);
        eof_     = std::exchange(other.eof_, false);
    }
    return *this;
}

void Subprocess::close_pipe() {
    if (read_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(read_));
        read_ = nullptr;
    }
}

bool Subprocess::running() const {
    return process_ != nullptr && !reaped_;
}

bool Subprocess::start(const std::vector<std::string>& argv,
                       const std::filesystem::path& cwd,
                       const std::vector<std::string>& extra_env,
                       std::string& error) {
    if (argv.empty()) {
        error = "no command given";
        return false;
    }

    // The write end is inheritable so the child can have it; the read end must
    // not be, or the pipe never reports end-of-file -- this process would still
    // hold a writer open and read_line would block forever.
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength        = sizeof(attributes);
    attributes.bInheritHandle = TRUE;

    HANDLE read_end  = nullptr;
    HANDLE write_end = nullptr;
    if (::CreatePipe(&read_end, &write_end, &attributes, 0) == 0) {
        error = "CreatePipe: " + last_error();
        return false;
    }
    ::SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);

    // A job the whole tree lands in, so terminate() reaches the compilers a
    // build spawns and not only the build tool.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits,
                                  sizeof(limits));
    }

    STARTUPINFOW startup{};
    startup.cb         = sizeof(startup);
    startup.dwFlags    = STARTF_USESTDHANDLES;
    startup.hStdOutput = write_end;
    startup.hStdError  = write_end;  // merged, the way the POSIX side merges them
    startup.hStdInput  = ::GetStdHandle(STD_INPUT_HANDLE);

    // The environment additions, as "NAME=VALUE" the way the POSIX side takes
    // them. Applied to this process before the child inherits it: there is no
    // fork to make the change private, so it is set and left, which matches
    // what putenv does on the other side.
    for (const std::string& entry : extra_env) {
        const std::size_t equals = entry.find('=');
        if (equals != std::string::npos) {
            ::SetEnvironmentVariableA(entry.substr(0, equals).c_str(),
                                      entry.c_str() + equals + 1);
        }
    }

    std::wstring line = command_line(argv);
    const std::wstring directory = cwd.empty() ? std::wstring() : cwd.wstring();

    PROCESS_INFORMATION info{};
    const BOOL started = ::CreateProcessW(
        nullptr, line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr,
        directory.empty() ? nullptr : directory.c_str(), &startup, &info);

    ::CloseHandle(write_end);  // the child owns its copy now
    if (started == 0) {
        error = argv[0] + ": " + last_error();
        ::CloseHandle(read_end);
        if (job != nullptr) {
            ::CloseHandle(job);
        }
        return false;
    }

    // Suspended until it is in the job, so a process that spawns children
    // immediately cannot produce one outside it.
    if (job != nullptr) {
        ::AssignProcessToJobObject(job, info.hProcess);
    }
    ::ResumeThread(info.hThread);
    ::CloseHandle(info.hThread);

    process_ = info.hProcess;
    job_     = job;
    read_    = read_end;
    status_  = -1;
    reaped_  = false;
    eof_     = false;
    buffer_.clear();
    return true;
}

bool Subprocess::read_line(std::string& line) {
    for (;;) {
        // Serve a complete line out of what we already have before reading
        // more, so a chunk containing several lines is not lost.
        const std::size_t newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            return true;
        }

        if (eof_) {
            if (buffer_.empty()) {
                return false;
            }
            line = std::move(buffer_);
            buffer_.clear();
            return true;
        }

        char  chunk[4096];
        DWORD got = 0;
        if (read_ == nullptr
            || ::ReadFile(static_cast<HANDLE>(read_), chunk, sizeof(chunk), &got, nullptr) == 0
            || got == 0) {
            // A closed write end arrives as ERROR_BROKEN_PIPE rather than a
            // zero-length read, and either way there is nothing more coming.
            eof_ = true;
        } else {
            buffer_.append(chunk, static_cast<std::size_t>(got));
        }
    }
}

int Subprocess::wait() {
    if (reaped_) {
        return status_;
    }
    if (process_ == nullptr) {
        return -1;
    }
    ::WaitForSingleObject(static_cast<HANDLE>(process_), INFINITE);

    DWORD code = 0;
    status_ = ::GetExitCodeProcess(static_cast<HANDLE>(process_), &code) != 0
                  ? static_cast<int>(code)
                  : -1;
    reaped_ = true;
    return status_;
}

void Subprocess::terminate() {
    interrupt();
    close_pipe();  // unblocks a read_line() waiting on output
}

void Subprocess::interrupt() {
    if (job_ != nullptr) {
        ::TerminateJobObject(static_cast<HANDLE>(job_), 1);
    } else if (process_ != nullptr) {
        ::TerminateProcess(static_cast<HANDLE>(process_), 1);
    }
}

bool on_path(const std::string& program) {
    if (program.empty()) {
        return true;
    }
    // SearchPathW is what the loader itself uses, so it agrees with what will
    // actually run -- including PATHEXT, which a hand-rolled PATH walk gets
    // wrong for "cmake" meaning "cmake.exe".
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, program.c_str(),
                                           static_cast<int>(program.size()), nullptr, 0);
    std::wstring name(static_cast<std::size_t>(wide), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, program.c_str(), static_cast<int>(program.size()),
                          name.data(), wide);

    wchar_t found[MAX_PATH];
    if (::SearchPathW(nullptr, name.c_str(), L".exe", MAX_PATH, found, nullptr) != 0) {
        return true;
    }
    return ::SearchPathW(nullptr, name.c_str(), nullptr, MAX_PATH, found, nullptr) != 0;
}

#endif  // _WIN32

}  // namespace crucible::util
