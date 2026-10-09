// SPDX-License-Identifier: MIT
//
// What a cook did, while it is doing it and afterwards.
//
// A cook runs for an hour and takes a hundred small actions. Two different
// questions get asked about that, and they want different things:
//
//   "what is it doing right now" -- answered live, in the output window, one
//   line per action as it happens.
//
//   "what did it actually change, and how long did it take" -- asked days
//   later, about a cook that has finished, and answered from disk.
//
// One record serves both. The journal is appended to as the cook runs and is
// the same structure that is written out at the end, so the history is not a
// summary of what happened but the thing itself.
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

#include "crucible/routing/expert.hpp"
#include "crucible/tools/attachments.hpp"

namespace crucible {

/// "30m" -> 1800, "2h" -> 7200, "45s" -> 45, "30" -> 1800.
///
/// A bare number is minutes, because that is the unit people say cooks in --
/// "give it twenty" means twenty minutes, and nobody sets one for twenty
/// seconds. Returns nothing when the text is not a duration at all, which is
/// how `/cook fix the tests` is told apart from `/cook 30m fix the tests`.
std::optional<int> parse_duration_seconds(std::string_view text);

/// "1h 20m", "45m", "30s" -- a duration as it is shown on screen.
std::string format_duration(std::chrono::seconds seconds);

/// Where a cook is in its life.
enum class CookState {
    Idle,      ///< nothing is cooking
    Working,   ///< an expert is taking actions towards the goal
    Asking,    ///< stopped on a question, waiting for the user
    Finishing, ///< the budget ran out or the user stopped it; tidying up
    Done,      ///< finished on its own terms
    Stopped,   ///< the user ended it
    Failed,
};

std::string_view cook_state_name(CookState state);
CookState        cook_state_from_name(std::string_view name);

/// One action, as it happened.
struct CookStep {
    int         iteration = 0;
    ExpertId    expert;
    /// The tool verb, lower case ("write", "run", "note"), or "think" for a
    /// round the expert spent reasoning without calling anything.
    std::string kind;
    /// One line, as it appears on screen and in the history.
    std::string summary;

    /// What the step expands into: a diff for a write, the captured output for
    /// a run. Empty for the rest. Kept apart from `summary` so a journal of a
    /// hundred steps stays scannable and still holds what actually happened.
    std::string detail;

    bool        ok = true;
    long        ms = 0;
    /// Paths this step changed, relative to the project root.
    std::vector<std::string> changed;

    /// Which task of a build this step belongs to, by index, or -1 for a
    /// step of the build itself -- the plan, the check, the review -- and for
    /// every step of a cook, which has no tasks.
    int         task = -1;

    /// Where a picture this step produced was written -- a screenshot, a
    /// picture it looked at -- so the window can show it. The journal keeps
    /// the path and not the bytes: it is saved after every step and sent to
    /// the window many times a second, and a screenshot is most of a megabyte.
    std::string picture;
};

/// One piece of a build's plan, and what became of it.
///
/// A build is a directive cut into tasks by an architect, each given to the
/// expert that fits it. The window draws these as the agents: who has the
/// work, what each has done, which are still waiting.
struct CookTask {
    int         index = 0;
    std::string title;
    std::string detail;
    std::string needs;              ///< the expertise, in the architect's words
    std::vector<std::string> files; ///< what the plan said it would touch
    std::vector<int> after;         ///< tasks that must finish first

    ExpertId    expert;             ///< who had it; empty until it started
    /// waiting | working | done | incomplete | failed | skipped | stopped
    std::string state = "waiting";
    std::string outcome;            ///< what the agent said it made, or why it did not
    std::int64_t started_unix = 0;
    std::int64_t ended_unix   = 0;
};

/// What the architect said about the whole: the summary, and the commands
/// that run, check and package it.
struct CookPlan {
    std::string summary;
    std::string run;
    std::string check;
    std::string ship;

    bool empty() const { return summary.empty() && run.empty() && check.empty() && ship.empty(); }
};

/// A whole cook, running or finished.
struct Cook {
    /// "20260904-142530", also the file name. Sortable, and the same shape the
    /// session store uses.
    std::string id;
    std::string goal;
    CookState   state = CookState::Idle;

    /// "cook" or "build". A build is a cook with a plan: the same loop of
    /// actions and the same journal, with the plan and its tasks on top.
    std::string kind = "cook";
    CookPlan    plan;
    std::vector<CookTask> tasks;

    bool is_build() const { return kind == "build"; }

    /// What was attached to the goal. Read once when the cook starts and put
    /// in front of every round, so it is not trimmed away like the rest.
    std::vector<attach::Tile> attachments;

    /// Seconds the user asked for, or 0 for "until I stop it".
    int budget_seconds = 0;

    std::int64_t started_unix = 0;
    std::int64_t ended_unix   = 0;

    int iterations = 0;
    std::vector<CookStep> steps;

    /// What it says it achieved, written in the finishing pass.
    std::string outcome;

    /// The question it is waiting on, when the state is Asking.
    std::string question;

    /// Every distinct path any step changed, in the order first touched.
    /// The answer to "what did this cook actually do to my project".
    std::vector<std::string> files_touched() const;

    /// Every expert that held the seat, in the order they took it.
    ///
    /// A cook is not one expert: a HANDOFF sends the next piece of work back
    /// through the delegator, so a long one may pass from a programming expert
    /// to a writing one and back. This is who worked on it.
    std::vector<ExpertId> experts_used() const;

    /// How long it ran. Uses the wall clock while it is still running.
    std::chrono::seconds duration() const;

    /// "3 files, 24 steps over 41 minutes" -- the one-line history entry.
    std::string headline() const;
};

/// Enough about a stored cook to choose one from a list.
struct CookSummary {
    std::string  id;
    std::string  goal;
    std::string  kind = "cook";
    CookState    state = CookState::Done;
    int          tasks = 0;   ///< a build's, so a list can say "4 tasks" beside it
    std::int64_t started_unix = 0;
    int          iterations = 0;
    int          steps = 0;
    int          files = 0;
    std::chrono::seconds duration{0};
    std::filesystem::path file;

    /// "2 hours ago", "yesterday", "12 Aug" -- the same phrasing sessions use.
    std::string when() const;
};

/// Reading and writing one project's cooks.
///
/// Beside the sessions and keyed the same way, because a cook is about the
/// directory it ran in exactly as a conversation is. Saving is whole-file and
/// happens as the cook runs, not only at the end: a cook that is killed after
/// fifty minutes should still be able to tell you what it changed.
/// A cook as JSON, and back: the shape a journal file is written in, and the
/// shape the orchestrator publishes a cook in as it runs. One function each
/// way, so the two cannot drift apart. A field that is missing reads as its
/// default; `fallback_id` stands in for a missing id.
nlohmann::json cook_to_json(const Cook& cook);
Cook           cook_from_json(const nlohmann::json& doc, const std::string& fallback_id = {});

class CookLog {
public:
    /// `project_dir` is the per-project history folder -- `Project::dir`, the
    /// same one the sessions live in.
    ///
    /// A path rather than a Project, so this header can be included by the UI
    /// snapshot without dragging the session store in behind it. The snapshot
    /// carries a live Cook, the store includes the snapshot, and taking the
    /// whole Project here would close that loop.
    explicit CookLog(std::filesystem::path project_dir);

    /// Where cooks are kept: the project folder's `cooks` subdirectory.
    std::filesystem::path dir() const;

    /// Write `cook`, creating or replacing its file. Cheap enough to call after
    /// every step; a long cook's record is tens of kilobytes.
    bool save(const Cook& cook, std::string& error) const;

    /// This project's cooks, newest first.
    std::vector<CookSummary> list(std::size_t limit = 50) const;

    /// Load one back in full.
    std::optional<Cook> load(const std::string& id) const;

    bool remove(const std::string& id, std::string& error) const;

    /// A fresh id from the current time, in the same format as the sessions'.
    static std::string new_id();

private:
    std::filesystem::path file_for(const std::string& id) const;

    std::filesystem::path project_dir_;
};

}  // namespace crucible
