// SPDX-License-Identifier: MIT
//
// Version control: git, and GitHub through gh.
//
// Both are run as programs the machine already has, the way curl is -- git
// is on every machine that builds software and gh on the ones that talk to
// GitHub from a terminal -- and nothing of either is linked in. Crucible
// starts them in the project root and reads what they print.
//
// Three callers share this. The workshop's GIT and GH verbs hand an expert's
// line to the program as arguments; the build loop commits after each task;
// and the window's Source panel asks for the status, the diff and the log
// and makes commits and pushes of its own. What is shared is how the program
// is found and run, how long it may take, and what a refusal looks like.
//
// What this does not do is confine git. `git -C /elsewhere` and `git clone
// <url> /elsewhere` would act outside the project, so the arguments that
// move git's own idea of where it is are refused; beyond that, git is a
// program that writes where it is told, exactly as RUN is. See the note at
// the top of workshop.hpp.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace crucible::tools::git {

/// What a program said, and whether it worked.
struct Output {
    bool        ok = false;
    int         status = -1;
    std::string text;       ///< stdout and stderr, in the order they came
    std::string error;      ///< why it could not be run at all, when it could not
};

/// Is git here? Is gh?
bool available();
bool gh_available();

/// Run git with `args` in `root`. `timeout_seconds` bounds it; a push over a
/// slow link is allowed its minutes, a status is not.
Output run(const std::filesystem::path& root, const std::vector<std::string>& args,
           int timeout_seconds = 120);

/// Run gh with `args` in `root`. Fails in words when gh is not installed.
Output run_gh(const std::filesystem::path& root, const std::vector<std::string>& args,
              int timeout_seconds = 300);

/// Split a line an expert wrote into arguments the way a shell would, with
/// quotes respected -- so `commit -m "add the parser"` is three arguments
/// and not five -- and without a shell, so nothing in it is interpreted.
std::vector<std::string> split_arguments(std::string_view line);

/// An argument that would move git out of the project, or empty.
std::string escaping_argument(const std::vector<std::string>& args);

/// Whether `root` is inside a git repository.
bool is_repo(const std::filesystem::path& root);

/// Make `root` a repository. Returns a reason it could not, or empty.
std::string init(const std::filesystem::path& root);

/// One changed path, as `git status --porcelain` lists it.
struct Change {
    std::string path;
    std::string status;   ///< "M", "A", "D", "R", "??" -- git's own letters
    bool        staged = false;
};

struct Status {
    bool                repo = false;
    std::string         branch;
    std::string         remote;        ///< the URL of origin, or empty
    int                 ahead  = 0;
    int                 behind = 0;
    std::vector<Change> changes;
    std::string         error;
};

Status status(const std::filesystem::path& root);

/// The diff of the working tree, or of one path, against what is committed.
std::string diff(const std::filesystem::path& root, std::string_view path = {});

struct Commit {
    std::string hash;      ///< short
    std::string subject;
    std::string author;
    std::string when;      ///< relative, as git says it: "2 hours ago"
};

std::vector<Commit> log(const std::filesystem::path& root, int limit = 30);

/// Stage everything and commit it. Returns a one-line account of what was
/// committed ("committed 3f2a1c7 Add the parser  ·  3 files") in `summary`
/// and empty on success, or the reason it could not.
///
/// A repository with no identity set cannot commit, and the first commit of a
/// build on a fresh machine is exactly where that bites: it is committed as
/// "Crucible <crucible@localhost>" then, and the summary says so, rather than
/// failing with git's lecture about who you are.
std::string commit(const std::filesystem::path& root, std::string_view message,
                   std::string& summary);

/// Push the current branch, setting its upstream when it has none. Returns
/// what git said on failure, or empty.
std::string push(const std::filesystem::path& root);

/// Fetch and merge what the remote has. Returns what git said on failure.
std::string pull(const std::filesystem::path& root);

/// Create a GitHub repository for `root` and push to it, through gh.
/// `name` is the repository's, `is_private` whether it is.
std::string publish(const std::filesystem::path& root, std::string_view name, bool is_private,
                    std::string& url);

/// Make a GitHub release `tag` with `notes`, through gh.
std::string release(const std::filesystem::path& root, std::string_view tag, std::string_view notes,
                    std::string& url);

// --- exposed for the tests ---------------------------------------------------

namespace detail {

/// `git status --porcelain=v1 -b` as a Status, without running anything.
Status parse_status(std::string_view porcelain);

/// `git log --format=...` as commits.
std::vector<Commit> parse_log(std::string_view text);

}  // namespace detail

}  // namespace crucible::tools::git
