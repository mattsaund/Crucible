// SPDX-License-Identifier: MIT
//
// Drawing the conversation: one block per turn, the notices above it, and the
// first-run guidance when there is nothing to show yet.
//
// Every turn carries the route line -- which expert answered, how confident the
// delegator was, and what the swap cost -- because that is the part of Crucible
// worth watching.
#include "crucible/ui/app.hpp"

#include <algorithm>
#include <filesystem>
#include <sstream>

#include "crucible/config/paths.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/ui/theme.hpp"
#include "crucible/ui/widgets/markdown_view.hpp"
#include "crucible/ui/widgets/scroll.hpp"
#include "crucible/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

Element App::render_turn(const Turn& turn) const {
    Elements block;

    block.push_back(hbox({
        text("you ") | color(theme::kUser) | bold,
        text("▸ ") | color(theme::kMeta),
        paragraph(turn.prompt) | flex,
    }));

    // The route line is the whole point of the expert panel made textual: which
    // expert took this turn, how sure Crucible was, and what the swap cost.
    if (turn.route) {
        const RouteDecision& route = *turn.route;
        std::string line = "⟶ " + expert_label(config_.roster, route.expert);
        line += " · " + format::number(route.confidence, 2);
        line += " · " + std::string(route_source_name(route.source));
        if (!route.detail.empty()) {
            line += " · " + route.detail;
        }
        if (turn.load_ms > 0) {
            line += " · swap " + format::duration_ms(turn.load_ms);
        }
        block.push_back(text(line) | color(theme::kRoute));
    }

    // What this turn did besides talk: a page it fetched, a file it rewrote, a
    // command it ran. Always shown, never folded away -- a local-first program
    // that reaches the internet or the disk owes the user a plain record of
    // when it did. Each line says what it was; there is no prefix to add.
    for (const std::string& line : turn.actions) {
        block.push_back(hbox({
            text("\u00b7 ") | color(theme::kRoute) | bold,
            paragraph(line) | color(theme::kRoute) | flex,
        }));
    }

    // A reasoning model's working, when there is any.
    //
    // Shown while it happens, because watching an expert think is the only
    // sign of life during the seconds before the answer starts -- and then put
    // away, because it is not the answer and reading it back is rarely what
    // anybody wants. "Show reasoning" in settings keeps it on screen for good.
    if (!turn.reasoning.empty() && (config_.ui.show_reasoning || turn.reply.empty())) {
        block.push_back(text("thinking") | color(theme::kMeta) | dim | bold);
        for (Element& line : render_markdown(turn.reasoning, /*dim_all=*/true)) {
            block.push_back(std::move(line));
        }
    }

    if (!turn.reply.empty()) {
        // Models answer in markdown whether or not they are asked to, so it is
        // drawn rather than printed. A failed turn is an error message, not a
        // document, and is left alone.
        if (turn.failed) {
            block.push_back(paragraph(turn.reply) | color(theme::kError));
        } else {
            for (Element& line : render_markdown(turn.reply)) {
                block.push_back(std::move(line));
            }
        }
    } else if (turn.streaming && turn.reasoning.empty()) {
        block.push_back(text("…") | color(theme::kMeta) | dim);
    }

    if (!turn.streaming && turn.output_tokens > 0) {
        std::string stats = std::to_string(turn.output_tokens) + " tok";
        if (turn.tokens_per_second > 0.0) {
            stats += " · " + format::number(turn.tokens_per_second, 1) + " tok/s";
        }
        if (turn.canceled) {
            stats += " · canceled";
        }
        block.push_back(text(stats) | color(theme::kMeta) | dim);
    }

    block.push_back(text(" "));
    return vbox(std::move(block));
}

Element App::render_welcome() const {
    const std::vector<ExpertId> configured = config_.configured_experts();
    if (!configured.empty()) {
        // Just "ready". The seat count that used to be here read "10 of 9
        // expert seats are filled" once a custom subject was added, which is
        // both wrong and a number nobody was waiting for.
        return vbox({
            text("Crucible is ready.") | color(theme::kMeta),
            text("Ask anything; Crucible picks the expert. /help lists the commands.")
                | color(theme::kMeta) | dim,
            text(" "),
        });
    }

    // First run. Two lines: what is missing, and the one key that fixes it.
    // Everything else this used to say -- where models go, what size delegator
    // to pick -- is said again in settings, at the moment it is actually
    // needed, and saying it twice only made the first screen a wall of text.
    return vbox({
        text("No expert models are assigned yet.") | color(theme::kNotice) | bold,
        text("Type /settings to assign one.") | color(theme::kMeta) | dim,
        text(" "),
    });
}

/// The color a step's verb takes.
///
/// Only the two that change something get heat. A cook is mostly reading and
/// thinking, and coloring all of it would make the two lines that matter --
/// the file it wrote, the command that failed -- impossible to find while
/// scrolling.
ftxui::Color step_color(const CookStep& step) {
    if (!step.ok) {
        return theme::kError;
    }
    if (step.kind == "write") {
        return theme::kSeatActive;
    }
    if (step.kind == "run" || step.kind == "done" || step.kind == "handoff") {
        return theme::kAccent;
    }
    return theme::kMeta;
}

Element App::render_cook(const Cook& cook) const {
    Elements rows;

    // The header answers the two questions someone watching a cook actually
    // has: what is it trying to do, and how much longer.
    std::string clock = format_duration(cook.duration());
    if (cook.budget_seconds > 0 && cook.state == CookState::Working) {
        const long left = cook.budget_seconds - cook.duration().count();
        clock += left > 0 ? "  ·  " + format_duration(std::chrono::seconds{left}) + " left"
                          : "  ·  time up";
    }

    rows.push_back(hbox({
        text("cook ") | color(theme::kAccent) | bold,
        text("▸ ") | color(theme::kMeta),
        paragraph(cook.goal) | flex,
    }));
    rows.push_back(hbox({
        text("     "),
        text(std::string(cook_state_name(cook.state))) | color(theme::kAccent),
        text("  ·  round " + std::to_string(cook.iterations)) | color(theme::kMeta),
        text("  ·  " + clock) | color(theme::kMeta),
    }));
    rows.push_back(text(" "));

    // The last few steps, not all of them. A cook runs to hundreds and the
    // transcript already scrolls; what belongs on screen is what it is doing
    // now. The whole record is in /cooks and on disk.
    constexpr std::size_t kVisible = 14;
    const std::size_t from = cook.steps.size() > kVisible ? cook.steps.size() - kVisible : 0;
    if (from > 0) {
        rows.push_back(text("     ... " + std::to_string(from) + " earlier steps")
                       | color(theme::kMeta) | dim);
    }
    for (std::size_t i = from; i < cook.steps.size(); ++i) {
        const CookStep& step = cook.steps[i];
        std::string verb = step.kind;
        verb.resize(8, ' ');
        rows.push_back(hbox({
            text("     "),
            text(verb) | color(step_color(step)),
            paragraph(step.summary) | color(step.ok ? theme::kPanelText : theme::kError) | flex,
        }));
    }

    if (cook.state == CookState::Asking && !cook.question.empty()) {
        rows.push_back(text(" "));
        rows.push_back(hbox({
            text("   ? ") | color(theme::kAccent) | bold,
            paragraph(cook.question) | bold | flex,
        }));
        rows.push_back(hbox({
            text("     "),
            text("type an answer and press enter") | color(theme::kMeta) | dim,
        }));
    }

    if (!cook.outcome.empty()) {
        rows.push_back(text(" "));
        rows.push_back(hbox({
            text("     "),
            paragraph(cook.outcome) | color(theme::kNotice) | flex,
        }));
    }

    const std::vector<std::string> files = cook.files_touched();
    const bool finished = cook.state == CookState::Done || cook.state == CookState::Stopped
                       || cook.state == CookState::Failed;
    if (!files.empty()) {
        rows.push_back(text(" "));
        std::string list;
        for (std::size_t i = 0; i < files.size(); ++i) {
            list += (i == 0 ? "" : ", ") + files[i];
        }
        rows.push_back(hbox({
            text("     changed  ") | color(theme::kMeta),
            paragraph(list) | color(theme::kSeatActive) | flex,
        }));
    } else if (finished) {
        // Said plainly, because the outcome above is the expert's account of
        // itself and this is the fact. They disagree more often than you would
        // like: a model that talked its way through an edit it never made will
        // write a confident summary of having made it, and the only thing that
        // catches that is the journal saying nothing was written.
        rows.push_back(text(" "));
        rows.push_back(hbox({
            text("     changed  ") | color(theme::kMeta),
            text("no files -- whatever it says above, nothing on disk moved")
                | color(theme::kError) | flex,
        }));
    }

    rows.push_back(text(" "));
    return vbox(std::move(rows));
}

Element App::render_transcript(const Snapshot& snapshot) const {
    Elements rows;

    if (!snapshot.notices.empty()) {
        Elements notices;
        for (const std::string& notice : snapshot.notices) {
            // paragraph, not text: several of these are long enough to be cut
            // off at the panel edge, and a truncated warning is a useless one.
            notices.push_back(hbox({
                text("• ") | color(theme::kNotice),
                paragraph(notice) | color(theme::kNotice) | dim | flex,
            }));
        }
        rows.push_back(vbox(std::move(notices)));
        rows.push_back(text(" "));
    }

    if (snapshot.turns.empty()) {
        rows.push_back(render_welcome());
    }

    for (const Turn& turn : snapshot.turns) {
        rows.push_back(render_turn(turn));
    }

    // Below the conversation, because it is the newest thing and the transcript
    // follows the bottom.
    // The edit gate. The terminal cannot put two files side by side in eighty
    // columns and stay readable, so it stacks them -- the question is the same
    // one and the answer is typed rather than clicked.
    if (snapshot.pending_edit) {
        const PendingEdit& edit = *snapshot.pending_edit;
        rows.push_back(text(" "));
        rows.push_back(hbox({
            text("   ? ") | color(theme::kAccent) | bold,
            text(edit.before.empty() ? "create " : "rewrite ") | bold,
            text(edit.path) | color(theme::kAccent) | bold,
        }));
        const auto listing = [&rows](const char* title, const std::string& body,
                                     ftxui::Color tint) {
            rows.push_back(hbox({text("     "), text(title) | color(tint) | bold}));
            std::size_t start = 0;
            int shown = 0;
            while (start <= body.size() && shown < 20) {
                const std::size_t end = body.find('\n', start);
                rows.push_back(hbox({
                    text("       "),
                    text(std::string(body.substr(
                        start, end == std::string::npos ? end : end - start)))
                        | color(theme::kMeta),
                }));
                ++shown;
                if (end == std::string::npos) {
                    break;
                }
                start = end + 1;
            }
        };
        if (!edit.before.empty()) {
            listing("now", edit.before, theme::kError);
        }
        listing("proposed", edit.after, theme::kSeatActive);
        rows.push_back(hbox({
            text("     "),
            text("type y to apply it, n to leave the file alone")
                | color(theme::kMeta) | dim,
        }));
    }

    if (snapshot.cook) {
        rows.push_back(render_cook(*snapshot.cook));
    }

    // Scrolling by exact lines. `focusPosition` puts the frame's focus at an
    // absolute line and the frame centers it, so the line that ends up at the
    // bottom of the window is the one asked for. Both heights come from the
    // previous frame -- see widgets/scroll.hpp for why that is enough.
    const int overflow = std::max(0, content_height_ - viewport_height_);
    const int scroll   = follow_ ? 0 : std::clamp(scroll_, 0, overflow);
    const int anchor   = overflow - scroll + viewport_height_ / 2;

    return measure_height(vbox(std::move(rows)), &content_height_)
         | focusPosition(0, anchor)
         | yframe
         | Decorator([this](Element child) {
               return measure_height(std::move(child), &viewport_height_);
           });
}

}  // namespace crucible::ui
