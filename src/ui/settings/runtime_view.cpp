// SPDX-License-Identifier: MIT
//
// The Runtimes panel. See runtime_view.hpp for what it is for.
#include "crucible/ui/settings/runtime_view.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <string>

#include "crucible/config/paths.hpp"
#include "crucible/ui/theme.hpp"
#include "crucible/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {
namespace {

/// A short phrase for the state a runtime is in, and the color to say it in.
/// `highlighted` picks the readable shade for a row under the cursor.
std::pair<std::string, Color> state_of(const RuntimeStatus& runtime, bool highlighted) {
    // Before anything else: a module built against another llama.cpp will load
    // and then crash, so it does not matter what else is true of it.
    if (runtime.stale) {
        return {"built for llama.cpp " + runtime.llama_tag + " · press enter to rebuild",
                Color(theme::kError)};
    }
    if (runtime.active) {
        const std::string devices = std::to_string(runtime.device_count) +
                                    (runtime.device_count == 1 ? " device" : " devices");
        return {"active · " + devices, Color(theme::kSeatActive)};
    }
    if (runtime.installed) {
        // Installed but contributing nothing. Runtimes are registered the
        // moment they finish building, so this is not "not loaded yet" -- it
        // means the module loaded and found no hardware it can drive, which is
        // what a Vulkan install looks like on a machine with no Vulkan driver.
        return {"installed · no device", Color(theme::kSeatDormant)};
    }
    if (!runtime.buildable) {
        return {"not installed · " + runtime.blocker, meta_color(highlighted)};
    }
    return {"not installed", meta_color(highlighted)};
}

/// Is the CPU runtime installed? Duplicated from the builder rather than
/// shared, because here it is a question about what to tell the user and there
/// it is a question about what to compile.
bool cpu_installed() {
    for (const RuntimeStatus& runtime : RuntimeRegistry::scan()) {
        if (runtime.kind == BackendKind::Cpu) {
            return runtime.installed;
        }
    }
    return false;
}

}  // namespace

RuntimeView::RuntimeView(std::function<void()> wake) : wake_(std::move(wake)) {}

void RuntimeView::refresh() {
    runtimes_ = RuntimeRegistry::scan();
    devices_  = compute_devices();
    if (selected_ >= runtimes_.size()) {
        selected_ = runtimes_.empty() ? 0 : runtimes_.size() - 1;
    }
}

void RuntimeView::shutdown() {
    builder_.stop();
}

bool RuntimeView::take_activation() {
    const BuildProgress build = builder_.progress();
    const bool activated =
        build.phase == BuildProgress::Phase::Done && build.error.empty();

    if (!activated) {
        // Reset on anything else, so the next build reports itself too.
        activation_reported_ = false;
        return false;
    }
    if (activation_reported_) {
        return false;
    }
    activation_reported_ = true;
    // The device list is what changed; the panel is very likely on screen.
    refresh();
    return true;
}

void RuntimeView::start_install() {
    if (selected_ >= runtimes_.size()) {
        return;
    }
    const RuntimeStatus& runtime = runtimes_[selected_];
    const BackendInfo&   info    = backend_info(runtime.kind);

    if (!RuntimeRegistry::loadable_backends_supported()) {
        status_ = "this build has its backend compiled in; runtimes cannot be added";
        return;
    }
    if (!runtime.buildable) {
        const std::string hint = install_hint(info);
        status_ = hint.empty() ? runtime.blocker : runtime.blocker + ".  " + hint;
        return;
    }

    if (!builder_.start(runtime.kind, wake_)) {
        status_ = "a runtime is already being built";
        return;
    }

    // Say it up front rather than letting a second runtime appear in the list
    // unannounced. llama.cpp cannot load a model without the CPU backend even
    // when every layer is on a GPU, so this is not an optional extra.
    const bool also_cpu = runtime.kind != BackendKind::Cpu && !cpu_installed();
    status_ = "building " + std::string(info.name) +
              (also_cpu ? ", and CPU with it -- every runtime needs CPU" : "") +
              ".  You can keep using Crucible";
}

void RuntimeView::start_remove() {
    if (selected_ >= runtimes_.size()) {
        return;
    }
    const RuntimeStatus& runtime = runtimes_[selected_];

    std::string error;
    if (!RuntimeRegistry::remove(runtime.kind, error)) {
        status_ = error;
        return;
    }

    status_ = std::string(backend_info(runtime.kind).name) +
              " removed.  Takes effect on restart";
    refresh();

    // ggml cannot unload a backend that a loaded model may still be using, so
    // removal only takes effect on the next start -- which makes it the one
    // place where the list on screen and what Crucible can actually do come
    // apart, and the one place worth saying so.
    const bool nothing_left =
        std::none_of(runtimes_.begin(), runtimes_.end(),
                     [](const RuntimeStatus& left) { return left.installed; });
    if (nothing_left) {
        status_ = std::string(backend_info(runtime.kind).name) +
                  " removed.  No runtimes left, so models stop loading on restart";
    }
}

RuntimeAction RuntimeView::handle(const Event& event) {
    if (!open_) {
        return RuntimeAction::None;
    }

    const BuildProgress build = builder_.progress();

    if (event == Event::Escape || event == Event::Character('q')) {
        if (build.running()) {
            // Leaving the panel does not stop the build -- it runs on its own
            // thread and the settings screen reports it when you come back.
            status_ = "the build continues in the background";
        }
        open_ = false;
        return RuntimeAction::Close;
    }

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (selected_ > 0) { --selected_; }
        confirming_remove_ = false;
        return RuntimeAction::None;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (selected_ + 1 < runtimes_.size()) { ++selected_; }
        confirming_remove_ = false;
        return RuntimeAction::None;
    }

    if (event == Event::Character('r')) {
        refresh();
        status_ = "rescanned " + format::short_path(paths::runtimes_dir());
        return RuntimeAction::Notify;
    }

    if (event == Event::Character('c') && build.running()) {
        builder_.cancel();
        status_ = "canceling the build";
        return RuntimeAction::Notify;
    }

    if (event == Event::Return) {
        if (build.finished()) {
            // Enter on a finished build clears it and picks up the result.
            builder_.dismiss();
            refresh();
            return RuntimeAction::Notify;
        }
        if (build.running()) {
            status_ = "a build is already running -- press c to cancel it";
            return RuntimeAction::Notify;
        }
        confirming_remove_ = false;
        start_install();
        return RuntimeAction::Notify;
    }

    if (event == Event::Character('d') || event == Event::Delete) {
        if (build.running()) {
            status_ = "wait for the build to finish before removing a runtime";
            return RuntimeAction::Notify;
        }
        if (selected_ < runtimes_.size() && !runtimes_[selected_].installed) {
            status_ = "that runtime is not installed";
            return RuntimeAction::Notify;
        }
        if (!confirming_remove_) {
            confirming_remove_ = true;
            return RuntimeAction::Notify;
        }
        confirming_remove_ = false;
        start_remove();
        return RuntimeAction::Notify;
    }

    return RuntimeAction::None;
}

Element RuntimeView::render_runtime(const RuntimeStatus& runtime, bool selected) const {
    const BackendInfo& info = backend_info(runtime.kind);
    const auto [state_text, state_color] = state_of(runtime, selected);

    std::string name(info.name);
    name.resize(std::max<std::size_t>(name.size(), 8), ' ');

    // One line each. The blurb that used to sit under every row explained what
    // CPU, CUDA and Vulkan are to an audience that already knows -- three
    // paragraphs of gray to say what three words say.
    Elements row{
        text(selected ? " ▸ " : "   "),
        text(name) | bold,
        text(state_text) | color(state_color),
    };
    if (runtime.installed && runtime.bytes > 0) {
        row.push_back(text("  ·  " + runtime.size_label()) | color(meta_color(selected)));
    }
    if (!runtime.built_at.empty()) {
        row.push_back(text("  ·  built " + runtime.built_at) | color(meta_color(selected)));
    }

    Element line = hbox(std::move(row));
    return selected ? line | bgcolor(theme::kHighlight) : line;
}

Element RuntimeView::render_build() const {
    const BuildProgress build = builder_.progress();
    if (build.phase == BuildProgress::Phase::Idle) {
        return text("");
    }

    const BackendInfo& info = backend_info(build.kind);

    Elements lines;
    lines.push_back(hbox({
        text(" " + std::string(info.name) + ": ") | bold,
        text(build.label()),
    }));

    if (build.phase == BuildProgress::Phase::Compiling) {
        lines.push_back(hbox({
            text(" "),
            gauge(build.percent) | flex | color(theme::kFlameBusy),
            text(" " + build.step) | color(theme::kMeta) | dim,
        }));
    } else if (build.running()) {
        lines.push_back(hbox({
            text(" "),
            text(build.step) | color(theme::kMeta) | dim,
        }));
    }

    if (build.phase == BuildProgress::Phase::Failed) {
        lines.push_back(text(" " + build.error) | color(theme::kError));
        for (const std::string& line : build.log_tail) {
            lines.push_back(text("   " + line) | color(theme::kMeta) | dim);
        }
        lines.push_back(text(" full log: " + build.log_file.string()) |
                        color(theme::kMeta) | dim);
        lines.push_back(text(" enter dismisses this") | color(theme::kMeta) | dim);
    }

    if (build.phase == BuildProgress::Phase::Done) {
        // The builder registers what it made with ggml before reporting Done,
        // so unless that failed the runtime is live already. A restart is only
        // worth mentioning when it is actually needed.
        if (build.error.empty()) {
            lines.push_back(text(" ready to use") | color(theme::kSeatActive) | bold);
        } else {
            lines.push_back(text(" built, but " + build.error) | color(theme::kNotice));
            lines.push_back(text(" restart Crucible to load it") | color(theme::kNotice));
        }
        lines.push_back(text(" enter dismisses this") | color(theme::kMeta));
    }

    return vbox(std::move(lines)) | border;
}

Element RuntimeView::render_devices() const {
    if (devices_.empty()) {
        return text("   no compute devices -- no runtime is loaded") |
               color(theme::kMeta) | dim;
    }

    Elements lines;
    for (const ComputeDevice& device : devices_) {
        lines.push_back(hbox({
            text("   [" + std::to_string(device.index) + "] "),
            text(device.label()) | color(device.is_gpu ? Color(theme::kSeatDormant)
                                                       : Color(theme::kMeta)),
            text("  " + device.backend) | color(theme::kMeta) | dim,
        }));
    }
    return vbox(std::move(lines));
}

Element RuntimeView::render() const {
    Elements rows;
    for (std::size_t i = 0; i < runtimes_.size(); ++i) {
        rows.push_back(render_runtime(runtimes_[i], i == selected_));
    }

    std::string hint = "↑↓ choose   enter install   d remove   r rescan   esc back";
    Color hint_color = Color(theme::kMeta);
    if (confirming_remove_) {
        hint = "press d again to remove this runtime";
        hint_color = Color(theme::kError);
    } else if (builder_.progress().running()) {
        hint = "c cancels the build   ·   esc leaves it running in the background";
    }

    Elements body{
        hbox({
            // Pinned: without a fixed width FTXUI shrinks both children when
            // the path is long, and the first thing to go is the title.
            text(" runtimes ") | bold | color(theme::kFlame) | size(WIDTH, EQUAL, 10),
            text("· " + format::short_path(paths::runtimes_dir())) | color(theme::kMeta)
                | flex_shrink,
        }),
        separator(),
    };

    // The state every fresh install starts in. Without this the screen is a
    // list of three things that all say "not installed" and no indication that
    // one of them has to be.
    //
    // Read from the scan this screen already did, not from the directory: this
    // runs on every frame.
    const bool nothing_installed =
        std::none_of(runtimes_.begin(), runtimes_.end(),
                     [](const RuntimeStatus& runtime) { return runtime.installed; });
    if (nothing_installed) {
        body.push_back(text(" No runtime installed, so no model can load.") |
                       color(theme::kNotice));
        body.push_back(text(" Pick one and press enter; it is compiled here, "
                            "which takes a few minutes.") |
                       color(theme::kMeta));
        body.push_back(separator());
    }

    body.push_back(vbox(std::move(rows)));
    if (builder_.progress().phase != BuildProgress::Phase::Idle) {
        body.push_back(render_build());
    }
    body.push_back(separator());
    body.push_back(text(" devices llama.cpp can see") | color(theme::kMeta));
    body.push_back(render_devices());

    if (!status_.empty()) {
        body.push_back(separator());
        body.push_back(paragraph(status_) | color(theme::kNotice));
    }
    body.push_back(separator());
    body.push_back(text(hint) | color(hint_color) | dim);

    return vbox(std::move(body)) | border | bgcolor(theme::kPanel) | clear_under;
}

}  // namespace crucible::ui
