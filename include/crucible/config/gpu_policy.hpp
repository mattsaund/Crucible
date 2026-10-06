// SPDX-License-Identifier: MIT
//
// Turning the GPU split setting into the numbers llama.cpp wants.
//
// Kept apart from Config because it needs the device list, which does not
// exist until a runtime has been loaded -- so this runs on the engine thread
// at load time, not when the config is parsed.
#pragma once

#include <string>
#include <vector>

#include "crucible/config/config.hpp"

namespace crucible {

/// Translate the machine-wide GPU settings into per-model load parameters.
///
/// Two independent things happen here:
///
///   * The memory policy -- GpuConfig::gpu_only and ::vram_only -- is stamped
///     onto every model, whatever the split mode is. Whether the processor
///     does any of the computing is not a question about how many cards there
///     are.
///   * `tensor_split` and `main_gpu` are filled in from the split mode. That
///     part is a no-op in "auto" mode, which leaves any hand-written
///     `tensor_split` in the config file exactly as it was.
///
/// Returns a one-line description of the split that was applied, for the log
/// and the settings screen, or an empty string when the split was left alone.
std::string apply_gpu_policy(Config& config);

/// Re-plan one model's split against the video memory that is free right now.
///
/// apply_gpu_policy runs once, at startup, against a device list nothing has
/// been loaded onto yet. By the time an expert is actually loaded that picture
/// is out of date twice over: the delegator has taken a gigabyte, and the
/// desktop's own use of the display card moves by more than that on its own.
/// A plan made from the old numbers hands a card more than it still has, and
/// the load fails at the last allocation with the whole model already uploaded.
///
/// So this runs immediately before every load, from ModelHost, which is the one
/// place every load goes through. Returns the same one-line description
/// apply_gpu_policy does, or an empty string when the mode leaves splits alone.
std::string refresh_gpu_split(ModelParams& params, const GpuConfig& gpu);

/// Put the delegator on the one card it belongs on.
///
/// Not a split: the delegator is a small model that lives for the whole session
/// and answers in sixteen tokens, so dividing it buys nothing and costs a
/// pipeline hop per decision. What matters more is that a delegator spread over
/// three cards takes a bite out of all three, which is exactly the memory an
/// expert needs to be whole in -- and the expert is the model that will not fit
/// if anything goes wrong.
///
/// The card is the last one in the priority order, since that is the one
/// experts reach last. Without a priority order it is `main_gpu`. In "auto"
/// mode it does nothing at all: that mode is the user handing the decision to
/// llama.cpp, and this is a decision.
void place_delegator(ModelParams& params, const GpuConfig& gpu);

/// Every card the delegator could have to itself, in the order to try them:
/// the one place_delegator names, then the others by free memory, most first.
/// "One card only" offers only that card, and "auto" none -- both are the user
/// saying where models go. Empty with fewer than two cards.
///
/// Because "a small model" is an assumption, not a rule: a delegator can be
/// a 20B, and pinned to a 12 GB card it does not fit while a 16 GB one beside
/// it sits there with room.
std::vector<int> delegator_cards(const GpuConfig& gpu);

/// Put a model wholly on one card, by device index.
void pin_to_card(ModelParams& params, int device);

}  // namespace crucible
