// SPDX-License-Identifier: MIT
//
// The Runtimes page: building and removing the GPU backends.
//
// Crucible installs with no compute runtime, because a backend has to be
// compiled against the machine it will run on and a ten-minute build does not
// belong in an installer. This is where that build happens, and it is the same
// RuntimeBuilder the terminal program drives -- one implementation, two faces.
//
// A build takes minutes, so it runs on its own thread and this page only reads
// its progress. The builder lives on App rather than here, which is what lets
// you leave the page, watch the cook, and come back to a finished build.
#include "../app.hpp"

#include <algorithm>

#include <imgui.h>

#include <GLFW/glfw3.h>

#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

namespace {

const char* backend_name(BackendKind kind) {
    switch (kind) {
        case BackendKind::Cpu:    return "CPU";
        case BackendKind::Cuda:   return "CUDA";
        case BackendKind::Vulkan: return "Vulkan";
        case BackendKind::Metal:  return "Metal";
    }
    return "?";
}

/// The two facts about a runtime that decide anything: which llama.cpp it is
/// built against, and what has to be installed to build it.
///
/// There used to be a sentence here about what each backend is for. It read as
/// a sales pitch for a thing you already own -- nobody comes to this page to be
/// told that CUDA is fast -- and it pushed the buttons down the screen. What is
/// left is what you cannot work out by looking.
std::string backend_facts(const RuntimeStatus& runtime) {
    const BackendInfo& info = backend_info(runtime.kind);

    std::string line = "llama.cpp ";
    line += runtime.installed && !runtime.llama_tag.empty()
              ? runtime.llama_tag
              : std::string(RuntimeStatus::required_llama_tag());

    if (!info.required_tool.empty()) {
        line += "  ·  needs ";
        line += std::string(info.required_tool);
    }
    if (runtime.installed && !runtime.built_at.empty()) {
        line += "  ·  built " + runtime.built_at;
    }
    return line;
}

}  // namespace

void App::take_runtime_activation() {
    const BuildProgress build = runtime_builder_.progress();
    const bool activated =
        build.phase == BuildProgress::Phase::Done && build.error.empty();

    if (!activated) {
        // Reset on anything else, so the next build reports itself too.
        runtime_activated_ = false;
        return;
    }
    if (runtime_activated_) {
        return;
    }
    runtime_activated_ = true;

    // What is installed changed, and so did the devices under it. Both lists
    // are read straight from ggml, so re-reading them is the whole refresh.
    runtimes_    = RuntimeRegistry::scan();
    any_runtime_ = RuntimeRegistry::any_installed();

    // The models, though, are not: one picks its devices when it loads and
    // keeps them. Dropping them is what puts the next prompt on the new GPU.
    if (engine_) {
        engine_->reload_models();
    }
    say(std::string(backend_name(build.kind))
        + " is ready -- models will use it from the next prompt");
}

void App::draw_settings_runtimes() {
    title("Runtimes");
    wrapped(theme::kTextDim,
            "Compiled here, because a backend built somewhere else crashes here. "
            "A few minutes each.");

    // Scanned when the page is first opened rather than at startup: it reads
    // the runtimes directory, and most sessions never come here.
    if (!runtimes_scanned_) {
        runtimes_        = RuntimeRegistry::scan();
        runtimes_scanned_ = true;
    }

    const BuildProgress build = runtime_builder_.progress();

    if (!RuntimeRegistry::loadable_backends_supported()) {
        wrapped(theme::kError,
                "This build has its backend compiled in, so runtimes cannot be "
                "added or removed. Rebuild with CRUCIBLE_BACKEND_DL=ON for the "
                "loadable arrangement.");
        return;
    }

    section("INSTALLED");
    for (const RuntimeStatus& runtime : runtimes_) {
        ImGui::PushID(static_cast<int>(runtime.kind));

        const bool busy_here = build.running() && build.kind == runtime.kind;

        ImGui::PushFont(theme::bold());
        text_colored(runtime.active    ? theme::kFlame
                      : runtime.stale   ? theme::kError
                      : runtime.installed ? theme::kText
                                          : theme::kTextFaint,
                      "%s", backend_name(runtime.kind));
        ImGui::PopFont();
        ImGui::SameLine();

        if (runtime.stale) {
            text_colored(theme::kError, "built against %s, this build needs %s",
                          runtime.llama_tag.c_str(),
                          std::string(RuntimeStatus::required_llama_tag()).c_str());
        } else if (runtime.active) {
            text_colored(theme::kTextDim, "%d device%s   %s", runtime.device_count,
                          runtime.device_count == 1 ? "" : "s",
                          runtime.size_label().c_str());
        } else if (runtime.installed) {
            text_colored(theme::kTextDim, "installed, no devices   %s",
                          runtime.size_label().c_str());
        } else if (!runtime.buildable) {
            text_colored(theme::kTextFaint, "%s", runtime.blocker.c_str());
        } else {
            text_colored(theme::kTextFaint, "not installed");
        }

        text_colored(theme::kTextFaint, "%s", backend_facts(runtime).c_str());

        ImGui::BeginDisabled(build.running());
        if (runtime.installed) {
            if (ImGui::Button(runtime.stale ? "Rebuild" : "Reinstall",
                              ImVec2(em(8.0F), 0))) {
                runtime_error_.clear();
                // glfwPostEmptyEvent wakes the render loop, which otherwise
                // sleeps for half a second between frames and would show the
                // build advancing in visible steps.
                runtime_builder_.start(runtime.kind, [] { glfwPostEmptyEvent(); });
            }
            ImGui::SameLine();
            if (ImGui::Button("Remove", ImVec2(em(7.0F), 0))) {
                std::string error;
                if (RuntimeRegistry::remove(runtime.kind, error)) {
                    say(std::string(backend_name(runtime.kind)) + " runtime removed");
                    runtimes_    = RuntimeRegistry::scan();
                    any_runtime_ = RuntimeRegistry::any_installed();
                } else {
                    runtime_error_ = error;
                }
            }
        } else {
            ImGui::BeginDisabled(!runtime.buildable);
            if (ImGui::Button("Build and install", ImVec2(em(13.0F), 0))) {
                runtime_error_.clear();
                runtime_builder_.start(runtime.kind, [] { glfwPostEmptyEvent(); });
            }
            ImGui::EndDisabled();
        }
        ImGui::EndDisabled();

        if (busy_here) {
            ImGui::SameLine();
            text_colored(theme::kFlameBright, "%s", build.label().c_str());
        }

        ImGui::Dummy(ImVec2(0, em(0.4F)));
        ImGui::Separator();
        ImGui::PopID();
    }

    if (!runtime_error_.empty()) {
        wrapped(theme::kError, runtime_error_);
    }

    if (build.phase != BuildProgress::Phase::Idle) {
        section("BUILD");
        text_colored(theme::kText, "%s  %s", backend_name(build.kind),
                      build.label().c_str());

        if (build.phase == BuildProgress::Phase::Compiling) {
            ImGui::ProgressBar(build.percent, ImVec2(-FLT_MIN, em(1.0F)));
        }
        if (!build.step.empty() && build.running()) {
            text_colored(theme::kTextFaint, "%s", build.step.c_str());
        }

        if (build.running()) {
            if (ImGui::Button("Cancel", ImVec2(em(7.0F), 0))) {
                runtime_builder_.cancel();
            }
        } else {
            if (build.phase == BuildProgress::Phase::Failed) {
                wrapped(theme::kError, build.error);
                // The tail is what a failed build is actually about; the full
                // log is on disk and named here so it can be sent on.
                for (const std::string& line : build.log_tail) {
                    text_colored(theme::kTextFaint, "%s", line.c_str());
                }
                if (!build.log_file.empty()) {
                    text_colored(theme::kTextDim, "full log: %s",
                                  build.log_file.string().c_str());
                }
            }
            if (ImGui::Button("Dismiss", ImVec2(em(7.0F), 0))) {
                runtime_builder_.dismiss();
                // A finished build changes what is installed and, if it
                // loaded, what devices exist.
                runtimes_    = RuntimeRegistry::scan();
                any_runtime_ = RuntimeRegistry::any_installed();
            }
        }
    }
}

}  // namespace crucible::gui
