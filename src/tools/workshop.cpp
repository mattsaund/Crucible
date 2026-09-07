// SPDX-License-Identifier: MIT
//
// Parsing the tool protocol, and carrying out what it asks for.
//
// workshop.hpp says what the tools are and why they are bounded the way they
// are; this is how a line of model output becomes an action. Three stages:
// verb_of finds the verb a line opens with, parse_tool_call turns the line and
// any fenced body into a ToolCall, and run_tool dispatches to the do_* that
// carries it out.
//
// The parsing is more defensive than it looks, and each guard is here because
// a cook was lost to what it prevents.
//
//   - A verb only counts at the start of a line. An expert explaining that you
//     should "RUN: make test" is writing prose, and running it is not what it
//     asked for.
//   - The colon is optional for LIST and READ and required for the rest, since
//     misreading a read costs a turn and misreading a write costs a file. See
//     colon_optional.
//   - A WRITE target that looks like a sentence is refused outright, because a
//     model that means its text as a description will otherwise get a file
//     named after it. See plausible_path.
//   - attempted_tool_call spots the near-misses -- the right verb in the wrong
//     shape -- so the cook loop can answer with the syntax instead of treating
//     a malformed call as an answer.
//
// The containment itself is resolve_in_root, and its order is the point:
// weakly_canonical runs first, so a symlink is followed before anything is
// compared, and the comparison is component by component rather than on the
// strings. Both matter, and neither is obvious from the outside.
#include "crucible/tools/workshop.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#include "crucible/util/diff.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::tools {
namespace {

struct Verb {
    std::string_view word;
    ToolKind         kind;
};

// Upper case, followed by a colon. Upper case because it is what a model
// reliably reproduces from an instruction and what almost never occurs by
// accident in prose, and a colon because it makes the argument obvious.
constexpr std::array<Verb, 9> kVerbs{{
    {"LIST",   ToolKind::List},
    {"READ",   ToolKind::Read},
    {"WRITE",  ToolKind::Write},
    {"RUN",    ToolKind::Run},
    {"SEARCH", ToolKind::Search},
    {"ASK",    ToolKind::Ask},
    {"NOTE",   ToolKind::Note},
    {"DONE",   ToolKind::Done},
    {"HANDOFF", ToolKind::Handoff},
}};

std::string trim(std::string_view text) {
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.front())) != 0)) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (std::isspace(static_cast<unsigned char>(text.back())) != 0)) {
        text.remove_suffix(1);
    }
    return std::string(text);
}

/// Strip the punctuation a model wraps a path in.
///
/// Measured, not guessed: a 1.2B expert asked to list a directory wrote
/// ``LIST: `calc.py` `` forty times in a row and was told forty times that
/// "`calc.py`" was not a directory. Models quote paths because prose quotes
/// paths, and refusing the quoted form buys nothing -- a file called
/// "`calc.py`" with the backticks in its name does not exist.
std::string unquote(std::string_view text) {
    std::string out = trim(text);
    while (out.size() >= 2) {
        const char front = out.front();
        const char back  = out.back();
        const bool paired = (front == '`'  && back == '`')
                         || (front == '"'  && back == '"')
                         || (front == '\'' && back == '\'');
        if (!paired) {
            break;
        }
        out = trim(std::string_view(out).substr(1, out.size() - 2));
    }
    return out;
}

/// Whether a colon may be left off this verb.
///
/// Models drop it. Measured: an expert given a cook wrote `WRITE /path "text"`
/// and `READ /path` for a solid minute, and every one was read as prose because
/// of one character.
///
/// Accepting the colon-less form is only safe where being wrong is free, so it
/// is allowed for the two read-only verbs whose argument is a single path, and
/// refused for everything else. `RUN the tests first` is a sentence, and
/// executing it because it opens with a verb would be much worse than not
/// running it. `WRITE /path "some text"` is worse still: the model that writes
/// that means the text as a description, and obeying it would put the
/// description into the file in place of the code.
///
/// The cook loop notices the colon-less form of the strict verbs and replies
/// with the syntax rather than guessing -- see engine_cook.cpp.
bool colon_optional(ToolKind kind) {
    return kind == ToolKind::List || kind == ToolKind::Read;
}

/// The verb a line opens with, or None.
///
/// The line must *begin* with it, ignoring indentation. A sentence that happens
/// to contain "RUN:" halfway through is prose, and treating it as a call is how
/// an expert explaining a command ends up executing it.
ToolKind verb_of(std::string_view line, std::string& argument) {
    const std::string trimmed = trim(line);
    for (const Verb& verb : kVerbs) {
        if (trimmed.size() <= verb.word.size() ||
            trimmed.compare(0, verb.word.size(), verb.word) != 0) {
            continue;
        }
        const char after = trimmed[verb.word.size()];
        const bool separated = after == ':'
                            || (colon_optional(verb.kind) && (after == ' ' || after == '\t'));
        if (!separated) {
            continue;
        }
        argument = trim(std::string_view(trimmed).substr(verb.word.size() + 1));
        // Everything but RUN names a thing rather than a command line, and a
        // quoted name is the same name. RUN keeps its argument verbatim:
        // quoting is meaningful to a shell.
        if (verb.kind != ToolKind::Run) {
            argument = unquote(argument);
        }
        return verb.kind;
    }
    return ToolKind::None;
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string              line;
    std::istringstream       stream{std::string(text)};
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        lines.push_back(std::move(line));
    }
    return lines;
}

bool is_fence(const std::string& line) {
    const std::string trimmed = trim(line);
    return trimmed.rfind("```", 0) == 0 || trimmed.rfind("~~~", 0) == 0;
}

/// Collect a WRITE body, starting at `index` (the line after the verb).
///
/// Three shapes are accepted, because models produce all three whatever they
/// are told: a ``` fence, an explicit `<<<`/`>>>` pair, and -- when neither is
/// there -- everything up to the next verb or the end. The fence is first
/// because it is what a model reaches for by reflex.
std::string collect_body(const std::vector<std::string>& lines, std::size_t& index) {
    while (index < lines.size() && trim(lines[index]).empty()) {
        ++index;
    }
    if (index >= lines.size()) {
        return {};
    }

    std::string body;
    const auto  append = [&body](const std::string& line) {
        if (!body.empty()) {
            body += '\n';
        }
        body += line;
    };

    if (is_fence(lines[index])) {
        ++index;  // the opening fence, and whatever language tag it carried
        while (index < lines.size() && !is_fence(lines[index])) {
            append(lines[index]);
            ++index;
        }
        if (index < lines.size()) {
            ++index;  // the closing fence
        }
        return body;
    }

    if (trim(lines[index]) == "<<<") {
        ++index;
        while (index < lines.size() && trim(lines[index]) != ">>>") {
            append(lines[index]);
            ++index;
        }
        if (index < lines.size()) {
            ++index;
        }
        return body;
    }

    std::string ignored;
    while (index < lines.size() && verb_of(lines[index], ignored) == ToolKind::None) {
        append(lines[index]);
        ++index;
    }
    return body;
}

/// Resolve a path argument, forgiving the prose a model appends to it.
///
/// Measured: a small expert wrote `LIST: /home/me/proj  What is the structure
/// of this directory?` and got told the whole string was outside the project.
/// The path was right; the question after it was not part of it.
///
/// So the whole argument is tried first -- a path really can contain spaces --
/// and only if that names nothing is the first word tried instead. Being lenient
/// second rather than first is what keeps "my notes/to do.md" working.
std::optional<std::filesystem::path> resolve_argument(const std::filesystem::path& root,
                                                      const std::string& argument,
                                                      std::string& used) {
    used = argument;
    if (const std::optional<std::filesystem::path> whole = resolve_in_root(root, argument)) {
        std::error_code ec;
        if (std::filesystem::exists(*whole, ec)) {
            return whole;
        }
    }
    const std::size_t space = argument.find_first_of(" \t");
    if (space == std::string::npos) {
        return resolve_in_root(root, argument);  // nothing to trim; report as-is
    }
    const std::string first = argument.substr(0, space);
    if (const std::optional<std::filesystem::path> head = resolve_in_root(root, first)) {
        std::error_code ec;
        if (std::filesystem::exists(*head, ec)) {
            used = first;
            return head;
        }
    }
    return resolve_in_root(root, argument);
}

/// Whether `target` is plausibly the name of a file rather than a sentence.
///
/// This exists because of what actually happens. Given a cook, an expert wrote
/// `WRITE: Run python src/calc.py`, `WRITE: The fix is complete and verified.`
/// and `WRITE: src/calc.py:2: return a + b` -- and each one created a file with
/// that name. Refusing junk is not tidiness: a project that comes back from a
/// cook with a file called "The fix is complete and verified." in it is worse
/// than one where the write was refused and the model was told why.
///
/// Whitespace is the strongest signal and the rule is therefore blunt: a path
/// with a space in it is refused. Real ones exist, and the cost of refusing one
/// is a clear message the model can act on; the cost of accepting a sentence is
/// a file named after it.
bool plausible_path(std::string_view target) {
    if (target.empty() || target.size() > 200) {
        return false;
    }
    // A colon is how "src/calc.py:2: return a + b" gets in, and it has no place
    // in a relative path inside a project.
    constexpr std::string_view kRefused = " \t\n\r\"\'`&|;<>*?$()[]{}:";
    return target.find_first_of(kRefused) == std::string_view::npos;
}

/// `file` as it should be recorded: relative to the root, always.
///
/// An expert names the same file two ways over a long cook -- "src/calc.py"
/// once and the absolute path the next time -- and both are accepted, because
/// both are inside the root. Recording what it typed made the journal count one
/// file as two, so "3 files changed" was a number about the model's phrasing
/// rather than about the project.
std::string relative_to_root(const std::filesystem::path& root,
                             const std::filesystem::path& file) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return file.string();
    }
    const std::filesystem::path relative = std::filesystem::relative(file, base, ec);
    return ec || relative.empty() ? file.string() : relative.generic_string();
}

ToolResult failure(std::string message) {
    ToolResult result;
    result.ok      = false;
    result.summary = message;
    result.output  = "ERROR: " + std::move(message);
    return result;
}

/// LIST
ToolResult do_list(const ToolCall& call, const WorkshopSettings& settings) {
    std::string target = call.argument.empty() ? "." : call.argument;
    const std::optional<std::filesystem::path> dir =
        resolve_argument(settings.root, target, target);
    if (!dir) {
        return failure(target + " is outside the project");
    }

    std::error_code ec;
    if (!std::filesystem::is_directory(*dir, ec)) {
        // Naming the way out. An expert that lists a file wanted to look at it,
        // and telling it only what it did wrong leaves it guessing -- which is
        // how a weak model ends up making the same call twenty times.
        return failure(std::filesystem::exists(*dir, ec)
                           ? target + " is a file, not a directory -- use READ: " + target
                           : target + " does not exist");
    }

    std::vector<std::string> entries;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(*dir, ec)) {
        const std::string name = entry.path().filename().string();
        // A project's .git is thousands of files an expert has no business
        // reading, and listing it is the fastest way to fill a context window
        // with nothing.
        if (name == ".git" || name == "node_modules" || name == "__pycache__") {
            continue;
        }
        entries.push_back(entry.is_directory(ec) ? name + "/" : name);
        if (entries.size() >= settings.max_entries) {
            entries.push_back("... (more entries not listed)");
            break;
        }
    }
    std::sort(entries.begin(), entries.end());

    ToolResult result;
    result.ok      = true;
    result.summary = "listed " + target + " (" + std::to_string(entries.size()) + " entries)";
    result.output  = target + ":\n";
    for (const std::string& entry : entries) {
        result.output += "  " + entry + "\n";
    }
    if (entries.empty()) {
        result.output += "  (empty)\n";
    }
    return result;
}

/// READ
ToolResult do_read(const ToolCall& call, const WorkshopSettings& settings) {
    std::string target = call.argument;
    const std::optional<std::filesystem::path> file =
        resolve_argument(settings.root, target, target);
    if (!file) {
        return failure(target + " is outside the project");
    }

    std::ifstream in(*file, std::ios::binary);
    if (!in) {
        return failure("cannot read " + target);
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();

    ToolResult result;
    result.ok      = true;
    result.summary = "read " + target;
    // Numbered, because the next thing an expert wants to say about a file is
    // which line to change.
    std::string numbered;
    int         line_number = 0;
    for (const std::string& line : split_lines(clamp_output(buffer.str(),
                                                            settings.max_read_bytes))) {
        numbered += std::to_string(++line_number) + "\t" + line + "\n";
    }
    result.output = target + ":\n" + numbered;
    return result;
}

/// WRITE
ToolResult do_write(const ToolCall& call, const WorkshopSettings& settings) {
    if (!plausible_path(call.argument)) {
        return failure("\"" + call.argument.substr(0, 60)
                     + "\" is not a file name. WRITE takes a path relative to the "
                       "project root, with no spaces, and the contents go in a fenced "
                       "block on the lines after it");
    }
    const std::optional<std::filesystem::path> file =
        resolve_in_root(settings.root, call.argument);
    if (!file) {
        return failure(call.argument + " is outside the project");
    }
    if (call.content.empty()) {
        // Refused rather than obeyed. A WRITE whose body did not parse looks
        // exactly like a WRITE of an empty file, and one of those two silently
        // destroys the file the expert was working on.
        return failure("WRITE for " + call.argument + " had no content block");
    }

    std::error_code ec;
    std::filesystem::create_directories(file->parent_path(), ec);

    const bool existed = std::filesystem::exists(*file, ec);

    // Read before overwriting, so the journal can say what changed rather than
    // only that something did. "updated calc.py" is true and almost useless: it
    // does not distinguish one character moving from the file being replaced
    // with something unrelated.
    std::string previous;
    if (existed) {
        std::ifstream in(*file, std::ios::binary);
        std::ostringstream buffer;
        buffer << in.rdbuf();
        previous = buffer.str();
    }

    std::string written = call.content;
    if (written.back() != '\n') {
        written += '\n';
    }
    {
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return failure("cannot write " + call.argument);
        }
        out << written;
    }

    const util::DiffStat stat = util::diff_stat(previous, written);

    const std::string recorded = relative_to_root(settings.root, *file);

    ToolResult result;
    result.ok      = true;
    result.changed = {recorded};
    result.detail  = util::unified_diff(previous, written);
    result.summary = (existed ? "updated " : "created ") + recorded
                   + "  " + stat.summary();
    result.output  = "wrote " + recorded + " (" + stat.summary() + ")";
    return result;
}

/// RUN
ToolResult do_run(const ToolCall& call, const WorkshopSettings& settings,
                  const CancelCallback& cancel) {
    if (!settings.allow_run) {
        return failure("running commands is switched off");
    }
    if (call.argument.empty()) {
        return failure("RUN needs a command");
    }

    util::Subprocess child;
    std::string      error;
    // Through a shell, because that is what the command was written for: pipes,
    // redirections and `&&` are how anyone describes running a project, and an
    // argv split would break all three.
    //
    // The shell starts in the project root and that is the whole of the
    // containment -- a shell can cd out of it, and one has, in testing. The
    // file verbs above are confined; this is not, which is why it is a separate
    // switch and why the interface says so. See workshop.hpp.
    if (!child.start(util::shell_command(call.argument), settings.root, {}, error)) {
        return failure("could not run: " + error);
    }

    // A watchdog rather than a poll: read_line blocks, which is what keeps the
    // output in order and the loop simple, so the deadline has to be enforced
    // from outside it.
    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> stopped{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(settings.run_timeout_seconds);
        while (!finished.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (cancel && cancel()) {
                stopped.store(true, std::memory_order_relaxed);
                child.terminate();
                return;
            }
            if (std::chrono::steady_clock::now() >= deadline) {
                timed_out.store(true, std::memory_order_relaxed);
                child.terminate();
                return;
            }
        }
    });

    std::string output;
    std::string line;
    while (child.read_line(line)) {
        output += line;
        output += '\n';
    }
    const int status = child.wait();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();

    ToolResult result;
    result.ok = status == 0 && !timed_out.load(std::memory_order_relaxed);
    result.summary = "$ " + call.argument
                   + (timed_out.load(std::memory_order_relaxed)
                          ? "  -- timed out"
                          : stopped.load(std::memory_order_relaxed)
                                ? "  -- stopped"
                                : "  -- exit " + std::to_string(status));

    result.detail = clamp_output(output, settings.max_output_bytes);
    result.output = "$ " + call.argument + "\n" + result.detail;
    if (timed_out.load(std::memory_order_relaxed)) {
        result.output += "\n(killed after " + std::to_string(settings.run_timeout_seconds)
                       + " seconds)";
    } else {
        result.output += "\n(exit status " + std::to_string(status) + ")";
    }
    return result;
}

/// SEARCH
ToolResult do_search(const ToolCall& call, const SearchSettings& search) {
    if (!search.enabled) {
        return failure("web search is switched off");
    }
    std::string error;
    const std::vector<SearchResult> results = tools::search(call.argument, search, error);

    ToolResult result;
    result.ok      = !results.empty();
    result.summary = "searched \"" + call.argument + "\" ("
                   + std::to_string(results.size()) + " results)";
    result.output  = results.empty()
        ? (error.empty() ? "nothing found" : error)
        : format_for_model(call.argument, results);
    return result;
}

}  // namespace

std::string_view tool_kind_name(ToolKind kind) {
    switch (kind) {
        case ToolKind::List:   return "list";
        case ToolKind::Read:   return "read";
        case ToolKind::Write:  return "write";
        case ToolKind::Run:    return "run";
        case ToolKind::Search: return "search";
        case ToolKind::Ask:    return "ask";
        case ToolKind::Note:   return "note";
        case ToolKind::Done:    return "done";
        case ToolKind::Handoff: return "handoff";
        case ToolKind::None:   break;
    }
    return "none";
}

std::optional<ToolCall> parse_tool_call(std::string_view answer, std::string_view reasoning) {
    const auto scan = [](std::string_view text) -> std::optional<ToolCall> {
        const std::vector<std::string> lines = split_lines(text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            ToolCall call;
            call.kind = verb_of(lines[i], call.argument);
            if (call.kind == ToolKind::None) {
                continue;
            }
            if (call.kind == ToolKind::Write) {
                std::size_t body = i + 1;
                call.content = collect_body(lines, body);
            }
            return call;
        }
        return std::nullopt;
    };

    if (const std::optional<ToolCall> call = scan(answer)) {
        return call;
    }
    // Only when there was nothing for the user. A model that has written an
    // answer is answering, and a tool-trained model that writes its call on a
    // reasoning channel has not.
    if (trim(answer).empty()) {
        return scan(reasoning);
    }
    return std::nullopt;
}

ToolKind attempted_tool_call(std::string_view answer, std::string_view reasoning) {
    const auto scan = [](std::string_view text) {
        for (const std::string& line : split_lines(text)) {
            const std::string trimmed = trim(line);
            for (const Verb& verb : kVerbs) {
                if (colon_optional(verb.kind)) {
                    continue;  // those already parse without one
                }
                if (trimmed.size() > verb.word.size() &&
                    trimmed.compare(0, verb.word.size(), verb.word) == 0 &&
                    trimmed[verb.word.size()] != ':') {
                    return verb.kind;
                }
                // A bare verb on a line of its own -- "DONE" -- is the same
                // mistake with nothing after it.
                if (trimmed == verb.word) {
                    return verb.kind;
                }
            }
        }
        return ToolKind::None;
    };
    if (const ToolKind kind = scan(answer); kind != ToolKind::None) {
        return kind;
    }
    return trim(answer).empty() ? scan(reasoning) : ToolKind::None;
}

std::optional<std::filesystem::path> resolve_in_root(const std::filesystem::path& root,
                                                     std::string_view relative) {
    if (root.empty()) {
        return std::nullopt;
    }
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    if (ec) {
        return std::nullopt;
    }

    std::filesystem::path candidate(relative);
    if (candidate.is_absolute()) {
        // An absolute path inside the root is fine and is what a model produces
        // after reading its own file listing. One outside is not, and falls out
        // of the containment check below.
        candidate = std::filesystem::weakly_canonical(candidate, ec);
    } else {
        candidate = std::filesystem::weakly_canonical(base / candidate, ec);
    }
    if (ec) {
        return std::nullopt;
    }

    // Compared component by component rather than as strings: "/home/matt/proj"
    // is a prefix of "/home/matt/project-two" as text and is not a parent of it
    // as a path, and that difference is the whole containment guarantee.
    auto base_it = base.begin();
    auto cand_it = candidate.begin();
    for (; base_it != base.end(); ++base_it, ++cand_it) {
        if (cand_it == candidate.end() || *cand_it != *base_it) {
            return std::nullopt;
        }
    }
    return candidate;
}

std::string clamp_output(std::string_view text, std::size_t limit) {
    if (text.size() <= limit || limit == 0) {
        return std::string(text);
    }
    // Head and tail, because a compiler puts the first error at the top and the
    // summary at the bottom, and the middle of a long log is the part nobody
    // reads.
    const std::size_t half = limit / 2;
    const std::size_t dropped = text.size() - (half * 2);

    std::string out(text.substr(0, half));
    out += "\n\n... (" + std::to_string(dropped) + " bytes not shown) ...\n\n";
    out += std::string(text.substr(text.size() - half));
    return out;
}

ToolResult run_tool(const ToolCall& call, const WorkshopSettings& settings,
                    const SearchSettings& search, const CancelCallback& cancel) {
    if (!settings.enabled) {
        return failure("the workshop is switched off");
    }
    switch (call.kind) {
        case ToolKind::List:   return do_list(call, settings);
        case ToolKind::Read:   return do_read(call, settings);
        case ToolKind::Write:  return do_write(call, settings);
        case ToolKind::Run:    return do_run(call, settings, cancel);
        case ToolKind::Search: return do_search(call, search);
        case ToolKind::Note: {
            ToolResult result;
            result.ok      = true;
            result.summary = call.argument;
            result.output  = "noted";
            return result;
        }
        case ToolKind::Ask:
        case ToolKind::Done:
        case ToolKind::Handoff:
        case ToolKind::None:
            break;
    }
    // Ask, Done and Handoff are answers to the cook loop rather than actions,
    // and it handles them before ever reaching here.
    return failure("nothing to do");
}

std::string workshop_instructions(const WorkshopSettings& settings,
                                  ToolAudience audience) {
    if (!settings.enabled) {
        return {};
    }
    std::string text =
        "\n\nYou can act on the project, not only describe it. To do so, write ONE "
        "of these on a line of its own and then stop -- you will be given the result "
        "and can continue:\n"
        "LIST: <directory>          what is in it (\".\" is the project root)\n"
        "READ: <file>               its contents, with line numbers\n"
        "WRITE: <file>              then the whole new contents in a ``` block\n";
    if (settings.allow_run) {
        text += "RUN: <command>             run it in the project root and see the output\n";
    }
    text += "NOTE: <what you are doing>  recorded in the log, no other effect\n";
    if (audience == ToolAudience::Cook) {
        text +=
            "ASK: <question>            ask the user something you cannot work out\n"
            "DONE: <what you changed>   this piece of work is finished\n"
            "HANDOFF: <the next work>   this piece is done and the next needs a "
            "different\n"
            "                           kind of expertise -- say what it is, in one "
            "line,\n"
            "                           and the right expert will be brought in\n";
    }
    text +=
        "\nThe colon is required. One call per reply, on its own line, and nothing "
        "after it. Paths are relative to the project root and cannot leave it.\n";
    if (audience == ToolAudience::Chat) {
        // Said plainly, because the alternative failure is the visible one: an
        // expert that has finished the work and then writes "DONE: fixed it",
        // leaving the person who asked with a status line instead of an answer.
        text +=
            "\nWhen you have finished, write the answer for the person who asked --"
            " no verb, no status line. If you need something from them, just ask it"
            " in the reply.\n";
    }
    // A worked example, because the rules alone are not enough. Told only the
    // shape, models write `WRITE path "a description of the change"` and expect
    // that to be applied -- which would put the description into the file in
    // place of the code.
    text +=
        "\nWRITE replaces the whole file and the new contents go in a fenced block "
        "on the following lines, never on the same line. Like this:\n"
        "\nWRITE: src/calc.py\n"
        "```\n"
        "def add(a, b):\n"
        "    return a + b\n"
        "```\n"
        "\nRead a file before rewriting it, unless you are creating it.\n";
    return text;
}

}  // namespace crucible::tools
