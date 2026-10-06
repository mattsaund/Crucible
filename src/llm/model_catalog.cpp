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
    return extension == ".gguf";
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
    return models_dir / path;
}

namespace {

/// How far down a model is looked for. Four levels: a folder of models, a
/// publisher's folder of them (LM Studio's layout), a folder of those, and
/// Hugging Face's own cache -- models--org--name/snapshots/<hash>/ -- which
/// is where an MLX model fetched by mlx_lm lands. An MLX model is a folder,
/// and is listed whole, never looked inside.
constexpr int kDeepest = 3;

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
            ModelFile file;
            file.name  = inside.generic_string();
            file.path  = it->path();
            file.bytes = std::filesystem::file_size(it->path(), entry_ec);
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
