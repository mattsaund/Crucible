// SPDX-License-Identifier: MIT
//
// See terminal.hpp. Each terminal has a thread of its own that reads what the
// shell prints and writes what is typed into it (and on Windows a second,
// since a pipe there cannot be waited on both ways at once), and they are
// kept in one table behind one lock. What a terminal printed is kept until it
// is closed, the last few megabytes of it, and read from by a count of bytes,
// so a page that has fallen behind, or been loaded again, reads on from
// wherever it is.
#include "crucible/tools/terminal.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>

#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"

#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#else
#  include <cerrno>
#  include <csignal>
#  include <fcntl.h>
#  include <poll.h>
#  include <pwd.h>
#  include <sys/ioctl.h>
#  include <sys/types.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#  if defined(__linux__)
#    include <sys/syscall.h>
#  endif
#  if defined(__APPLE__)
#    include <crt_externs.h>
#  endif
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
extern char** environ;
#endif

namespace crucible::tools::terminal {
namespace {

/// How much of what a terminal printed is kept for the page to read: a
/// screen many times over, and what a page loaded again draws from.
constexpr std::size_t kKeep = std::size_t{2} << 20;

/// The most that may wait to be typed into a shell that is not reading it.
constexpr std::size_t kMostTyped = std::size_t{4} << 20;

/// A shell asked to go has this long before it is made to.
constexpr std::chrono::milliseconds kGrace{1000};

struct Term {
    int         id = 0;
    std::string shell;
    std::string cwd;

    /// Everything below it, and the handle a resize uses.
    std::mutex    mutex;
    std::string   kept;          ///< the last kKeep bytes or so of what it printed
    std::uint64_t total = 0;     ///< everything it has printed, counted
    std::string   typed;         ///< written to it, and not yet passed on
    bool          running = true;
    int           status  = 0;

    std::atomic<bool> closing{false};
    std::thread       worker;

#if defined(_WIN32)
    void*       console = nullptr;   ///< the pseudo console, an HPCON
    HANDLE      process = nullptr;
    HANDLE      job     = nullptr;   ///< what the shell started, so all of it ends with it
    HANDLE      to      = nullptr;   ///< what is typed, to the console
    HANDLE      from    = nullptr;   ///< what the console prints
    HANDLE      stirred = nullptr;   ///< an event: something was typed, or it is closing
    std::thread reader;
#else
    int   master  = -1;
    pid_t pid     = -1;
    int   wake[2] = {-1, -1};       ///< the worker's own pipe, to stir it from a wait
#endif

    Term() = default;
    Term(const Term&)            = delete;
    Term& operator=(const Term&) = delete;

    ~Term() {
#if !defined(_WIN32)
        for (const int fd : wake) {
            if (fd >= 0) {
                ::close(fd);
            }
        }
#else
        if (stirred != nullptr) {
            ::CloseHandle(stirred);
        }
#endif
    }

    /// More of what it printed.
    void printed(const char* data, std::size_t size) {
        const std::lock_guard<std::mutex> lock(mutex);
        kept.append(data, size);
        total += size;
        // Cut back by half as much again at a time, so a terminal printing
        // fast is not moving two megabytes for every piece of it.
        if (kept.size() > kKeep + kKeep / 2) {
            kept.erase(0, kept.size() - kKeep);
        }
    }

    /// Stir the worker from its wait: something was typed, or it is closing.
    void stir() {
#if defined(_WIN32)
        ::SetEvent(stirred);
#else
        const char one = 1;
        [[maybe_unused]] const auto put = ::write(wake[1], &one, 1);
#endif
    }

    /// It has ended, as `code` says.
    void ended(int code) {
        const std::lock_guard<std::mutex> lock(mutex);
        running = false;
        status  = code;
    }
};

/// Every terminal there is, by id. Never destroyed: a static's destructor
/// runs as the program exits, and a terminal's thread could still be
/// finishing then.
struct Table {
    std::mutex                            mutex;
    std::map<int, std::shared_ptr<Term>>  terms;
    int                                   next = 1;
};

Table& table() {
    static Table* const one = new Table;
    return *one;
}

std::shared_ptr<Term> find(int id) {
    Table&                            all = table();
    const std::lock_guard<std::mutex> lock(all.mutex);
    const auto                        it = all.terms.find(id);
    return it == all.terms.end() ? nullptr : it->second;
}

int keep(const std::shared_ptr<Term>& term) {
    Table&                            all = table();
    const std::lock_guard<std::mutex> lock(all.mutex);
    term->id                 = all.next++;
    all.terms[term->id]      = term;
    return term->id;
}

/// Where `size` bytes from `at` in `text` end on a whole character: earlier,
/// when the last of them starts one they do not finish.
std::size_t whole_end(const std::string& text, std::size_t at, std::size_t end) {
    for (std::size_t back = 1; back <= 3 && end >= at + back; ++back) {
        const auto c = static_cast<unsigned char>(text[end - back]);
        if ((c & 0xC0U) == 0x80U) {
            continue;   // inside a character: look further back
        }
        const std::size_t length = c < 0x80U ? 1 : static_cast<std::size_t>(crucible::detail::utf8_length(c));
        return back < length ? end - back : end;
    }
    return end;
}

/// Its last stage: the worker has stopped, the terminal is out of the table.
void finish(const std::shared_ptr<Term>& term) {
    term->closing.store(true);
    term->stir();
#if defined(_WIN32)
    // A write blocked on a shell that is not reading is let go.
    if (term->worker.joinable()) {
        ::CancelSynchronousIo(static_cast<HANDLE>(term->worker.native_handle()));
    }
#endif
    if (term->worker.joinable()) {
        term->worker.join();
    }
}

#if !defined(_WIN32)

// --- Linux and macOS: a pseudo-terminal -----------------------------------------

char** environment() {
#if defined(__APPLE__)
    return *_NSGetEnviron();
#else
    return environ;
#endif
}

bool runnable(const std::string& path) {
    return !path.empty() && path.front() == '/' && ::access(path.c_str(), X_OK) == 0;
}

/// The shell the person logs in with: $SHELL, or the account's, or sh.
std::string login_shell() {
    if (const char* shell = std::getenv("SHELL"); shell != nullptr && runnable(shell)) {
        return shell;
    }
    passwd             account{};
    passwd*            found = nullptr;
    std::vector<char>  buffer(16384);
    if (::getpwuid_r(::getuid(), &account, buffer.data(), buffer.size(), &found) == 0 && found != nullptr
        && found->pw_shell != nullptr && runnable(found->pw_shell)) {
        return found->pw_shell;
    }
    return runnable("/bin/bash") ? "/bin/bash" : "/bin/sh";
}

/// `program` as a path: as it is when it has a slash in it, else found on
/// PATH. Done before the fork, since after it only system calls are safe.
std::string resolve(const std::string& program) {
    if (program.find('/') != std::string::npos) {
        return ::access(program.c_str(), X_OK) == 0 ? program : std::string();
    }
    const char*       path = std::getenv("PATH");
    const std::string dirs = path != nullptr ? path : "/usr/bin:/bin";
    for (std::size_t start = 0; start <= dirs.size();) {
        const std::size_t end  = std::min(dirs.find(':', start), dirs.size());
        const std::string dir  = dirs.substr(start, end - start);
        const std::string here = (dir.empty() ? std::string(".") : dir) + "/" + program;
        if (::access(here.c_str(), X_OK) == 0) {
            return here;
        }
        start = end + 1;
    }
    return {};
}

std::vector<char*> pointers(std::vector<std::string>& strings) {
    std::vector<char*> out;
    out.reserve(strings.size() + 1);
    for (std::string& one : strings) {
        out.push_back(one.data());
    }
    out.push_back(nullptr);
    return out;
}

int exit_code(int status) {
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
}

/// The worker: what the shell prints in, what is typed out, until the
/// shell has gone or the terminal is closed.
void serve(const std::shared_ptr<Term>& term) {
    std::vector<char> buffer(std::size_t{64} << 10);
    bool              reaped = false;
    int               code   = 0;
    const auto drain = [&term, &buffer] {
        for (;;) {
            const ssize_t got = ::read(term->master, buffer.data(), buffer.size());
            if (got <= 0) {
                return;
            }
            term->printed(buffer.data(), static_cast<std::size_t>(got));
        }
    };
    // Nothing holds the other end: the shell has gone, or let go of its
    // terminal. Watched no more, or the poll would answer at once forever.
    bool hung_up = false;
    while (!term->closing.load()) {
        bool typing = false;
        {
            const std::lock_guard<std::mutex> lock(term->mutex);
            typing = !term->typed.empty();
        }
        pollfd waits[2] = {{hung_up ? -1 : term->master, static_cast<short>(POLLIN | (typing ? POLLOUT : 0)), 0},
                           {term->wake[0], POLLIN, 0}};
        // Woken by output, by typing and by closing; and four times a second
        // regardless, to see whether the shell is still there, which its
        // terminal does not say while something it started holds it open.
        if (::poll(waits, 2, 250) < 0 && errno != EINTR) {
            break;
        }
        if ((waits[1].revents & POLLIN) != 0) {
            char stirred[64];
            while (::read(term->wake[0], stirred, sizeof(stirred)) > 0) {
            }
        }
        if ((waits[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0) {
            const ssize_t got = ::read(term->master, buffer.data(), buffer.size());
            if (got > 0) {
                term->printed(buffer.data(), static_cast<std::size_t>(got));
            } else if (got == 0 || (errno != EAGAIN && errno != EINTR)) {
                hung_up = true;
            }
        }
        if (!hung_up && (waits[0].revents & POLLOUT) != 0) {
            std::string out;
            {
                const std::lock_guard<std::mutex> lock(term->mutex);
                out = term->typed.substr(0, 4096);
            }
            const ssize_t put = ::write(term->master, out.data(), out.size());
            if (put > 0) {
                const std::lock_guard<std::mutex> lock(term->mutex);
                term->typed.erase(0, static_cast<std::size_t>(put));
            }
        }
        int status = 0;
        if (::waitpid(term->pid, &status, WNOHANG) == term->pid) {
            reaped = true;
            code   = exit_code(status);
            drain();   // what it said last
            break;
        }
    }
    // The terminal hangs up: the shell, and what runs in it, are sent SIGHUP
    // as they would be by a terminal window being closed.
    {
        const std::lock_guard<std::mutex> lock(term->mutex);
        ::close(term->master);
        term->master = -1;
    }
    if (!reaped) {
        const auto until = std::chrono::steady_clock::now() + kGrace;
        int        status = 0;
        while (!reaped && std::chrono::steady_clock::now() < until) {
            if (::waitpid(term->pid, &status, WNOHANG) == term->pid) {
                reaped = true;
                code   = exit_code(status);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        if (!reaped) {
            ::kill(-term->pid, SIGKILL);
            ::kill(term->pid, SIGKILL);
            if (::waitpid(term->pid, &status, 0) == term->pid) {
                code = exit_code(status);
            }
        }
    }
    term->ended(code);
}

int start(const std::shared_ptr<Term>& term, const Options& options, std::string& error) {
    std::vector<std::string> argv = options.command;
    if (argv.empty()) {
        argv = {login_shell()};
#if defined(__APPLE__)
        // A login shell, as Terminal starts: it reads the profile that sets
        // up PATH, which a Mac's shells otherwise start without.
        argv.emplace_back("-l");
#endif
    }
    const std::string program = resolve(argv.front());
    if (program.empty()) {
        error = argv.front() + " was not found";
        return 0;
    }
    term->shell = std::filesystem::path(program).filename().string();
    term->cwd   = options.cwd.string();

    const int master = ::posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0) {
        error = std::string("no pseudo-terminal: ") + std::strerror(errno);
        return 0;
    }
    ::fcntl(master, F_SETFD, FD_CLOEXEC);
    std::string slave;
    if (::grantpt(master) == 0 && ::unlockpt(master) == 0) {
        // ptsname answers in a buffer of its own, which two at once share.
        static std::mutex           naming;
        const std::lock_guard<std::mutex> lock(naming);
        if (const char* name = ::ptsname(master); name != nullptr) {
            slave = name;
        }
    }
    if (slave.empty()) {
        error = std::string("no pseudo-terminal: ") + std::strerror(errno);
        ::close(master);
        return 0;
    }
    ::fcntl(master, F_SETFL, ::fcntl(master, F_GETFL) | O_NONBLOCK);
    // The shell's end, opened here and held across the fork, as openpty
    // does: a terminal whose other end nothing has opened yet reads as hung
    // up, and the worker would take the shell for gone before it started.
    const int tty = ::open(slave.c_str(), O_RDWR | O_NOCTTY);
    if (tty < 0) {
        error = std::string("no pseudo-terminal: ") + std::strerror(errno);
        ::close(master);
        return 0;
    }
    ::fcntl(tty, F_SETFD, FD_CLOEXEC);
    winsize size{};
    size.ws_col = static_cast<unsigned short>(std::clamp(options.cols, 2, 1000));
    size.ws_row = static_cast<unsigned short>(std::clamp(options.rows, 1, 500));
    ::ioctl(tty, TIOCSWINSZ, &size);
#if defined(IUTF8)
    // So a backspace takes away a whole character, not a byte of one.
    termios modes{};
    if (::tcgetattr(tty, &modes) == 0) {
        modes.c_iflag |= IUTF8;
        ::tcsetattr(tty, TCSANOW, &modes);
    }
#endif

    int wake[2] = {-1, -1};
    if (::pipe(wake) != 0) {
        error = std::string("pipe: ") + std::strerror(errno);
        ::close(master);
        ::close(tty);
        return 0;
    }
    for (const int fd : wake) {
        ::fcntl(fd, F_SETFD, FD_CLOEXEC);
        ::fcntl(fd, F_SETFL, ::fcntl(fd, F_GETFL) | O_NONBLOCK);
    }

    // Its environment is this one's, saying what kind of terminal it is in.
    std::vector<std::string> env;
    for (char** at = environment(); at != nullptr && *at != nullptr; ++at) {
        const std::string_view entry(*at);
        const std::string_view name = entry.substr(0, entry.find('='));
        if (name == "TERM" || name == "COLORTERM" || name == "TERM_PROGRAM" || name == "TERM_PROGRAM_VERSION"
            || name == "LINES" || name == "COLUMNS") {
            continue;
        }
        env.emplace_back(entry);
    }
    env.emplace_back("TERM=xterm-256color");
    env.emplace_back("COLORTERM=truecolor");
    env.emplace_back("TERM_PROGRAM=Crucible");
    env.emplace_back("TERM_PROGRAM_VERSION=" CRUCIBLE_VERSION);
    std::vector<char*> args = pointers(argv);
    std::vector<char*> envp = pointers(env);
    const std::string  cwd  = options.cwd.string();
    const long         open_max = ::sysconf(_SC_OPEN_MAX);
    const int          most_fd  = static_cast<int>(std::clamp<long>(open_max > 0 ? open_max : 1024, 256, 65536));

    const pid_t pid = ::fork();
    if (pid < 0) {
        error = std::string("fork: ") + std::strerror(errno);
        ::close(master);
        ::close(tty);
        ::close(wake[0]);
        ::close(wake[1]);
        return 0;
    }
    if (pid == 0) {
        // --- the child: system calls and nothing else, until exec ---------
        //
        // A session of its own, with the terminal as the one it is in, the
        // way a login does it.
        ::setsid();
#if defined(TIOCSCTTY)
        ::ioctl(tty, TIOCSCTTY, 0);
#endif
        ::dup2(tty, STDIN_FILENO);
        ::dup2(tty, STDOUT_FILENO);
        ::dup2(tty, STDERR_FILENO);
        if (tty > STDERR_FILENO) {
            ::close(tty);
        }
        // Nothing of this process's goes with it: a model's file, the API
        // server's socket, another terminal.
        bool closed = false;
#if defined(__linux__) && defined(SYS_close_range)
        closed = ::syscall(SYS_close_range, 3U, ~0U, 0U) == 0;
#endif
        for (int fd = 3; !closed && fd < most_fd; ++fd) {
            ::close(fd);
        }
        // Nor what it ignores or holds back. A shell started with SIGPIPE
        // ignored, as this process has it, never ends a `yes | head`.
#if defined(NSIG)
        for (int sig = 1; sig < NSIG; ++sig) {
            ::signal(sig, SIG_DFL);
        }
#endif
        sigset_t none;
        ::sigemptyset(&none);
        ::sigprocmask(SIG_SETMASK, &none, nullptr);
        if (!cwd.empty()) {
            [[maybe_unused]] const int moved = ::chdir(cwd.c_str());
        }
        ::execve(program.c_str(), args.data(), envp.data());
        ::_exit(127);
    }
    ::close(tty);   // the shell has it now
    term->master  = master;
    term->pid     = pid;
    term->wake[0] = wake[0];
    term->wake[1] = wake[1];
    // Running before it is in the table, so a close can never find one
    // whose thread has not started.
    term->worker = std::thread([term] { serve(term); });
    return keep(term);
}

bool resize_now(Term& term, int cols, int rows) {
    const std::lock_guard<std::mutex> lock(term.mutex);
    if (term.master < 0) {
        return false;
    }
    winsize size{};
    size.ws_col = static_cast<unsigned short>(cols);
    size.ws_row = static_cast<unsigned short>(rows);
    return ::ioctl(term.master, TIOCSWINSZ, &size) == 0;
}

#else

// --- Windows: a pseudo console ---------------------------------------------------
//
// Found when it is first wanted rather than linked: a Windows 10 older than
// 1809 has none, and a program that named it would not start there at all.

using CreateConsole = HRESULT(WINAPI*)(COORD, HANDLE, HANDLE, DWORD, void**);
using ResizeConsole = HRESULT(WINAPI*)(void*, COORD);
using CloseConsole  = void(WINAPI*)(void*);

struct Consoles {
    CreateConsole create = nullptr;
    ResizeConsole resize = nullptr;
    CloseConsole  close  = nullptr;
};

const Consoles& consoles() {
    static const Consoles found = [] {
        Consoles  calls;
        HMODULE   kernel = ::GetModuleHandleW(L"kernel32.dll");
        if (kernel != nullptr) {
            calls.create = reinterpret_cast<CreateConsole>(::GetProcAddress(kernel, "CreatePseudoConsole"));
            calls.resize = reinterpret_cast<ResizeConsole>(::GetProcAddress(kernel, "ResizePseudoConsole"));
            calls.close  = reinterpret_cast<CloseConsole>(::GetProcAddress(kernel, "ClosePseudoConsole"));
        }
        return calls;
    }();
    return found;
}

/// ProcThreadAttributeValue(22, FALSE, TRUE, FALSE), for an SDK that does
/// not name it.
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif

std::wstring wide(const std::string& text) {
    if (text.empty()) {
        return {};
    }
    const int size = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size > 0 ? size : 0), L'\0');
    if (size > 0) {
        ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    }
    return out;
}

std::string last_error() {
    const DWORD code = ::GetLastError();
    char*       text = nullptr;
    const DWORD length =
        ::FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                         nullptr, code, 0, reinterpret_cast<char*>(&text), 0, nullptr);
    std::string message = length > 0 && text != nullptr ? std::string(text, length) : "error " + std::to_string(code);
    if (text != nullptr) {
        ::LocalFree(text);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return message;
}

/// PowerShell, the newer where there is one, or cmd.
std::vector<std::string> default_shell() {
    if (util::on_path("pwsh")) {
        return {"pwsh.exe", "-NoLogo"};
    }
    if (util::on_path("powershell")) {
        return {"powershell.exe", "-NoLogo"};
    }
    return {"cmd.exe"};
}

/// The worker: what is typed, written in, until the shell has gone or the
/// terminal is closed. Reading is the reader's, which blocks on the pipe.
void serve(const std::shared_ptr<Term>& term) {
    int code = 0;
    for (;;) {
        HANDLE      waits[2] = {term->process, term->stirred};
        const DWORD woke     = ::WaitForMultipleObjects(2, waits, FALSE, INFINITE);
        if (term->closing.load()) {
            break;
        }
        if (woke == WAIT_OBJECT_0) {
            DWORD exit = 0;
            ::GetExitCodeProcess(term->process, &exit);
            code = static_cast<int>(exit);
            break;
        }
        if (woke != WAIT_OBJECT_0 + 1) {
            break;
        }
        for (;;) {
            std::string out;
            {
                const std::lock_guard<std::mutex> lock(term->mutex);
                out = term->typed.substr(0, 4096);
            }
            DWORD put = 0;
            if (out.empty() || term->closing.load()
                || ::WriteFile(term->to, out.data(), static_cast<DWORD>(out.size()), &put, nullptr) == 0) {
                break;
            }
            const std::lock_guard<std::mutex> lock(term->mutex);
            term->typed.erase(0, put);
        }
    }
    // Not under the lock: closing the console waits for the reader to have
    // read the last of it, and the reader takes the lock to keep it.
    void* console = nullptr;
    {
        const std::lock_guard<std::mutex> lock(term->mutex);
        console       = term->console;
        term->console = nullptr;
    }
    if (console != nullptr) {
        consoles().close(console);
    }
    if (term->reader.joinable()) {
        ::CancelSynchronousIo(static_cast<HANDLE>(term->reader.native_handle()));
        term->reader.join();
    }
    // Whatever the shell started goes with it.
    if (term->job != nullptr) {
        ::TerminateJobObject(term->job, 1);
        ::CloseHandle(term->job);
        term->job = nullptr;
    }
    if (code == 0 && term->closing.load()) {
        DWORD exit = 0;
        if (::GetExitCodeProcess(term->process, &exit) != 0 && exit != STILL_ACTIVE) {
            code = static_cast<int>(exit);
        }
    }
    for (HANDLE* handle : {&term->process, &term->to, &term->from}) {
        if (*handle != nullptr) {
            ::CloseHandle(*handle);
            *handle = nullptr;
        }
    }
    term->ended(code);
}

int start(const std::shared_ptr<Term>& term, const Options& options, std::string& error) {
    const Consoles& calls = consoles();
    if (calls.create == nullptr || calls.resize == nullptr || calls.close == nullptr) {
        error = "this Windows has no pseudo console: a terminal needs Windows 10 1809 or later";
        return 0;
    }
    std::vector<std::string> argv = options.command.empty() ? default_shell() : options.command;
    term->shell = std::filesystem::path(argv.front()).stem().string();
    term->cwd   = options.cwd.string();

    HANDLE in_read = nullptr;
    HANDLE in_write = nullptr;
    HANDLE out_read = nullptr;
    HANDLE out_write = nullptr;
    if (::CreatePipe(&in_read, &in_write, nullptr, 0) == 0) {
        error = "CreatePipe: " + last_error();
        return 0;
    }
    if (::CreatePipe(&out_read, &out_write, nullptr, 0) == 0) {
        error = "CreatePipe: " + last_error();
        ::CloseHandle(in_read);
        ::CloseHandle(in_write);
        return 0;
    }
    COORD size{};
    size.X = static_cast<SHORT>(std::clamp(options.cols, 2, 1000));
    size.Y = static_cast<SHORT>(std::clamp(options.rows, 1, 500));
    void* console = nullptr;
    const HRESULT made = calls.create(size, in_read, out_write, 0, &console);
    // The console has its own copies of its ends.
    ::CloseHandle(in_read);
    ::CloseHandle(out_write);
    if (FAILED(made) || console == nullptr) {
        error = "the pseudo console could not be made";
        ::CloseHandle(in_write);
        ::CloseHandle(out_read);
        return 0;
    }
    const auto give_up = [&](std::string why) {
        error = std::move(why);
        calls.close(console);
        ::CloseHandle(in_write);
        ::CloseHandle(out_read);
        return 0;
    };

    SIZE_T bytes = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
    std::vector<unsigned char> memory(bytes);
    auto* attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
    if (bytes == 0 || ::InitializeProcThreadAttributeList(attributes, 1, 0, &bytes) == 0) {
        return give_up("InitializeProcThreadAttributeList: " + last_error());
    }
    if (::UpdateProcThreadAttribute(attributes, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE, console, sizeof(console),
                                    nullptr, nullptr)
        == 0) {
        ::DeleteProcThreadAttributeList(attributes);
        return give_up("UpdateProcThreadAttribute: " + last_error());
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    // Standard handles asked for and none given, so the shell has the
    // console's and none of this process's: the engine's are pipes.
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.lpAttributeList     = attributes;

    std::string line;
    for (const std::string& arg : argv) {
        if (!line.empty()) {
            line += ' ';
        }
        line += arg.find_first_of(" \t") == std::string::npos ? arg : "\"" + arg + "\"";
    }
    std::wstring       command   = wide(line);
    const std::wstring directory = options.cwd.empty() ? std::wstring() : options.cwd.wstring();

    // A job the whole tree lands in, so closing the terminal ends what was
    // started in it.
    HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
    if (job != nullptr) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
    }
    PROCESS_INFORMATION info{};
    const BOOL started = ::CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE,
                                          EXTENDED_STARTUPINFO_PRESENT | CREATE_SUSPENDED, nullptr,
                                          directory.empty() ? nullptr : directory.c_str(), &startup.StartupInfo,
                                          &info);
    ::DeleteProcThreadAttributeList(attributes);
    if (started == 0) {
        const std::string why = argv.front() + ": " + last_error();
        if (job != nullptr) {
            ::CloseHandle(job);
        }
        return give_up(why);
    }
    if (job != nullptr) {
        ::AssignProcessToJobObject(job, info.hProcess);
    }
    ::ResumeThread(info.hThread);
    ::CloseHandle(info.hThread);

    term->console = console;
    term->process = info.hProcess;
    term->job     = job;
    term->to      = in_write;
    term->from    = out_read;
    term->stirred = ::CreateEventW(nullptr, FALSE, FALSE, nullptr);
    term->reader  = std::thread([term] {
        std::vector<char> buffer(std::size_t{64} << 10);
        DWORD             got = 0;
        while (::ReadFile(term->from, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr) != 0
               && got > 0) {
            term->printed(buffer.data(), got);
        }
    });
    term->worker = std::thread([term] { serve(term); });
    return keep(term);
}

bool resize_now(Term& term, int cols, int rows) {
    const std::lock_guard<std::mutex> lock(term.mutex);
    if (term.console == nullptr) {
        return false;
    }
    COORD size{};
    size.X = static_cast<SHORT>(cols);
    size.Y = static_cast<SHORT>(rows);
    return SUCCEEDED(consoles().resize(term.console, size));
}

#endif

}  // namespace

int open(const Options& options, std::string& error) {
    auto term = std::make_shared<Term>();
    return start(term, options, error);
}

std::optional<Output> read(int id, std::uint64_t since, std::size_t most) {
    const std::shared_ptr<Term> term = find(id);
    if (!term) {
        return std::nullopt;
    }
    Output                            out;
    const std::lock_guard<std::mutex> lock(term->mutex);
    const std::uint64_t               oldest = term->total - term->kept.size();
    out.lost = since < oldest;
    std::size_t at = static_cast<std::size_t>(std::min(std::max(since, oldest), term->total) - oldest);
    // Read on from the oldest kept, never from inside a character.
    while (out.lost && at < term->kept.size() && (static_cast<unsigned char>(term->kept[at]) & 0xC0U) == 0x80U) {
        ++at;
    }
    const std::size_t end = whole_end(term->kept, at, std::min(term->kept.size(), at + std::max<std::size_t>(most, 4)));
    out.data    = crucible::detail::scrub_utf8(std::string_view(term->kept).substr(at, end - at));
    out.next    = oldest + end;
    out.running = term->running;
    out.status  = term->status;
    return out;
}

bool write(int id, std::string_view data) {
    const std::shared_ptr<Term> term = find(id);
    if (!term) {
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(term->mutex);
        if (!term->running || term->typed.size() + data.size() > kMostTyped) {
            return false;
        }
        term->typed.append(data);
    }
    term->stir();
    return true;
}

bool resize(int id, int cols, int rows) {
    const std::shared_ptr<Term> term = find(id);
    return term && resize_now(*term, std::clamp(cols, 2, 1000), std::clamp(rows, 1, 500));
}

bool close(int id) {
    std::shared_ptr<Term> term;
    {
        Table&                            all = table();
        const std::lock_guard<std::mutex> lock(all.mutex);
        const auto                        it = all.terms.find(id);
        if (it == all.terms.end()) {
            return false;
        }
        term = it->second;
        all.terms.erase(it);
    }
    finish(term);
    return true;
}

std::vector<Info> list() {
    std::vector<std::shared_ptr<Term>> terms;
    {
        Table&                            all = table();
        const std::lock_guard<std::mutex> lock(all.mutex);
        for (const auto& [id, term] : all.terms) {
            terms.push_back(term);
        }
    }
    std::vector<Info> out;
    for (const std::shared_ptr<Term>& term : terms) {
        const std::lock_guard<std::mutex> lock(term->mutex);
        out.push_back(Info{term->id, term->shell, term->cwd, term->running, term->status});
    }
    return out;
}

void close_all() {
    std::vector<std::shared_ptr<Term>> terms;
    {
        Table&                            all = table();
        const std::lock_guard<std::mutex> lock(all.mutex);
        for (const auto& [id, term] : all.terms) {
            terms.push_back(term);
        }
        all.terms.clear();
    }
    // Asked all at once, so they take one grace between them and not one each.
    for (const std::shared_ptr<Term>& term : terms) {
        term->closing.store(true);
        term->stir();
    }
    for (const std::shared_ptr<Term>& term : terms) {
        finish(term);
    }
}

}  // namespace crucible::tools::terminal
