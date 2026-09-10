// SPDX-License-Identifier: MIT
// Deciding which expert answers.
//
// Two implementations share one interface so the delegation logic never has to
// care which is in play: the delegator model (the real thing) and a keyword
// scorer (the fallback when no delegator GGUF is configured, which also keeps
// the whole UI usable with no models installed at all).
#pragma once

#include <memory>
#include <utility>
#include <vector>
#include <string>

#include "crucible/llm/model_host.hpp"
#include <memory>

#include "crucible/routing/expert.hpp"

namespace crucible {

/// Where a routing decision came from, so the UI can be honest about how much
/// to trust it.
enum class RouteSource {
    Model,      ///< the router model chose it
    Keyword,    ///< keyword scoring chose it
    Fallback,   ///< nothing chose it; policy substituted this one
    Forced,     ///< the user pinned a subject with a slash command
};

struct RouteDecision {
    /// Empty, because that is what "nobody has decided yet" means. A
    /// default-constructed decision reaches the engine whenever the delegator
    /// could not run at all, and defaulting to a real expert sent every one of
    /// those prompts to that expert as though it had been chosen.
    ///
    /// An id rather than an index into the roster: a decision outlives the
    /// roster it was made against -- it is written into the session history and
    /// read back weeks later -- and an index would then name whichever expert
    /// had since moved into that slot.
    ExpertId    expert;
    float       confidence = 0.0F;
    RouteSource source     = RouteSource::Fallback;
    std::string detail;     ///< short human-readable note for the status line
};

std::string_view route_source_name(RouteSource source);

/// The inverse, for reading a stored session back. An unrecognized name is
/// Fallback, which is the honest answer for "this came from somewhere we no
/// longer understand".
RouteSource route_source_from_name(std::string_view name);

/// What has to follow the assistant header before a scored label is the very
/// next token, for whatever chat format `rendered` is in.
///
/// Scoring a label means asking how likely the model is to write it *next*. For
/// most formats the assistant's turn begins with its answer and the answer is
/// next. For a reasoning format it does not: harmony opens the turn with a
/// channel marker, so `<|start|>assistant` is followed by `<|channel|>` and
/// never by a word. Nine subject names compared at that position are nine
/// things that all essentially cannot happen, and the ranking between them is
/// tokenization noise.
///
/// Measured: gpt-oss-20b as the delegator scored 13% -- barely above the 11%
/// that guessing gives -- and put 52 of 54 prompts in one seat. With the header
/// completed it reads the question instead.
///
/// Returns an empty string for a format that needs nothing, which is most of
/// them.
std::string answer_prefix(std::string_view rendered);

class Router {
public:
    virtual ~Router() = default;
    virtual RouteDecision route(const std::string& prompt, const CancelCallback& cancel) = 0;
};

/// Scores the prompt against per-subject keyword sets. No model, no latency,
/// fully deterministic -- but blind to anything the word list does not cover.
class KeywordRouter final : public Router {
public:
    explicit KeywordRouter(std::shared_ptr<const Roster> roster);

    RouteDecision route(const std::string& prompt, const CancelCallback& cancel) override;

private:
    std::shared_ptr<const Roster> roster_;
};

/// Asks the resident delegator which subject a prompt belongs to.
///
/// It does not generate an answer. It scores every subject tag as a
/// continuation of the same prompt and takes the best, which makes naming a
/// subject that does not exist impossible rather than merely unlikely, gives a
/// confidence worth thresholding on, and costs one forward pass instead of a
/// dozen sampled tokens.
///
/// The scores are then calibrated: see `bias_`.
class ModelRouter final : public Router {
public:
    /// `system_prompt_override` is for experiments; empty uses the one the
    /// roster generates.
    ModelRouter(LoadedModel& model, ModelParams params,
                std::shared_ptr<const Roster> roster,
                std::string system_prompt_override = {});

    RouteDecision route(const std::string& prompt, const CancelCallback& cancel) override;

    /// How much of the measured bias to subtract, in [0, 1]. 0 disables
    /// calibration entirely. Exposed for crucible-routebench, which sweeps it;
    /// the shipped value is `kCalibration`.
    void set_calibration(float strength) { calibration_ = strength; }

    /// The raw label scores for one prompt, uncalibrated, parallel to
    /// `Roster::experts()`. For crucible-routebench --explain, which is how a
    /// delegator that never picks one of the seats gets diagnosed.
    std::vector<float> raw_scores(const std::string& prompt);

    /// The measured bias, so it survives a reload of the same delegator.
    ///
    /// Calibration costs three forward passes, which is nothing once a session
    /// but is a third of the work again on every prompt when the delegator is
    /// loaded and freed each time. It depends on the model and the prompt,
    /// neither of which a reload changes.
    const std::vector<float>& bias() const { return bias_; }
    void set_bias(std::vector<float> bias) {
        bias_       = std::move(bias);
        calibrated_ = !bias_.empty();
    }

    /// How much of the measured bias to subtract.
    ///
    /// It used to matter a great deal. With one worked example per subject the
    /// sweep read 76% at 0, 87% at 0.5 and 78% at 1.0, and calibration was the
    /// difference between Philosophy being chosen once in fifty-four prompts
    /// and being chosen every time it should be.
    ///
    /// With two examples per subject the benchmark no longer separates them:
    /// 0, 0.25 and 0.5 all score 50/54. A better prompt left less bias to
    /// correct, which is the shape you would expect. So this is not a measured
    /// optimum any more -- it is the smaller of two corrections the evidence
    /// cannot tell apart, kept because a delegator with a differently shaped
    /// prior may still need it and the cost is three forward passes once.
    ///
    /// Subtracting all of the bias over-corrects either way: a subject the
    /// model rarely names has a small prior, and dividing by it magnifies noise
    /// as readily as signal. See ModelRouter::bias_.
    static constexpr float kCalibration = 0.25F;

private:
    /// Build the conversation the delegator sees: the instructions, the worked
    /// examples as real turns, and `question` as the last one.
    std::string conversation(const std::string& question) const;

    /// Measure and store `bias_`. Called once, lazily, on the first route.
    void calibrate();

    LoadedModel&  model_;
    ModelParams   params_;
    std::shared_ptr<const Roster> roster_;
    std::string   system_prompt_;
    std::vector<std::pair<std::string, std::string>> examples_;
    std::vector<ExpertId>    ids_;     ///< the seats, in roster order
    std::vector<std::string> labels_;  ///< their names, parallel to ids_

    /// What the delegator answers when it is asked nothing at all.
    ///
    /// A small model reading nine choices does not start from an even prior: it
    /// leans on whichever came first in the list, and on a 1.2B delegator that
    /// lean is strong enough to swallow six questions in nineteen. Measuring it
    /// against content-free input and subtracting it is the standard remedy --
    /// what is left is what the question itself moved.
    std::vector<float> bias_;
    float              calibration_ = kCalibration;
    bool               calibrated_  = false;

    KeywordRouter fallback_;
};

}  // namespace crucible
