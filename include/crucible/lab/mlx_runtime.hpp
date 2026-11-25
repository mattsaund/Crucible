// SPDX-License-Identifier: MIT
//
// MLX as a runtime: the mlx and mlx-lm packages in Crucible's Python
// environment, which run MLX models (llm/mlx_server.cpp) and train on Apple
// silicon. Their versions are read off the disk, an update is asked of pip
// within the bounds the environment was installed with (pyenv::plan), and
// made on a thread of its own: as llama.cpp's runtimes are, without a new
// Crucible. See runtime/llama_versions.hpp for those.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace crucible::lab::mlx {

/// What is installed, what an update would bring, and one under way.
struct Versions {
    bool        present = false;   ///< mlx is installed in the environment
    std::string mlx;               ///< "0.29.1"
    std::string mlx_lm;            ///< "0.28.0"
    std::string newest_mlx;        ///< what an update installs, once checked
    std::string newest_mlx_lm;
    std::int64_t checked_at = 0;   ///< seconds, of the last check
    std::string check_error;
    bool        updating = false;
    std::string said;              ///< the update's last line
    std::string error;             ///< why the last update stopped
};

/// The versions installed, read from the packages' own records: no Python
/// is started. With what the last check and update found.
Versions versions();

/// Ask pip what an update would install, without installing it: what
/// `newest_mlx` and `newest_mlx_lm` say afterwards. Seconds, and the network.
bool check(std::string& error);

/// Whether a check found something newer than what is installed.
bool update_available();

/// Update mlx and mlx-lm on a thread of their own, to the newest within the
/// environment's bounds, or mlx-lm to `mlx_lm` exactly. False when one is
/// under way or there is no MLX to update.
bool start_update(std::string mlx_lm = {});

/// The version in a package's record folder's name, "mlx_lm-0.28.0.dist-info"
/// for "mlx-lm": empty when `folder` is not that package's.
std::string version_in(std::string_view folder, std::string_view package);

/// The versions pip's --report would install, from its JSON.
bool read_report(std::string_view report, std::string& mlx, std::string& mlx_lm);

/// Where the environment's packages are.
std::filesystem::path site_packages();

}  // namespace crucible::lab::mlx
