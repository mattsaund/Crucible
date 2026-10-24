// SPDX-License-Identifier: MIT
//
// Reading the models directory.
//
// Only the file extension is checked, never the contents: validating every GGUF
// would mean opening gigabytes of files to draw a list. A file that is not
// really a model fails later, at load, with a clear message.
#include "crucible/llm/model_catalog.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <regex>
#include <system_error>
#include <utility>

#include "crucible/config/paths.hpp"
#include "crucible/llm/mlx_server.hpp"
#include "crucible/util/format.hpp"

namespace crucible {
namespace {

bool has_gguf_extension(const std::filesystem::path& path) {
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (extension != ".gguf") {
        return false;
    }
    // A vision model's projector is a GGUF too, and not a model to chat with:
    // it is loaded beside the model it belongs to. See ModelHost::load.
    std::string name = path.filename().string();
    std::transform(name.begin(), name.end(), name.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return name.find("mmproj") == std::string::npos;
}

}  // namespace

std::string ModelFile::size_label() const {
    return format::bytes(bytes);
}

// Both of these ask the path library rather than looking for a '/'. On Windows
// a model can be "C:\ggufs\phys.gguf", which has no forward slash in it at all:
// it read as a bare name and was looked for inside the models directory.

bool is_bare_name(std::string_view reference) {
    if (reference.empty() || reference.front() == '~') {
        return false;
    }
    const std::filesystem::path path{std::string(reference)};
    return !path.has_root_path() && !path.has_parent_path();
}

std::filesystem::path resolve_model_ref(const std::filesystem::path& models_dir,
                                        std::string_view reference) {
    if (reference.empty()) {
        return {};
    }
    // A root of any kind -- "/opt/x.gguf", "C:\x.gguf", "\\server\share\x.gguf" --
    // or a home-relative "~/x.gguf" names a file outside the models directory.
    const std::filesystem::path path{std::string(reference)};
    if (reference.front() == '~' || path.has_root_path()) {
        return paths::expand_user(reference);
    }
    // Anything else, including "subfolder/model.gguf", is relative to the
    // models directory.
    const std::filesystem::path direct = models_dir / path;
    // A bare name that is not at the top: the folder was tidied into
    // subfolders after the seat was set. Looked for by name, as deep as the
    // catalog looks, rather than every seat reading as missing.
    std::error_code ec;
    if (is_bare_name(reference) && !std::filesystem::exists(direct, ec)) {
        for (const ModelFile& file : scan_models(models_dir)) {
            if (file.path.filename() == path) {
                return file.path;
            }
        }
    }
    return direct;
}

namespace {

/// How far down a model is looked for. Four levels: a folder of models, a
/// publisher's folder of them (LM Studio's layout), a folder of those, and
/// Hugging Face's own cache -- models--org--name/snapshots/<hash>/ -- which
/// is where an MLX model fetched by mlx_lm lands. An MLX model is a folder,
/// and is listed whole, never looked inside.
constexpr int kDeepest = 3;

/// For part `n` of a split GGUF -- "x-00002-of-00003.gguf" -- its number and
/// how many there are; {0, 0} for a GGUF in one piece.
std::pair<int, int> split_part(const std::string& name) {
    static const std::regex split(R"(-(\d{5})-of-(\d{5})\.gguf$)", std::regex::icase);
    std::smatch match;
    if (!std::regex_search(name, match, split)) {
        return {0, 0};
    }
    return {std::stoi(match[1].str()), std::stoi(match[2].str())};
}

/// A split GGUF's size: every part of it in the folder.
std::uintmax_t split_bytes(const std::filesystem::path& first, int parts) {
    const std::string name = first.filename().string();
    const std::size_t at = name.size() - std::string("-00001-of-00000.gguf").size();
    std::uintmax_t total = 0;
    char number[32];
    for (int part = 1; part <= parts; ++part) {
        std::snprintf(number, sizeof number, "-%05d-of-%05d", part, parts);
        std::error_code ec;
        const std::uintmax_t size = std::filesystem::file_size(
            first.parent_path() / (name.substr(0, at) + number + name.substr(at + 15)), ec);
        total += ec ? 0 : size;
    }
    return total;
}

void scan_into(const std::filesystem::path& root, const std::filesystem::path& relative, int depth,
               std::vector<ModelFile>& found) {
    std::error_code ec;
    for (std::filesystem::directory_iterator it(root / relative, ec), end; !ec && it != end;
         it.increment(ec)) {
        const std::string leaf = it->path().filename().string();
        if (leaf.empty() || leaf.front() == '.') {
            continue;
        }
        // Follow symlinks: a models folder full of links to a big external
        // drive is a perfectly reasonable way to organize this.
        std::error_code entry_ec;
        const std::filesystem::path inside = relative / leaf;
        if (it->is_regular_file(entry_ec) && !entry_ec && has_gguf_extension(it->path())) {
            // A model split in parts is one model, named by its first part,
            // which is the one llama.cpp is given: it finds the rest beside it.
            const auto [part, parts] = split_part(leaf);
            if (part > 1) {
                continue;
            }
            ModelFile file;
            file.name  = inside.generic_string();
            file.path  = it->path();
            file.bytes = parts > 1 ? split_bytes(it->path(), parts)
                                   : std::filesystem::file_size(it->path(), entry_ec);
            if (entry_ec) {
                file.bytes = 0;
            }
            found.push_back(std::move(file));
        } else if (it->is_directory(entry_ec) && !entry_ec) {
            if (mlx::is_model_dir(it->path())) {
                ModelFile model;
                model.name   = inside.generic_string();
                model.path   = it->path();
                model.bytes  = mlx::model_bytes(it->path());
                model.format = "mlx";
                found.push_back(std::move(model));
            } else if (depth < kDeepest) {
                scan_into(root, inside, depth + 1, found);
            }
        }
    }
}

}  // namespace

std::vector<ModelFile> scan_models(const std::filesystem::path& dir) {
    std::vector<ModelFile> found;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }
    // The folder chosen may itself be one MLX model rather than a folder of
    // them. It is listed by its whole path, which is what a seat names to
    // reach it: a name is otherwise read as relative to this folder.
    if (mlx::is_model_dir(dir)) {
        ModelFile model;
        model.name   = dir.string();
        model.path   = dir;
        model.bytes  = mlx::model_bytes(dir);
        model.format = "mlx";
        found.push_back(std::move(model));
        return found;
    }
    scan_into(dir, {}, 0, found);
    std::sort(found.begin(), found.end(),
              [](const ModelFile& a, const ModelFile& b) { return a.name < b.name; });
    return found;
}

}  // namespace crucible
