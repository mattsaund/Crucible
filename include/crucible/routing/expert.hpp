// SPDX-License-Identifier: MIT
// Who Crucible delegates to.
//
// The roster used to be an enum of nine subjects fixed at compile time. It is
// now a list the user owns: `/newexpert` adds a seat, `/ejectexpert` removes
// one, and everything the delegator sees -- its labels, its system prompt, its
// worked examples -- is generated from whatever is currently in the list. The
// nine that ship are still there, but as defaults rather than as the shape of
// the program.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace crucible {

/// A stable key for one expert: lower case, hyphenated, derived from the name
/// when the expert is created and never changed afterwards.
///
/// This is what the config file, the stored session history and `/thermo how
/// hot ...` are all written in terms of, so it has to survive a rename of the
/// display name and must not be an index into anything -- adding an expert
/// renumbers a list, and a session recorded last week would then claim it was
/// answered by whoever moved into that slot.
using ExpertId = std::string;

/// One expert on the list.
struct Expert {
    ExpertId    id;
    std::string name;    ///< human-facing ("Mathematics", "Rust Async")
    std::string tag;     ///< <= 4 chars, upper case, unique. The chip in the expert panel.

    /// What this expert is trained in, in the user's own words. Fed to the
    /// delegator as the description of the seat, and shown in settings.
    ///
    /// This is the single field that decides whether routing works. A vague
    /// blurb gives the delegator nothing to separate this seat from its
    /// neighbors, which is why `/newexpert` asks for it in a box of its own
    /// rather than inferring it from the name.
    std::string blurb;

    /// Questions this expert should obviously take, as worked examples for the
    /// delegator.
    ///
    /// Two of them, and the same number for every expert so none is favored by
    /// having more. Measured on the 54-prompt benchmark with LFM2.5-1.2B: one
    /// example each scores 89% and never once reaches the ninth seat; two
    /// scores 96% and reaches every seat.
    ///
    /// For a user-made expert these are written by the delegator itself at
    /// creation time -- see `example_request_prompt`. A description is what a
    /// person can be asked for; two well-chosen example questions is not.
    std::vector<std::string> examples;

    /// Whole words that mark this expert's territory, for the model-free
    /// router. Derived from the name and the blurb for a user-made expert.
    std::vector<std::string> keywords;
};

/// The live roster, in the order the seats are drawn.
///
/// Every artifact the delegator consumes is generated from this list rather
/// than stored beside it, so adding a seat cannot leave the system prompt
/// describing eight experts while the label set offers nine.
class Roster {
public:
    /// A roster with nothing on it.
    ///
    /// What every fresh install starts as, and what a config with an explicit
    /// empty expert list loads as. Crucible ships no experts: the seats are the
    /// user's from the first one, so there is no built-in set to restore and no
    /// "is this mine or theirs" state to carry around.
    static Roster bare();

    std::size_t size() const { return experts_.size(); }
    bool        empty() const { return experts_.empty(); }
    const std::vector<Expert>& experts() const { return experts_; }
    const Expert& at(std::size_t index) const;

    /// Look up by id, by tag, or by display name. Case-insensitive, and the
    /// reason `/math`, `/MATH` and `/mathematics` all reach the same seat.
    std::optional<std::size_t> find(std::string_view key) const;

    /// Add a seat at the end.
    ///
    /// Fills in the id, tag and keywords if they are blank, and refuses a name
    /// that collides with a seat already present. Returns false and sets
    /// `error` to something a user can act on.
    bool add(Expert expert, std::string& error);

    /// Remove by id, tag or name. An empty roster is allowed: someone who
    /// ejected every expert meant to, and the UI says so rather than quietly
    /// putting one back.
    bool remove(std::string_view key, std::string& error);

    /// Replace one seat's mutable fields, matched by id. Used by the settings
    /// screen and by the delegator when it writes examples for a new expert.
    bool update(const ExpertId& id, const Expert& replacement);

    // --- everything the delegator sees, generated from the list above ------

    /// The labels the delegator chooses between, in roster order.
    std::vector<std::string> router_labels() const;

    /// The system prompt handed to the delegator, listing every seat and its
    /// remit.
    std::string router_system_prompt() const;

    /// Worked examples as (question, label) pairs.
    ///
    /// Sent as real user/assistant turns rather than pasted into the system
    /// prompt. On a 1.2B model that difference took routing accuracy from 42%
    /// to 74%: a small instruct model follows a demonstrated dialogue far more
    /// reliably than a block of Q/A text.
    std::vector<std::pair<std::string, std::string>> router_examples() const;

private:
    std::vector<Expert> experts_;
};

/// The display name for an id, or the id itself when no seat carries it.
///
/// Every status line, notice and transcript header goes through this. A message
/// naming "rust-async" is worse than one naming "Rust Async" and far better
/// than one naming nothing, which is what a seat ejected mid-turn would
/// otherwise produce.
std::string expert_label(const Roster& roster, const ExpertId& id);

/// Slugify a display name into an id: lower case, runs of anything that is not
/// a letter or digit collapsed to a single hyphen, trimmed.
///
/// Returns an empty string if nothing survives, which is how a name of only
/// punctuation is rejected rather than silently becoming "".
std::string make_expert_id(std::string_view name);

/// A tag of at most four upper-case characters, unique against `taken`.
///
/// Initials for a multi-word name ("Rust Async" -> "RA"), otherwise the first
/// four letters. Collisions are broken by replacing the last character with a
/// digit, because two seats sharing a chip is a bug you only notice once the
/// wrong one lights up.
std::string make_expert_tag(std::string_view name, const std::vector<std::string>& taken);

/// Content words from a name and a description, for the model-free router.
///
/// Stop words and anything shorter than four characters are dropped: the
/// keyword router matches whole words and scores by count, so a list containing
/// "the" scores every prompt ever written.
std::vector<std::string> derive_keywords(std::string_view name, std::string_view blurb);

/// The prompt that asks a loaded model to write worked examples for a new
/// expert, one question per line.
///
/// Kept here beside everything else the roster generates so the wording lives
/// with the format that parses it.
std::string example_request_prompt(std::string_view name, std::string_view blurb);

/// Pull example questions out of whatever the model replied with.
///
/// Tolerant on purpose: models number their lists, bullet them, quote them, and
/// wrap them in a sentence of preamble. Anything that survives stripping that
/// and still ends up looking like a question is kept, and at most `wanted` are
/// returned.
std::vector<std::string> parse_examples(std::string_view reply, std::size_t wanted = 2);

}  // namespace crucible
