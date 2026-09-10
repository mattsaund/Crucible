// SPDX-License-Identifier: MIT
//
// The dialog `/newexpert` opens: two boxes, and nothing else.
//
// A name, and what the expert is trained in. Everything else a seat needs --
// its id, its chip, its keyword set, its worked examples for the delegator --
// is derived or generated, because those are things a person should not have
// to invent and a program can work out. Asking for two fields is the whole
// design: an expert you can add in fifteen seconds is one you will actually
// add.
#pragma once

#include <optional>
#include <string>

#include <ftxui/component/event.hpp>
#include <ftxui/dom/elements.hpp>

#include "crucible/routing/expert.hpp"
#include "crucible/ui/settings/line_editor.hpp"

namespace crucible::ui {

class ExpertForm {
public:
    /// What the form produced when the user committed it.
    struct Result {
        std::string name;
        std::string blurb;
    };

    /// Open on an empty form, cursor in the name box.
    void open();

    /// Reopen with the boxes already filled, for editing an existing seat.
    void open(const Expert& expert);

    void close();
    bool active() const { return active_; }

    /// Apply one key.
    ///
    /// Tab and the arrows move between the two boxes; Enter on the second one
    /// commits, and Enter on the first moves to the second rather than
    /// submitting a half-filled form -- pressing Enter after typing a name is
    /// what everyone does, and treating it as "done" would create an expert
    /// with no description, which is the one field that has to be there.
    ///
    /// Returns the filled form once committed, and nothing while it is still
    /// open or was canceled.
    std::optional<Result> handle(const ftxui::Event& event);

    /// Show why the last attempt was refused. The form stays open with what was
    /// typed still in it, because a name collision is fixed by editing the
    /// name, not by typing the whole thing again.
    void set_error(std::string message);

    ftxui::Element render() const;

private:
    /// Which box has the cursor. Two of them, so this is a bool in spirit, but
    /// the enum keeps the render and the key handling readable.
    enum class Field { Name, Blurb };

    bool        active_ = false;
    bool        editing_existing_ = false;
    Field       field_ = Field::Name;
    LineEditor  name_;
    LineEditor  blurb_;
    std::string error_;
};

}  // namespace crucible::ui
