// SPDX-License-Identifier: MIT
//
// Settings -> Training: the Python environment fine-tuning runs in.
//
// A page rather than a line on another one, for the same reasons Runtimes is
// a page. It is several gigabytes, so somebody will want to know where it
// went and how to get it back. It can be half-installed -- a machine with no
// Python at install time, a proxy that blocked PyPI -- so it needs a repair
// button that is not "reinstall the program". And it is the one place that
// can say, before a run rather than during one, whether training will work
// here at all.
#include "../app.hpp"

#include <imgui.h>

#include <GLFW/glfw3.h>

#include "crucible/config/paths.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

#include "../theme.hpp"
#include "../widgets.hpp"

namespace crucible::gui {

namespace pyenv = lab::pyenv;

void App::refresh_pyenv(bool force) {
    if (force) {
        pyenv_checked_ = false;
    }
    if (!pyenv_checked_) {
        pyenv_status_  = pyenv::status();
        pyenv_checked_ = true;
    }
}

std::filesystem::path App::convert_script() const {
    // llama.cpp's own exporter, from the source tree the runtime builder
    // keeps. Present when a runtime was compiled here and absent when one was
    // downloaded, which is why the trainer treats it as optional rather than
    // required: a fine-tune that produced a model directory is still a
    // fine-tune, and the page says what happened.
    const std::filesystem::path script = paths::runtime_src_dir() / "convert_hf_to_gguf.py";
    std::error_code             ec;
    return std::filesystem::exists(script, ec) ? script : std::filesystem::path{};
}

std::filesystem::path App::quantize_bin() const {
    std::error_code ec;
    const std::filesystem::path beside =
        util::executable_path().parent_path() / "llama-quantize";
    if (std::filesystem::exists(beside, ec)) {
        return beside;
    }
    return util::on_path("llama-quantize") ? std::filesystem::path("llama-quantize")
                                           : std::filesystem::path{};
}

void App::draw_settings_training() {
    title("Training");
    text_colored(theme::kTextFaint,
                 "What fine-tuning runs in. Crucible's own Python, in its own folder.");

    const pyenv::Progress progress = pyenv_installer_.progress();
    if (!progress.running()) {
        refresh_pyenv();
    }

    // --- what is there ------------------------------------------------------
    section("ENVIRONMENT");
    if (pyenv_status_.ready) {
        text_colored(theme::kFlame, "ready");
        ImGui::SameLine();
        text_colored(theme::kTextFaint, "%s  \xc2\xb7  %s",
                     std::string(pyenv::flavor_id(pyenv_status_.flavor)).c_str(),
                     std::string(pyenv::flavor_note(pyenv_status_.flavor)).c_str());
    } else if (pyenv_status_.present) {
        text_colored(theme::kError, "%s", pyenv_status_.note.c_str());
    } else {
        text_colored(theme::kTextDim, "not installed -- training is the only thing "
                                      "this is for, and nothing else needs it");
    }

    const auto fact = [](const char* label, const std::string& value) {
        if (value.empty()) {
            return;
        }
        text_colored(theme::kTextFaint, "%s", label);
        ImGui::SameLine(em(9.0F));
        text_colored(theme::kText, "%s", value.c_str());
    };
    if (pyenv_status_.present) {
        fact("Python", pyenv_status_.python_version);
        fact("Torch", pyenv_status_.torch_version);
        fact("Installed", pyenv_status_.installed_at);
        if (pyenv_status_.bytes > 0) {
            fact("On disk", format::bytes(pyenv_status_.bytes));
        }

        // The cards, and the ones that do not count. This is the whole
        // reason the page reports anything at all: a wheel built for older
        // architectures does not run slowly on a newer card, it does not run
        // -- and nothing about the install said so.
        if (!pyenv_status_.usable_gpus.empty()) {
            std::string cards = pyenv_status_.usable_gpus.front();
            for (std::size_t i = 1; i < pyenv_status_.usable_gpus.size(); ++i) {
                cards += ", " + pyenv_status_.usable_gpus[i];
            }
            fact("Trains on", cards);
        }
        for (const std::string& card : pyenv_status_.unusable_gpus) {
            text_colored(theme::kTextFaint, "Cannot use");
            ImGui::SameLine(em(9.0F));
            text_colored(theme::kFlameBright,
                         "%s -- this PyTorch has no kernels for it", card.c_str());
        }
        text_colored(theme::kTextFaint, "%s",
                     middle_out(pyenv::root().string(),
                                ImGui::GetContentRegionAvail().x).c_str());
    }

    // --- installing it ------------------------------------------------------
    if (progress.running()) {
        section("INSTALLING");
        text_colored(theme::kText, "%s", progress.label().c_str());
        ImGui::ProgressBar(progress.percent, ImVec2(em(24.0F), em(0.8F)), "");
        if (ImGui::Button("Stop", ImVec2(em(7.0F), 0))) {
            pyenv_installer_.cancel();
        }
        // The page ends here while it runs: the buttons below would all be
        // disabled anyway, and a screen of disabled controls under a
        // progress bar is noise.
        return;
    }

    if (progress.phase == pyenv::Progress::Phase::Failed
        || progress.phase == pyenv::Progress::Phase::Canceled) {
        section(progress.phase == pyenv::Progress::Phase::Failed ? "IT FAILED" : "STOPPED");
        if (!progress.error.empty()) {
            wrapped(theme::kError, progress.error);
        }
        for (const std::string& entry : progress.log_tail) {
            text_colored(theme::kTextFaint, "%s", elide(entry,
                         ImGui::GetContentRegionAvail().x).c_str());
        }
        if (!progress.log_file.empty()) {
            text_colored(theme::kTextFaint, "full log: %s",
                         middle_out(progress.log_file.string(),
                                    ImGui::GetContentRegionAvail().x - em(9.0F)).c_str());
        }
        ImGui::Dummy(ImVec2(0, em(0.4F)));
        if (ImGui::Button("Dismiss", ImVec2(em(7.0F), 0))) {
            pyenv_installer_.dismiss();
            refresh_pyenv(true);
        }
        ImGui::SameLine();
    } else {
        ImGui::Dummy(ImVec2(0, em(0.8F)));
    }

    // --- the buttons --------------------------------------------------------
    const pyenv::Flavor want = pyenv_status_.present ? pyenv_status_.flavor
                                                     : pyenv::flavor_here();
    std::string version;
    const bool  have_python = !pyenv::host_python(version).empty();

    ImGui::BeginDisabled(!have_python);
    const char* verb = pyenv_status_.ready ? "Reinstall"
                                           : (pyenv_status_.present ? "Repair" : "Install");
    if (ImGui::Button(verb, ImVec2(em(9.0F), 0))) {
        pyenv_installer_.start(want, [this]() { glfwPostEmptyEvent(); });
    }
    ImGui::EndDisabled();
    ImGui::SetItemTooltip("About %s to download, %s on disk.",
                          format::bytes(pyenv::download_bytes(want)).c_str(),
                          format::bytes(pyenv::installed_bytes(want)).c_str());

    if (pyenv_status_.present) {
        ImGui::SameLine();
        if (ImGui::Button("Remove", ImVec2(em(7.0F), 0))) {
            ImGui::OpenPopup("Remove the training environment?");
        }
    }

    if (!have_python) {
        wrapped(theme::kError,
                version.empty()
                    ? "No Python on this machine, and this is built from one."
                    : "Python " + version + " is here, and 3.10 or newer is needed.");
        const std::string hint = pyenv::python_hint();
        if (!hint.empty()) {
            text_colored(theme::kTextFaint, "%s", hint.c_str());
        }
    } else {
        text_colored(theme::kTextFaint,
                     "About %s to download, %s on disk. Built from Python %s.",
                     format::bytes(pyenv::download_bytes(want)).c_str(),
                     format::bytes(pyenv::installed_bytes(want)).c_str(),
                     version.c_str());
    }

    // --- what it would install ----------------------------------------------
    //
    // Named rather than summarized. Somebody about to let a program download
    // several gigabytes is entitled to read the list, and it is short.
    if (ImGui::TreeNodeEx("What goes in it", ImGuiTreeNodeFlags_SpanAvailWidth)) {
        for (const pyenv::Step& step : pyenv::plan(want)) {
            std::string line = step.label + ":  ";
            for (std::size_t i = 0; i < step.packages.size(); ++i) {
                line += (i > 0 ? ", " : "") + step.packages[i];
            }
            text_colored(theme::kTextFaint, "%s", line.c_str());
            if (!step.index_url.empty()) {
                text_colored(theme::kTextFaint, "    from %s", step.index_url.c_str());
            }
        }
        text_colored(theme::kTextFaint,
                     "Nothing is installed outside %s.", pyenv::root().string().c_str());
        if (want == pyenv::Flavor::Cuda) {
            // Where the surprise goes, since "a few gigabytes" is a number
            // people reasonably expect to be wrong about. Most of it is not
            // Crucible and not even PyTorch: it is NVIDIA's own runtime
            // libraries, which any PyTorch install on any machine pulls in.
            wrapped(theme::kTextFaint,
                    "About three fifths of the size is NVIDIA's CUDA libraries -- "
                    "cuDNN, cuBLAS and the rest. PyTorch itself is under two "
                    "gigabytes and everything above is a few hundred megabytes.");
            // Where consent actually happens, so where the license is worth
            // naming. Everything else here is Apache, BSD or MIT; the NVIDIA
            // wheels are not, and somebody should not have to read
            // THIRD_PARTY.md to find that out after the fact.
            wrapped(theme::kTextFaint,
                    "Those are proprietary, under NVIDIA's own license rather than "
                    "an open-source one. Everything else is Apache-2.0, BSD or MIT. "
                    "The cpu and mlx builds pull in nothing proprietary.");
        }
        ImGui::TreePop();
    }

    ImGui::SetNextWindowSize(ImVec2(em(30.0F), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("Remove the training environment?", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        wrapped(theme::kTextDim,
                "The folder goes, and with it the base models it downloaded while "
                "training. Recipes and finished models are somewhere else and stay "
                "where they are.");
        ImGui::Dummy(ImVec2(0, em(0.5F)));
        if (ImGui::Button("Remove", ImVec2(em(7.0F), 0))) {
            std::string error;
            if (!pyenv::remove(error)) {
                say(error);
            } else {
                say("the training environment was removed");
            }
            refresh_pyenv(true);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Keep", ImVec2(em(7.0F), 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

}  // namespace crucible::gui
