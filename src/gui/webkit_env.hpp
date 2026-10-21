// SPDX-License-Identifier: MIT
//
// What WebKitGTK has to be told before the first window is made. Shared by
// Crucible and crucible-app, which draw their windows the same way.
#pragma once

#if !defined(_WIN32) && !defined(__APPLE__)

#include <cstdlib>
#include <filesystem>
#include <system_error>

namespace crucible::gui {

/// Turn off WebKitGTK's DMA-BUF renderer on a machine running NVIDIA's
/// driver, unless somebody has already said which they want.
///
/// With it on, NVIDIA's driver and a Wayland desktop end the program at the
/// window's first frame: "Error 71 (Protocol error) dispatching to Wayland
/// display", and nothing else said. KDE Plasma on Wayland with driver 615 did
/// that to Crucible on every start. With it off, WebKit draws the way it did
/// before that renderer existed. A machine without NVIDIA's driver keeps it.
///
/// Called first thing in main(): setting the environment is only safe before
/// there is a second thread to read it.
inline void avoid_nvidia_dmabuf_renderer() {
    if (std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") != nullptr) {
        return;
    }
    std::error_code ec;
    if (std::filesystem::exists("/proc/driver/nvidia/version", ec)) {
        ::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    }
}

}  // namespace crucible::gui

#endif
