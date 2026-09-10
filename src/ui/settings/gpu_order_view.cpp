// SPDX-License-Identifier: MIT
//
// The GPU priority panel. See gpu_order_view.hpp for what it is for.
#include "crucible/ui/settings/gpu_order_view.hpp"

#include <algorithm>

#include "crucible/ui/theme.hpp"
#include "crucible/util/format.hpp"

using namespace ftxui;  // NOLINT(google-build-using-namespace)

namespace crucible::ui {

void GpuOrderView::open(const std::vector<int>& order) {
    // The rearranging itself lives in runtime/devices.cpp: it is a fact about
    // device lists, not about this screen, and it is testable there.
    gpus_ = apply_priority_order(gpu_devices(), order);
    selected_ = 0;
    holding_.reset();
    status_.clear();
    open_ = true;
}

std::vector<int> GpuOrderView::order() const {
    // One GPU has no order worth storing, and writing a one-element list into
    // the config would make "priority" look configured when it means nothing.
    if (gpus_.size() < 2) {
        return {};
    }
    std::vector<int> indices;
    indices.reserve(gpus_.size());
    for (const ComputeDevice& gpu : gpus_) {
        indices.push_back(gpu.index);
    }
    return indices;
}

GpuOrderAction GpuOrderView::handle(const Event& event) {
    if (!open_) {
        return GpuOrderAction::None;
    }

    if (event == Event::Escape || event == Event::Character('q')) {
        // Escape while holding a card puts it back rather than leaving: it is
        // the same key that cancels every other half-finished action here.
        if (holding_.has_value()) {
            holding_.reset();
            status_ = "put it back";
            return GpuOrderAction::None;
        }
        open_ = false;
        return GpuOrderAction::Close;
    }

    if (event == Event::ArrowUp || event == Event::Character('k')) {
        if (selected_ > 0) {
            --selected_;
        }
        return GpuOrderAction::None;
    }
    if (event == Event::ArrowDown || event == Event::Character('j')) {
        if (selected_ + 1 < gpus_.size()) {
            ++selected_;
        }
        return GpuOrderAction::None;
    }

    if (event == Event::Return) {
        if (gpus_.empty()) {
            return GpuOrderAction::None;
        }
        if (!holding_.has_value()) {
            holding_ = selected_;
            status_  = "moving " + gpus_[selected_].label() +
                      " -- enter on the card whose place it should take";
            return GpuOrderAction::None;
        }

        const std::size_t from = *holding_;
        holding_.reset();
        if (from == selected_) {
            status_ = "left where it was";
            return GpuOrderAction::None;
        }

        // Swap, not insert. Swapping is the one rearrangement whose result is
        // obvious before you press the key: the two cards you can see trade
        // places and nothing else on the list moves.
        std::swap(gpus_[from], gpus_[selected_]);
        status_ = gpus_[selected_].label() + " is now " +
                  (selected_ == 0 ? "first" : "#" + std::to_string(selected_ + 1));
        return GpuOrderAction::Apply;
    }

    return GpuOrderAction::None;
}

Element GpuOrderView::render_gpu(std::size_t position) const {
    const ComputeDevice& gpu = gpus_[position];
    const bool selected = position == selected_;
    const bool held     = holding_.has_value() && *holding_ == position;

    // The rank is the whole point of the screen, so it leads the row.
    std::string rank = std::to_string(position + 1) + ".";
    rank.resize(3, ' ');

    // Highlighted rows need the readable shade of gray: kMeta is the same
    // color as the highlight itself.
    const bool lit = selected || held;

    Elements meta;
    meta.push_back(text("  ·  device " + std::to_string(gpu.index)) | color(meta_color(lit)));
    if (!gpu.backend.empty()) {
        meta.push_back(text("  ·  " + gpu.backend) | color(meta_color(lit)));
    }

    Element row = hbox({
        text(held ? " ↕ " : (selected ? " ▸ " : "   ")),
        text(rank) | color(position == 0 ? Color(theme::kSeatActive) : meta_color(lit)),
        text(gpu.label()) | bold,
        hbox(std::move(meta)),
    });

    if (held) {
        row = row | color(theme::kNotice) | bgcolor(theme::kHighlight);
    } else if (selected) {
        row = row | bgcolor(theme::kHighlight);
    }
    return row;
}

Element GpuOrderView::render() const {
    Elements body{
        hbox({
            text(" gpu priority order ") | bold | color(theme::kFlame),
            text("· first card is filled first") | color(theme::kMeta) | dim,
        }),
        separator(),
    };

    if (gpus_.empty()) {
        body.push_back(text("   no GPUs are visible") | color(theme::kMeta));
        body.push_back(text("   install a CUDA or Vulkan runtime first") |
                       color(theme::kMeta) | dim);
    } else {
        Elements rows;
        for (std::size_t i = 0; i < gpus_.size(); ++i) {
            rows.push_back(render_gpu(i));
        }
        body.push_back(vbox(std::move(rows)));

        if (gpus_.size() < 2) {
            body.push_back(separator());
            body.push_back(text("   only one GPU, so there is nothing to order") |
                           color(theme::kMeta) | dim);
        }
    }

    if (!status_.empty()) {
        body.push_back(separator());
        body.push_back(paragraph(status_) | color(theme::kNotice));
    }

    body.push_back(separator());
    body.push_back(text(holding_.has_value()
                            ? "enter drops it into place   ·   esc puts it back"
                            : "↑↓ choose   enter pick up   esc back") |
                   color(theme::kMeta) | dim);

    return vbox(std::move(body)) | border | bgcolor(theme::kPanel) | clear_under;
}

}  // namespace crucible::ui
