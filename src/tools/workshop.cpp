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
#include "crucible/util/text.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <ctime>
#include <fstream>
#include <sstream>
#include <system_error>
#include <thread>

#include "crucible/tools/computer.hpp"
#include "crucible/tools/fetch.hpp"
#include "crucible/tools/git.hpp"
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
//
// Longer words before shorter ones that share a start, so SCREENSHOT is
// tried before SCROLL and neither is mistaken for the other.
constexpr std::array<Verb, 23> kVerbs{{
    {"LIST",       ToolKind::List},
    {"READ",       ToolKind::Read},
    {"WRITE",      ToolKind::Write},
    {"RUN",        ToolKind::Run},
    {"SEARCH",     ToolKind::Search},
    {"ASK",        ToolKind::Ask},
    {"NOTE",       ToolKind::Note},
    {"DONE",       ToolKind::Done},
    {"HANDOFF",    ToolKind::Handoff},
    {"FETCH",      ToolKind::Fetch},
    {"GIT",        ToolKind::Git},
    {"GH",         ToolKind::Gh},
    {"PYTHON",     ToolKind::Python},
    {"START",      ToolKind::Start},
    {"STOP",       ToolKind::Stop},
    {"LOGS",       ToolKind::Logs},
    {"SCREENSHOT", ToolKind::Screenshot},
    {"CLICK",      ToolKind::Click},
    {"MOVE",       ToolKind::Move},
    {"TYPE",       ToolKind::Type},
    {"KEY",        ToolKind::Key},
    {"SCROLL",     ToolKind::Scroll},
    {"LOOK",       ToolKind::Screenshot},   // what a model writes when it means a screenshot
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

/// Whether the verb's argument is a name or a command line: names are
/// unquoted, commands are kept as written, because quoting means something to
/// a shell, to git and to a keyboard.
bool keeps_argument_verbatim(ToolKind kind) {
    return kind == ToolKind::Run || kind == ToolKind::Git || kind == ToolKind::Gh
        || kind == ToolKind::Start || kind == ToolKind::Type || kind == ToolKind::Python;
}

/// The verb a line opens with, or None.
///
/// The line must *begin* with it, ignoring indentation. A sentence that happens
/// to contain "RUN:" halfway through is prose, and treating it as a call is how
/// an expert explaining a command ends up executing it.
///
/// `RUN bash: ...` names a shell: the verb, a space, a known shell's name and
/// the colon. A RUN followed by a space and anything else is still prose.
ToolKind verb_of(std::string_view line, std::string& argument, std::string& shell) {
    const std::string trimmed = trim(line);
    shell.clear();
    for (const Verb& verb : kVerbs) {
        if (trimmed.size() <= verb.word.size() ||
            trimmed.compare(0, verb.word.size(), verb.word) != 0) {
            continue;
        }
        const char after = trimmed[verb.word.size()];
        std::size_t rest = verb.word.size() + 1;
        bool separated = after == ':'
                      || (colon_optional(verb.kind) && (after == ' ' || after == '\t'));
        if (!separated && verb.kind == ToolKind::Run && after == ' ') {
            // "RUN zsh: ls": the word between the verb and the colon is the shell.
            const std::size_t colon = trimmed.find(':', rest);
            if (colon != std::string::npos) {
                const std::string named = format::to_lower(trim(trimmed.substr(rest, colon - rest)));
                if (known_shell(named)) {
                    shell     = named;
                    rest      = colon + 1;
                    separated = true;
                }
            }
        }
        if (!separated) {
            continue;
        }
        argument = trim(std::string_view(trimmed).substr(rest));
        if (!keeps_argument_verbatim(verb.kind)) {
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

/// Collect a verb's body, starting at `index` (the line after the verb).
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
    std::string shell;
    while (index < lines.size() && verb_of(lines[index], ignored, shell) == ToolKind::None) {
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
#if defined(_WIN32)
    // Except the colon every absolute Windows path starts with. Absolute paths
    // inside the root are accepted -- it is what a model writes after reading
    // its own listing -- and on Windows this refused every one of them, drive
    // letter first. Only a drive at the very start and followed by a
    // separator; a colon anywhere after that is still a sentence.
    if (target.size() >= 3 && std::isalpha(static_cast<unsigned char>(target[0])) != 0
        && target[1] == ':' && (target[2] == '\\' || target[2] == '/')) {
        target.remove_prefix(2);
    }
#endif
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

/// Run `argv` in `cwd` and keep what it printed, with a deadline and a Stop.
///
/// A watchdog rather than a poll: read_line blocks, which is what keeps the
/// output in order and the loop simple, so the deadline has to be enforced
/// from outside it. `timed_out` and `stopped` say which of the two ended it.
struct Captured {
    bool        started   = false;
    bool        timed_out = false;
    bool        stopped   = false;
    int         status    = -1;
    std::string output;
    std::string error;
};

Captured capture(const std::vector<std::string>& argv, const std::filesystem::path& cwd,
                 int timeout_seconds, const CancelCallback& cancel) {
    Captured captured;
    util::Subprocess child;
    if (!child.start(argv, cwd, {}, captured.error)) {
        return captured;
    }
    captured.started = true;

    std::atomic<bool> finished{false};
    std::atomic<bool> timed_out{false};
    std::atomic<bool> stopped{false};
    std::thread watchdog([&] {
        const auto deadline = std::chrono::steady_clock::now()
                            + std::chrono::seconds(timeout_seconds);
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

    std::string line;
    while (child.read_line(line)) {
        captured.output += line;
        captured.output += '\n';
    }
    captured.status = child.wait();
    finished.store(true, std::memory_order_relaxed);
    watchdog.join();
    // In the console's code page on Windows, not UTF-8. See console_to_utf8.
    captured.output    = crucible::detail::console_to_utf8(captured.output);
    captured.timed_out = timed_out.load(std::memory_order_relaxed);
    captured.stopped   = stopped.load(std::memory_order_relaxed);
    return captured;
}

/// A command's result, as every verb that runs one reports it.
ToolResult command_result(const std::string& shown, const Captured& captured,
                          const WorkshopSettings& settings) {
    if (!captured.started) {
        return failure("could not run: " + captured.error);
    }
    ToolResult result;
    result.ok = captured.status == 0 && !captured.timed_out && !captured.stopped;
    result.summary = shown
                   + (captured.timed_out ? "  -- timed out"
                      : captured.stopped ? "  -- stopped"
                                         : "  -- exit " + std::to_string(captured.status));
    result.detail = clamp_output(captured.output, settings.max_output_bytes);
    result.output = shown + "\n" + result.detail;
    if (captured.timed_out) {
        result.output += "\n(killed after " + std::to_string(settings.run_timeout_seconds)
                       + " seconds -- a program that should keep running wants START, not RUN)";
    } else {
        result.output += "\n(exit status " + std::to_string(captured.status) + ")";
    }
    return result;
}

/// A fresh file name in the scratch folder, for a screenshot or a snippet.
std::filesystem::path scratch_file(const WorkshopSettings& settings, const char* stem,
                                   const char* extension) {
    std::error_code ec;
    std::filesystem::path dir = settings.scratch;
    if (dir.empty()) {
        dir = std::filesystem::temp_directory_path(ec) / "crucible-scratch";
    }
    std::filesystem::create_directories(dir, ec);
    const std::time_t now = std::time(nullptr);
    std::tm parts = util::local_time(now);
    char when[32] = {};
    std::strftime(when, sizeof(when), "%Y%m%d-%H%M%S", &parts);
    static std::atomic<int> counter{0};
    return dir / (std::string(stem) + "-" + when + "-" + std::to_string(++counter) + extension);
}

/// A picture, as a tool hands it on: the bytes for a model that sees, the
/// words off it for one that does not, and where it is for the journal.
void attach_picture(ToolResult& result, const std::filesystem::path& file, const std::string& what) {
    const attach::Image image = attach::picture(file, 24ULL << 20);
    if (image.data.empty()) {
        result.output += "\n(" + what + " could not be read back as a picture)";
        return;
    }
    result.pictures.push_back(image);
    result.picture_path = file.string();
    std::string why;
    const std::string words = computer::read_text(file, why);
    if (!words.empty()) {
        result.output += "\nThe words in " + what + ", read off it:\n" + clamp_output(words, 6000);
    } else if (!why.empty()) {
        result.output += "\n(" + why + ")";
    }
}

// --- the verbs ---------------------------------------------------------------

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
///
/// Text as it is, numbered. A document -- a PDF, a Word file, a spreadsheet
/// -- as the text inside it, read by the same readers an attachment is. A
/// picture as a picture, for a model that sees one, and as the words on it
/// for one that does not.
ToolResult do_read(const ToolCall& call, const WorkshopSettings& settings) {
    std::string target = call.argument;
    const std::optional<std::filesystem::path> file =
        resolve_argument(settings.root, target, target);
    if (!file) {
        return failure(target + " is outside the project");
    }
    std::error_code ec;
    if (std::filesystem::is_directory(*file, ec)) {
        return failure(target + " is a directory -- use LIST: " + target);
    }
    if (!std::filesystem::exists(*file, ec)) {
        return failure(target + " does not exist");
    }

    const attach::Kind kind = attach::kind_of(*file);
    if (kind == attach::Kind::Image) {
        ToolResult result;
        result.ok      = true;
        result.summary = "looked at " + target;
        result.output  = "[picture: " + target + ", " + format::bytes(std::filesystem::file_size(*file, ec)) + "]";
        attach_picture(result, *file, target);
        return result;
    }
    if (kind == attach::Kind::Document && !attach::is_markup_source(*file)) {
        const attach::Text text = attach::read(*file, settings.max_read_bytes);
        ToolResult result;
        result.ok      = true;
        result.summary = "read " + target;
        result.output  = target + " (" + attach::label_for(*file) + "):\n" + text.body;
        if (text.body.empty()) {
            result.output += "(" + (text.note.empty() ? std::string("nothing in it could be read") : text.note) + ")";
        } else if (text.cut) {
            result.output += "\n[cut: the first " + std::to_string(text.body.size()) + " of "
                           + std::to_string(text.total) + " characters]";
        }
        return result;
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
    // Through a shell, because that is what the command was written for: pipes,
    // redirections and `&&` are how anyone describes running a project, and an
    // argv split would break all three.
    //
    // The shell starts in the project root and that is the whole of the
    // containment -- a shell can cd out of it, and one has, in testing. The
    // file verbs above are confined; this is not, which is why it is a separate
    // switch and why the interface says so. See workshop.hpp.
    std::vector<std::string> argv;
    if (call.shell.empty()) {
        argv = util::shell_command(call.argument);
    } else {
        argv = shell_argv(call.shell, call.argument);
        if (argv.empty() || !util::on_path(argv.front())) {
            return failure(call.shell + " is not installed on this machine");
        }
    }
    const Captured captured = capture(argv, settings.root, settings.run_timeout_seconds, cancel);
    const std::string shown = (call.shell.empty() ? "$ " : call.shell + "$ ") + call.argument;
    return command_result(shown, captured, settings);
}

/// PYTHON
ToolResult do_python(const ToolCall& call, const WorkshopSettings& settings,
                     const CancelCallback& cancel) {
    if (!settings.allow_run) {
        return failure("running commands is switched off");
    }
    if (settings.python.empty()) {
        return failure("Crucible's Python is not installed yet, so PYTHON cannot run -- it is "
                       "fetched when Crucible starts, or by crucible --install-python");
    }
    const std::string code = call.content.empty() ? call.argument : call.content;
    if (trim(code).empty()) {
        return failure("PYTHON needs the code in a fenced block on the lines after it");
    }
    const std::filesystem::path file = scratch_file(settings, "snippet", ".py");
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return failure("could not write the snippet to " + file.string());
        }
        out << code << '\n';
    }
    // -I: isolated, so a planted module in the project cannot be what
    // `import json` finds. The project is still the working directory, which
    // is what the snippet is for.
    const Captured captured = capture({settings.python.string(), "-I", file.string()}, settings.root,
                                      settings.run_timeout_seconds, cancel);
    std::string first = code.substr(0, code.find('\n'));
    if (first.size() > 60) {
        first.resize(60);
        first += "…";
    }
    return command_result("python: " + first, captured, settings);
}

/// GIT and GH
ToolResult do_git(const ToolCall& call, const WorkshopSettings& settings, bool github) {
    if (!settings.allow_run) {
        return failure("running commands is switched off");
    }
    const std::vector<std::string> args = git::split_arguments(call.argument);
    if (args.empty()) {
        return failure(std::string(github ? "GH" : "GIT") + " needs arguments, like `status` or "
                       + (github ? "`pr list`" : "`log --oneline -5`"));
    }
    const git::Output out = github ? git::run_gh(settings.root, args, settings.run_timeout_seconds)
                                   : git::run(settings.root, args, settings.run_timeout_seconds);
    const std::string shown = std::string(github ? "gh " : "git ") + call.argument;
    if (!out.error.empty()) {
        return failure(shown + ": " + out.error);
    }
    ToolResult result;
    result.ok      = out.ok;
    result.summary = shown + "  -- exit " + std::to_string(out.status);
    result.detail  = clamp_output(out.text, settings.max_output_bytes);
    result.output  = shown + "\n" + result.detail + "\n(exit status " + std::to_string(out.status) + ")";
    return result;
}

/// FETCH
ToolResult do_fetch(const ToolCall& call, const WorkshopSettings& settings) {
    if (!settings.web) {
        return failure("the web is switched off -- Settings, Tools, Web search");
    }
    if (call.argument.empty()) {
        return failure("FETCH needs an address");
    }
    const Page page = fetch(call.argument, settings.max_fetch_chars, 45);
    if (!page.ok) {
        return failure("could not fetch " + call.argument + ": " + page.error);
    }
    ToolResult result;
    result.ok      = true;
    result.summary = "fetched " + call.argument + (page.title.empty() ? "" : "  ·  " + page.title)
                   + "  ·  " + (page.how == "browser" ? "rendered in a browser" : "as served");
    result.output  = (page.title.empty() ? "" : page.title + "\n") + page.url + "\n\n" + page.text;
    result.detail  = clamp_output(page.text, settings.max_output_bytes);
    return result;
}

/// START, STOP and LOGS
ToolResult do_processes(const ToolCall& call, const WorkshopSettings& settings) {
    if (!settings.allow_run) {
        return failure("running commands is switched off");
    }
    if (settings.processes == nullptr) {
        return failure("nothing can be left running from here");
    }
    Processes& processes = *settings.processes;
    ToolResult result;
    result.ok = true;
    if (call.kind == ToolKind::Start) {
        std::string error;
        const std::string name = processes.start(call.argument, settings.root, error);
        if (name.empty()) {
            return failure("could not start it: " + error);
        }
        // A moment, so a program that dies at once is reported as dead now
        // rather than on the next LOGS.
        std::this_thread::sleep_for(std::chrono::milliseconds(700));
        std::string logs;
        bool running = true;
        int status = -1;
        processes.logs(name, 2000, logs, running, status);
        result.summary = "started " + name + ": " + call.argument + (running ? "" : "  -- it exited at once");
        result.output  = "started " + name + " (" + call.argument + ")" + (running ? ", running" : ", which exited with status " + std::to_string(status))
                       + ".\nRead what it prints with LOGS: " + name + " and stop it with STOP: " + name + "."
                       + (logs.empty() ? "" : "\nSo far:\n" + logs);
        result.ok      = running;
        return result;
    }
    // The name is the first word. A model echoes what it was last told --
    // `LOGS: p4 exited with status 127` -- and the name was right.
    std::string name = call.argument;
    if (const std::size_t space = name.find_first_of(" \t"); space != std::string::npos) {
        name.resize(space);
    }
    if (call.kind == ToolKind::Stop) {
        std::string error;
        if (!processes.stop(name, error)) {
            return failure(error);
        }
        result.summary = "stopped " + name;
        result.output  = "stopped " + name;
        return result;
    }
    if (name.empty()) {
        const std::vector<ProcessInfo> all = processes.list();
        result.summary = all.empty() ? "nothing is running" : std::to_string(all.size()) + " running";
        result.output  = all.empty() ? "Nothing was started with START." : "Processes:\n";
        for (const ProcessInfo& one : all) {
            result.output += "  " + one.name + "  " + (one.running ? "running" : "exited " + std::to_string(one.status))
                           + "  " + std::to_string(one.seconds) + "s  " + one.command + "\n";
        }
        return result;
    }
    std::string logs;
    bool running = false;
    int status = -1;
    if (!processes.logs(name, settings.max_output_bytes, logs, running, status)) {
        return failure("there is no process called " + name);
    }
    result.summary = "read the output of " + name + (running ? "" : " (exited)");
    result.output  = name + (running ? " is running" : " exited with status " + std::to_string(status))
                   + ". Its output so far:\n" + (logs.empty() ? "(nothing yet)" : logs);
    result.detail  = logs;
    return result;
}

/// The screen
ToolResult do_computer(const ToolCall& call, const WorkshopSettings& settings) {
    if (!settings.computer_control) {
        return failure("using the screen, mouse and keyboard is switched off -- "
                       "Settings, Tools, Let experts use this computer");
    }
    std::istringstream words(call.argument);
    const auto number = [&words](int& out) {
        std::string word;
        if (!(words >> word)) {
            return false;
        }
        try {
            out = std::stoi(word);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    };
    computer::Outcome outcome;
    ToolResult result;
    switch (call.kind) {
        case ToolKind::Screenshot: {
            const std::filesystem::path file = scratch_file(settings, "screenshot", ".png");
            outcome = computer::screenshot(file);
            if (!outcome.ok) {
                return failure(outcome.error);
            }
            result.ok      = true;
            result.summary = outcome.summary;
            const std::string size = computer::screen_size();
            result.output = "[screenshot of the screen" + (size.empty() ? "" : ", " + size + " pixels") + "]";
            attach_picture(result, file, "the screenshot");
            result.output += "\nCoordinates for CLICK and MOVE are screen pixels from the top left.";
            return result;
        }
        case ToolKind::Click:
        case ToolKind::Move: {
            int x = 0, y = 0;
            if (!number(x) || !number(y)) {
                return failure(std::string(call.kind == ToolKind::Click ? "CLICK" : "MOVE")
                               + " takes two numbers: the x and y of a screen pixel");
            }
            if (call.kind == ToolKind::Move) {
                outcome = computer::move(x, y);
                break;
            }
            std::string button = "left";
            int count = 1;
            std::string word;
            while (words >> word) {
                word = format::to_lower(word);
                if (word == "right" || word == "middle" || word == "left") { button = word; }
                else if (word == "double") { count = 2; }
                else if (word == "triple") { count = 3; }
            }
            outcome = computer::click(x, y, button, count);
            break;
        }
        case ToolKind::Type: {
            const std::string text = call.content.empty() ? call.argument : call.content;
            outcome = computer::type_text(text);
            break;
        }
        case ToolKind::Key:
            outcome = computer::press(call.argument);
            break;
        case ToolKind::Scroll: {
            std::string direction;
            words >> direction;
            direction = format::to_lower(direction);
            int notches = 3;
            number(notches);
            const int dy = direction == "up" ? -notches : direction == "down" ? notches : 0;
            const int dx = direction == "left" ? -notches : direction == "right" ? notches : 0;
            if (dx == 0 && dy == 0) {
                return failure("SCROLL takes up, down, left or right, and a number of notches");
            }
            outcome = computer::scroll(dx, dy);
            break;
        }
        default:
            return failure("nothing to do");
    }
    if (!outcome.ok) {
        return failure(outcome.error);
    }
    result.ok      = true;
    result.summary = outcome.summary;
    result.output  = outcome.summary + ". Take a SCREENSHOT: to see what happened.";
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
        case ToolKind::List:       return "list";
        case ToolKind::Read:       return "read";
        case ToolKind::Write:      return "write";
        case ToolKind::Run:        return "run";
        case ToolKind::Search:     return "search";
        case ToolKind::Ask:        return "ask";
        case ToolKind::Note:       return "note";
        case ToolKind::Done:       return "done";
        case ToolKind::Handoff:    return "handoff";
        case ToolKind::Fetch:      return "fetch";
        case ToolKind::Git:        return "git";
        case ToolKind::Gh:         return "gh";
        case ToolKind::Python:     return "python";
        case ToolKind::Start:      return "start";
        case ToolKind::Stop:       return "stop";
        case ToolKind::Logs:       return "logs";
        case ToolKind::Screenshot: return "screenshot";
        case ToolKind::Click:      return "click";
        case ToolKind::Move:       return "move";
        case ToolKind::Type:       return "type";
        case ToolKind::Key:        return "key";
        case ToolKind::Scroll:     return "scroll";
        case ToolKind::None:       break;
    }
    return "none";
}

bool known_shell(std::string_view shell) {
    for (const char* name : {"sh", "bash", "zsh", "fish", "dash", "ksh", "cmd", "powershell", "pwsh"}) {
        if (shell == name) {
            return true;
        }
    }
    return false;
}

std::vector<std::string> shell_argv(std::string_view shell, const std::string& command) {
    const std::string name(shell);
    if (!known_shell(shell)) {
        return {};
    }
    if (name == "cmd") {
        return {"cmd", "/c", command};
    }
    if (name == "powershell" || name == "pwsh") {
        return {name, "-NoProfile", "-NonInteractive", "-Command", command};
    }
    return {name, "-c", command};
}

std::optional<ToolCall> parse_tool_call(std::string_view answer, std::string_view reasoning) {
    const auto scan = [](std::string_view text) -> std::optional<ToolCall> {
        const std::vector<std::string> lines = split_lines(text);
        for (std::size_t i = 0; i < lines.size(); ++i) {
            ToolCall call;
            call.kind = verb_of(lines[i], call.argument, call.shell);
            if (call.kind == ToolKind::None) {
                continue;
            }
            if (call.kind == ToolKind::Write || call.kind == ToolKind::Python
                || (call.kind == ToolKind::Type && call.argument.empty())) {
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
                    trimmed[verb.word.size()] != ':' &&
                    // A word that merely begins with a verb -- "STOPPING the
                    // server" -- is prose, and GH alone would match "GHOST".
                    !(std::isalpha(static_cast<unsigned char>(trimmed[verb.word.size()])) != 0)) {
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

namespace {
ToolResult run_tool_as_is(const ToolCall& call, const WorkshopSettings& settings,
                          const SearchSettings& search, const CancelCallback& cancel);
}  // namespace

ToolResult run_tool(const ToolCall& call, const WorkshopSettings& settings,
                    const SearchSettings& search, const CancelCallback& cancel) {
    // Whatever a tool brought in -- a Latin-1 file, a listing of names that
    // are bytes, output cut at the size limit in the middle of a character --
    // leaves here as UTF-8, because it is about to be kept as JSON: in the
    // session, in a cook's journal, and in what the window is sent.
    ToolResult result = run_tool_as_is(call, settings, search, cancel);
    result.output  = crucible::detail::scrub_utf8(result.output);
    result.summary = crucible::detail::scrub_utf8(result.summary);
    result.detail  = crucible::detail::scrub_utf8(result.detail);
    for (std::string& path : result.changed) {
        path = crucible::detail::scrub_utf8(path);
    }
    return result;
}

namespace {

ToolResult run_tool_as_is(const ToolCall& call, const WorkshopSettings& settings,
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
        case ToolKind::Fetch:  return do_fetch(call, settings);
        case ToolKind::Git:    return do_git(call, settings, false);
        case ToolKind::Gh:     return do_git(call, settings, true);
        case ToolKind::Python: return do_python(call, settings, cancel);
        case ToolKind::Start:
        case ToolKind::Stop:
        case ToolKind::Logs:   return do_processes(call, settings);
        case ToolKind::Screenshot:
        case ToolKind::Click:
        case ToolKind::Move:
        case ToolKind::Type:
        case ToolKind::Key:
        case ToolKind::Scroll: return do_computer(call, settings);
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

}  // namespace

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
        "READ: <file>               its contents, with line numbers; a PDF or Office file as "
        "its text; a picture as itself\n"
        "WRITE: <file>              then the whole new contents in a ``` block\n";
    if (settings.allow_run) {
        text += "RUN: <command>             run it in the project root and see the output; "
                "RUN bash: or RUN powershell: picks the shell\n"
                "PYTHON:                    then Python code in a ``` block, run in the project root\n"
                "GIT: <arguments>           git, in the project: GIT: status, GIT: log --oneline -5, "
                "GIT: commit -am \"message\"\n";
        if (git::gh_available()) {
            text += "GH: <arguments>            GitHub's command line: GH: pr list, GH: repo create, "
                    "GH: release create v1.0\n";
        }
        if (settings.processes != nullptr) {
            text += "START: <command>           run it and leave it running (a server), named p1, p2...\n"
                    "LOGS: <name>               what it has printed so far; LOGS: alone lists them\n"
                    "STOP: <name>               stop it\n";
        }
    }
    if (settings.web) {
        text += "SEARCH: <query>            search the web; the results come back to you\n"
                "FETCH: <url>               read a web page as text\n";
    }
    if (settings.computer_control) {
        text += "SCREENSHOT:                a picture of the screen, with its text read off it\n"
                "CLICK: <x> <y>             click a screen pixel; add right, middle or double\n"
                "MOVE: <x> <y>              move the mouse\n"
                "TYPE: <text>               type it into whatever has the focus\n"
                "KEY: <keys>                press enter, tab, escape, ctrl+c, cmd+s, alt+f4...\n"
                "SCROLL: up|down <notches>  scroll at the mouse\n";
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
    // Said when it is so, because a model told it may look at a project looks
    // -- and a new chat's folder has nothing in it. Listing it is a round
    // spent learning nothing, and an empty listing drawn in the transcript
    // reads as the interface showing a blank box.
    {
        std::error_code ec;
        if (std::filesystem::is_directory(settings.root, ec)
            && std::filesystem::directory_iterator(settings.root, ec)
                   == std::filesystem::directory_iterator()
            && !ec) {
            text += "\nThe project folder is empty: there is nothing in it to list, read or "
                    "run yet. Answer from what you know, and make a file only when what "
                    "was asked for is something to be made.\n";
        }
    }
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
    if (settings.computer_control) {
        text += "\nTo use the screen: SCREENSHOT: first, find what you want in the picture, "
                "then CLICK: its pixel, then SCREENSHOT: again to see the result. One "
                "action at a time.\n";
    }
    return text;
}

}  // namespace crucible::tools
