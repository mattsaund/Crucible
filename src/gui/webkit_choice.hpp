// SPDX-License-Identifier: MIT
//
// Which way WebKitGTK should draw on a machine running NVIDIA's driver: the
// decision on its own, with nothing of GTK in it, so a test can ask it.
// webkit_env.hpp is what acts on it.
#pragma once

#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace crucible::gui {

/// The first NVIDIA driver series WebKit's GPU renderer has been seen to work
/// with on a Wayland desktop. 615 ended the program at its first frame.
inline constexpr int kNvidiaDmabufFrom = 620;

/// The driver's version in the text of /proc/driver/nvidia/version, "620.73.04",
/// or nothing. It is the first thing on the first line shaped like a version:
/// the words around it differ between the open and the closed kernel module.
inline std::string nvidia_version_in(std::string_view text) {
    text = text.substr(0, text.find('\n'));
    for (std::size_t at = 0; at < text.size(); ++at) {
        const bool starts = std::isdigit(static_cast<unsigned char>(text[at])) != 0
                         && (at == 0 || text[at - 1] == ' ' || text[at - 1] == '\t');
        if (!starts) {
            continue;
        }
        std::size_t end  = at;
        bool        dots = false;
        while (end < text.size() && (std::isdigit(static_cast<unsigned char>(text[end])) != 0 || text[end] == '.')) {
            dots = dots || text[end] == '.';
            ++end;
        }
        if (dots && (end == text.size() || text[end] == ' ' || text[end] == '\t')) {
            return std::string(text.substr(at, end - at));
        }
        at = end;
    }
    return {};
}

enum class WebkitRenderer {
    Leave,  ///< Not ours to say: no NVIDIA driver, or somebody has set it.
    Off,    ///< WebKit draws on the processor.
    Try,    ///< The GPU, with the way back if the desktop refuses it.
};

/// `chosen`: the environment already says. `nvidia`: the driver's version, or
/// empty. `wayland`: the window will be a Wayland one. `failed_here`: the GPU
/// renderer already ended the program with this driver and this WebKit.
inline WebkitRenderer webkit_renderer_for(bool chosen, std::string_view nvidia, bool wayland, bool failed_here) {
    if (chosen || nvidia.empty()) {
        return WebkitRenderer::Leave;
    }
    const int series = std::atoi(std::string(nvidia).c_str());
    return wayland && series >= kNvidiaDmabufFrom && !failed_here ? WebkitRenderer::Try : WebkitRenderer::Off;
}

}  // namespace crucible::gui
