// SPDX-License-Identifier: MIT
//
// A shell on a terminal of its own, for the person at the keyboard.
//
// The shelf's Terminal pane is a real terminal: the person's own shell,
// started in the project on a pseudo-terminal (posix_openpt on Linux and
// macOS, a pseudo console on Windows), so a prompt, its colors, line
// editing, a pager, an editor and anything else written for a terminal
// behave as they would in one. What the shell prints is kept here, the last
// few megabytes of it, and the page reads it as it comes and draws it
// (ui/term.js). The one-line runner it replaced ran a command, waited for it
// to finish and printed what it said, and nothing that asked a question or
// drew a screen could be run in it.
//
// Not a tool. No expert reaches these: a model runs a command through RUN,
// which is confined to the project, timed and shown in the transcript. A
// terminal is the person's own, as one beside the window would be.
#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::tools::terminal {

/// How to start one.
struct Options {
    /// Where the shell starts.
    std::filesystem::path cwd;

    /// Its size, in characters.
    int cols = 80;
    int rows = 24;

    /// What to run in place of the person's own shell, argv[0] a path or a
    /// name on PATH. Empty for the shell they log in with: $SHELL on Linux
    /// and macOS, PowerShell on Windows.
    std::vector<std::string> command;
};

/// One terminal, as a list of them says it.
struct Info {
    int         id = 0;
    std::string shell;          ///< the program, by name: "bash", "pwsh"
    std::string cwd;            ///< where it started
    bool        running = true;
    int         status  = 0;    ///< how it ended, once it has
};

/// Start a shell. Its id, or 0 with `error` saying why not.
int open(const Options& options, std::string& error);

/// What a terminal has printed.
struct Output {
    /// From where it was asked for, or from the oldest kept when that is
    /// later (`lost`), ending on a whole character. UTF-8.
    std::string data;

    /// Where to read from next: a count of bytes since the terminal started.
    std::uint64_t next = 0;

    /// Some of what was asked for is no longer kept: `data` starts after it.
    bool lost = false;

    bool running = true;
    int  status  = 0;
};

/// What terminal `id` has printed from `since`, at most `most` bytes of it.
/// Nothing when there is no such terminal.
std::optional<Output> read(int id, std::uint64_t since, std::size_t most = std::size_t{256} << 10);

/// Type `data` into it, as the keys pressed send it. False when there is no
/// such terminal or it has ended.
bool write(int id, std::string_view data);

/// It is now `cols` by `rows`: the shell and whatever is drawing in it are
/// told, and draw again at that size.
bool resize(int id, int cols, int rows);

/// End it, and what it started. False when there is no such terminal.
bool close(int id);

/// Every terminal, oldest first: those that have ended too, until closed.
std::vector<Info> list();

/// End every one: for when the window closes, so no shell outlives it.
void close_all();

}  // namespace crucible::tools::terminal
