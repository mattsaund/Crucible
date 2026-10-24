// SPDX-License-Identifier: MIT
//
// Running a child process and reading its output line by line.
//
// The runtime manager compiles a GPU backend, which means driving cmake for
// several minutes while the window stays responsive and the user keeps the option
// of giving up. popen() cannot do that -- it hands back no process id, so
// there is nothing to signal when the user cancels. This is fork/exec with a
// pipe, which can.
#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::util {

/// A running child, with stdout and stderr merged into one stream.
///
/// Merging is deliberate: compilers report progress on one and errors on the
/// other, and a build log that interleaves them in the order they happened is
/// far easier to read than two logs that have to be reconciled by hand.
class Subprocess {
public:
    Subprocess() = default;
    ~Subprocess();
    Subprocess(const Subprocess&)            = delete;
    Subprocess& operator=(const Subprocess&) = delete;
    Subprocess(Subprocess&& other) noexcept;
    Subprocess& operator=(Subprocess&& other) noexcept;

    /// Start `argv` (argv[0] is looked up on PATH). `cwd` may be empty to
    /// inherit the current directory. `extra_env` entries are "NAME=VALUE".
    /// Returns false and fills `error` if the child could not be started.
    bool start(const std::vector<std::string>& argv,
               const std::filesystem::path& cwd,
               const std::vector<std::string>& extra_env,
               std::string& error);

    /// How the child's streams are wired, when the defaults will not do.
    struct Streams {
        /// A pipe to the child's standard input, for write_line. Without it
        /// the child inherits this process's.
        bool input = false;

        /// Where the child's standard error goes, appended. Empty merges it
        /// into the output read_line reads, which is what a build log wants.
        /// A protocol spoken on standard output wants the opposite: a warning
        /// written halfway through a message would corrupt the message.
        std::filesystem::path errors;
    };

    /// start() with the streams wired as asked.
    bool start(const std::vector<std::string>& argv,
               const std::filesystem::path& cwd,
               const std::vector<std::string>& extra_env,
               std::string& error,
               const Streams& streams);

    /// Write `line` and a newline to the child's standard input. Only with
    /// Streams::input. False once the child has gone away -- a broken pipe is
    /// reported here, not raised as a signal that would end this process.
    bool write_line(std::string_view line);

    /// Close the child's standard input, which a child reading it sees as the
    /// end of what it will be told.
    void close_input();

    /// Read one line, without its newline. Returns false at end of output,
    /// and soon after terminate(). Blocks, so call it from the thread that
    /// is allowed to wait.
    bool read_line(std::string& line);

    /// Reap the child and return its exit status. A process killed by a signal
    /// reports 128 + signal, matching what a shell would say. A child asked to
    /// stop -- terminate(), interrupt(), from any thread -- that is still
    /// there three seconds later is killed, with everything it started.
    /// Safe to call more than once; later calls return the same status.
    int wait();

    /// Reap the child if it has ended, without waiting for it: true, with
    /// its status, once it has.
    bool try_wait(int& status);

    /// Ask the child to stop, and stop reading it: read_line ends within a
    /// moment even when something the child started holds the pipe open.
    /// The pipe itself is left to the reader's thread, so this can be called
    /// from another one. Returns immediately; call wait() afterwards.
    void terminate();

    /// Signal the child to stop and touch nothing else: the reader sees the
    /// end of the output the ordinary way, once the child has gone. The
    /// caller keeps this from racing wait(), the one other thing that
    /// touches the process handle.
    void interrupt();

    bool running() const;

private:
    void close_pipe();

#if defined(_WIN32)
    // HANDLEs, held as void* so <windows.h> stays out of a header this widely
    // included -- it defines `min`, `max` and `ERROR` as macros, and every
    // translation unit downstream would pay for that.
    //
    // `job_` is the equivalent of the POSIX process group: a build is
    // cmake -> ninja -> a dozen compilers, and terminating only the first would
    // leave the rest running.
    void* process_ = nullptr;
    void* job_     = nullptr;
    void* read_    = nullptr;
    void* write_   = nullptr;   ///< the child's standard input, with Streams::input
#else
    std::atomic<int> pid_{-1};   ///< read by interrupt() from any thread
    int  fd_     = -1;
    int  in_fd_  = -1;          ///< the child's standard input, with Streams::input
#endif
    int  status_ = -1;
    bool reaped_ = false;
    std::string buffer_;   ///< holds a partial line between read_line() calls
    bool eof_    = false;
    std::atomic<std::int64_t> stop_asked_ms_{0};   ///< when a stop was asked; 0 for never
    std::atomic<bool>         abandoned_{false};   ///< terminate() was called: read no more
};

/// How a command run with run() ended, and what it printed.
struct Ran {
    bool        started   = false;
    bool        timed_out = false;
    bool        stopped   = false;   ///< `stop` said so
    int         status    = -1;
    std::string output;              ///< the start and the end of it, past `keep`
    std::string error;               ///< why it did not start
};

/// Run `argv` to its end, or until `timeout_seconds` have passed, or until
/// `stop` says so: the deadline and the Stop every command an expert runs
/// needs, in one place. Past `keep` bytes, output is cut from the middle --
/// how it started and how it ended are what a log is read for -- so a loop
/// printing forever costs that much memory and no more.
Ran run(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
        const std::vector<std::string>& extra_env, int timeout_seconds,
        const std::function<bool()>& stop = {}, std::size_t keep = std::size_t{4} << 20);

/// Stop every run() going and refuse new ones: for when the window closes,
/// and nothing a command was run for is wanted any more.
void stop_runs();

/// True when `program` can be found on PATH. Used to turn a missing SDK into
/// advice before a ten-minute build discovers the same thing.
bool on_path(const std::string& program);

/// Start `argv` as a program of its own, which goes on when this one exits:
/// what finishes an update once Crucible has closed. Nothing is read from it.
bool start_detached(const std::vector<std::string>& argv, std::string& error);

}  // namespace crucible::util
