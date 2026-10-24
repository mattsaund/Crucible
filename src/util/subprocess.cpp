// SPDX-License-Identifier: MIT
//
// fork/exec with a merged output pipe. See subprocess.hpp for why not popen.
#include "crucible/util/subprocess.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <deque>
#include <mutex>
#include <thread>
#include <utility>

#if defined(_WIN32)
#  define WIN32_LEAN_AND_MEAN
#  include <windows.h>
#else
#  include <fcntl.h>
#  include <poll.h>
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace crucible::util {
namespace {

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

/// How long a child asked to stop has before it is killed.
constexpr std::int64_t kGraceMs = 3000;

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
    close_input();
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
        close_input();
        pid_.store(other.pid_.exchange(-1));
        fd_     = std::exchange(other.fd_, -1);
        in_fd_  = std::exchange(other.in_fd_, -1);
        status_ = std::exchange(other.status_, -1);
        reaped_ = std::exchange(other.reaped_, false);
        buffer_ = std::move(other.buffer_);
        eof_    = std::exchange(other.eof_, false);
        stop_asked_ms_.store(other.stop_asked_ms_.exchange(0));
        abandoned_.store(other.abandoned_.exchange(false));
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
    return start(argv, cwd, extra_env, error, Streams{});
}

bool Subprocess::start(const std::vector<std::string>& argv,
                       const std::filesystem::path& cwd,
                       const std::vector<std::string>& extra_env,
                       std::string& error,
                       const Streams& streams) {
    if (argv.empty()) {
        error = "no command given";
        return false;
    }

    // Every end of every pipe is close-on-exec, the child's as well: dup2
    // gives the child its own copy without the flag, and a program started
    // at the same moment on another thread must inherit none of them, or it
    // holds this pipe open and the end of it never comes. The flags are set
    // apart from pipe() where there is no pipe2, so pipes and forks take
    // turns: a fork between the two would copy an end without its flag.
    static std::mutex      spawning;
    std::unique_lock<std::mutex> turn(spawning);
    const auto private_pipe = [](int ends[2]) {
#if defined(__linux__)
        return ::pipe2(ends, O_CLOEXEC) == 0;
#else
        if (::pipe(ends) != 0) {
            return false;
        }
        ::fcntl(ends[0], F_SETFD, FD_CLOEXEC);
        ::fcntl(ends[1], F_SETFD, FD_CLOEXEC);
        return true;
#endif
    };
    int fds[2] = {-1, -1};
    if (!private_pipe(fds)) {
        error = std::string("pipe: ") + std::strerror(errno);
        return false;
    }

    int in[2] = {-1, -1};
    if (streams.input) {
        if (!private_pipe(in)) {
            error = std::string("pipe: ") + std::strerror(errno);
            ::close(fds[0]);
            ::close(fds[1]);
            return false;
        }
        // A write to a child that has died raises SIGPIPE, whose default is
        // to end this process. write_line reports it as a failed write.
        static std::once_flag ignored;
        std::call_once(ignored, [] { ::signal(SIGPIPE, SIG_IGN); });
    }

    int errors = -1;
    if (!streams.errors.empty()) {
        // Its folder made first: an MCP server's log is in the data folder,
        // which a fresh machine does not have yet, and the server would
        // otherwise not start for want of somewhere to complain.
        std::error_code made;
        std::filesystem::create_directories(streams.errors.parent_path(), made);
        errors = ::open(streams.errors.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (errors < 0) {
            error = "could not open " + streams.errors.string() + ": " + std::strerror(errno);
            ::close(fds[0]);
            ::close(fds[1]);
            if (in[0] >= 0) {
                ::close(in[0]);
                ::close(in[1]);
            }
            return false;
        }
    }

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(fds[0]);
        ::close(fds[1]);
        if (in[0] >= 0) {
            ::close(in[0]);
            ::close(in[1]);
        }
        if (errors >= 0) {
            ::close(errors);
        }
        return false;
    }

    if (pid == 0) {
        // --- child ---------------------------------------------------------
        // Nothing here may allocate or throw in a way that matters: on any
        // failure the child calls _exit directly rather than unwinding through
        // a parent's destructors.
        ::close(fds[0]);
        ::dup2(fds[1], STDOUT_FILENO);
        ::dup2(errors >= 0 ? errors : fds[1], STDERR_FILENO);
        ::close(fds[1]);
        if (in[0] >= 0) {
            ::close(in[1]);
            ::dup2(in[0], STDIN_FILENO);
            ::close(in[0]);
        }

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
    if (in[0] >= 0) {
        ::close(in[0]);
    }
    if (errors >= 0) {
        ::close(errors);
    }
    turn.unlock();
    close_input();
    pid_    = pid;
    fd_     = fds[0];
    in_fd_  = in[1];
    status_ = -1;
    reaped_ = false;
    eof_    = false;
    buffer_.clear();
    stop_asked_ms_.store(0);
    abandoned_.store(false);
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

        // Waited on a few times a second rather than for good, so a
        // terminate() from another thread is seen without the pipe being
        // closed under this read.
        if (fd_ < 0 || abandoned_.load(std::memory_order_relaxed)) {
            eof_ = true;
            continue;
        }
        pollfd watched{fd_, POLLIN, 0};
        const int ready = ::poll(&watched, 1, 200);
        if (ready == 0 || (ready < 0 && errno == EINTR)) {
            continue;
        }
        if (ready < 0) {
            eof_ = true;
            continue;
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

bool Subprocess::try_wait(int& status) {
    const int pid = pid_.load();
    if (reaped_ || pid <= 0) {
        status = reaped_ ? status_ : -1;
        return true;
    }
    int raw = 0;
    const pid_t got = ::waitpid(pid, &raw, WNOHANG);
    if (got == 0 || (got < 0 && errno == EINTR)) {
        return false;
    }
    if (got < 0) {
        status_ = -1;
    } else if (WIFEXITED(raw)) {
        status_ = WEXITSTATUS(raw);
    } else if (WIFSIGNALED(raw)) {
        status_ = 128 + WTERMSIG(raw);
    } else {
        status_ = -1;
    }
    reaped_ = true;
    pid_    = -1;
    status  = status_;
    return true;
}

int Subprocess::wait() {
    // Looked at rather than blocked on, so a stop asked for meanwhile, on
    // any thread, is followed through: one ignored for kGraceMs ends with
    // SIGKILL, to the whole group, while it is still there to be sent to.
    int  status = -1;
    int  nap    = 1;
    bool killed = false;
    while (!try_wait(status)) {
        const std::int64_t asked = stop_asked_ms_.load();
        if (asked != 0 && !killed && now_ms() - asked >= kGraceMs) {
            ::kill(-pid_.load(), SIGKILL);
            killed = true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(nap));
        nap = std::min(nap * 2, 50);
    }
    return status;
}

void Subprocess::terminate() {
    interrupt();
    abandoned_.store(true);   // read_line ends at its next look; the pipe stays its thread's
}

void Subprocess::interrupt() {
    if (const int pid = pid_.load(); pid > 0) {
        // Negative pid signals the whole process group, which is why the
        // child put itself in one: a build is cmake -> make -> a dozen
        // compilers.
        std::int64_t never = 0;
        stop_asked_ms_.compare_exchange_strong(never, now_ms());
        ::kill(-pid, SIGTERM);
    }
}

bool Subprocess::running() const {
    return pid_ > 0;
}

bool Subprocess::write_line(std::string_view line) {
    if (in_fd_ < 0) {
        return false;
    }
    std::string out(line);
    out += '\n';
    std::size_t written = 0;
    while (written < out.size()) {
        const ssize_t sent = ::write(in_fd_, out.data() + written, out.size() - written);
        if (sent > 0) {
            written += static_cast<std::size_t>(sent);
        } else if (sent < 0 && errno == EINTR) {
            continue;
        } else {
            return false;   // EPIPE: the child is gone
        }
    }
    return true;
}

void Subprocess::close_input() {
    if (in_fd_ >= 0) {
        ::close(in_fd_);
        in_fd_ = -1;
    }
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

bool start_detached(const std::vector<std::string>& argv, std::string& error) {
    // A subshell put in the background: the program is the shell's
    // grandchild, handed to the system when the shell exits, and outlives
    // this one.
    std::vector<std::string> shell{"/bin/sh", "-c", "(\"$0\" \"$@\" >/dev/null 2>&1 </dev/null &)"};
    shell.insert(shell.end(), argv.begin(), argv.end());
    Subprocess child;
    if (!child.start(shell, {}, {}, error)) {
        return false;
    }
    child.wait();
    return true;
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

std::wstring wide_of(std::string_view text) {
    const int wide = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(wide), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), wide);
    return out;
}

/// A variable's name: up to the first '=' after its first character, since
/// "=C:=C:\\work" is the current directory of drive C, named "=C:".
std::wstring name_of(const std::wstring& entry) {
    return entry.substr(0, entry.find(L'=', 1));
}

bool same_name(const std::wstring& a, const std::wstring& b) {
    return ::CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
}

/// The child's environment: this process's, with `extra` -- "NAME=VALUE" --
/// over it, as a block of its own. Set on this process instead, they were
/// handed to every program started after the one they were for: a GitHub
/// token to the next build's compiler.
std::wstring environment_block(const std::vector<std::string>& extra) {
    std::vector<std::wstring> entries;
    if (wchar_t* inherited = ::GetEnvironmentStringsW(); inherited != nullptr) {
        for (const wchar_t* at = inherited; *at != L'\0'; at += std::wcslen(at) + 1) {
            entries.emplace_back(at);
        }
        ::FreeEnvironmentStringsW(inherited);
    }
    for (const std::string& one : extra) {
        if (one.find('=', 1) == std::string::npos) {
            continue;
        }
        const std::wstring entry = wide_of(one);
        const std::wstring name  = name_of(entry);
        const auto same = std::find_if(entries.begin(), entries.end(),
                                       [&name](const std::wstring& e) { return same_name(name_of(e), name); });
        if (same != entries.end()) {
            *same = entry;
        } else {
            entries.push_back(entry);
        }
    }
    // In order of name, without regard to case, the way Windows keeps them.
    std::sort(entries.begin(), entries.end(), [](const std::wstring& a, const std::wstring& b) {
        return ::CompareStringOrdinal(name_of(a).c_str(), -1, name_of(b).c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    std::wstring block;
    for (const std::wstring& entry : entries) {
        block += entry;
        block += L'\0';
    }
    block += L'\0';
    return block;
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
    close_input();
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
        close_input();
        process_ = std::exchange(other.process_, nullptr);
        job_     = std::exchange(other.job_, nullptr);
        read_    = std::exchange(other.read_, nullptr);
        write_   = std::exchange(other.write_, nullptr);
        status_  = std::exchange(other.status_, -1);
        reaped_  = std::exchange(other.reaped_, false);
        buffer_  = std::move(other.buffer_);
        eof_     = std::exchange(other.eof_, false);
        stop_asked_ms_.store(other.stop_asked_ms_.exchange(0));
        abandoned_.store(other.abandoned_.exchange(false));
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
    return start(argv, cwd, extra_env, error, Streams{});
}

bool Subprocess::start(const std::vector<std::string>& argv,
                       const std::filesystem::path& cwd,
                       const std::vector<std::string>& extra_env,
                       std::string& error,
                       const Streams& streams) {
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

    // Standard input, the other way round: the child's end is inheritable
    // and this process's is not.
    HANDLE input_read  = nullptr;
    HANDLE input_write = nullptr;
    if (streams.input) {
        if (::CreatePipe(&input_read, &input_write, &attributes, 0) == 0) {
            error = "CreatePipe: " + last_error();
            ::CloseHandle(read_end);
            ::CloseHandle(write_end);
            return false;
        }
        ::SetHandleInformation(input_write, HANDLE_FLAG_INHERIT, 0);
    }

    HANDLE errors = nullptr;
    if (!streams.errors.empty()) {
        std::error_code made;   // its folder made first, as above
        std::filesystem::create_directories(streams.errors.parent_path(), made);
        errors = ::CreateFileW(streams.errors.wstring().c_str(), FILE_APPEND_DATA,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (errors == INVALID_HANDLE_VALUE) {
            error = "could not open " + streams.errors.string() + ": " + last_error();
            ::CloseHandle(read_end);
            ::CloseHandle(write_end);
            if (input_read != nullptr) {
                ::CloseHandle(input_read);
                ::CloseHandle(input_write);
            }
            return false;
        }
    }

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
    // Merged, the way the POSIX side merges them, unless asked for a file.
    startup.hStdError  = errors != nullptr ? errors : write_end;
    startup.hStdInput  = input_read != nullptr ? input_read : ::GetStdHandle(STD_INPUT_HANDLE);

    // The environment additions, as "NAME=VALUE" the way the POSIX side takes
    // them: in a block of the child's own, this process's left as it was.
    std::wstring environment = extra_env.empty() ? std::wstring() : environment_block(extra_env);

    std::wstring line = command_line(argv);
    const std::wstring directory = cwd.empty() ? std::wstring() : cwd.wstring();

    PROCESS_INFORMATION info{};
    const BOOL started = ::CreateProcessW(
        nullptr, line.data(), nullptr, nullptr, TRUE,
        CREATE_NO_WINDOW | CREATE_SUSPENDED | (environment.empty() ? 0 : CREATE_UNICODE_ENVIRONMENT),
        environment.empty() ? nullptr : environment.data(),
        directory.empty() ? nullptr : directory.c_str(), &startup, &info);

    ::CloseHandle(write_end);  // the child owns its copy now
    if (input_read != nullptr) {
        ::CloseHandle(input_read);
    }
    if (errors != nullptr) {
        ::CloseHandle(errors);
    }
    if (started == 0) {
        error = argv[0] + ": " + last_error();
        ::CloseHandle(read_end);
        if (input_write != nullptr) {
            ::CloseHandle(input_write);
        }
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

    close_input();
    process_ = info.hProcess;
    job_     = job;
    read_    = read_end;
    write_   = input_write;
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
        if (read_ == nullptr || abandoned_.load(std::memory_order_relaxed)
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

bool Subprocess::try_wait(int& status) {
    if (reaped_ || process_ == nullptr) {
        status = reaped_ ? status_ : -1;
        return true;
    }
    if (::WaitForSingleObject(static_cast<HANDLE>(process_), 0) != WAIT_OBJECT_0) {
        return false;
    }
    status = wait();
    return true;
}

void Subprocess::terminate() {
    // Ending the job ends everything in it at once, so there is no grace to
    // give and nothing left to hold the pipe open.
    interrupt();
    abandoned_.store(true);
}

bool Subprocess::write_line(std::string_view line) {
    if (write_ == nullptr) {
        return false;
    }
    std::string out(line);
    out += '\n';
    std::size_t written = 0;
    while (written < out.size()) {
        DWORD sent = 0;
        if (::WriteFile(static_cast<HANDLE>(write_), out.data() + written,
                        static_cast<DWORD>(out.size() - written), &sent, nullptr) == 0
            || sent == 0) {
            return false;   // the child is gone
        }
        written += sent;
    }
    return true;
}

void Subprocess::close_input() {
    if (write_ != nullptr) {
        ::CloseHandle(static_cast<HANDLE>(write_));
        write_ = nullptr;
    }
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

bool start_detached(const std::vector<std::string>& argv, std::string& error) {
    // Not through Subprocess, whose job object ends everything in it when
    // this program does -- the one thing a program meant to outlive it must
    // not be part of.
    std::wstring line = command_line(argv);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION info{};
    if (::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                         CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW | CREATE_BREAKAWAY_FROM_JOB,
                         nullptr, nullptr, &startup, &info) == 0
        && ::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE,
                            CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW, nullptr, nullptr, &startup,
                            &info) == 0) {
        error = "could not start " + (argv.empty() ? std::string() : argv.front()) + ": " + last_error();
        return false;
    }
    ::CloseHandle(info.hThread);
    ::CloseHandle(info.hProcess);
    return true;
}

#endif  // _WIN32

namespace {
std::atomic<bool> g_runs_stopped{false};
}  // namespace

void stop_runs() {
    g_runs_stopped.store(true);
}

Ran run(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
        const std::vector<std::string>& extra_env, int timeout_seconds,
        const std::function<bool()>& stop, std::size_t keep) {
    Ran        ran;
    if (g_runs_stopped.load()) {
        ran.error = "Crucible is closing";
        return ran;
    }
    Subprocess child;
    if (!child.start(argv, cwd, extra_env, ran.error)) {
        return ran;
    }
    ran.started = true;

    // The reading on a thread of its own, and everything else -- the
    // deadline, the Stop, the reaping -- on this one, so nothing that
    // touches the process races the read. The first half of `keep` is
    // kept whole; after it, the latest half.
    std::string             head;
    std::deque<std::string> tail;
    std::size_t             tail_bytes = 0;
    std::size_t             cut        = 0;
    std::atomic<bool>       read{false};
    std::thread reader([&] {
        std::string line;
        while (child.read_line(line)) {
            line += '\n';
            if (head.size() + line.size() <= keep / 2) {
                head += line;
                continue;
            }
            tail_bytes += line.size();
            tail.push_back(std::move(line));
            while (tail_bytes > keep / 2 && tail.size() > 1) {
                tail_bytes -= tail.front().size();
                cut        += tail.front().size();
                tail.pop_front();
            }
        }
        read.store(true);
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(timeout_seconds, 1));
    std::chrono::steady_clock::time_point ended{};
    bool asked = false;
    for (;;) {
        const bool done = read.load();
        if (ended == std::chrono::steady_clock::time_point{} && child.try_wait(ran.status)) {
            ended = std::chrono::steady_clock::now();
        }
        if (done && ended != std::chrono::steady_clock::time_point{}) {
            break;
        }
        // Over, but something it left running in the background still holds
        // the output open: what it printed is all there is to wait for.
        if (ended != std::chrono::steady_clock::time_point{}
            && std::chrono::steady_clock::now() - ended > std::chrono::seconds(1)) {
            child.terminate();
        } else if (!asked && ((stop && stop()) || g_runs_stopped.load())) {
            asked       = true;
            ran.stopped = true;
            child.terminate();
        } else if (!asked && std::chrono::steady_clock::now() >= deadline) {
            asked         = true;
            ran.timed_out = true;
            child.terminate();
        }
        // Asked and ignored: wait() kills it after its grace.
        if (asked && done) {
            ran.status = child.wait();
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    reader.join();
    if (ended == std::chrono::steady_clock::time_point{}) {
        ran.status = child.wait();
    }
    ran.output = std::move(head);
    if (cut > 0) {
        ran.output += "[... " + std::to_string(cut) + " bytes not kept ...]\n";
    }
    for (std::string& line : tail) {
        ran.output += line;
    }
    return ran;
}

}  // namespace crucible::util
