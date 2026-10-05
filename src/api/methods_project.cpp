// SPDX-License-Identifier: MIT
//
// Which folder we are working in, and whether we may.
#include "methods.hpp"

#include <algorithm>

#include "crucible/config/paths.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/format.hpp"

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

Reply trust_answer(const json& params, Host& host) {
    host.answer_trust(params.value("trusted", false));
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
    table.push_back({"trust.answer", nullptr, trust_answer});
    table.push_back({"projects",     projects, nullptr});
    table.push_back({"browse",       browse, nullptr});
}

}  // namespace crucible::api
