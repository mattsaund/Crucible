// SPDX-License-Identifier: MIT
// XDG-aware locations for Crucible's own files.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::paths {

/// $XDG_CONFIG_HOME/crucible, falling back to ~/.config/crucible.
std::filesystem::path config_dir();

/// $XDG_DATA_HOME/crucible, falling back to ~/.local/share/crucible.
std::filesystem::path data_dir();

/// config_dir()/config.json
std::filesystem::path config_file();

/// config_dir()/trust.json
std::filesystem::path trust_file();

/// data_dir()/models -- the default place Crucible looks for GGUF files.
std::filesystem::path models_dir();

/// data_dir()/crucible.log -- where llama.cpp's chatter is redirected so it
/// cannot scribble over the TUI.
std::filesystem::path log_file();

/// data_dir()/runtimes -- the loadable ggml backends. One directory holding
/// libggml-cuda.so and friends; adding or removing a file here is exactly what
/// installing or uninstalling a runtime means.
std::filesystem::path runtimes_dir();

/// data_dir()/runtime-src -- the llama.cpp checkout GPU runtimes are built
/// from. Seeded by install.sh, and cloned on demand if it is missing.
std::filesystem::path runtime_src_dir();

/// data_dir()/runtime-build -- scratch build trees, one per backend. Safe to
/// delete at any time; the next build just takes longer.
std::filesystem::path runtime_build_dir();

/// data_dir()/projects -- per-project session history, keyed by directory.
std::filesystem::path projects_dir();

/// Expand a leading `~` and resolve to an absolute path. Does not require the
/// path to exist, so it is safe to call on a model path the user has not
/// downloaded yet.
std::filesystem::path expand_user(std::string_view raw);

}  // namespace crucible::paths
