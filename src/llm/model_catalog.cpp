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
#include <cstdio>

#include "crucible/config/paths.hpp"
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

bool is_bare_name(std::string_view reference) {
    if (reference.empty()) {
        return false;
    }
    return reference.front() != '/' && reference.front() != '~'
        && reference.find('/') == std::string_view::npos;
}

std::filesystem::path resolve_model_ref(const std::filesystem::path& models_dir,
                                        std::string_view reference) {
    if (reference.empty()) {
        return {};
    }
    if (reference.front() == '/' || reference.front() == '~') {
        return paths::expand_user(reference);
    }
    // Anything else, including "subfolder/model.gguf", is relative to the
    // models directory.
    return models_dir / std::filesystem::path(std::string(reference));
}

std::vector<ModelFile> scan_models(const std::filesystem::path& dir) {
    std::vector<ModelFile> found;

    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        return found;
    }

    for (std::filesystem::directory_iterator it(dir, ec), end; it != end; it.increment(ec)) {
        if (ec) {
            break;
        }
        // Follow symlinks: a models folder full of links to a big external
        // drive is a perfectly reasonable way to organize this.
        std::error_code entry_ec;
        if (!it->is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        if (!has_gguf_extension(it->path())) {
            continue;
        }

        ModelFile file;
        file.name  = it->path().filename().string();
        file.path  = it->path();
        file.bytes = std::filesystem::file_size(it->path(), entry_ec);
        if (entry_ec) {
            file.bytes = 0;
        }
        found.push_back(std::move(file));
    }

    std::sort(found.begin(), found.end(),
              [](const ModelFile& a, const ModelFile& b) { return a.name < b.name; });
    return found;
}

}  // namespace crucible
