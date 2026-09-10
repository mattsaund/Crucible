// SPDX-License-Identifier: MIT
//
// The expert panel.
//
// Crucible on the left with a dot of his own, and the experts in a column beside
// him with a dot each, drawn in roster order.
//
// A line joins Crucible's dot to whichever seat the delegation chose, for as long
// as work is flowing to it. The dots say what is happening rather than what is
// in memory: a seat lights up when it is given the turn and goes dark when the
// answer is finished, and Crucible's own dot is lit exactly when the delegator is
// loaded and waiting -- which, with the delegator set to load on demand, is not
// most of the time.
#include "crucible/ui/widgets/expert_panel.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "crucible/ui/theme.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace) -- DOM builders read far better unqualified

namespace crucible::ui {
namespace {

Color seat_color(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Active:       return theme::kSeatActive;
        case SeatPhase::Loading:      return theme::kSeatLoading;
        case SeatPhase::Dormant:      return theme::kSeatDormant;
        case SeatPhase::Missing:      return theme::kError;
        case SeatPhase::Unconfigured: break;
    }
    return theme::kSeatUnconfigured;
}

/// The marker to the left of a seat's name, which is how the ring reads at a
/// glance: filled means resident, hollow means on disk, a dot means no model.
std::string seat_marker(SeatPhase phase, std::size_t tick) {
    switch (phase) {
        case SeatPhase::Active:  return "◆";  // ◆ loaded and answering
        case SeatPhase::Dormant: return "◇";  // ◇ configured, on disk
        case SeatPhase::Loading: {
            static constexpr std::array<const char*, 4> kSpinner{{
                "◴", "◷", "◶", "◵"}};  // ◴◷◶◵
            return kSpinner[(tick / 2) % kSpinner.size()];
        }
        case SeatPhase::Missing:      return "✗";  // ✗ assigned, file not found
        case SeatPhase::Unconfigured: break;
    }
    return "·";  // · empty seat
}

/// One seat.
///
/// `narrow` drops the load percentage and the fixed width, for the one-line
/// strip where nine full-width chips would not fit in 80 columns.
Element seat(const Expert& expert, const SeatState& state, std::size_t tick,
             bool narrow = false) {
    // The tag is padded to a fixed width for the one-line strip, where the
    // chips are laid end to end and the padding is what lines them up. In the
    // ring the box is already a fixed width and the label is centered in it, so
    // padding there would only push the short tags half a column off center.
    std::string tag = expert.tag;
    if (narrow) {
        tag.resize(4, ' ');
    }
    std::string label = seat_marker(state.phase, tick) + " " + tag;
    if (state.phase == SeatPhase::Loading && !narrow) {
        const int percent = static_cast<int>(state.progress * 100.0F);
        label += " " + std::to_string(percent) + "%";
    }

    Element chip = text(label) | color(seat_color(state.phase));
    if (state.phase == SeatPhase::Active) {
        chip = chip | bold;
    }
    if (state.phase == SeatPhase::Unconfigured) {
        chip = chip | dim;
    }
    if (narrow) {
        return chip;
    }
    // In the ring, every seat reserves room for the widest label
    // ("◴ MATH 100%") so the layout does not jitter while a model loads -- and
    // the label is centered in that room rather than left-aligned in it. A
    // six-column chip in an eleven-column box reads as three columns further
    // left than it is, which was enough to make the whole ring look adrift of
    // the crucible it is supposed to be arranged around.
    return chip | hcenter | size(WIDTH, EQUAL, 11);
}

/// A snapshot taken before the first configure_seats has no roster. Drawing
/// nothing for one frame is better than a null dereference, and better than
/// pretending the shipped nine are there when the config may not have them.
const std::shared_ptr<const Roster>& kEmptyRoster() {
    static const std::shared_ptr<const Roster> empty =
        std::make_shared<const Roster>(Roster::bare());
    return empty;
}

/// Width of the gap the connector is drawn in, and where its vertical runs.
constexpr int kLinkWidth  = 8;
constexpr int kLinkColumn = 3;

/// One row of the connector between Crucible's dot and the chosen seat's.
///
/// An elbow: out from Crucible, down or up the column, then in to the seat. Drawn
/// a row at a time because that is how the rest of the panel is built, and the
/// three shapes it can take are the three cases below.
std::string link_row(int row, int from, int to) {
    // Box-drawing characters are multi-byte, so a row is assembled rather than
    // indexed.
    if (to < 0) {
        return std::string(static_cast<std::size_t>(kLinkWidth), ' ');  // nothing is running
    }
    const int lo = std::min(from, to);
    const int hi = std::max(from, to);

    std::string out;
    if (from == to && row == from) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += "─";
        }
        return out;
    }
    if (row == from) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i < kLinkColumn ? "─" : (i == kLinkColumn ? (to > from ? "┐" : "┘") : " ");
        }
        return out;
    }
    if (row == to) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i < kLinkColumn ? " " : (i == kLinkColumn ? (to > from ? "└" : "┌") : "─");
        }
        return out;
    }
    if (row > lo && row < hi) {
        for (int i = 0; i < kLinkWidth; ++i) {
            out += i == kLinkColumn ? "│" : " ";
        }
        return out;
    }
    return std::string(static_cast<std::size_t>(kLinkWidth), ' ');
}

}  // namespace

Element expert_panel(const Snapshot& snapshot, const FlameSprite& sprite, std::size_t tick,
                   bool compact) {
    // Drawn in roster order, which is the order the user put them in.
    const Roster& roster = snapshot.roster ? *snapshot.roster : *kEmptyRoster();
    const std::size_t rows = roster.size();

    // -1 is "nothing is running", which is what link_row draws a blank gap
    // for. A linked expert that is no longer on the roster lands here too: it
    // was ejected mid-turn, and there is no row left to draw a line to.
    int to_row = -1;
    if (snapshot.linked) {
        if (const std::optional<std::size_t> row = roster.find(*snapshot.linked)) {
            to_row = static_cast<int>(*row);
        }
    }

    // Crucible sits level with the middle of the list, so the connector reaches
    // as far up as it does down.
    const int sprite_row = (static_cast<int>(rows) - 1) / 2;

    // --- the seats, one per row ------------------------------------------
    Elements seat_rows;
    for (std::size_t i = 0; i < rows; ++i) {
        const Expert&    expert = roster.at(i);
        const SeatState& state  = i < snapshot.seats.size() ? snapshot.seats[i] : SeatState{};
        const bool       lit    = to_row == static_cast<int>(i);

        std::string label = expert.name;
        if (state.phase == SeatPhase::Loading) {
            label += " " + std::to_string(static_cast<int>(state.progress * 100.0F)) + "%";
        }

        Element name = text(label) | color(seat_color(state.phase));
        if (lit) {
            name = name | bold;
        }
        if (state.phase == SeatPhase::Unconfigured) {
            name = name | dim;
        }

        seat_rows.push_back(hbox({
            text(link_row(static_cast<int>(i), sprite_row, to_row))
                | color(to_row >= 0 ? theme::kSeatActive : theme::kMeta),
            text(seat_marker(state.phase, tick) + " ") | color(seat_color(state.phase)),
            std::move(name),
        }));
    }

    // --- Crucible, and his own dot -------------------------------------------
    //
    // The dot is lit when the delegator is loaded and waiting, which is exactly
    // when it can route the next prompt. With the delegator set to load on
    // demand it goes dark while an expert has the card, and that is the true
    // picture rather than a decoration.
    // The same diamond the seats use, and for the same reason: filled means
    // this one is doing something, hollow means it is there and waiting.
    const bool        ready = snapshot.delegator_ready && !snapshot.busy;
    const SeatPhase   phase = ready ? SeatPhase::Active : SeatPhase::Dormant;
    const std::string dot   = seat_marker(phase, tick);

    Elements sprite_lines;
    for (const std::string& line : sprite.render(snapshot.mood, tick, compact)) {
        sprite_lines.push_back(text(line) | color(mood_color(snapshot.mood)));
    }
    Element mark = vbox(std::move(sprite_lines));

    // The label goes on the row directly above the dot, so the two read as one
    // thing rather than as a heading over the whole column.
    //
    // Both are pushed to the right of the column, which is what puts the dot
    // against the line: the connector leaves from the cell immediately after
    // it, so its horizontal run meets the diamond's right vertex rather than
    // starting somewhere out in the gap.
    Elements dot_column;
    for (int row = 0; row < static_cast<int>(rows); ++row) {
        if (row == sprite_row - 1) {
            dot_column.push_back(
                hbox({filler(), text("Crucible") | color(theme::kFlame) | bold}));
        } else if (row == sprite_row) {
            dot_column.push_back(
                hbox({filler(), text(dot) | color(seat_color(phase))}));
        } else {
            dot_column.push_back(text(" "));
        }
    }

    // All three columns are centered against each other, not just the mark.
    //
    // The mark is a fixed seven rows however hard the flame is burning, and a
    // roster is however many experts you have -- so one of the two is always
    // shorter than the panel. Centring only the mark left the seats hanging off
    // the top with the flame beside their tail, and the connector reaching down
    // out of a name into nothing.
    Element table = hbox({
        vbox({filler(), std::move(mark), filler()}),
        text("  "),
        // Wide enough for the name above the dot. It was six when the name was
        // six characters long; "Crucible" is eight, and a column sized to the
        // old name silently clipped it to "Crucib".
        vbox({filler(), vbox(std::move(dot_column)), filler()}) | size(WIDTH, EQUAL, 9),
        vbox({filler(), vbox(std::move(seat_rows)), filler()}),
    });

    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    if (compact || bubble.empty()) {
        return hbox({filler(), std::move(table), filler()});
    }
    return vbox({
        hbox({filler(), std::move(table), filler()}),
        text(bubble) | color(theme::kAccent) | hcenter,
    });
}

Element expert_strip(const Snapshot& snapshot, std::size_t tick) {
    const Roster& roster = snapshot.roster ? *snapshot.roster : *kEmptyRoster();

    Elements chips;
    for (std::size_t i = 0; i < roster.size(); ++i) {
        if (!chips.empty()) {
            chips.push_back(text("  "));
        }
        const SeatState& state = i < snapshot.seats.size() ? snapshot.seats[i] : SeatState{};
        chips.push_back(seat(roster.at(i), state, tick, /*narrow=*/true));
    }

    // Ten narrow chips come to about 76 columns, which leaves the mood very
    // little on an 80-column terminal -- and the seats are what the strip is
    // for. So the mood shrinks first: better a clipped mood than a clipped
    // expert panel. A roster with twenty seats on it will overflow, and the strip
    // is already the fallback layout for a terminal with no room; the ring
    // above is where a large list is meant to be read.
    const std::string bubble = thought_bubble(snapshot.mood, snapshot.status, tick);
    chips.push_back(filler());
    chips.push_back(text(" "));  // never let the mood touch the last chip
    chips.push_back(text(bubble.empty() ? std::string("( idle )") : bubble)
                    | color(mood_color(snapshot.mood)) | flex_shrink);

    return hbox(std::move(chips));
}

}  // namespace crucible::ui
