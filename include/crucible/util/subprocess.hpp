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

#include <filesystem>
#include <memory>
#include <string>
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

    /// Read one line, without its newline. Returns false at end of output.
    /// Blocks, so call it from the thread that is allowed to wait.
    bool read_line(std::string& line);

    /// Reap the child and return its exit status. A process killed by a signal
    /// reports 128 + signal, matching what a shell would say.
    /// Safe to call more than once; later calls return the same status.
    int wait();

    /// Ask the child to stop: SIGTERM, then SIGKILL if it is still there.
    /// Returns immediately; call wait() afterwards to reap it.
    void terminate();

    /// Signal the child to stop and touch nothing else.
    ///
    /// terminate() also closes the pipe, which is what unblocks a reader --
    /// and is a second thread changing a descriptor the first is in the middle
    /// of a read on. This only sends the signal: the child exits, its end of
    /// the pipe closes, and the reader sees the end of the output the ordinary
    /// way. The caller still has to keep this from racing wait(), which is the
    /// one other thing that touches the process handle.
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
#else
    int  pid_    = -1;
    int  fd_     = -1;
#endif
    int  status_ = -1;
    bool reaped_ = false;
    std::string buffer_;   ///< holds a partial line between read_line() calls
    bool eof_    = false;
};

/// True when `program` can be found on PATH. Used to turn a missing SDK into
/// advice before a ten-minute build discovers the same thing.
bool on_path(const std::string& program);

}  // namespace crucible::util
