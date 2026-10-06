// SPDX-License-Identifier: MIT
//
// Remembering conversations, per project.
//
// Crucible is started inside a directory and is about that directory, so history
// belongs to it rather than to the machine: `/resume` in ~/code/foo must offer
// the conversations about foo and nothing else. Each project gets a folder
// keyed by its absolute path, holding one JSON file per session.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "crucible/engine/state.hpp"
#include "crucible/session/usage.hpp"

namespace crucible {

/// The directory Crucible was started in, and where its history is kept.
struct Project {
    std::filesystem::path root;  ///< the working directory itself
    std::string           name;  ///< its last path component, for display
    std::filesystem::path dir;   ///< projects_dir()/<slug>-<hash>

    /// The project containing `cwd`, which is simply `cwd` -- Crucible does not
    /// walk up looking for a repository root, because the directory you start
    /// in is the one you meant.
    static Project current();

    /// The project at `root`. The desktop app opens one this way; the terminal
    /// program is always `current()`, because you cd to it.
    static Project at(const std::filesystem::path& root);
};

/// Projects Crucible has been opened in, newest first.
///
/// For the desktop app, which has no `cd` to be told where it is and so has to
/// offer a list. Kept beside the per-project histories rather than in the config
/// file: it is a record of what happened, not a setting, and a config someone
/// hand-edits should not be full of paths they visited once.
///
/// Paths that no longer exist are dropped on read. A recent-projects list whose
/// entries open onto nothing is worse than a short one.
std::vector<Project> recent_projects(std::size_t limit = 12);

/// Put `root` at the top of that list -- unless it cannot be a project. See
/// is_project_place.
void remember_project(const std::filesystem::path& root);

/// Take `root` off that list. The folder and its history are not touched,
/// and opening it again puts it back.
void forget_project(const std::filesystem::path& root);

/// Whether `root` is somewhere a person keeps work, rather than a place the
/// system or Crucible itself keeps things.
///
/// Not a drive's root or the home folder itself, not the system's own folders
/// (Windows and Program Files; /System, /usr and the like), and not where
/// Crucible is installed or keeps its data. Older versions opened whatever
/// folder they were started in -- System32 from the Windows Start menu, the
/// install folder from a shortcut -- and remembered it as a project; those are
/// never listed, and never remembered again.
bool is_project_place(const std::filesystem::path& root);

/// Whether `root` is a chat's scratch folder -- inside
/// paths::scratchpad_dir() -- rather than a project somebody opened.
bool is_scratch(const std::filesystem::path& root);

/// Enough about a stored session to choose one from a list.
struct SessionSummary {
    std::string id;          ///< "20260830-142530", also the file name
    std::string started_at;  ///< "2026-08-30 14:25"
    std::string title;       ///< the first prompt, trimmed to one line
    std::string name;        ///< what it is about, in a few words, once named
    std::filesystem::path project;   ///< the folder it was had in
    int         turns = 0;
    TokenUsage  usage;
    std::filesystem::path file;

    /// "2 hours ago", "yesterday", "12 Aug"
    std::string when() const;
};

/// The conversations had lately, across every project Crucible has kept a
/// history for -- scratch folders included -- newest first. Only the newest
/// few files of each project are read, so this stays cheap as history grows.
/// One whose folder is gone is left out: it could not be opened again.
std::vector<SessionSummary> recent_chats(std::size_t limit);

/// Reading and writing one project's sessions.
///
/// Saving is whole-file and synchronous. Sessions are small -- a long one is a
/// few hundred kilobytes -- and doing it after each completed turn means a
/// crash costs at most the turn in flight.
class SessionStore {
public:
    explicit SessionStore(Project project);

    const Project& project() const { return project_; }

    /// Start recording a new session. Nothing is written until the first turn
    /// completes, so merely opening Crucible does not litter the history.
    void begin_new_session();

    /// Adopt an existing session id, so further turns append to it. Used by
    /// `/resume` -- resuming continues a conversation rather than forking it.
    void adopt(std::string id);

    const std::string& session_id() const { return session_id_; }

    /// What the conversation being recorded is about, in a few words --
    /// "Math homework" -- written with every save from now on. Empty until
    /// it has been named; see Engine::name_session.
    void set_name(std::string name) { name_ = std::move(name); }
    const std::string& name() const { return name_; }

    /// Name a stored conversation, in its file. For one that is not the one
    /// being recorded: a name that arrives after the conversation was put
    /// away.
    bool rename(const std::string& id, const std::string& name, std::string& error) const;

    /// Write the current session. Turns still streaming are skipped: a
    /// half-finished reply is not something to resume into.
    bool save(const std::vector<Turn>& turns, const TokenUsage& usage, std::string& error);

    /// Sessions for this project, newest first.
    std::vector<SessionSummary> list(std::size_t limit = 50) const;

    /// Load one session's turns back. `usage` receives its token totals.
    bool load(const std::string& id, std::vector<Turn>& turns, TokenUsage& usage,
              std::string& error) const;

    /// Delete one stored session.
    bool remove(const std::string& id, std::string& error) const;

    /// Every token this project has ever spent, summed across its sessions.
    /// Read from a running total rather than by opening each file, so it stays
    /// cheap as history grows.
    TokenUsage project_usage() const;

private:
    std::filesystem::path session_file(const std::string& id) const;
    std::filesystem::path totals_file() const;

    /// Fold a finished session's numbers into the project running total.
    void update_project_total(const TokenUsage& usage) const;

    Project     project_;
    std::string session_id_;
    std::string name_;
    /// What was already counted for this session, so re-saving it does not
    /// add the same tokens to the project total twice.
    TokenUsage  counted_;
};

}  // namespace crucible
