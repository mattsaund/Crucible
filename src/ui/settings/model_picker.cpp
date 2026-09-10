// SPDX-License-Identifier: MIT
#include "crucible/ui/settings/model_picker.hpp"

#include "crucible/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

void ModelPicker::open(std::vector<ModelFile> models, const std::string& current,
                       std::string title) {
    active_ = true;
    models_ = std::move(models);
    title_  = std::move(title);
    index_  = 0;

    for (std::size_t i = 0; i < models_.size(); ++i) {
        if (models_[i].name == current) {
            index_ = i + 1;  // slot 0 is "(none)"
            break;
        }
    }
}

std::optional<std::string> ModelPicker::handle(const Event& event) {
    const std::size_t count = models_.size() + 1;

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        index_ = (index_ + count - 1) % count;
        return std::nullopt;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        index_ = (index_ + 1) % count;
        return std::nullopt;
    }
    if (event == Event::Return) {
        active_ = false;
        if (index_ == 0 || index_ - 1 >= models_.size()) {
            return std::string{};  // "(none)": clear the seat
        }
        // Store the bare file name, so the config stays portable if the models
        // directory later moves.
        return models_[index_ - 1].name;
    }
    if (event == Event::Escape) {
        active_ = false;
    }
    return std::nullopt;
}

Element ModelPicker::render(const std::string& models_dir) const {
    Elements items;

    const auto entry = [&](std::size_t slot, const std::string& label,
                           const std::string& note) {
        const bool on = slot == index_;
        Element line = hbox({
            text(on ? " > " : "   ") | color(theme::kAccent),
            text(label) | flex,
            // An explicit gap: flex collapses to nothing when the file name is
            // long, which would run the name straight into its size.
            text("  "),
            // kMeta is the same gray as the highlight below, so the note
            // has to brighten on the row that is selected or it disappears.
            text(note) | color(meta_color(on)),
        });
        if (!on) {
            return line;
        }
        // focus is what makes the enclosing yframe scroll. Without it the
        // selection simply walks off the bottom of a long list.
        return line | bgcolor(theme::kHighlight) | bold | ftxui::focus;
    };

    items.push_back(entry(0, "(none)", "leave this seat empty"));
    for (std::size_t i = 0; i < models_.size(); ++i) {
        items.push_back(entry(i + 1, models_[i].name, models_[i].size_label()));
    }

    if (models_.empty()) {
        items.push_back(text(" "));
        items.push_back(text("  No .gguf files in " + models_dir) | color(theme::kNotice));
        items.push_back(text("  Put models there, then press r to rescan.")
                        | color(theme::kMeta) | dim);
    }

    // The explicit background matters: dbox composites this over the settings
    // list, and any cell the dialog does not paint lets the text underneath
    // show through -- which reads as corrupted file names.
    return window(text(" Choose a model for " + title_ + " ") | bold | color(theme::kAccent),
                  vbox({
                      vbox(std::move(items)) | yframe | flex,
                      separator(),
                      text(" ↑↓ move   enter choose   esc cancel ") | color(theme::kMeta) | dim,
                  }))
        | size(WIDTH, LESS_THAN, 76) | size(HEIGHT, LESS_THAN, 22)
        | color(theme::kPanelText) | bgcolor(theme::kPanel) | clear_under | center;
}

}  // namespace crucible::ui
