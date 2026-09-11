// SPDX-License-Identifier: MIT
#include "crucible/ui/settings/line_editor.hpp"

#include <algorithm>
#include <utility>

#include "crucible/util/text.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

void LineEditor::begin(std::string initial) {
    active_ = true;
    buffer_ = std::move(initial);
    cursor_ = buffer_.size();
}

void LineEditor::cancel() {
    active_ = false;
    buffer_.clear();
    cursor_ = 0;
}

bool LineEditor::handle(const Event& event) {
    if (event == Event::ArrowLeft) {
        cursor_ = detail::utf8_prev(buffer_, cursor_);
        return true;
    }
    if (event == Event::ArrowRight) {
        cursor_ = detail::utf8_next(buffer_, cursor_);
        return true;
    }
    if (event == Event::Home || event == Event::CtrlA) {
        cursor_ = 0;
        return true;
    }
    if (event == Event::End || event == Event::CtrlE) {
        cursor_ = buffer_.size();
        return true;
    }
    if (event == Event::CtrlU) {
        buffer_.clear();
        cursor_ = 0;
        return true;
    }
    if (event == Event::CtrlW) {
        // Delete the previous path component. Skip any trailing separators
        // first, so a second press keeps walking up rather than stalling on
        // the slash the first press left behind.
        std::size_t cut = cursor_;
        while (cut > 0 && (buffer_[cut - 1] == '/' || buffer_[cut - 1] == ' ')) {
            --cut;
        }
        while (cut > 0 && buffer_[cut - 1] != '/' && buffer_[cut - 1] != ' ') {
            --cut;
        }
        buffer_.erase(cut, cursor_ - cut);
        cursor_ = cut;
        return true;
    }
    if (event == Event::Backspace) {
        if (cursor_ > 0) {
            const std::size_t previous = detail::utf8_prev(buffer_, cursor_);
            buffer_.erase(previous, cursor_ - previous);
            cursor_ = previous;
        }
        return true;
    }
    if (event == Event::Delete) {
        if (cursor_ < buffer_.size()) {
            buffer_.erase(cursor_, detail::utf8_next(buffer_, cursor_) - cursor_);
        }
        return true;
    }
    if (event.is_character()) {
        buffer_.insert(cursor_, event.character());
        cursor_ += event.character().size();
        return true;
    }
    return false;
}

Element LineEditor::render() const {
    // Split around the cursor so it can be drawn where it actually is. At the
    // end of the buffer there is no character to invert, so a space stands in.
    const std::size_t cut = std::min(cursor_, buffer_.size());
    const std::string before = buffer_.substr(0, cut);
    const std::string under  = cut < buffer_.size()
        ? buffer_.substr(cut, detail::utf8_next(buffer_, cut) - cut)
        : std::string(" ");
    const std::string after  = cut < buffer_.size()
        ? buffer_.substr(detail::utf8_next(buffer_, cut))
        : std::string();

    // xframe plus focus scrolls the line so the cursor stays on screen; for a
    // long path being corrected, that means the end stays visible.
    return hbox({
        text(before),
        text(under) | inverted,
        text(after),
    }) | xframe | ftxui::focus;
}

}  // namespace crucible::ui
