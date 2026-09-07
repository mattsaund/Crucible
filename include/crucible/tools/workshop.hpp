// SPDX-License-Identifier: MIT
//
// The workshop: what an expert can do to a project rather than say about it.
//
// Reading a file, writing one, listing a directory, running a command, looking
// something up, and asking the user a question. Together with the cook loop
// these are what make Crucible able to work on a project over an hour instead of
// answering one question about it.
//
// Three things are load-bearing here and none of them is the tool list.
//
// The first is that every path LIST, READ and WRITE are given is resolved
// inside one root and anything that escapes it is refused. An expert that can
// write outside the project is not a coding assistant, it is a remote shell,
// and the difference has to be structural rather than a matter of the model
// behaving.
//
// RUN is the exception, and it is worth being exact about rather than
// comfortable. The command starts with the project root as its working
// directory, and that is the whole of the containment: a shell can `cd`
// anywhere, read anything the user can read, and reach the network. This was
// observed rather than reasoned about -- an expert given a cook wrote
// `RUN: cd /tmp/... && grep -r add src/` and it ran, exactly as a shell should.
//
// That is not a hole to be plugged with a blocklist. `cd`, `..`, `$(...)`, an
// absolute path and `find /` are five ways to do the same thing and there are
// fifty more; refusing a subset while claiming confinement would be worse than
// saying plainly what this is. Real confinement means a sandbox -- bubblewrap,
// Landlock, seatbelt -- which is per-platform and is not here yet. So RUN is a
// second switch, off-able on its own, and the interface says what it does:
// letting a model edit a project you already trusted and letting it run
// commands as you are different decisions, and only one of them is bounded by
// a directory.
//
// The second is that all of this is off until it is switched on, and the folder
// has to have been trusted. `crucible` is meant to be run by cd-ing into a
// project, and the trust gate has always said it would eventually read and
// write files there. This is that.
//
// The third is that the protocol is text. llama.cpp applies a chat template, it
// does not negotiate a tool schema, and Crucible cannot know which model is in
// the seat -- a convention every model can follow beats one that only the
// tool-trained ones can. This is the same reasoning, and the same shape, as the
// `SEARCH:` line in web_search.hpp.
#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/llm/model_host.hpp"
#include "crucible/tools/web_search.hpp"

namespace crucible::tools {

/// What an expert asked to do.
enum class ToolKind {
    None,
    List,    ///< LIST: <dir>
    Read,    ///< READ: <file>
    Write,   ///< WRITE: <file>, followed by a block
    Run,     ///< RUN: <command>, in the project root but not confined to it
    Search,  ///< SEARCH: <query>
    Ask,     ///< ASK: <question>   -- pauses the cook for an answer
    Note,    ///< NOTE: <what it is doing>, journalled and shown, no effect
    Done,    ///< DONE: <summary>   -- this piece of work is finished
    /// HANDOFF: <the next piece of work> -- ends the iteration and sends that
    /// line back through the delegator, which may put a different expert in the
    /// seat. How a programming expert that has finished the code says the next
    /// thing needed is documentation.
    Handoff,
};

std::string_view tool_kind_name(ToolKind kind);

struct ToolCall {
    ToolKind    kind = ToolKind::None;
    /// The path, command, query, question or summary on the verb's line.
    std::string argument;
    /// The body of a WRITE. Empty for every other verb.
    std::string content;
};

/// What happened, in the two registers it has to be reported in: `output` goes
/// back to the model, `summary` goes on screen and into the journal.
struct ToolResult {
    bool        ok = false;
    std::string output;
    std::string summary;

    /// The long form, for a reader rather than for the model: a diff for a
    /// WRITE, the captured output for a RUN. Kept apart from `summary` because
    /// a journal has to stay scannable -- this is what a step expands into,
    /// not what it says at rest.
    std::string detail;

    /// Paths this call changed, relative to the root. The journal's record of
    /// what a cook actually did to the project.
    std::vector<std::string> changed;
};

struct WorkshopSettings {
    /// Off until switched on, like web search and for the same reason.
    bool enabled = false;

    /// Every path is resolved inside this and anything that escapes is refused.
    /// Normally the project directory the user started Crucible in.
    std::filesystem::path root;

    /// Whether RUN is available.
    ///
    /// Reading and writing a project you already trusted is one decision;
    /// executing arbitrary commands as you is another, and some people will
    /// want the first without the second. It is a separate switch because a
    /// command is not bounded by the root the way a path is -- see the note at
    /// the top of this file.
    bool allow_run = true;

    /// How long a single command may take before it is killed. A build is
    /// minutes; a command that has not finished in this long is stuck, and a
    /// cook that waits forever on it has stopped cooking.
    int run_timeout_seconds = 120;

    /// How much of a command's output comes back to the model.
    ///
    /// A test suite can print a megabyte, and feeding that to an expert with an
    /// 8k context does not fail loudly -- it silently pushes the actual task out
    /// of the window. Truncation keeps the head and the tail, because a
    /// compiler puts the first error at the top and the summary at the bottom.
    std::size_t max_output_bytes = 6000;

    /// How much of a file a READ returns, for the same reason.
    std::size_t max_read_bytes = 32000;

    /// How many entries a LIST returns.
    std::size_t max_entries = 200;
};

/// The first tool call in a reply, or nothing when the expert is just talking.
///
/// `reasoning` is consulted only when `answer` has none, which is the same rule
/// web_search follows: a model that has written something for the user is
/// answering, not calling a tool, and a programming expert's reply about a
/// shell command is not a request to run it.
std::optional<ToolCall> parse_tool_call(std::string_view answer, std::string_view reasoning);

/// The verb a reply was reaching for but wrote wrongly, or None.
///
/// A line that opens with WRITE, RUN, ASK or DONE and no colon is a tool call
/// with a typo in it, not prose -- but it cannot be executed, because the
/// argument is not reliably an argument. `WRITE /path "fixed the bug"` means
/// the quoted text as a description, and obeying it would put the description
/// into the file instead of the code.
///
/// So it is detected rather than guessed at, and the caller answers with the
/// syntax. Returns None for a reply that was not trying to call anything.
ToolKind attempted_tool_call(std::string_view answer, std::string_view reasoning);

/// Resolve `relative` inside `root`.
///
/// Returns nothing when the result would be outside it -- an absolute path, a
/// `..` that climbs past the top, or a symlink pointing away. Symlinks are
/// resolved before the check rather than after, because a link is exactly how
/// you would get out of a directory that only compared strings.
///
/// The path does not have to exist: this is also how a WRITE decides where a
/// new file may go.
std::optional<std::filesystem::path> resolve_in_root(const std::filesystem::path& root,
                                                     std::string_view relative);

/// Keep the head and the tail of `text`, with a line in the middle saying what
/// was dropped. Returns `text` unchanged when it already fits.
std::string clamp_output(std::string_view text, std::size_t limit);

/// Do it.
///
/// Blocking -- a RUN can take the whole timeout -- so call it from the engine
/// thread. `cancel` is checked while a command runs, which is what makes
/// Ctrl-C during a cook stop the build rather than wait for it.
///
/// ToolKind::Ask and ToolKind::Done are not executed here: they are answers to
/// the cook loop rather than actions, and it handles them.
ToolResult run_tool(const ToolCall& call, const WorkshopSettings& settings,
                    const SearchSettings& search, const CancelCallback& cancel);

/// Who the instructions are being written for.
///
/// The verbs are the same either way; what differs is how the work ends. A cook
/// runs until it says DONE or HANDOFF, and those lines are answers to the loop
/// rather than actions. A chat turn ends when the expert stops calling tools and
/// writes something for the person who asked -- so telling it about DONE would
/// invite it to end a reply with a status line instead of an answer, and telling
/// it about HANDOFF would offer it a seat change that chat has no way to make.
enum class ToolAudience {
    Cook,  ///< the cook loop: DONE and HANDOFF are how a piece of work ends
    Chat,  ///< one turn: the answer is how it ends
};

/// What to add to an expert's system prompt so it knows the workshop is there.
/// Lists only the verbs the settings and the audience actually allow.
std::string workshop_instructions(const WorkshopSettings& settings,
                                  ToolAudience audience);

}  // namespace crucible::tools
