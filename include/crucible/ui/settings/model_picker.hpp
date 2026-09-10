// SPDX-License-Identifier: MIT
//
// The dialog that assigns a GGUF to a seat.
//
// Lists what is actually in the models directory, with sizes, so choosing a
// model never means remembering a file name.
#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "crucible/llm/model_catalog.hpp"

namespace crucible::ui {

class ModelPicker {
public:
    /// Open over `models`, with the cursor on `current` if it is among them so
    /// re-opening shows the existing choice rather than the top of the list.
    /// `title` names the seat being filled.
    void open(std::vector<ModelFile> models, const std::string& current, std::string title);

    void close() { active_ = false; }
    bool active() const { return active_; }

    /// Apply one key.
    ///
    /// Returns the chosen model's file name once the user commits -- an empty
    /// string meaning "leave this seat empty" -- and nothing while the dialog
    /// is still open or was canceled.
    std::optional<std::string> handle(const ftxui::Event& event);

    ftxui::Element render(const std::string& models_dir) const;

private:
    bool                   active_ = false;
    std::string            title_;
    std::vector<ModelFile> models_;
    /// Slot 0 is "(none)"; models start at 1.
    std::size_t            index_ = 0;
};

}  // namespace crucible::ui
