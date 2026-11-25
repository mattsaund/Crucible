// SPDX-License-Identifier: MIT
//
// See shelf.hpp.
#include "crucible/session/shelf.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <set>

#include "crucible/config/paths.hpp"

namespace crucible::shelf {
namespace {

std::string lowered(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return text;
}

/// The extension without its dot, in lower case, or the whole name for a file
/// known by its name alone ("Makefile").
std::string extension_of(const std::filesystem::path& path) {
    const std::string ext = path.extension().string();
    return lowered(ext.empty() ? path.filename().string() : ext.substr(1));
}

/// Downloads, by kind. Anything not here and not code is a plain "file".
const std::vector<std::pair<std::string, std::vector<std::string>>>& kinds() {
    static const std::vector<std::pair<std::string, std::vector<std::string>>> table{
        {"pdf",      {"pdf"}},
        {"document", {"doc", "docx", "odt", "rtf", "epub", "pages"}},
        {"sheet",    {"xls", "xlsx", "ods", "csv", "tsv", "numbers"}},
        {"slides",   {"ppt", "pptx", "odp", "key"}},
        {"image",    {"png", "jpg", "jpeg", "gif", "webp", "bmp", "tif", "tiff", "ico", "heic", "avif"}},
        {"audio",    {"mp3", "wav", "flac", "ogg", "m4a", "aac", "opus"}},
        {"video",    {"mp4", "webm", "mov", "mkv", "avi", "m4v"}},
        {"archive",  {"zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar", "dmg", "iso", "deb", "rpm",
                      "appimage", "exe", "msi", "apk", "whl", "jar", "gguf", "safetensors", "bin"}},
        {"text",     {"md", "markdown", "txt", "log"}},
    };
    return table;
}

/// Folders nothing comes from: version control, dependencies, caches.
bool skipped(const std::string& name) {
    static const std::set<std::string> names{
        ".git", ".hg", ".svn", "node_modules", ".venv", "venv", "__pycache__", ".mypy_cache",
        ".pytest_cache", ".ruff_cache", ".cache", ".next", ".nuxt", ".svelte-kit", ".gradle",
        ".idea", ".tox", "site-packages", ".parcel-cache", ".turbo", ".crucible",
    };
    return names.count(name) != 0;
}

/// A build's output folders: what is made there is kept only when it is not
/// code, since a compile writes thousands of files nobody asked for by name.
bool output_folder(const std::string& name) {
    static const std::set<std::string> names{"target", "build", "dist", "out", "bin", "obj", ".output"};
    return names.count(name) != 0;
}

}  // namespace

std::string kind_of(const std::filesystem::path& path) {
    const std::string ext = extension_of(path);
    for (const auto& [kind, extensions] : kinds()) {
        if (std::find(extensions.begin(), extensions.end(), ext) != extensions.end()) {
            return kind;
        }
    }
    return "file";
}

bool is_code(const std::filesystem::path& path) {
    static const std::set<std::string> code{
        "c", "cc", "cpp", "cxx", "h", "hh", "hpp", "hxx", "cs", "java", "kt", "kts", "scala", "go",
        "rs", "swift", "m", "mm", "py", "pyw", "rb", "php", "pl", "lua", "r", "jl", "dart", "ex",
        "exs", "erl", "hs", "ml", "zig", "nim", "js", "mjs", "cjs", "ts", "tsx", "jsx", "vue",
        "svelte", "astro", "html", "htm", "css", "scss", "sass", "less", "json", "jsonc", "yaml",
        "yml", "toml", "ini", "cfg", "conf", "xml", "svg", "sh", "bash", "zsh", "fish", "ps1", "bat",
        "cmd", "sql", "graphql", "proto", "gradle", "cmake", "mk", "makefile", "dockerfile", "env",
        "gitignore", "editorconfig", "lock", "license", "readme", "gemfile", "procfile", "wasm",
        "ipynb", "tex", "bib", "glsl", "wgsl", "hlsl", "sol", "tf",
    };
    return code.count(extension_of(path)) != 0;
}

std::vector<std::string> made_since(const std::filesystem::path& root,
                                    std::filesystem::file_time_type since, std::size_t limit) {
    std::vector<std::string> made;
    std::error_code          ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) {
        return made;
    }
    // Bounded, so a folder with a few hundred thousand files in it costs a
    // moment rather than the rest of the turn.
    std::size_t looked = 0;
    for (std::filesystem::recursive_directory_iterator it{
             root, std::filesystem::directory_options::skip_permission_denied, ec},
         end;
         it != end && !ec && made.size() < limit && looked < 50000; it.increment(ec)) {
        ++looked;
        const std::filesystem::directory_entry& entry = *it;
        const std::string name = entry.path().filename().string();
        if (entry.is_directory(ec)) {
            if (skipped(name) || entry.is_symlink(ec)) {
                it.disable_recursion_pending();
            }
            continue;
        }
        if (!entry.is_regular_file(ec)) {
            continue;
        }
        std::error_code when;
        if (entry.last_write_time(when) < since || when) {
            continue;
        }
        const std::filesystem::path relative = entry.path().lexically_relative(root);
        bool in_output = false;
        for (const std::filesystem::path& part : relative.parent_path()) {
            in_output = in_output || output_folder(part.string());
        }
        if (in_output && is_code(entry.path())) {
            continue;
        }
        made.push_back(relative.generic_string());
    }
    std::sort(made.begin(), made.end());
    return made;
}

bool merge(std::vector<Entry>& entries, const std::vector<std::string>& made, std::int64_t now) {
    bool added = false;
    for (const std::string& path : made) {
        const bool known = std::any_of(entries.begin(), entries.end(),
                                       [&path](const Entry& entry) { return entry.path == path; });
        if (!known) {
            entries.push_back(Entry{path, now});
            added = true;
        }
    }
    return added;
}

nlohmann::json to_json(const std::vector<Entry>& entries) {
    nlohmann::json out = nlohmann::json::array();
    for (const Entry& entry : entries) {
        out.push_back(nlohmann::json{{"path", entry.path}, {"added", entry.added}});
    }
    return out;
}

std::vector<Entry> from_json(const nlohmann::json& value) {
    std::vector<Entry> out;
    if (!value.is_array()) {
        return out;
    }
    for (const nlohmann::json& one : value) {
        if (one.is_object() && one.contains("path") && one["path"].is_string()) {
            out.push_back(Entry{one["path"].get<std::string>(), one.value("added", std::int64_t{0})});
        }
    }
    return out;
}

std::filesystem::path downloads_dir() {
    const std::filesystem::path home = paths::home_dir();
#if !defined(_WIN32) && !defined(__APPLE__)
    // Where the desktop says, which is not always ~/Downloads: a translated
    // desktop names it in its own language.
    const char*                 config = std::getenv("XDG_CONFIG_HOME");
    const std::filesystem::path dirs   = (config != nullptr && *config != '\0' ? std::filesystem::path(config)
                                                                               : home / ".config")
                                       / "user-dirs.dirs";
    std::ifstream in(dirs);
    std::string   line;
    while (std::getline(in, line)) {
        const std::string key = "XDG_DOWNLOAD_DIR=\"";
        if (line.rfind(key, 0) != 0 || line.back() != '"') {
            continue;
        }
        std::string value = line.substr(key.size(), line.size() - key.size() - 1);
        if (value.rfind("$HOME", 0) == 0) {
            value = home.string() + value.substr(5);
        }
        if (!value.empty() && value != home.string() && value != home.string() + "/") {
            return value;
        }
    }
#endif
    return home / "Downloads";
}

std::filesystem::path download_target(const std::string& name) {
    const std::filesystem::path folder = downloads_dir();
    const std::filesystem::path wanted = std::filesystem::path(name).filename();
    std::filesystem::path       target = folder / wanted;
    std::error_code             ec;
    for (int n = 2; std::filesystem::exists(target, ec) && n < 1000; ++n) {
        target = folder / (wanted.stem().string() + " (" + std::to_string(n) + ")" + wanted.extension().string());
    }
    return target;
}

std::filesystem::path inside(const std::filesystem::path& root, const std::string& relative) {
    if (root.empty() || relative.empty()) {
        return {};
    }
    const std::filesystem::path base   = root.lexically_normal();
    const std::filesystem::path joined = (base / relative).lexically_normal();
    // What is left of it past the root: nothing, the root itself, or a way
    // out of it are all refused, an absolute path included.
    const std::filesystem::path past = joined.lexically_relative(base);
    if (past.empty() || past == "." || *past.begin() == "..") {
        return {};
    }
    return joined;
}

}  // namespace crucible::shelf
