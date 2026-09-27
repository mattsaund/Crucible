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

#include "crucible/config/paths.hpp"
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
    text_colored(theme::kTextFaint, "Compiled on this machine. A few minutes each.");

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
        } else {
            text_colored(theme::kTextFaint, "not installed");
        }

        text_colored(theme::kTextFaint, "%s", backend_facts(runtime).c_str());

        // What is actually on the disk under this name.
        //
        // A runtime is not one file. The CPU backend installs fourteen modules,
        // one per x86-64 feature level, and runs exactly one of them; a stale
        // build leaves its own set sitting there taking room. Folded away by
        // default because the answer is usually "the one it says", and openable
        // because when it is not, nothing else in the program will tell you.
        if (runtime.installed && !runtime.files.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Header, ImVec4(0, 0, 0, 0));
            const std::string summary =
                std::to_string(runtime.files.size())
                + (runtime.files.size() == 1 ? " module" : " modules") + "###files";
            if (ImGui::TreeNodeEx(summary.c_str(), ImGuiTreeNodeFlags_SpanAvailWidth)) {
                for (const std::filesystem::path& file : runtime.files) {
                    std::error_code ec;
                    const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
                    const bool in_use = !runtime.preferred.empty()
                                            ? file == runtime.preferred
                                            : runtime.files.size() == 1;
                    text_colored(in_use ? theme::kText : theme::kTextFaint,
                                 "%s   %s%s", file.filename().string().c_str(),
                                 ec ? "?" : format::bytes(bytes).c_str(),
                                 in_use ? "   in use" : "");
                }
                ImGui::TreePop();
            }
            ImGui::PopStyleColor();
        }

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
            // Never disabled any more.
            //
            // The blocker says what compiling this backend would need, and
            // compiling is now the second thing tried: a prebuilt module for
            // this platform and this llama.cpp tag is downloaded when there is
            // one, and that is exactly the path for the person with an NVIDIA
            // card and no toolkit. Disabling the button on a missing nvcc would
            // shut out the people the download is for. If it does come to
            // compiling, the build says what is missing and stops.
            if (ImGui::Button("Install", ImVec2(em(9.0F), 0))) {
                runtime_error_.clear();
                runtime_builder_.start(runtime.kind, [] { glfwPostEmptyEvent(); });
            }
            if (!runtime.buildable) {
                ImGui::SameLine();
                text_colored(theme::kTextFaint, "downloads, or %s to compile",
                             runtime.blocker.c_str());
            }
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

    // Where they are and what they cost. A runtime is the largest thing
    // Crucible puts on the disk that is not a model, and until this line
    // existed the only way to find out how much of it there was, or where to
    // look, was to know the XDG layout by heart.
    {
        std::uintmax_t total = 0;
        int            stale = 0;
        for (const RuntimeStatus& runtime : runtimes_) {
            total += runtime.bytes;
            stale += runtime.stale ? 1 : 0;
        }
        ImGui::Dummy(ImVec2(0, em(0.4F)));
        ImGui::Separator();
        // Elided from the middle: this is the one line on the page holding a
        // path, and a path that runs off the edge of the panel says neither
        // which disk it is on nor which directory it ends in.
        const std::string where = paths::runtimes_dir().string();
        const std::string size  = total == 0 ? "empty" : format::bytes(total);
        const float room = ImGui::GetContentRegionAvail().x
                         - ImGui::CalcTextSize(size.c_str()).x - em(2.0F);
        text_colored(theme::kTextFaint, "%s   %s",
                     middle_out(where, room).c_str(), size.c_str());
        if (stale > 0) {
            // Built against another llama.cpp: they load and then crash on the
            // first tensor, so saying "installed" without saying this would be
            // the most expensive kind of true.
            text_colored(theme::kError,
                         "%d built against another llama.cpp -- rebuild or remove",
                         stale);
        }
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
