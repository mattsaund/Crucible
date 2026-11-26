// SPDX-License-Identifier: MIT
//
// See native_tools.hpp.
#include "crucible/tools/native_tools.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <initializer_list>
#include <system_error>
#include <utility>
#include <vector>

#include "crucible/tools/git.hpp"
#include "crucible/tools/mcp.hpp"

namespace crucible::tools {
namespace {

using json = nlohmann::json;
using ordered = nlohmann::ordered_json;

struct Field {
    const char* name;
    const char* type;
    const char* about;
    bool        required = true;
};

ordered tool(const char* name, const char* about, std::initializer_list<Field> fields) {
    ordered properties = ordered::object();
    ordered required   = ordered::array();
    for (const Field& field : fields) {
        properties[field.name] = ordered{{"type", field.type}, {"description", field.about}};
        if (field.required) {
            required.push_back(field.name);
        }
    }
    return ordered{{"type", "function"},
                   {"function",
                    {{"name", name},
                     {"description", about},
                     {"parameters", {{"type", "object"}, {"properties", properties}, {"required", required}}}}}};
}

/// The first of `names` the call has, as text. Models trained on other
/// people's tools reach for their names for the same thing: file_path for
/// path, old_string for search.
std::optional<std::string> text_of(const json& arguments, std::initializer_list<const char*> names) {
    for (const char* const name : names) {
        const auto found = arguments.find(name);
        if (found == arguments.end() || found->is_null()) {
            continue;
        }
        return found->is_string() ? found->get<std::string>() : found->dump();
    }
    return std::nullopt;
}

std::string path_of(const json& arguments) {
    return text_of(arguments, {"path", "file_path", "filepath", "file", "filename", "directory", "dir"}).value_or("");
}

/// A whole number the call has, written as one or as a string of one.
std::optional<long> number_of(const json& arguments, std::initializer_list<const char*> names) {
    const std::optional<std::string> text = text_of(arguments, names);
    if (!text || text->empty()) {
        return std::nullopt;
    }
    char*      end   = nullptr;
    const long value = std::strtol(text->c_str(), &end, 10);
    return end != text->c_str() ? std::optional<long>(value) : std::nullopt;
}

}  // namespace

ordered native_tools(const WorkshopSettings& settings, bool search) {
    ordered list = ordered::array();
    if (!settings.enabled) {
        return list;
    }
    list.push_back(tool("list_files", "List what is in a folder of the project.",
                        {{"path", "string", "The folder, relative to the project root. \".\" is the root."}}));
    list.push_back(tool("read_file",
                        "Read a file of the project, with line numbers. A PDF or Office file comes back as its "
                        "text, and a picture as itself.",
                        {{"path", "string", "The file, relative to the project root."},
                         {"start_line", "integer", "The first line to read, for a long file.", false},
                         {"end_line", "integer", "The last line to read.", false}}));
    list.push_back(tool("find_in_files", "Find the lines of the project's files that have some text.",
                        {{"text", "string", "The text to find."},
                         {"files", "string", "Which files to look in, as a pattern like *.css.", false}}));
    list.push_back(tool("write_file",
                        "Create a file, or replace a whole file, with exactly these contents. This is how a file "
                        "gets made: showing its contents in a reply writes nothing.",
                        {{"path", "string", "The file, relative to the project root."},
                         {"content", "string", "The complete contents of the file."}}));
    list.push_back(tool("edit_file",
                        "Change part of a file that exists: replace text that is in it. Read the file first and "
                        "copy the text as it is.",
                        {{"path", "string", "The file, relative to the project root."},
                         {"search", "string", "The exact text to replace, with enough lines to be unique."},
                         {"replace", "string", "What it becomes."},
                         {"at_line", "integer", "For text the file has more than once, the line of the one to "
                                                "change.", false}}));
    list.push_back(tool("render_page",
                        "Draw an HTML page or an SVG of the project into a PNG or PDF, to see how it looks.",
                        {{"page", "string", "The page, relative to the project root."},
                         {"output", "string", "The .png or .pdf to write, relative to the project root."},
                         {"size", "string", "The window size for a PNG, like 1280x800.", false}}));
    if (settings.allow_run) {
        list.push_back(tool("run_command", "Run a shell command in the project root and see what it prints.",
                            {{"command", "string", "The command, on one line."},
                             {"shell", "string", "The shell to use, like bash or powershell.", false}}));
        list.push_back(tool("run_python", "Run Python code in the project root and see what it prints.",
                            {{"code", "string", "The code."}}));
        list.push_back(tool("git", "Run git in the project.",
                            {{"arguments", "string", "What follows `git`, like: status, or log --oneline -5."}}));
        if (git::gh_available()) {
            list.push_back(tool("github", "Run GitHub's command line in the project.",
                                {{"arguments", "string", "What follows `gh`, like: pr list."}}));
        }
        if (settings.processes != nullptr) {
            list.push_back(tool("start_process",
                                "Run a command and leave it running, like a server. It is named p1, p2 and so on.",
                                {{"command", "string", "The command."}}));
            list.push_back(tool("process_logs", "See what a started process has printed, or list them.",
                                {{"name", "string", "Its name, like p1. Leave it out to list them.", false}}));
            list.push_back(tool("stop_process", "Stop a started process.",
                                {{"name", "string", "Its name, like p1."}}));
        }
    }
    if (settings.web) {
        list.push_back(tool("fetch_url", "Read a web page as text.", {{"url", "string", "The address."}}));
    }
    if (search || settings.web) {
        list.push_back(tool("web_search", "Search the web. The results come back to you.",
                            {{"query", "string", "What to search for."}}));
    }
    if (settings.computer_control) {
        list.push_back(tool("screenshot", "Take a picture of the screen.", {}));
        list.push_back(tool("click", "Click a pixel of the last screenshot.",
                            {{"x", "integer", "Across."},
                             {"y", "integer", "Down."},
                             {"button", "string", "right or middle, for those.", false},
                             {"double", "boolean", "A double click.", false}}));
        list.push_back(tool("move_mouse", "Move the mouse.", {{"x", "integer", "Across."}, {"y", "integer", "Down."}}));
        list.push_back(tool("type_text", "Type into whatever has the focus.", {{"text", "string", "The text."}}));
        list.push_back(tool("press_key", "Press a key or a combination.",
                            {{"keys", "string", "Like enter, tab, escape or ctrl+c."}}));
        list.push_back(tool("scroll", "Scroll at the mouse.",
                            {{"direction", "string", "up, down, left or right."},
                             {"notches", "integer", "How far.", false}}));
    }
    if (settings.mcp != nullptr) {
        const std::vector<mcp::Tool> offered = settings.mcp->tools();
        if (!offered.empty()) {
            std::string about = "Use a tool another program offers. These:";
            std::size_t shown = 0;
            for (const mcp::Tool& one : offered) {
                if (++shown > 40) {
                    about += "\n(and " + std::to_string(offered.size() - 40) + " more)";
                    break;
                }
                about += "\n" + mcp::detail::describe(one);
            }
            ordered entry = tool("use_tool", about.c_str(),
                                 {{"tool", "string", "The server and the tool, like github/create_issue."},
                                  {"arguments", "object", "Its arguments.", false}});
            list.push_back(std::move(entry));
        }
    }
    list.push_back(tool(kHandOffTool.data(),
                        "Pass the next step to an expert with the right expertise, who takes over from here. Only "
                        "when the next step needs a different kind of expertise.",
                        {{"next_step", "string", "The next step, in one line."}}));
    return list;
}

std::string native_instructions(const WorkshopSettings& settings) {
    if (!settings.enabled) {
        return {};
    }
    std::string text =
        "\n\nYou are working in the person's project, and you can act on it with your tools: look through its "
        "files, read them, write and edit them"
        + std::string(settings.allow_run ? ", and run things" : "")
        + ". Paths are relative to the project root and cannot leave it.\n"
          "\nWhen you are asked to make or change something, do it with the tools, one step at a time, until all "
          "of what was asked is done. A file changes only through write_file or edit_file: showing its contents "
          "in your reply writes nothing. Read a file before you edit it. Check your work when you can.\n"
          "\nWhen it is all done, or when you were only asked a question, answer the person in plain words. If "
          "you need something from them, just ask.\n";
    std::error_code ec;
    if (std::filesystem::is_directory(settings.root, ec)
        && std::filesystem::directory_iterator(settings.root, ec) == std::filesystem::directory_iterator() && !ec) {
        text += "\nThe project folder is empty: there is nothing in it to list, read or run yet. Make a file only "
                "when what was asked for is something to be made.\n";
    }
    return text;
}

std::optional<ToolCall> call_from_native(std::string_view name, std::string_view arguments, std::string& why) {
    json given = json::object();
    if (!arguments.empty()) {
        given = json::parse(arguments, nullptr, false);
        if (given.is_discarded() || !given.is_object()) {
            why = "the arguments of " + std::string(name) + " were not a JSON object";
            return std::nullopt;
        }
    }
    const auto needs = [&](const char* what) {
        why = std::string(name) + " needs " + what;
        return std::nullopt;
    };

    ToolCall call;
    if (name == "list_files") {
        call.kind     = ToolKind::List;
        call.argument = path_of(given);
        if (call.argument.empty()) {
            call.argument = ".";
        }
    } else if (name == "read_file") {
        call.kind     = ToolKind::Read;
        call.argument = path_of(given);
        if (call.argument.empty()) {
            return needs("a path");
        }
        const std::optional<long> first = number_of(given, {"start_line", "start", "offset", "from"});
        const std::optional<long> last  = number_of(given, {"end_line", "end", "to"});
        if (first || last) {
            const long from = std::max(1L, first.value_or(1));
            call.argument += " " + std::to_string(from) + "-" + std::to_string(std::max(from, last.value_or(from + 399)));
        }
    } else if (name == "find_in_files") {
        call.kind     = ToolKind::Find;
        call.argument = text_of(given, {"text", "query", "pattern", "search"}).value_or("");
        if (call.argument.empty()) {
            return needs("the text to find");
        }
        if (const std::optional<std::string> files = text_of(given, {"files", "glob", "include", "path"});
            files && !files->empty() && *files != ".") {
            call.argument += " in " + *files;
        }
    } else if (name == "write_file") {
        call.kind     = ToolKind::Write;
        call.argument = path_of(given);
        const std::optional<std::string> content = text_of(given, {"content", "contents", "text", "body", "data"});
        if (call.argument.empty()) {
            return needs("a path");
        }
        if (!content) {
            return needs("the contents of the file");
        }
        call.content = *content;
    } else if (name == "edit_file") {
        call.kind     = ToolKind::Edit;
        call.argument = path_of(given);
        const std::optional<std::string> search =
            text_of(given, {"search", "old_string", "old_text", "old_str", "find"});
        const std::optional<std::string> replace =
            text_of(given, {"replace", "new_string", "new_text", "new_str", "replacement"});
        if (call.argument.empty()) {
            return needs("a path");
        }
        if (!search || search->empty()) {
            return needs("the text to replace");
        }
        if (!replace) {
            return needs("what it becomes");
        }
        const std::optional<long> line = number_of(given, {"at_line", "line", "line_number"});
        call.content = "<<<<<<< SEARCH" + (line && *line > 0 ? " line " + std::to_string(*line) : std::string())
                     + "\n" + *search + "\n=======\n" + *replace + "\n>>>>>>> REPLACE\n";
    } else if (name == "render_page") {
        call.kind = ToolKind::Render;
        const std::string page   = text_of(given, {"page", "path", "file", "input"}).value_or("");
        const std::string output = text_of(given, {"output", "out", "to", "output_path"}).value_or("");
        if (page.empty() || output.empty()) {
            return needs("a page and an output file");
        }
        call.argument = page + " to " + output;
        if (const std::optional<std::string> size = text_of(given, {"size"}); size && !size->empty()) {
            call.argument += " at " + *size;
        }
    } else if (name == "run_command") {
        call.kind     = ToolKind::Run;
        call.argument = text_of(given, {"command", "cmd", "script"}).value_or("");
        if (call.argument.empty()) {
            return needs("a command");
        }
        call.shell = text_of(given, {"shell"}).value_or("");
    } else if (name == "run_python") {
        call.kind    = ToolKind::Python;
        call.content = text_of(given, {"code", "script", "source"}).value_or("");
        if (call.content.empty()) {
            return needs("the code");
        }
    } else if (name == "git" || name == "github") {
        call.kind     = name == "git" ? ToolKind::Git : ToolKind::Gh;
        call.argument = text_of(given, {"arguments", "args", "command"}).value_or("");
        if (call.argument.empty()) {
            return needs("its arguments");
        }
    } else if (name == "start_process") {
        call.kind     = ToolKind::Start;
        call.argument = text_of(given, {"command", "cmd"}).value_or("");
        if (call.argument.empty()) {
            return needs("a command");
        }
    } else if (name == "process_logs") {
        call.kind     = ToolKind::Logs;
        call.argument = text_of(given, {"name"}).value_or("");
    } else if (name == "stop_process") {
        call.kind     = ToolKind::Stop;
        call.argument = text_of(given, {"name"}).value_or("");
        if (call.argument.empty()) {
            return needs("the process's name");
        }
    } else if (name == "fetch_url") {
        call.kind     = ToolKind::Fetch;
        call.argument = text_of(given, {"url", "address"}).value_or("");
        if (call.argument.empty()) {
            return needs("an address");
        }
    } else if (name == "web_search") {
        call.kind     = ToolKind::Search;
        call.argument = text_of(given, {"query", "q", "text"}).value_or("");
        if (call.argument.empty()) {
            return needs("a query");
        }
    } else if (name == "screenshot") {
        call.kind = ToolKind::Screenshot;
    } else if (name == "click" || name == "move_mouse") {
        const std::optional<long> x = number_of(given, {"x"});
        const std::optional<long> y = number_of(given, {"y"});
        if (!x || !y) {
            return needs("x and y");
        }
        call.kind     = name == "click" ? ToolKind::Click : ToolKind::Move;
        call.argument = std::to_string(*x) + " " + std::to_string(*y);
        if (name == "click") {
            if (const std::optional<std::string> button = text_of(given, {"button"});
                button && (*button == "right" || *button == "middle")) {
                call.argument += " " + *button;
            }
            if (given.value("double", false)) {
                call.argument += " double";
            }
        }
    } else if (name == "type_text") {
        call.kind    = ToolKind::Type;
        call.content = text_of(given, {"text", "content"}).value_or("");
        if (call.content.empty()) {
            return needs("the text");
        }
    } else if (name == "press_key") {
        call.kind     = ToolKind::Key;
        call.argument = text_of(given, {"keys", "key"}).value_or("");
        if (call.argument.empty()) {
            return needs("the keys");
        }
    } else if (name == "scroll") {
        call.kind     = ToolKind::Scroll;
        call.argument = text_of(given, {"direction"}).value_or("");
        if (call.argument.empty()) {
            return needs("a direction");
        }
        if (const std::optional<long> notches = number_of(given, {"notches", "amount"})) {
            call.argument += " " + std::to_string(*notches);
        }
    } else if (name == "use_tool") {
        call.kind     = ToolKind::Mcp;
        call.argument = text_of(given, {"tool", "name"}).value_or("");
        if (call.argument.empty()) {
            return needs("the server and the tool");
        }
        if (const auto inner = given.find("arguments"); inner != given.end()) {
            call.content = inner->is_string() ? inner->get<std::string>() : inner->dump();
        }
    } else if (name == kHandOffTool) {
        call.kind     = ToolKind::Handoff;
        call.argument = text_of(given, {"next_step", "step", "task"}).value_or("");
        if (call.argument.empty()) {
            return needs("the next step");
        }
    } else {
        why = "there is no tool called " + std::string(name);
        return std::nullopt;
    }
    return call;
}

namespace {

std::string_view trimmed(std::string_view text) {
    const auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
    while (!text.empty() && space(text.front())) {
        text.remove_prefix(1);
    }
    while (!text.empty() && space(text.back())) {
        text.remove_suffix(1);
    }
    return text;
}

/// A value between tags, without the one line break a template puts on each
/// side of it. Any others are the value's.
std::string_view unwrapped(std::string_view value) {
    if (!value.empty() && value.front() == '\n') {
        value.remove_prefix(1);
    }
    if (!value.empty() && value.back() == '\n') {
        value.remove_suffix(1);
    }
    return value;
}

/// `<open>key</close>value` pairs and the like: every `start ... end` in `text`.
std::vector<std::string_view> between(std::string_view text, std::string_view start, std::string_view end) {
    std::vector<std::string_view> found;
    for (std::size_t at = text.find(start); at != std::string_view::npos; at = text.find(start, at)) {
        at += start.size();
        const std::size_t stop = text.find(end, at);
        if (stop == std::string_view::npos) {
            break;
        }
        found.push_back(text.substr(at, stop - at));
        at = stop + end.size();
    }
    return found;
}

/// The call inside a <tool_call> block, as a name and arguments.
std::optional<ToolCall> call_in_block(std::string_view body) {
    body = trimmed(body);
    std::string why;
    // Hermes: {"name": "read_file", "arguments": {...}}
    if (!body.empty() && body.front() == '{') {
        const json given = json::parse(body, nullptr, false);
        if (given.is_discarded() || !given.is_object() || !given.contains("name")) {
            return std::nullopt;
        }
        const json arguments = given.value("arguments", json::object());
        return call_from_native(given.value("name", std::string()),
                                arguments.is_string() ? arguments.get<std::string>() : arguments.dump(), why);
    }
    // Qwen: <function=read_file><parameter=path>app.js</parameter></function>
    if (const std::size_t named = body.find("<function="); named != std::string_view::npos) {
        const std::size_t close = body.find('>', named);
        if (close == std::string_view::npos) {
            return std::nullopt;
        }
        const std::string_view name = body.substr(named + 10, close - named - 10);
        json                   arguments = json::object();
        for (std::size_t at = body.find("<parameter=", close); at != std::string_view::npos;
             at = body.find("<parameter=", at)) {
            const std::size_t key_end = body.find('>', at);
            const std::size_t end     = key_end == std::string_view::npos ? key_end : body.find("</parameter>", key_end);
            if (end == std::string_view::npos) {
                return std::nullopt;
            }
            arguments[std::string(body.substr(at + 11, key_end - at - 11))] =
                std::string(unwrapped(body.substr(key_end + 1, end - key_end - 1)));
            at = end;
        }
        return call_from_native(name, arguments.dump(), why);
    }
    // flint: read_file<arg_key>path</arg_key><arg_value>app.js</arg_value>
    const std::size_t first = body.find("<arg_key>");
    const std::string_view name = trimmed(body.substr(0, first));
    json arguments = json::object();
    if (first != std::string_view::npos) {
        const std::vector<std::string_view> keys   = between(body, "<arg_key>", "</arg_key>");
        const std::vector<std::string_view> values = between(body, "<arg_value>", "</arg_value>");
        if (keys.empty() || keys.size() != values.size()) {
            return std::nullopt;
        }
        for (std::size_t i = 0; i < keys.size(); ++i) {
            arguments[std::string(trimmed(keys[i]))] = std::string(values[i]);
        }
    }
    return call_from_native(name, arguments.dump(), why);
}

}  // namespace

std::optional<CallInText> call_in_text(std::string_view answer) {
    const std::string_view text = trimmed(answer);

    // LFM: <|tool_call_start|>[read_file(path='app.js')]<|tool_call_end|>
    constexpr std::string_view kListOpen  = "<|tool_call_start|>";
    constexpr std::string_view kListClose = "<|tool_call_end|>";
    if (const std::size_t at = text.rfind(kListOpen); at != std::string_view::npos) {
        std::string_view rest = text.substr(at);
        if (rest.size() >= kListClose.size() && rest.substr(rest.size() - kListClose.size()) == kListClose) {
            rest.remove_suffix(kListClose.size());
        }
        // Whole, and the last thing in the reply.
        if (!rest.empty() && trimmed(rest).back() == ']') {
            if (std::optional<ToolCall> call = parse_tool_call(rest, {})) {
                return CallInText{std::move(*call), std::string(trimmed(text.substr(0, at)))};
            }
        }
    }

    // Qwen, flint and Hermes: <tool_call>...</tool_call>
    constexpr std::string_view kBlockOpen  = "<tool_call>";
    constexpr std::string_view kBlockClose = "</tool_call>";
    if (const std::size_t at = text.rfind(kBlockOpen); at != std::string_view::npos
        && text.size() >= kBlockClose.size() && text.substr(text.size() - kBlockClose.size()) == kBlockClose) {
        const std::string_view body =
            text.substr(at + kBlockOpen.size(), text.size() - kBlockClose.size() - at - kBlockOpen.size());
        if (std::optional<ToolCall> call = call_in_block(body)) {
            return CallInText{std::move(*call), std::string(trimmed(text.substr(0, at)))};
        }
    }

    // gpt-oss: ...to=functions.read_file <|constrain|>json<|message|>{...}<|call|>
    constexpr std::string_view kChannel = "to=functions.";
    constexpr std::string_view kMessage = "<|message|>";
    constexpr std::string_view kCall    = "<|call|>";
    if (const std::size_t at = text.rfind(kChannel); at != std::string_view::npos) {
        const std::size_t name_at = at + kChannel.size();
        std::size_t       name_end = name_at;
        while (name_end < text.size() && (std::isalnum(static_cast<unsigned char>(text[name_end])) != 0
                                          || text[name_end] == '_')) {
            ++name_end;
        }
        const std::size_t message = text.find(kMessage, name_end);
        if (message != std::string_view::npos) {
            std::string_view arguments = text.substr(message + kMessage.size());
            if (arguments.size() >= kCall.size() && arguments.substr(arguments.size() - kCall.size()) == kCall) {
                arguments.remove_suffix(kCall.size());
            }
            std::string why;
            if (json::accept(arguments)) {
                if (std::optional<ToolCall> call = call_from_native(text.substr(name_at, name_end - name_at), arguments, why)) {
                    std::size_t start = text.rfind("<|start|>", at);
                    if (start == std::string_view::npos) {
                        start = text.rfind("<|channel|>", at);
                    }
                    return CallInText{std::move(*call), std::string(trimmed(text.substr(0, start == std::string_view::npos ? at : start)))};
                }
            }
        }
    }
    return std::nullopt;
}

namespace {

/// `code` as UTF-8, for a \u escape.
void append_utf8(std::string& out, std::uint32_t code) {
    if (code < 0x80) {
        out += static_cast<char>(code);
    } else if (code < 0x800) {
        out += static_cast<char>(0xC0 | (code >> 6));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else if (code < 0x10000) {
        out += static_cast<char>(0xE0 | (code >> 12));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (code >> 18));
        out += static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (code & 0x3F));
    }
}

/// Four hex digits at `at`, or -1 when there are not four there yet.
long hex4(std::string_view text, std::size_t at) {
    if (at + 4 > text.size()) {
        return -1;
    }
    long value = 0;
    for (std::size_t i = at; i < at + 4; ++i) {
        const char c = text[i];
        const int  digit = c >= '0' && c <= '9' ? c - '0'
                         : c >= 'a' && c <= 'f' ? c - 'a' + 10
                         : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                                : -1;
        if (digit < 0) {
            return -1;
        }
        value = value * 16 + digit;
    }
    return value;
}

/// The JSON string whose opening quote is at `at`, decoded as far as it goes,
/// and `at` moved past its closing quote. `closed` says whether it had one.
/// An escape cut off at the end is left out until the rest of it comes.
std::string read_string(std::string_view text, std::size_t& at, bool& closed) {
    std::string out;
    closed = false;
    ++at;   // the opening quote
    while (at < text.size()) {
        const char c = text[at];
        if (c == '"') {
            ++at;
            closed = true;
            return out;
        }
        if (c != '\\') {
            out += c;
            ++at;
            continue;
        }
        if (at + 1 >= text.size()) {
            break;
        }
        const char e = text[at + 1];
        if (e == 'u') {
            long code = hex4(text, at + 2);
            if (code < 0) {
                break;
            }
            std::size_t used = 6;
            // A surrogate pair is one character in two escapes.
            if (code >= 0xD800 && code < 0xDC00) {
                if (at + 12 > text.size()) {
                    break;
                }
                const long low = text[at + 6] == '\\' && text[at + 7] == 'u' ? hex4(text, at + 8) : -1;
                if (low >= 0xDC00 && low < 0xE000) {
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                    used = 12;
                }
            }
            append_utf8(out, static_cast<std::uint32_t>(code));
            at += used;
            continue;
        }
        switch (e) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            default:  out += e;    break;   // \" \\ \/
        }
        at += 2;
    }
    return out;
}

}  // namespace

std::optional<std::string> partial_argument(std::string_view text, std::string_view key) {
    std::size_t at = 0;
    const auto skip_space = [&] {
        while (at < text.size() && std::isspace(static_cast<unsigned char>(text[at])) != 0) {
            ++at;
        }
    };
    skip_space();
    if (at >= text.size() || text[at] != '{') {
        return std::nullopt;
    }
    ++at;
    for (;;) {
        skip_space();
        if (at >= text.size() || text[at] == '}') {
            return std::nullopt;
        }
        if (text[at] == ',') {
            ++at;
            continue;
        }
        if (text[at] != '"') {
            return std::nullopt;   // not the JSON a model writes
        }
        bool              closed = false;
        const std::string name   = read_string(text, at, closed);
        if (!closed) {
            return std::nullopt;
        }
        skip_space();
        if (at >= text.size() || text[at] != ':') {
            return std::nullopt;
        }
        ++at;
        skip_space();
        if (at >= text.size()) {
            return name == key ? std::optional<std::string>(std::string()) : std::nullopt;
        }
        if (text[at] == '"') {
            std::string value = read_string(text, at, closed);
            if (name == key) {
                return value;
            }
            if (!closed) {
                return std::nullopt;
            }
            continue;
        }
        // A number, a word, an object or a list: passed over to the comma
        // after it, and its text for the key wanted.
        const std::size_t start     = at;
        int               depth     = 0;
        bool              in_string = false;
        for (; at < text.size(); ++at) {
            const char c = text[at];
            if (in_string) {
                if (c == '\\') {
                    ++at;
                } else if (c == '"') {
                    in_string = false;
                }
                continue;
            }
            if (c == '"') {
                in_string = true;
            } else if (c == '{' || c == '[') {
                ++depth;
            } else if (c == '}' || c == ']') {
                if (depth == 0) {
                    break;
                }
                --depth;
            } else if (c == ',' && depth == 0) {
                break;
            }
        }
        if (name == key) {
            return std::string(trimmed(text.substr(start, at - start)));
        }
    }
}

std::optional<WritingInText> writing_in_text(std::string_view answer) {
    // The last WRITE or EDIT at the start of a line, as the lines a model is
    // told to write put them.
    std::size_t found = std::string_view::npos;
    std::string verb;
    for (const auto& [word, name] : {std::pair<std::string_view, const char*>{"WRITE:", "write"},
                                     std::pair<std::string_view, const char*>{"EDIT:", "edit"}}) {
        for (std::size_t at = answer.rfind(word); at != std::string_view::npos;
             at = at == 0 ? std::string_view::npos : answer.rfind(word, at - 1)) {
            std::size_t line = at;
            while (line > 0 && (answer[line - 1] == ' ' || answer[line - 1] == '\t')) {
                --line;
            }
            if (line == 0 || answer[line - 1] == '\n') {
                if (found == std::string_view::npos || at > found) {
                    found = at;
                    verb  = name;
                }
                break;
            }
        }
    }
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    const std::size_t colon = answer.find(':', found);
    std::size_t       end   = answer.find('\n', colon);
    WritingInText     writing;
    writing.verb = verb;
    writing.path = std::string(trimmed(answer.substr(colon + 1, (end == std::string_view::npos ? answer.size() : end) - colon - 1)));
    if (writing.path.empty() || end == std::string_view::npos) {
        return writing.path.empty() ? std::nullopt : std::optional<WritingInText>(writing);
    }
    std::string_view body = answer.substr(end + 1);
    // A WRITE's contents are in a fenced block: what is inside it, and the
    // fence that closes it says it is done.
    if (verb == "write") {
        const std::size_t fence = body.find("```");
        if (fence == std::string_view::npos) {
            return writing;
        }
        const std::size_t open_end = body.find('\n', fence);
        if (open_end == std::string_view::npos) {
            return writing;
        }
        body = body.substr(open_end + 1);
        if (const std::size_t close = body.find("\n```"); close != std::string_view::npos) {
            body         = body.substr(0, close + 1);
            writing.done = true;
        } else if (body.rfind("```", 0) == 0) {
            body         = {};
            writing.done = true;
        }
    } else {
        // An EDIT's last block has closed when its REPLACE marker comes after
        // the last SEARCH.
        const std::size_t search  = body.rfind("<<<<<<< SEARCH");
        const std::size_t replace = body.rfind(">>>>>>> REPLACE");
        writing.done = replace != std::string_view::npos && (search == std::string_view::npos || replace > search);
    }
    writing.text = std::string(body);
    return writing;
}

}  // namespace crucible::tools
