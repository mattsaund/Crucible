// SPDX-License-Identifier: MIT
#include "crucible/ui/settings/directory_browser.hpp"

#include <algorithm>
#include <system_error>

#include "crucible/config/paths.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

void DirectoryBrowser::open(std::filesystem::path start) {
    active_             = true;
    wants_manual_entry_ = false;
    index_              = 0;
    path_               = std::move(start);

    std::error_code ec;
    while (!path_.empty() && !std::filesystem::is_directory(path_, ec)) {
        const std::filesystem::path parent = path_.parent_path();
        if (parent == path_) {
            break;
        }
        path_ = parent;
    }
    if (path_.empty() || !std::filesystem::is_directory(path_, ec)) {
        path_ = paths::expand_user("~");
    }
    rescan();
}

void DirectoryBrowser::rescan() {
    entries_.clear();
    hidden_count_ = 0;

    if (path_.has_parent_path() && path_.parent_path() != path_) {
        entries_.push_back({"..", path_.parent_path(), 0, true});
    }

    std::error_code ec;
    std::vector<Entry> children;
    for (std::filesystem::directory_iterator it(path_, ec), end; it != end;
         it.increment(ec)) {
        if (ec) {
            break;
        }
        std::error_code entry_ec;
        if (!it->is_directory(entry_ec) || entry_ec) {
            continue;
        }
        const std::string name = it->path().filename().string();
        const bool hidden = !name.empty() && name.front() == '.';
        if (hidden) {
            ++hidden_count_;
            if (!show_hidden_) {
                continue;
            }
        }
        children.push_back({name + "/", it->path(), scan_models(it->path()).size(), false});
    }

    std::sort(children.begin(), children.end(),
              [](const Entry& a, const Entry& b) { return a.label < b.label; });
    entries_.insert(entries_.end(), children.begin(), children.end());

    if (index_ >= entries_.size() + 1) {
        index_ = 0;
    }
}

void DirectoryBrowser::go_up() {
    const std::filesystem::path parent = path_.parent_path();
    if (!parent.empty() && parent != path_) {
        path_  = parent;
        index_ = 0;
        rescan();
    }
}

std::optional<std::filesystem::path> DirectoryBrowser::handle(const Event& event) {
    const std::size_t count = entries_.size() + 1;

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        index_ = (index_ + count - 1) % count;
        return std::nullopt;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        index_ = (index_ + 1) % count;
        return std::nullopt;
    }
    if (event == Event::Return) {
        // Slot 0 is "use this directory", so choosing is one keystroke from
        // wherever you have navigated to.
        if (index_ == 0) {
            active_ = false;
            return path_;
        }
        const std::size_t entry = index_ - 1;
        if (entry < entries_.size()) {
            path_  = entries_[entry].path;
            index_ = 0;
            rescan();
        }
        return std::nullopt;
    }
    if (event == Event::ArrowLeft || event == Event::Backspace
        || event == Event::Character('h')) {
        go_up();
        return std::nullopt;
    }
    if (event == Event::Character('~')) {
        path_  = paths::expand_user("~");
        index_ = 0;
        rescan();
        return std::nullopt;
    }
    if (event == Event::Character('.')) {
        // '.' rather than ctrl-. : terminals only encode ctrl with letters and
        // a few punctuation keys, and ctrl-. is not among them, so it would
        // arrive as a bare '.' or not at all. ctrl-h, the other convention, is
        // Backspace here and already means "up".
        show_hidden_ = !show_hidden_;
        index_       = 0;
        rescan();
        return std::nullopt;
    }
    if (event == Event::Character('e')) {
        // Escape hatch for paths easier pasted than navigated to, such as a
        // network mount outside home.
        active_             = false;
        wants_manual_entry_ = true;
        return std::nullopt;
    }
    if (event == Event::Escape) {
        active_ = false;
    }
    return std::nullopt;
}

Element DirectoryBrowser::render() const {
    Elements items;

    const std::size_t here = scan_models(path_).size();
    const auto line = [&](std::size_t slot, const std::string& label,
                          const std::string& note, Color tint) {
        const bool on = slot == index_;
        Element row = hbox({
            text(on ? " > " : "   ") | color(theme::kAccent),
            text(label) | color(tint) | flex,
            text("  "),
            // kMeta is the same gray as the highlight below, so the note
            // has to brighten on the row that is selected or it disappears.
            text(note) | color(meta_color(on)),
        });
        if (!on) {
            return row;
        }
        return row | bgcolor(theme::kHighlight) | bold | ftxui::focus;
    };

    items.push_back(line(0, "[ use this directory ]",
                         here == 0 ? "no models here"
                                   : std::to_string(here) + (here == 1 ? " model" : " models"),
                         here > 0 ? theme::kSeatActive : theme::kNotice));
    items.push_back(separator());

    for (std::size_t i = 0; i < entries_.size(); ++i) {
        const Entry& entry = entries_[i];
        // The model count is the whole point of browsing: it tells you which
        // folder is the one you meant without opening each in turn.
        const std::string note = entry.is_parent || entry.models == 0
            ? std::string{}
            : std::to_string(entry.models) + (entry.models == 1 ? " model" : " models");
        items.push_back(line(i + 1, entry.label, note,
                             entry.models > 0 ? theme::kSeatActive : theme::kUser));
    }

    if (entries_.empty()) {
        items.push_back(text("   (no subdirectories)") | color(theme::kMeta) | dim);
    }

    // Say plainly that something is being withheld, and which key reveals it --
    // otherwise a folder full of dotfiles simply looks empty.
    Element hidden_hint = text("");
    if (show_hidden_) {
        hidden_hint = text(" . hide dotfiles ") | color(theme::kSeatActive);
    } else if (hidden_count_ > 0) {
        hidden_hint = text(" . show " + std::to_string(hidden_count_) + " hidden ")
                    | color(theme::kNotice);
    }

    return window(text(" Models directory ") | bold | color(theme::kAccent),
                  vbox({
                      text(" " + path_.string()) | color(theme::kMeta),
                      separator(),
                      vbox(std::move(items)) | yframe | flex,
                      separator(),
                      hbox({
                          // Kept tight: the hidden-files hint shares this row,
                          // and an overflowing footer truncates mid-word.
                          text(" ↑↓  enter open  ← up  ~ home  e type  esc ")
                              | color(theme::kMeta) | dim,
                          filler(),
                          hidden_hint,
                      }),
                  }))
        | size(WIDTH, LESS_THAN, 80) | size(HEIGHT, LESS_THAN, 24)
        | color(theme::kPanelText) | bgcolor(theme::kPanel) | clear_under | center;
}

}  // namespace crucible::ui
