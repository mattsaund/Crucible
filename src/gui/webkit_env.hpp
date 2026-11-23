// SPDX-License-Identifier: MIT
//
// What WebKitGTK has to be told before the first window is made. Shared by
// Crucible and crucible-app, which draw their windows the same way.
#pragma once

#if !defined(_WIN32) && !defined(__APPLE__)

#include <glib.h>
#include <unistd.h>
#include <webkit2/webkit2.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <system_error>

#include "webkit_choice.hpp"

namespace crucible::gui {

namespace webkit_detail {

/// What a window that the desktop refused starts again with.
inline char**      relaunch = nullptr;
inline std::string refused_file;
inline std::string refused_key;

/// GTK says this and ends the process when the desktop has refused what the
/// window sent it, which is how the GPU renderer fails on NVIDIA's driver:
/// "Error 71 (Protocol error) dispatching to Wayland display." It is the one
/// moment to do something about it, so the program starts over in the same
/// process, drawing on the processor, and remembers not to try again until
/// the driver or WebKit has changed.
inline void on_gdk_message(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
    if (relaunch != nullptr && message != nullptr
        && std::strstr(message, "dispatching to Wayland display") != nullptr) {
        char** const argv = relaunch;
        relaunch          = nullptr;
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::path(refused_file).parent_path(), ec);
        std::ofstream(refused_file, std::ios::trunc) << refused_key;
        ::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 1);
        ::execv("/proc/self/exe", argv);
    }
    g_log_default_handler(domain, level, message, nullptr);
}

inline std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

/// Where the window will be: GTK takes Wayland when there is one, unless told
/// otherwise.
inline bool on_wayland() {
    const char* const display = std::getenv("WAYLAND_DISPLAY");
    if (display == nullptr || *display == '\0') {
        return false;
    }
    const char* const backend = std::getenv("GDK_BACKEND");
    return backend == nullptr || *backend == '\0' || std::strncmp(backend, "wayland", 7) == 0
        || std::strncmp(backend, "*", 1) == 0;
}

}  // namespace webkit_detail

/// Draw on the GPU where that works, and on the processor where it does not.
///
/// A window drawn on the processor is every pixel of it, every frame: on a 4K
/// display at 150% that is about 15 million of them, and scrolling a chat ran
/// at 31 frames a second where the GPU holds the display's rate on less than
/// half the CPU. It was turned off for all of NVIDIA in 0.9.2, because driver
/// 615 on a Wayland desktop ended the program at the window's first frame.
/// From driver 620 it is on, and if the desktop still refuses it the program
/// starts itself again without it. An X11 window, an older driver, or anybody
/// who has set WEBKIT_DISABLE_DMABUF_RENDERER keeps what they had.
///
/// Called first thing in main(): setting the environment is only safe before
/// there is a second thread to read it.
inline void choose_webkit_renderer(char** argv) {
    std::error_code ec;
    const std::string nvidia = nvidia_version_in(webkit_detail::read_text("/proc/driver/nvidia/version"));

    std::filesystem::path cache;
    if (const char* const set = std::getenv("XDG_CACHE_HOME"); set != nullptr && *set != '\0') {
        cache = set;
    } else if (const char* const home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        cache = std::filesystem::path(home) / ".cache";
    }
    const std::string key = nvidia + " " + std::to_string(webkit_get_major_version()) + "."
                          + std::to_string(webkit_get_minor_version()) + "."
                          + std::to_string(webkit_get_micro_version());
    const std::filesystem::path refused = cache / "crucible" / "webkit-gpu-refused";

    switch (webkit_renderer_for(std::getenv("WEBKIT_DISABLE_DMABUF_RENDERER") != nullptr, nvidia,
                                webkit_detail::on_wayland(),
                                !cache.empty() && webkit_detail::read_text(refused) == key)) {
    case WebkitRenderer::Leave:
        break;
    case WebkitRenderer::Off:
        ::setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
        break;
    case WebkitRenderer::Try:
        webkit_detail::relaunch     = argv;
        webkit_detail::refused_file = refused.string();
        webkit_detail::refused_key  = key;
        g_log_set_handler("Gdk", G_LOG_LEVEL_MESSAGE, webkit_detail::on_gdk_message, nullptr);
        break;
    }
}

}  // namespace crucible::gui

#endif
