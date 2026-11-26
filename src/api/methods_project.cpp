// SPDX-License-Identifier: MIT
//
// Which folder we are working in, and whether we may.
#include "methods.hpp"

#include <algorithm>
#include <fstream>

#include <miniz.h>

#include "crucible/config/paths.hpp"
#include "crucible/session/shelf.hpp"
#include "crucible/session/store.hpp"
#include "crucible/tools/attachments.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

namespace crucible::api {
namespace {

Reply project(const json&, Host& host) {
    const std::filesystem::path root   = host.project_root();
    const std::filesystem::path asking = host.pending_trust();
    json out{{"open", !root.empty()},
             {"root", root.string()},
             {"name", root.empty() ? "" : root.filename().string()}};
    // Reported with the project rather than as its own method, because "which
    // folder are we in" and "which folder is waiting to be allowed" are one
    // question asked at one moment.
    if (!asking.empty()) {
        out["pending_trust"] = asking.string();
    }
    return good(std::move(out));
}

Reply project_open(const json& params, Host& host) {
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        return bad("project.open needs a path");
    }
    const std::string error = host.open_project(paths::expand_user(path));
    return error.empty() ? good() : bad(error);
}

/// Off the recent projects. The folder is left exactly as it is. An action,
/// not a lookup: opening a project writes the same list, on this thread.
Reply project_forget(const json& params, Host&) {
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        return bad("project.forget needs a path");
    }
    forget_project(paths::expand_user(path));
    return good();
}

Reply trust_answer(const json& params, Host& host) {
    host.answer_trust(params.value("trusted", false));
    return good();
}

// ---------------------------------------------------------------------------
// The shelf: what the experts made in the conversation on screen
// ---------------------------------------------------------------------------

/// The shelf as the page draws it: the conversation's build, when there is
/// code in it or it is a project somebody opened, and every other thing
/// made, newest first. A file deleted since is left out.
Reply shelf_list(const json&, Host& host) {
    const std::filesystem::path root    = host.project_root();
    const bool                  project = !root.empty() && !is_scratch(root);
    json                        items   = json::array();
    std::size_t                 code    = 0;
    std::int64_t                updated = 0;
    std::string                 page;
    for (const shelf::Entry& entry : host.shelf_entries()) {
        const std::filesystem::path file = shelf::inside(root, entry.path);
        std::error_code             ec;
        if (file.empty() || !std::filesystem::is_regular_file(file, ec)) {
            continue;
        }
        if (shelf::is_code(file)) {
            ++code;
            updated = std::max(updated, entry.added);
            const std::string ext = file.extension().string();
            if (ext == ".html" || ext == ".htm") {
                page = entry.path;
            }
            continue;
        }
        items.push_back(json{{"path", entry.path}, {"name", file.filename().string()},
                             {"kind", shelf::kind_of(file)}, {"bytes", std::filesystem::file_size(file, ec)},
                             {"added", entry.added}});
    }
    std::reverse(items.begin(), items.end());
    json build = nullptr;
    if (code > 0 || project) {
        const std::string named = host.session_name();
        // Named as the project, or as the chat once it has a name.
        build = json{{"name", project ? Project::at(root).name : named},
                     {"files", code}, {"updated", updated}, {"project", project}};
        if (!page.empty()) {
            build["page"] = page;
        }
    }
    return good(json{{"root", format::short_path(root)}, {"build", build}, {"items", std::move(items)}});
}

/// Delete one thing on the shelf, or with `build`, the code the experts
/// wrote in a conversation with no project: the file goes with its entry.
Reply shelf_remove(const json& params, Host& host) {
    if (params.value("build", false)) {
        const std::filesystem::path root = host.project_root();
        if (root.empty() || !is_scratch(root)) {
            return bad("a project's code stays: delete files from it in Code");
        }
        for (const shelf::Entry& entry : host.shelf_entries()) {
            if (shelf::is_code(entry.path)) {
                if (const std::string error = host.shelf_remove(entry.path); !error.empty()) {
                    return bad(error);
                }
            }
        }
        return good();
    }
    const std::string error = host.shelf_remove(params.value("path", std::string{}));
    return error.empty() ? good() : bad(error);
}

/// Copy one thing on the shelf into the downloads folder, under a name not
/// taken there, as a browser would.
Reply shelf_download(const json& params, Host& host) {
    const auto path = params.value("path", std::string{});
    const std::vector<shelf::Entry> entries = host.shelf_entries();
    if (std::none_of(entries.begin(), entries.end(), [&path](const shelf::Entry& e) { return e.path == path; })) {
        return bad(path + " is not on the shelf");
    }
    const std::filesystem::path file = shelf::inside(host.project_root(), path);
    std::error_code             ec;
    if (file.empty() || !std::filesystem::is_regular_file(file, ec)) {
        return bad(path + " is not there any more");
    }
    const std::filesystem::path target = shelf::download_target(file.filename().string());
    std::filesystem::create_directories(target.parent_path(), ec);
    if (!std::filesystem::copy_file(file, target, ec) || ec) {
        return bad("could not save it to " + format::short_path(target.parent_path())
                   + (ec ? ": " + ec.message() : std::string()));
    }
    return good(json{{"saved", format::short_path(target)}});
}

// --- looking at one, and keeping the build ---------------------------------------
//
// Lookups, on a worker: a document's words take a moment to read out, and an
// archive of a build longer. They are handed the project and no more, so
// what they reach is what project.read does, the files inside it.

/// The most of a file's own text the viewer is sent, and of a document's
/// words: what a person reads in a pane, many times over.
constexpr std::size_t kViewText = std::size_t{1} << 20;

/// The largest thing the viewer draws from its bytes. Past it, a film is
/// downloaded and watched in a program made for it.
constexpr std::uintmax_t kViewBytes = std::uintmax_t{256} << 20;

/// How much of it each shelf.bytes sends: a few megabytes, so no one reply
/// holds the window up, and few enough of them for a film.
constexpr std::size_t kBytesPiece = std::size_t{4} << 20;

/// What is in an archive the shelf can open: zip, and the formats that are
/// zip underneath.
bool zip_family(const std::filesystem::path& file) {
    const std::string ext = format::to_lower(file.extension().string());
    for (const char* one : {".zip", ".jar", ".whl", ".apk", ".xpi", ".epub", ".nupkg", ".vsix"}) {
        if (ext == one) {
            return true;
        }
    }
    return false;
}

/// The file `path` names inside the project, or why there is none.
bool shelf_file(const json& params, const Scene& scene, std::filesystem::path& file, Reply& refusal) {
    const auto path = params.value("path", std::string{});
    if (path.empty()) {
        refusal = bad("it needs a path");
        return false;
    }
    file = shelf::inside(scene.project, path);
    std::error_code ec;
    if (scene.project.empty() || file.empty() || !std::filesystem::is_regular_file(file, ec)) {
        refusal = bad(path + " is not there any more");
        return false;
    }
    return true;
}

/// One thing on the shelf, as the viewer shows it (ui/panes.js), by `show`:
/// "pdf", "image", "audio" and "video" are drawn by the window from their
/// bytes, which shelf.bytes sends; "text" and "document" come with their
/// words; "archive" with what is in it; "none" is only what it is. With
/// `text`, a document's words whatever it is: a PDF's, read out of it.
Reply shelf_view(const json& params, const Scene& scene) {
    std::filesystem::path file;
    Reply                 refusal;
    if (!shelf_file(params, scene, file, refusal)) {
        return refusal;
    }
    std::error_code      ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    json out{{"path", params.value("path", std::string{})}, {"name", file.filename().string()},
             {"kind", shelf::kind_of(file)}, {"bytes", size}, {"show", "none"}};
    const auto words = [&out](const attach::Text& text) {
        out["text"] = text.body;
        out["cut"]  = text.cut;
        if (!text.note.empty()) {
            out["note"] = text.note;
        }
    };
    if (params.value("text", false)) {
        out["show"] = "document";
        words(attach::read(file, kViewText));
        return good(std::move(out));
    }
    if (const std::string type = shelf::media_type(file); !type.empty()) {
        if (size > kViewBytes) {
            out["note"] = "too large to show here";
            return good(std::move(out));
        }
        out["mime"] = type;
        out["show"] = type == "application/pdf" ? std::string("pdf") : type.substr(0, type.find('/'));
        return good(std::move(out));
    }
    if (zip_family(file)) {
        json         entries = json::array();
        mz_zip_archive zip{};
        if (mz_zip_reader_init_file(&zip, file.string().c_str(), 0) != 0) {
            // Its files, the first two thousand of them listed and all counted.
            std::size_t   files = 0;
            const mz_uint count = mz_zip_reader_get_num_files(&zip);
            for (mz_uint i = 0; i < count; ++i) {
                if (mz_zip_reader_is_file_a_directory(&zip, i) != 0) {
                    continue;
                }
                ++files;
                mz_zip_archive_file_stat stat;
                if (entries.size() < 2000 && mz_zip_reader_file_stat(&zip, i, &stat) != 0) {
                    entries.push_back(json{{"name", crucible::detail::scrub_utf8(stat.m_filename)},
                                           {"bytes", stat.m_uncomp_size}});
                }
            }
            out["count"] = files;
            mz_zip_reader_end(&zip);
            out["show"]    = "archive";
            out["entries"] = std::move(entries);
        }
        return good(std::move(out));
    }
    const attach::Kind kind = attach::kind_of(file);
    if (kind == attach::Kind::Document && !attach::is_markup_source(file)) {
        const attach::Text text = attach::read(file, kViewText);
        if (!text.body.empty()) {
            out["show"] = "document";
        }
        words(text);
        return good(std::move(out));
    }
    // Its own text, when it is text: no NUL in the first of it, which no
    // text has and nearly every other file does.
    std::ifstream in(file, std::ios::binary);
    std::string   body(static_cast<std::size_t>(std::min<std::uintmax_t>(size, kViewText)), '\0');
    in.read(body.data(), static_cast<std::streamsize>(body.size()));
    body.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    if (body.substr(0, 8192).find('\0') != std::string::npos) {
        return good(std::move(out));
    }
    const std::string ext = format::to_lower(file.extension().string());
    out["show"]     = "text";
    out["language"] = ext.size() > 1 ? ext.substr(1) : std::string();
    out["cut"]      = size > body.size();
    out["text"]     = crucible::detail::scrub_utf8(body);
    return good(std::move(out));
}

/// A piece of one thing on the shelf, from `at`, as base64: what the viewer
/// puts a page, a picture or a film together from.
Reply shelf_bytes(const json& params, const Scene& scene) {
    std::filesystem::path file;
    Reply                 refusal;
    if (!shelf_file(params, scene, file, refusal)) {
        return refusal;
    }
    std::error_code      ec;
    const std::uintmax_t size = std::filesystem::file_size(file, ec);
    if (ec || size > kViewBytes) {
        return bad("too large to show here");
    }
    const std::uintmax_t at = std::min<std::uintmax_t>(params.value("at", std::uintmax_t{0}), size);
    std::ifstream        in(file, std::ios::binary);
    std::string          piece(static_cast<std::size_t>(std::min<std::uintmax_t>(size - at, kBytesPiece)), '\0');
    in.seekg(static_cast<std::streamoff>(at));
    in.read(piece.data(), static_cast<std::streamsize>(piece.size()));
    if (!in && !in.eof()) {
        return bad("could not read " + file.filename().string());
    }
    piece.resize(static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0)));
    const std::uintmax_t next = at + piece.size();
    return good(json{{"data", format::base64(piece)}, {"at", at}, {"next", next}, {"total", size},
                     {"done", next >= size || piece.empty()}});
}

/// The build as one archive in the downloads folder, named `name` (the
/// build's, as the shelf shows it), under a name not taken there.
Reply shelf_archive(const json& params, const Scene& scene) {
    if (scene.project.empty()) {
        return bad("there is no build here");
    }
    const std::filesystem::path target =
        shelf::download_target(shelf::archive_name(format::trim(params.value("name", std::string{}))));
    if (const std::string error = shelf::save_archive(scene.project, target); !error.empty()) {
        return bad("could not save it: " + error);
    }
    std::error_code ec;
    return good(json{{"saved", format::short_path(target)}, {"bytes", std::filesystem::file_size(target, ec)}});
}

Reply trust_clear(const json&, Host& host) {
    if (const std::string error = host.clear_trust(); !error.empty()) {
        return bad(error);
    }
    return good();
}

Reply projects(const json&, const Scene&) {
    json out = json::array();
    for (const Project& one : recent_projects()) {
        out.push_back(json{{"root", one.root.string()}, {"name", one.name}});
    }
    return good(std::move(out));
}

/// One directory's worth of the picker Crucible draws itself.
///
/// The window asks the platform for its own file dialog first, and this is
/// what is used when there is none to ask -- a build with no dialog support,
/// a desktop with no portal. Hidden entries are left out, which is what every
/// file dialog does and what makes a home directory readable.
///
/// `files` asks for files as well as folders, and `extensions` narrows them:
/// picking a model wants `.gguf` and nothing else in the way.
Reply browse(const json& params, const Scene&) {
    const auto where = params.value("path", std::string{});
    const std::filesystem::path at = paths::expand_user(where.empty() ? "~" : where);
    std::error_code ec;
    if (!std::filesystem::is_directory(at, ec)) {
        return bad(at.string() + " is not a directory");
    }

    const bool with_files = params.value("files", false);
    std::vector<std::string> wanted;
    if (params.contains("extensions") && params["extensions"].is_array()) {
        for (const json& one : params["extensions"]) {
            if (one.is_string()) {
                wanted.push_back(format::to_lower(one.get<std::string>()));
            }
        }
    }

    std::vector<std::string> folders;
    json                     files = json::array();
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(
             at, std::filesystem::directory_options::skip_permission_denied, ec)) {
        std::error_code   each;
        const std::string name = entry.path().filename().string();
        if (name.empty() || name.front() == '.') {
            continue;
        }
        if (entry.is_directory(each)) {
            folders.push_back(name);
        } else if (with_files && entry.is_regular_file(each)) {
            const std::string extension =
                format::to_lower(entry.path().extension().string());
            if (wanted.empty()
                || std::find(wanted.begin(), wanted.end(), extension) != wanted.end()) {
                files.push_back(json{{"name", name}, {"bytes", entry.file_size(each)}});
            }
        }
    }
    std::sort(folders.begin(), folders.end());
    std::sort(files.begin(), files.end(), [](const json& a, const json& b) {
        return a["name"].get<std::string>() < b["name"].get<std::string>();
    });

    return good(json{{"path",    at.string()},
                     {"parent",  at.has_parent_path() && at.parent_path() != at
                                     ? at.parent_path().string() : std::string()},
                     {"home",    paths::expand_user("~").string()},
                     {"entries", folders},
                     {"files",   std::move(files)}});
}

}  // namespace

void project_methods(std::vector<Method>& table) {
    table.push_back({"project",      nullptr, project});
    table.push_back({"project.open", nullptr, project_open});
    table.push_back({"project.forget", nullptr, project_forget});
    table.push_back({"trust.answer", nullptr, trust_answer});
    table.push_back({"trust.clear",  nullptr, trust_clear});
    table.push_back({"shelf",          nullptr, shelf_list});
    table.push_back({"shelf.remove",   nullptr, shelf_remove});
    table.push_back({"shelf.download", nullptr, shelf_download});
    table.push_back({"shelf.view",     shelf_view, nullptr});
    table.push_back({"shelf.bytes",    shelf_bytes, nullptr});
    table.push_back({"shelf.archive",  shelf_archive, nullptr});
    table.push_back({"projects",     projects, nullptr});
    table.push_back({"browse",       browse, nullptr});
}

}  // namespace crucible::api
