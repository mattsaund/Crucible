// SPDX-License-Identifier: MIT
//
// The two routers.
//
// KeywordRouter needs no model at all and keeps Crucible usable with nothing
// installed. ModelRouter asks the delegator, and does it by scoring every
// subject rather than by generating one: an invalid answer is not merely
// unlikely, there is nowhere for it to come from.
//
// Both read their seats from the live roster rather than from a table compiled
// in, so an expert the user added a minute ago competes on equal terms with the
// nine that ship.
//
// Keyword matching is whole-word only. Substring matching sounds harmless until
// "ion" fires inside "function" and sends every programming question to
// Chemistry.
#include "crucible/routing/router.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <iterator>

namespace crucible {
namespace {

std::string to_lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool is_word_char(char c) {
    const auto byte = static_cast<unsigned char>(c);
    return (std::isalnum(byte) != 0) || c == '+' || c == '#' || c == '_';
}

/// Count whole-word occurrences of `needle`. Substring matching would let "ion"
/// fire inside "function", which is how naive keyword routers end up sending
/// programming questions to chemistry.
int count_occurrences(const std::string& haystack, std::string_view needle) {
    if (needle.empty()) {
        return 0;
    }
    int found = 0;
    std::size_t pos = haystack.find(needle);
    while (pos != std::string::npos) {
        const bool left_ok  = pos == 0 || !is_word_char(haystack[pos - 1]);
        const std::size_t after = pos + needle.size();
        const bool right_ok = after >= haystack.size() || !is_word_char(haystack[after]);
        if (left_ok && right_ok) {
            ++found;
        }
        pos = haystack.find(needle, pos + 1);
    }
    return found;
}

}  // namespace

std::string_view route_source_name(RouteSource source) {
    switch (source) {
        case RouteSource::Model:    return "router model";
        case RouteSource::Keyword:  return "keywords";
        case RouteSource::Forced:   return "pinned";
        case RouteSource::Fallback: break;
    }
    return "fallback";
}

RouteSource route_source_from_name(std::string_view name) {
    if (name == "router model") { return RouteSource::Model; }
    if (name == "keywords")     { return RouteSource::Keyword; }
    if (name == "pinned")       { return RouteSource::Forced; }
    return RouteSource::Fallback;
}

std::string answer_prefix(std::string_view rendered) {
    // Harmony, as llama.cpp's built-in formatter writes it: the assistant turn
    // is opened and left there. The channel has to be named before a message
    // can begin, and `final` is the channel an answer goes on.
    constexpr std::string_view kHarmonyOpen = "<|start|>assistant";
    if (rendered.size() >= kHarmonyOpen.size() &&
        rendered.compare(rendered.size() - kHarmonyOpen.size(), kHarmonyOpen.size(),
                         kHarmonyOpen) == 0) {
        return "<|channel|>final<|message|>";
    }
    return {};
}

// ---------------------------------------------------------------------------
// KeywordRouter
// ---------------------------------------------------------------------------

KeywordRouter::KeywordRouter(std::shared_ptr<const Roster> roster)
    : roster_(std::move(roster)) {
    if (!roster_) {
        roster_ = std::make_shared<const Roster>(Roster::bare());
    }
}

RouteDecision KeywordRouter::route(const std::string& prompt, const CancelCallback& /*cancel*/) {
    const std::string haystack = to_lower(prompt);
    const std::vector<Expert>& experts = roster_->experts();

    // A seat with no keywords simply scores zero and is never chosen by this
    // router, which is the honest outcome: it has told the keyword scorer
    // nothing to match on.
    std::vector<int> scores(experts.size(), 0);
    int total = 0;
    for (std::size_t i = 0; i < experts.size(); ++i) {
        int score = 0;
        for (const std::string& word : experts[i].keywords) {
            if (word.empty()) {
                continue;
            }
            score += count_occurrences(haystack, to_lower(word));
        }
        scores[i] = score;
        total += score;
    }

    RouteDecision decision;
    if (scores.empty()) {
        decision.detail = "no experts configured";
        return decision;
    }

    const auto best = std::max_element(scores.begin(), scores.end());
    const int best_score = *best;

    if (best_score == 0) {
        // Nothing matched, so there is no decision to report. An empty expert
        // is what "nobody chose" means; the route policy decides where that
        // goes.
        decision.expert.clear();
        decision.confidence = 0.0F;
        decision.source     = RouteSource::Fallback;
        decision.detail     = "no expert keywords matched";
        return decision;
    }

    decision.expert = experts[static_cast<std::size_t>(
        std::distance(scores.begin(), best))].id;
    decision.source = RouteSource::Keyword;
    // Confidence is this seat's share of all matches, so a prompt that hits
    // three fields at once reports the genuine ambiguity instead of false
    // certainty. Capped below 1.0 -- keywords are never proof.
    decision.confidence = total > 0
        ? std::min(0.95F, static_cast<float>(best_score) / static_cast<float>(total))
        : 0.30F;
    decision.detail = std::to_string(best_score) + " keyword match"
                    + (best_score == 1 ? "" : "es");
    return decision;
}

// ---------------------------------------------------------------------------
// ModelRouter
// ---------------------------------------------------------------------------

ModelRouter::ModelRouter(LoadedModel& model, ModelParams params,
                         std::shared_ptr<const Roster> roster,
                         std::string system_prompt_override)
    : model_(model),
      params_(std::move(params)),
      roster_(roster ? std::move(roster)
                     : std::make_shared<const Roster>(Roster::bare())),
      fallback_(roster_) {
    system_prompt_ = system_prompt_override.empty() ? roster_->router_system_prompt()
                                                    : std::move(system_prompt_override);
    examples_ = roster_->router_examples();
    labels_   = roster_->router_labels();

    // Parallel to labels_, and built from the same list in the same order, so
    // the index the scorer returns cannot name a different seat than the label
    // it scored.
    ids_.reserve(roster_->size());
    for (const Expert& expert : roster_->experts()) {
        ids_.push_back(expert.id);
    }
}

std::string ModelRouter::conversation(const std::string& question) const {
    // The worked examples go in as real user/assistant turns. Pasting the same
    // examples into the system prompt instead measured 42% against 74% on the
    // 1.2B delegator, so the shape of the conversation matters more here than
    // the wording does.
    std::vector<ChatMessage> messages;
    messages.reserve(examples_.size() * 2 + 2);
    messages.push_back({"system", system_prompt_});
    for (const auto& [example, answer] : examples_) {
        messages.push_back({"user", example});
        messages.push_back({"assistant", answer});
    }
    messages.push_back({"user", question});

    // The label is scored as the very next token, so the prompt has to end
    // where the answer would begin. See answer_prefix.
    std::string rendered = model_.format_chat(messages, true);
    rendered += answer_prefix(rendered);
    return rendered;
}

void ModelRouter::calibrate() {
    calibrated_ = true;
    bias_.assign(labels_.size(), 0.0F);

    // Content-free questions: whatever the delegator answers to these is not
    // about the question, because there is no question. Several of them, so the
    // measurement is of the model's prior rather than of one odd string.
    const std::array<const char*, 3> nothing{{"N/A", "", "..."}};

    int measured = 0;
    for (const char* question : nothing) {
        const std::vector<float> scores =
            model_.score_labels(conversation(question), labels_, {});
        if (scores.size() != bias_.size() || scores.front() <= kUnscored) {
            continue;
        }
        for (std::size_t i = 0; i < scores.size(); ++i) {
            bias_[i] += scores[i];
        }
        ++measured;
    }
    if (measured == 0) {
        bias_.assign(labels_.size(), 0.0F);  // uncalibrated is better than wrong
        return;
    }
    for (float& value : bias_) {
        value /= static_cast<float>(measured);
    }
}

std::vector<float> ModelRouter::raw_scores(const std::string& prompt) {
    return model_.score_labels(conversation(prompt), labels_, {});
}

RouteDecision ModelRouter::route(const std::string& prompt, const CancelCallback& cancel) {
    if (labels_.empty()) {
        // Nothing to choose between. Scoring an empty label set is not a
        // degenerate case to handle further down -- it is a question with no
        // possible answer, and saying so here keeps every caller below simple.
        RouteDecision decision;
        decision.source = RouteSource::Fallback;
        decision.detail = "no experts on the roster";
        return decision;
    }
    if (!calibrated_) {
        calibrate();
    }

    const auto start = std::chrono::steady_clock::now();
    std::vector<float> scores = model_.score_labels(conversation(prompt), labels_, cancel);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    if (cancel && cancel()) {
        RouteDecision decision;
        decision.source = RouteSource::Fallback;
        decision.detail = "routing canceled";
        return decision;
    }
    if (scores.size() != labels_.size() || scores.front() <= kUnscored) {
        // The model could not be scored at all -- a decode failure, or a
        // context too small for the prompt. Keywords still work.
        RouteDecision decision = fallback_.route(prompt, cancel);
        decision.detail = "delegator could not be scored, used keywords";
        return decision;
    }

    // Take out what the delegator would have said to no question at all, then
    // read the rest as a distribution. See ModelRouter::bias_.
    for (std::size_t i = 0; i < scores.size(); ++i) {
        scores[i] -= calibration_ * bias_[i];
    }

    const auto best = static_cast<std::size_t>(
        std::distance(scores.begin(), std::max_element(scores.begin(), scores.end())));

    const float highest = scores[best];
    float       sum     = 0.0F;
    for (const float score : scores) {
        sum += std::exp(score - highest);
    }

    RouteDecision decision;
    decision.expert     = ids_[best];
    decision.confidence = sum > 0.0F ? std::clamp(1.0F / sum, 0.0F, 1.0F) : 0.0F;
    decision.source     = RouteSource::Model;
    decision.detail     = std::to_string(elapsed.count()) + "ms";
    return decision;
}

}  // namespace crucible
