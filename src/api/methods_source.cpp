// SPDX-License-Identifier: MIT
//
// The project as a place: its files, its version control, a preview of what
// it shows, and what a build has recorded of its agents' work.
//
// What the Build view draws beside the agents. A person watching a build
// wants to click a file an agent just wrote and read it, see what changed
// since the last commit, commit or push it themselves, and see the page the
// build is making -- none of which asks anything of the engine, so nearly all
// of this is lookups, answered on a worker while the window keeps drawing.
#include "methods.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>

#include "crucible/app/setup.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/kit/kit.hpp"
#include "crucible/session/store.hpp"
#include "crucible/tools/apps.hpp"
#include "crucible/tools/attachments.hpp"
#include "crucible/tools/git.hpp"
#include "crucible/tools/preview.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

namespace crucible::api {
namespace {

/// The folder every path here is resolved inside, or the refusal.
bool project_of(const Scene& scene, std::filesystem::path& root, Reply& refusal) {
    if (scene.project.empty()) {
        refusal = bad("no project is open");
        return false;
    }
    root = scene.project;
    return true;
}

// ---------------------------------------------------------------------------
// The files
// ---------------------------------------------------------------------------

/// A fingerprint of a file's bytes: what the editor read, so a save can tell
/// whether something else -- an agent, another program -- wrote the file
/// since. FNV-1a: enough to tell two versions of a file apart, at no cost.
std::string fingerprint(std::string_view bytes) {
    std::uint64_t hash = 1469598103934665603ULL;
    for (const char c : bytes) {
        hash ^= static_cast<unsigned char>(c);
        hash *= 1099511628211ULL;
    }
    char text[17];
    std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(hash));
    return text;
}

/// The whole of a file, or nothing when it cannot be read.
std::optional<std::string> slurp(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Folders a tree does not descend into: nobody opens them to read.
bool hidden_folder(const std::string& name) {
    static const std::vector<std::string> skipped{"node_modules", ".git", "__pycache__", ".venv", "venv",
                                                  "dist", "build", "target", "out", ".cache", "DerivedData",
                                                  "Pods", ".next", ".nuxt", "coverage"};
    return std::find(skipped.begin(), skipped.end(), name) != skipped.end();
}

/// The project's files, as a flat list with depths, for the code pane.
Reply project_tree(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto where = params.value("path", std::string{});
    const std::optional<std::filesystem::path> at =
        tools::resolve_in_root(root, where.empty() ? "." : where);
    if (!at) {
        return bad(where + " is outside the project");
    }
    const int max_depth = std::clamp(params.value("depth", 4), 1, 8);
    const std::size_t limit = std::clamp<std::size_t>(params.value("limit", std::size_t{1500}), 50, 5000);

    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    json entries = json::array();
    bool cut = false;
    for (std::filesystem::recursive_directory_iterator it{*at, std::filesystem::directory_options::skip_permission_denied, ec}, end;
         it != end && !ec; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        const bool dir = it->is_directory(ec);
        if (dir && (hidden_folder(name) || it.depth() + 1 >= max_depth)) {
            it.disable_recursion_pending();
        }
        if (name.empty() || (name.front() == '.' && name != ".github")) {
            if (dir) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (entries.size() >= limit) {
            cut = true;
            break;
        }
        entries.push_back(json{{"path", std::filesystem::relative(it->path(), base, ec).generic_string()},
                               {"name", name}, {"dir", dir}, {"depth", it.depth()},
                               {"bytes", dir ? 0 : static_cast<std::uint64_t>(it->file_size(ec))}});
    }
    return good(json{{"entries", std::move(entries)}, {"cut", cut},
                     {"root", format::short_path(base)}});
}

/// One file, for reading in the code pane. Text as it is; a document as
/// its text; a picture as a note that it is one, which the page fetches
/// with attach.image.
Reply project_read(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        return bad("project.read needs a path");
    }
    const std::optional<std::filesystem::path> file = tools::resolve_in_root(root, path);
    if (!file) {
        return bad(path + " is outside the project");
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*file, ec)) {
        return bad(path + " is not a file");
    }
    const std::uintmax_t bytes = std::filesystem::file_size(*file, ec);
    const attach::Kind kind = attach::kind_of(*file);
    json out{{"path", path}, {"bytes", bytes}, {"kind", attach::kind_name(kind)},
             {"language", file->extension().string().size() > 1 ? file->extension().string().substr(1) : std::string()}};
    if (kind == attach::Kind::Image) {
        out["image"] = true;
        out["full"]  = file->string();
        return good(std::move(out));
    }
    const std::size_t max_chars = std::clamp<std::size_t>(params.value("max", std::size_t{400000}), 1000, 4000000);
    if (kind == attach::Kind::Document && !attach::is_markup_source(*file)) {
        const attach::Text text = attach::read(*file, max_chars);
        out["content"] = text.body;
        out["cut"]     = text.cut;
        out["note"]    = text.note;
        return good(std::move(out));
    }
    std::optional<std::string> content = slurp(*file);
    if (!content) {
        return bad("could not read " + path);
    }
    const bool cut = content->size() > max_chars;
    // Editable where what the editor shows is the file itself: text, whole,
    // and valid UTF-8 (anything else would be saved back changed). A PDF's
    // words or a picture are something to look at, never to write back.
    const bool text = kind == attach::Kind::Text
                   || (kind == attach::Kind::Document && attach::is_markup_source(*file));
    out["hash"] = fingerprint(*content);
    if (cut) {
        content->resize(max_chars);
    }
    std::string shown = crucible::detail::scrub_utf8(*content);
    out["editable"] = text && !cut && shown == *content;
    out["content"]  = std::move(shown);
    out["cut"]      = cut;
    return good(std::move(out));
}

/// Write one file from the window: the code pane's editor. An action, so it
/// is on the session's thread, in order with whatever the engine is told.
/// Save a file from the editor.
///
/// Allowed while a build runs: an editor that locks for the length of a
/// build is not one. What guards against an agent's write is the fingerprint
/// the editor read: when the file on disk is no longer that, the save is
/// refused with "changed on disk" and the window asks before overwriting
/// (`force`). Saving does not wait for anybody; it only refuses to lose work
/// silently.
Reply project_write(const json& params, Host& host) {
    const std::filesystem::path root = host.project_root();
    if (root.empty()) {
        return bad("no project is open");
    }
    const auto path = params.value("path", std::string{});
    if (path.empty() || !params.contains("content") || !params["content"].is_string()) {
        return bad("project.write needs a path and content");
    }
    const std::optional<std::filesystem::path> file = tools::resolve_in_root(root, path);
    if (!file) {
        return bad(path + " is outside the project");
    }
    std::error_code ec;
    const std::string expected = params.value("expected", std::string{});
    if (!expected.empty() && !params.value("force", false) && std::filesystem::exists(*file, ec)) {
        const std::optional<std::string> now = slurp(*file);
        if (now && fingerprint(*now) != expected) {
            return bad("changed on disk");
        }
    }
    std::filesystem::create_directories(file->parent_path(), ec);
    const std::string content = params["content"].get<std::string>();
    {
        std::ofstream out(*file, std::ios::binary | std::ios::trunc);
        if (!out) {
            return bad("could not write " + path);
        }
        out << content;
        if (!out) {
            return bad("could not write all of " + path);
        }
    }
    return good(json{{"path", path}, {"hash", fingerprint(content)}});
}

/// A new file or folder in the project, empty. Refused where something is
/// already, so a name typed twice never empties a file.
Reply project_create(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto path = format::trim(params.value("path", std::string{}));
    const std::optional<std::filesystem::path> at = tools::resolve_in_root(root, path);
    if (path.empty() || !at) {
        return bad(path.empty() ? "it needs a name" : path + " is outside the project");
    }
    std::error_code ec;
    if (std::filesystem::exists(*at, ec)) {
        return bad(path + " is already there");
    }
    if (params.value("dir", false)) {
        std::filesystem::create_directories(*at, ec);
        return ec ? bad("could not make " + path + ": " + ec.message()) : good(json{{"path", path}});
    }
    std::filesystem::create_directories(at->parent_path(), ec);
    std::ofstream out(*at, std::ios::binary);
    if (!out) {
        return bad("could not make " + path);
    }
    return good(json{{"path", path}, {"hash", fingerprint("")}});
}

/// Rename or move a file or folder within the project.
/// The entry `relative` names in the project, itself: a link is the link,
/// not what it points at. Its folder is resolved the way every path is, so
/// no link on the way leads out of the project; only the last part is taken
/// as written. resolve_in_root follows that one too, which is right for
/// reading a file and wrong for deleting one: through a link to a shared
/// folder, Delete removed the folder.
std::optional<std::filesystem::path> entry_in_root(const std::filesystem::path& root, const std::string& relative) {
    std::filesystem::path lexical = std::filesystem::path(relative).lexically_normal();
    if (!lexical.has_filename()) {
        lexical = lexical.parent_path();   // "src/" is src
    }
    const std::filesystem::path name = lexical.filename();
    if (relative.empty() || lexical.is_absolute() || name.empty() || name == "." || name == "..") {
        return std::nullopt;
    }
    const std::filesystem::path folder = lexical.parent_path();
    const std::optional<std::filesystem::path> in =
        tools::resolve_in_root(root, folder.empty() ? std::string(".") : folder.string());
    if (!in) {
        return std::nullopt;
    }
    return *in / name;
}

/// Whether `relative` is in git's own folder, in any case: on Windows and a
/// Mac ".GIT" is the same folder.
bool in_git_folder(const std::string& relative) {
    for (const auto& part : std::filesystem::path(relative).lexically_normal()) {
        std::string word = part.string();
        std::transform(word.begin(), word.end(), word.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (word == ".git") {
            return true;
        }
    }
    return false;
}

/// Whether there is an entry at `path`, a link that leads nowhere included.
bool entry_exists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::symlink_status(path, ec).type() != std::filesystem::file_type::not_found && !ec;
}

Reply project_rename(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto from = params.value("path", std::string{});
    const auto to   = format::trim(params.value("to", std::string{}));
    if (to.empty()) {
        return bad("it needs a name");
    }
    if (in_git_folder(from) || in_git_folder(to)) {
        return bad("git's own folder is not changed from here");
    }
    const std::optional<std::filesystem::path> a = entry_in_root(root, from);
    const std::optional<std::filesystem::path> b = entry_in_root(root, to);
    if (!a || !b) {
        return bad("both have to be in the project");
    }
    if (!entry_exists(*a)) {
        return bad(from + " is not there");
    }
    std::error_code ec;
    if (entry_exists(*b)) {
        // On Windows and a Mac readme.md and README.md are one file: a
        // change of case is a rename to itself, and allowed. Anything else
        // there already is somebody else's, and kept.
        const auto lower = [](std::string text) {
            std::transform(text.begin(), text.end(), text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        };
        const bool same_file = lower(a->string()) == lower(b->string()) && *a != *b
                            && std::filesystem::equivalent(*a, *b, ec);
        if (!same_file) {
            return bad(to + " is already there");
        }
    }
    std::filesystem::create_directories(b->parent_path(), ec);
    std::filesystem::rename(*a, *b, ec);
    return ec ? bad("could not rename it: " + ec.message()) : good(json{{"path", to}});
}

/// Delete a file, or a folder and everything in it. Never the project
/// itself, and never its git history. A link is removed as a link: what it
/// points at stays where it is.
Reply project_delete(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto path = params.value("path", std::string{});
    if (in_git_folder(path)) {
        return bad("git's own folder is not deleted from here");
    }
    const std::optional<std::filesystem::path> at = entry_in_root(root, path);
    std::error_code ec;
    if (!at) {
        return bad("that is not something in the project to delete");
    }
    if (!entry_exists(*at)) {
        return bad(path + " is not there");
    }
    // remove_all takes a link away without following it into its target.
    std::filesystem::remove_all(*at, ec);
    return ec ? bad("could not delete it: " + ec.message()) : good(json{{"path", path}});
}

/// Every line in the project with `query` in it: the editor's search across
/// files. Text files only, the folders a tree skips skipped, and a cap on
/// both the files read and the lines found so a search for "e" answers.
Reply project_search(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto query = params.value("query", std::string{});
    if (query.empty()) {
        return good(json{{"results", json::array()}, {"cut", false}});
    }
    const bool exact_case = params.value("case", false);
    const auto lower = [](std::string text) {
        std::transform(text.begin(), text.end(), text.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return text;
    };
    const std::string wanted = exact_case ? query : lower(query);

    constexpr std::size_t kMaxResults = 400;
    constexpr std::size_t kMaxFiles   = 4000;
    constexpr std::uintmax_t kMaxBytes = 2u << 20;
    json results = json::array();
    bool cut     = false;
    std::size_t files = 0;
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::weakly_canonical(root, ec);
    for (std::filesystem::recursive_directory_iterator it{root, std::filesystem::directory_options::skip_permission_denied, ec}, end;
         it != end && !ec && !cut; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec)) {
            if (hidden_folder(name) || (!name.empty() && name.front() == '.' && name != ".github")) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!it->is_regular_file(ec) || it->file_size(ec) > kMaxBytes
            || attach::kind_of(it->path()) != attach::Kind::Text) {
            continue;
        }
        if (++files > kMaxFiles) {
            cut = true;
            break;
        }
        std::ifstream in(it->path(), std::ios::binary);
        std::string   line;
        int           number = 0;
        while (std::getline(in, line)) {
            ++number;
            const std::string seen = exact_case ? line : lower(line);
            const std::size_t at   = seen.find(wanted);
            if (at == std::string::npos) {
                continue;
            }
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }
            if (line.size() > 240) {
                const std::size_t from = at > 80 ? at - 80 : 0;
                line = line.substr(from, 240);
            }
            results.push_back(json{{"path", std::filesystem::relative(it->path(), base, ec).generic_string()},
                                   {"line", number}, {"text", crucible::detail::scrub_utf8(line)}});
            if (results.size() >= kMaxResults) {
                cut = true;
                break;
            }
        }
    }
    return good(json{{"results", std::move(results)}, {"cut", cut}});
}

/// Run one command in the project, from the window: a Run button, a Ship
/// step. A lookup, so the window keeps drawing while it runs, bounded by the
/// command timeout; the engine is not involved and nothing of its is touched.
Reply project_run(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto command = format::trim(params.value("command", std::string{}));
    if (command.empty()) {
        return bad("project.run needs a command");
    }
    tools::WorkshopSettings workshop;
    workshop.enabled             = true;
    workshop.root                = root;
    workshop.allow_run           = true;
    workshop.run_timeout_seconds = std::clamp(params.value("timeout", scene.config.tools.workshop_timeout), 5, 3600);
    workshop.max_output_bytes    = 60000;
    tools::ToolCall call;
    call.kind     = tools::ToolKind::Run;
    call.argument = command;
    call.shell    = params.value("shell", std::string{});
    const tools::ToolResult result = tools::run_tool(call, workshop, tools::SearchSettings{}, {});
    return good(json{{"ok", result.ok}, {"summary", result.summary}, {"output", result.detail}});
}

// ---------------------------------------------------------------------------
// Version control
// ---------------------------------------------------------------------------

Reply git_ready(const json&, const Scene& scene) {
    const bool git = tools::git::available();
    return good(json{{"git", git}, {"gh", tools::git::gh_available()},
                     {"repo", git && !scene.project.empty() && tools::git::is_repo(scene.project)}});
}

Reply git_status(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    if (!tools::git::available()) {
        return good(json{{"git", false}, {"repo", false}, {"changes", json::array()}});
    }
    const tools::git::Status status = tools::git::status(root);
    json changes = json::array();
    for (const tools::git::Change& change : status.changes) {
        changes.push_back(json{{"path", change.path}, {"status", change.status}, {"staged", change.staged}});
    }
    return good(json{{"git", true}, {"repo", status.repo}, {"branch", status.branch},
                     {"remote", status.remote}, {"ahead", status.ahead}, {"behind", status.behind},
                     {"changes", std::move(changes)}, {"error", status.error},
                     {"gh", tools::git::gh_available()}});
}

/// The project's latest CI runs on GitHub, when it is there and gh is here.
Reply git_ci(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    if (!tools::git::gh_available()) {
        return good(json{{"available", false}, {"why", "gh, GitHub's command line, is not installed"}});
    }
    const tools::git::Status status = tools::git::status(root);
    if (status.remote.find("github.com") == std::string::npos) {
        return good(json{{"available", false}, {"why", "the project is not on GitHub"}});
    }
    const tools::git::Output out = tools::git::run_gh(
        root, {"run", "list", "--limit", "6", "--json",
               "databaseId,name,displayTitle,status,conclusion,headBranch,event,createdAt,url"}, 30);
    if (!out.ok) {
        return good(json{{"available", false}, {"why", format::trim(out.text)}});
    }
    const json runs = json::parse(out.text, nullptr, false);
    return good(json{{"available", true}, {"runs", runs.is_array() ? runs : json::array()}});
}

Reply git_diff(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    std::string diff = tools::git::diff(root, params.value("path", std::string{}));
    constexpr std::size_t kMost = 400000;
    const bool cut = diff.size() > kMost;
    if (cut) {
        diff.resize(kMost);
    }
    return good(json{{"diff", crucible::detail::scrub_utf8(diff)}, {"cut", cut}});
}

Reply git_log(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    json commits = json::array();
    for (const tools::git::Commit& commit : tools::git::log(root, std::clamp(params.value("limit", 30), 1, 200))) {
        commits.push_back(json{{"hash", commit.hash}, {"subject", commit.subject},
                               {"author", commit.author}, {"when", commit.when}});
    }
    return good(json{{"commits", std::move(commits)}});
}

Reply git_init(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const std::string error = tools::git::init(root);
    return error.empty() ? good() : bad(error);
}

Reply git_commit(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    std::string summary;
    const std::string error = tools::git::commit(root, params.value("message", std::string{}), summary);
    return error.empty() ? good(json{{"summary", summary}}) : bad(error);
}

Reply git_push(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const std::string error = tools::git::push(root);
    return error.empty() ? good() : bad(error);
}

Reply git_pull(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const std::string error = tools::git::pull(root);
    return error.empty() ? good() : bad(error);
}

Reply git_publish(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    std::string name = format::trim(params.value("name", std::string{}));
    if (name.empty()) {
        name = root.filename().string();
    }
    std::string url;
    const std::string error = tools::git::publish(root, name, params.value("private", true), url);
    return error.empty() ? good(json{{"url", url}}) : bad(error);
}

Reply git_release(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    std::string url;
    const std::string error = tools::git::release(root, params.value("tag", std::string{}),
                                                  params.value("notes", std::string{}), url);
    return error.empty() ? good(json{{"url", url}}) : bad(error);
}

/// Any git command, from the window's own box: the arguments as a person
/// would type them after "git ".
Reply git_run(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const std::vector<std::string> args = tools::git::split_arguments(params.value("args", std::string{}));
    if (args.empty()) {
        return bad("git.run needs arguments");
    }
    const bool gh = params.value("gh", false);
    const tools::git::Output out = gh ? tools::git::run_gh(root, args, 600) : tools::git::run(root, args, 600);
    if (!out.error.empty()) {
        return bad(out.error);
    }
    return good(json{{"ok", out.ok}, {"status", out.status}, {"output", crucible::detail::scrub_utf8(out.text)}});
}

// ---------------------------------------------------------------------------
// The preview
// ---------------------------------------------------------------------------

Reply preview_candidates(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    return good(json{{"pages", tools::preview::candidates(root)}});
}

Reply preview_page(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        return bad("preview.page needs a path");
    }
    const tools::preview::Bundle bundle = tools::preview::bundle(root, path);
    if (!bundle.ok) {
        return bad(bundle.error);
    }
    return good(json{{"path", path}, {"html", bundle.html}, {"inlined", bundle.inlined},
                     {"skipped", bundle.skipped}});
}

// ---------------------------------------------------------------------------
// Making the project's page a program
// ---------------------------------------------------------------------------

/// What the app would be called, and whether this Crucible can make one.
Reply app_name(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto page = params.value("page", std::string("index.html"));
    return good(json{{"name", tools::apps::name_for(root, page)},
                     {"can", !tools::apps::runner().empty()}});
}

/// Package the page as a program in the person's own Applications, Start
/// menu or launcher. Answered off the session's thread like a lookup: it
/// copies a program and, on a Mac, signs it, which is a second the window
/// should not spend frozen.
Reply app_package(const json& params, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const auto page = params.value("page", std::string("index.html"));
    if (!tools::resolve_in_root(root, page)) {
        return bad(page + " is not in the project");
    }
    std::string error;
    const std::optional<tools::apps::Made> made =
        tools::apps::package(root, page, params.value("name", std::string()), error);
    if (!made) {
        return bad(error);
    }
    return good(json{{"name", made->name}, {"program", made->program.string()},
                     {"launch", made->launch.string()}, {"where", made->where}});
}

/// Open a program Crucible made.
Reply app_open(const json& params, Host&) {
    const auto launch = params.value("launch", std::string());
    if (launch.empty()) {
        return bad("which program?");
    }
    std::string error;
    if (!tools::apps::open(launch, error)) {
        return bad(error);
    }
    return good();
}

// ---------------------------------------------------------------------------
// The kit: the programs Crucible fetches for its experts
// ---------------------------------------------------------------------------

/// Each piece, whether it is in the kit, and whether this machine wants it.
Reply kit_status(const json&, const Scene&) {
    std::vector<std::string> wanted;
    for (const kit::Piece& piece : kit::missing()) {
        wanted.push_back(piece.id);
    }
    json pieces = json::array();
    for (const kit::Piece& piece : kit::pieces()) {
        const bool missing = std::find(wanted.begin(), wanted.end(), piece.id) != wanted.end();
        const bool fetched = kit::installed(piece.id);
        // Not fetched and not missing: the machine has its own, or this
        // platform has no use for it -- OCR on a Mac, MinGit off Windows.
        pieces.push_back(json{{"id", piece.id}, {"label", piece.label}, {"why", piece.why},
                              {"bytes", piece.bytes}, {"fetched", fetched}, {"missing", missing}});
    }
    return good(json{{"pieces", pieces}, {"folder", kit::dir().string()}});
}

/// Fetch what is missing, through the setup the window shows progress for.
Reply kit_fetch(const json&, Host& host) {
    Setup* setup = host.setup();
    if (setup == nullptr) {
        return bad("this Crucible has no setup to fetch with");
    }
    if (setup->running()) {
        return bad("already fetching: see the card at the top of the chat");
    }
    setup->start();
    return good();
}

/// How each MCP server is doing: running, how many tools, why not.
Reply mcp_status(const json&, Host& host) {
    if (host.engine() == nullptr) {
        return good(json{{"servers", json::array()}});
    }
    json out = json::array();
    for (const tools::mcp::Status& one : host.engine()->mcp().status()) {
        out.push_back(json{{"name", one.name}, {"running", one.running}, {"tools", one.tools},
                           {"error", one.error}});
    }
    return good(json{{"servers", out}});
}

// ---------------------------------------------------------------------------
// Processes, and what a build recorded
// ---------------------------------------------------------------------------

Reply processes(const json&, Host& host) {
    if (host.engine() == nullptr) {
        return good(json{{"processes", json::array()}});
    }
    json out = json::array();
    for (const tools::ProcessInfo& one : host.engine()->processes().list()) {
        out.push_back(json{{"name", one.name}, {"command", one.command}, {"running", one.running},
                           {"status", one.status}, {"seconds", one.seconds}});
    }
    return good(json{{"processes", std::move(out)}});
}

Reply process_stop(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return no_engine();
    }
    std::string error;
    if (!host.engine()->processes().stop(params.value("name", std::string{}), error)) {
        return bad(error);
    }
    return good();
}

/// What builds have kept of their agents' work, per expert: how many
/// records, and the file, which Create can take as a dataset.
Reply teach_list(const json&, const Scene& scene) {
    std::filesystem::path root;
    Reply refusal;
    if (!project_of(scene, root, refusal)) {
        return refusal;
    }
    const std::filesystem::path dir = Project::at(root).dir / "teach";
    json out = json::array();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return good(json{{"experts", out}});
    }
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() != ".jsonl") {
            continue;
        }
        std::ifstream in(entry.path());
        std::size_t records = 0;
        std::string line;
        while (std::getline(in, line)) {
            records += line.empty() ? 0 : 1;
        }
        const std::string id = entry.path().stem().string();
        const std::optional<std::size_t> seat = scene.config.roster.find(id);
        out.push_back(json{{"expert", id},
                           {"name", seat ? scene.config.roster.at(*seat).name : id},
                           {"records", records},
                           {"path", entry.path().string()},
                           {"bytes", static_cast<std::uint64_t>(entry.file_size(ec))}});
    }
    return good(json{{"experts", std::move(out)}});
}

}  // namespace

void source_methods(std::vector<Method>& table) {
    table.push_back({"project.tree",       project_tree, nullptr});
    table.push_back({"project.read",       project_read, nullptr});
    table.push_back({"project.write",      nullptr, project_write});
    table.push_back({"project.create",     project_create, nullptr});
    table.push_back({"project.rename",     project_rename, nullptr});
    table.push_back({"project.delete",     project_delete, nullptr});
    table.push_back({"project.search",     project_search, nullptr});
    table.push_back({"project.run",        project_run, nullptr});
    table.push_back({"git.ready",          git_ready, nullptr});
    table.push_back({"git.status",         git_status, nullptr});
    table.push_back({"git.diff",           git_diff, nullptr});
    table.push_back({"git.log",            git_log, nullptr});
    table.push_back({"git.init",           git_init, nullptr});
    table.push_back({"git.commit",         git_commit, nullptr});
    table.push_back({"git.push",           git_push, nullptr});
    table.push_back({"git.pull",           git_pull, nullptr});
    table.push_back({"git.publish",        git_publish, nullptr});
    table.push_back({"git.release",        git_release, nullptr});
    table.push_back({"git.run",            git_run, nullptr});
    table.push_back({"git.ci",             git_ci, nullptr});
    table.push_back({"preview.candidates", preview_candidates, nullptr});
    table.push_back({"preview.page",       preview_page, nullptr});
    table.push_back({"app.name",           app_name, nullptr});
    table.push_back({"app.package",        app_package, nullptr});
    table.push_back({"app.open",           nullptr, app_open});
    table.push_back({"kit.status",         kit_status, nullptr});
    table.push_back({"kit.fetch",          nullptr, kit_fetch});
    table.push_back({"mcp.status",         nullptr, mcp_status});
    table.push_back({"processes",          nullptr, processes});
    table.push_back({"process.stop",       nullptr, process_stop});
    table.push_back({"teach.list",         teach_list, nullptr});
}

}  // namespace crucible::api
