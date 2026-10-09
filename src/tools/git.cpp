// SPDX-License-Identifier: MIT
//
// Driving git and gh. See git.hpp.
#include "crucible/tools/git.hpp"

#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <chrono>
#include <sstream>
#include <thread>

#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"

namespace crucible::tools::git {
namespace {

/// Run `argv` in `root` and keep what it printed, killing it after `timeout`.
///
/// The same watchdog the workshop's RUN uses, and for the same reason:
/// read_line blocks, which keeps the output in order, so the deadline has to
/// come from outside the read.
Output run_program(const std::filesystem::path& root, const std::vector<std::string>& argv,
                   int timeout_seconds) {
    Output out;
    util::Subprocess child;
    std::string error;
    if (!child.start(argv, root, {}, error)) {
        out.error = "could not run " + argv.front() + ": " + error;
        return out;
    }
    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
        while (!finished.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                child.terminate();
                return;
            }
        }
    });
    std::string line;
    while (child.read_line(line)) {
        out.text += line;
        out.text += '\n';
    }
    out.status = child.wait();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();
    out.text = crucible::detail::console_to_utf8(out.text);
    if (timed_out.load(std::memory_order_relaxed)) {
        out.error = argv.front() + " took longer than " + std::to_string(timeout_seconds)
                  + " seconds and was stopped";
        return out;
    }
    out.ok = out.status == 0;
    return out;
}

std::string trimmed(std::string text) {
    const std::size_t last = text.find_last_not_of(" \t\r\n");
    text.erase(last == std::string::npos ? 0 : last + 1);
    const std::size_t first = text.find_first_not_of(" \t\r\n");
    text.erase(0, first == std::string::npos ? text.size() : first);
    return text;
}

/// What git printed, as the reason something failed: its last lines, which
/// is where it puts the sentence that matters.
std::string said(const Output& out) {
    if (!out.error.empty()) {
        return out.error;
    }
    const std::string text = trimmed(out.text);
    return text.empty() ? "exit status " + std::to_string(out.status) : text;
}

/// Whether the repository at `root` has a name and an email to commit as.
bool has_identity(const std::filesystem::path& root) {
    const Output name  = run(root, {"config", "user.name"}, 20);
    const Output email = run(root, {"config", "user.email"}, 20);
    return name.ok && !trimmed(name.text).empty() && email.ok && !trimmed(email.text).empty();
}

}  // namespace

bool available() {
    return util::on_path("git");
}

bool gh_available() {
    return util::on_path("gh");
}

std::vector<std::string> split_arguments(std::string_view line) {
    std::vector<std::string> out;
    std::string current;
    bool in_word = false;
    char quote = 0;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (quote != 0) {
            if (c == quote) {
                quote = 0;
            } else if (c == '\\' && quote == '"' && i + 1 < line.size()
                       && (line[i + 1] == '"' || line[i + 1] == '\\')) {
                current += line[++i];
            } else {
                current += c;
            }
            continue;
        }
        if (c == '"' || c == '\'') {
            quote   = c;
            in_word = true;
            continue;
        }
        if (c == '\\' && i + 1 < line.size()) {
            current += line[++i];
            in_word = true;
            continue;
        }
        if (c == ' ' || c == '\t') {
            if (in_word) {
                out.push_back(current);
                current.clear();
                in_word = false;
            }
            continue;
        }
        current += c;
        in_word = true;
    }
    if (in_word) {
        out.push_back(current);
    }
    return out;
}

std::string escaping_argument(const std::vector<std::string>& args) {
    for (const std::string& arg : args) {
        if (arg == "-C" || arg.rfind("--git-dir", 0) == 0 || arg.rfind("--work-tree", 0) == 0
            || arg.rfind("-C", 0) == 0) {
            return arg;
        }
    }
    return {};
}

Output run(const std::filesystem::path& root, const std::vector<std::string>& args,
           int timeout_seconds) {
    Output out;
    if (!available()) {
        out.error = "git is not installed on this machine";
        return out;
    }
    if (const std::string bad = escaping_argument(args); !bad.empty()) {
        out.error = "\"" + bad + "\" would take git outside the project, and is not allowed";
        return out;
    }
    std::vector<std::string> argv{"git"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_program(root, argv, timeout_seconds);
}

Output run_gh(const std::filesystem::path& root, const std::vector<std::string>& args,
              int timeout_seconds) {
    Output out;
    if (!gh_available()) {
        out.error = "gh, GitHub's command line, is not installed -- it is what talks to GitHub. "
                    "https://cli.github.com";
        return out;
    }
    std::vector<std::string> argv{"gh"};
    argv.insert(argv.end(), args.begin(), args.end());
    return run_program(root, argv, timeout_seconds);
}

bool is_repo(const std::filesystem::path& root) {
    std::error_code ec;
    if (!std::filesystem::is_directory(root, ec)) {
        return false;
    }
    const Output out = run(root, {"rev-parse", "--is-inside-work-tree"}, 20);
    return out.ok && trimmed(out.text) == "true";
}

std::string init(const std::filesystem::path& root) {
    const Output out = run(root, {"init", "-q"}, 60);
    return out.ok ? std::string() : said(out);
}

namespace detail {

Status parse_status(std::string_view porcelain) {
    Status status;
    status.repo = true;
    std::istringstream in{std::string(porcelain)};
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.rfind("## ", 0) == 0) {
            // "## main...origin/main [ahead 1, behind 2]" or "## No commits yet on main"
            std::string head = line.substr(3);
            if (head.rfind("No commits yet on ", 0) == 0) {
                status.branch = head.substr(18);
                continue;
            }
            const std::size_t dots = head.find("...");
            const std::size_t space = head.find(' ');
            status.branch = head.substr(0, std::min(dots, space));
            if (const std::size_t ahead = head.find("ahead "); ahead != std::string::npos) {
                status.ahead = std::atoi(head.c_str() + ahead + 6);
            }
            if (const std::size_t behind = head.find("behind "); behind != std::string::npos) {
                status.behind = std::atoi(head.c_str() + behind + 7);
            }
            continue;
        }
        if (line.size() < 4) {
            continue;
        }
        Change change;
        const char index    = line[0];
        const char worktree = line[1];
        change.staged = index != ' ' && index != '?';
        change.status = index == '?' ? "??" : std::string(1, index != ' ' ? index : worktree);
        change.path   = line.substr(3);
        // "R  old -> new": the new name is the one that exists.
        if (const std::size_t arrow = change.path.find(" -> "); arrow != std::string::npos) {
            change.path = change.path.substr(arrow + 4);
        }
        status.changes.push_back(std::move(change));
    }
    return status;
}

std::vector<Commit> parse_log(std::string_view text) {
    std::vector<Commit> commits;
    std::istringstream in{std::string(text)};
    std::string line;
    while (std::getline(in, line)) {
        // hash<TAB>subject<TAB>author<TAB>when
        std::vector<std::string> fields;
        std::size_t start = 0;
        for (std::size_t tab = line.find('\t'); ; tab = line.find('\t', start)) {
            fields.push_back(line.substr(start, tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) {
                break;
            }
            start = tab + 1;
        }
        if (fields.size() < 4 || fields[0].empty()) {
            continue;
        }
        commits.push_back({fields[0], fields[1], fields[2], fields[3]});
    }
    return commits;
}

}  // namespace detail

Status status(const std::filesystem::path& root) {
    Status out;
    if (!available()) {
        out.error = "git is not installed";
        return out;
    }
    if (!is_repo(root)) {
        return out;   // repo stays false, which is the answer
    }
    const Output porcelain = run(root, {"status", "--porcelain=v1", "-b"}, 60);
    if (!porcelain.ok) {
        out.error = said(porcelain);
        out.repo  = true;
        return out;
    }
    out = detail::parse_status(porcelain.text);
    const Output remote = run(root, {"remote", "get-url", "origin"}, 20);
    if (remote.ok) {
        out.remote = trimmed(remote.text);
    }
    return out;
}

std::string diff(const std::filesystem::path& root, std::string_view path) {
    std::vector<std::string> args{"--no-pager", "diff", "HEAD", "--"};
    if (!path.empty()) {
        args.emplace_back(path);
    }
    Output out = run(root, args, 60);
    if (!out.ok) {
        // A repository with no commit yet has no HEAD to diff against: the
        // whole tree is new, and the diff against nothing says so.
        out = run(root, {"--no-pager", "diff", "--no-index", "--", "/dev/null",
                         path.empty() ? "." : std::string(path)}, 60);
        if (!out.ok && out.text.empty()) {
            return out.error;
        }
    }
    // Untracked files are not in `git diff`, and a new project is nothing
    // but untracked files. Each is shown as what it would add.
    if (path.empty()) {
        const Output untracked = run(root, {"ls-files", "--others", "--exclude-standard"}, 60);
        if (untracked.ok) {
            std::istringstream in(untracked.text);
            std::string file;
            while (std::getline(in, file)) {
                if (file.empty()) {
                    continue;
                }
                const Output one = run(root, {"--no-pager", "diff", "--no-index", "--", "/dev/null", file}, 60);
                out.text += one.text;
            }
        }
    }
    return out.text;
}

std::vector<Commit> log(const std::filesystem::path& root, int limit) {
    const Output out = run(root, {"--no-pager", "log", "--format=%h%x09%s%x09%an%x09%ar",
                                  "-n", std::to_string(std::max(1, limit))}, 60);
    return out.ok ? detail::parse_log(out.text) : std::vector<Commit>{};
}

std::string commit(const std::filesystem::path& root, std::string_view message, std::string& summary) {
    summary.clear();
    if (trimmed(std::string(message)).empty()) {
        return "a commit needs a message";
    }
    if (!is_repo(root)) {
        return "this folder is not a git repository";
    }
    const Output added = run(root, {"add", "-A"}, 120);
    if (!added.ok) {
        return said(added);
    }
    const Output staged = run(root, {"diff", "--cached", "--quiet"}, 60);
    if (staged.ok) {
        summary = "nothing to commit";
        return {};
    }
    std::vector<std::string> args;
    const bool anonymous = !has_identity(root);
    if (anonymous) {
        args = {"-c", "user.name=Crucible", "-c", "user.email=crucible@localhost"};
    }
    args.insert(args.end(), {"commit", "-q", "-m", std::string(message)});
    const Output made = run(root, args, 120);
    if (!made.ok) {
        return said(made);
    }
    const Output head = run(root, {"--no-pager", "log", "-1", "--format=%h%x09%s", "--shortstat"}, 20);
    std::string hash, subject, stat;
    if (head.ok) {
        std::istringstream in(head.text);
        std::string first;
        std::getline(in, first);
        const std::size_t tab = first.find('\t');
        hash    = first.substr(0, tab);
        subject = tab == std::string::npos ? std::string() : first.substr(tab + 1);
        std::string line;
        while (std::getline(in, line)) {
            if (!trimmed(line).empty()) {
                stat = trimmed(line);
            }
        }
    }
    summary = "committed " + hash + " " + subject;
    if (!stat.empty()) {
        summary += "  ·  " + stat;
    }
    if (anonymous) {
        summary += "  ·  as Crucible <crucible@localhost>, since git has no name for you yet";
    }
    return {};
}

std::string push(const std::filesystem::path& root) {
    const Output branch = run(root, {"rev-parse", "--abbrev-ref", "HEAD"}, 20);
    const std::string name = branch.ok ? trimmed(branch.text) : std::string();
    const Output upstream = run(root, {"rev-parse", "--abbrev-ref", "--symbolic-full-name", "@{u}"}, 20);
    std::vector<std::string> args{"push", "-q"};
    if (!upstream.ok) {
        args.insert(args.end(), {"-u", "origin", name.empty() ? "HEAD" : name});
    }
    const Output out = run(root, args, 600);
    return out.ok ? std::string() : said(out);
}

std::string pull(const std::filesystem::path& root) {
    const Output out = run(root, {"pull", "-q", "--ff-only"}, 600);
    return out.ok ? std::string() : said(out);
}

std::string publish(const std::filesystem::path& root, std::string_view name, bool is_private,
                    std::string& url) {
    url.clear();
    if (!is_repo(root)) {
        if (const std::string error = init(root); !error.empty()) {
            return error;
        }
    }
    const Output out = run_gh(root, {"repo", "create", std::string(name), is_private ? "--private" : "--public",
                                     "--source", ".", "--push", "--remote", "origin"}, 600);
    if (!out.ok) {
        return said(out);
    }
    // gh prints the repository's address; the last line that looks like one.
    std::istringstream in(out.text);
    std::string line;
    while (std::getline(in, line)) {
        if (trimmed(line).rfind("https://", 0) == 0) {
            url = trimmed(line);
        }
    }
    return {};
}

std::string release(const std::filesystem::path& root, std::string_view tag, std::string_view notes,
                    std::string& url) {
    url.clear();
    if (trimmed(std::string(tag)).empty()) {
        return "a release needs a tag, like v1.0.0";
    }
    const Output out = run_gh(root, {"release", "create", std::string(tag), "--title", std::string(tag),
                                     "--notes", std::string(notes)}, 600);
    if (!out.ok) {
        return said(out);
    }
    url = trimmed(out.text);
    return {};
}

}  // namespace crucible::tools::git
