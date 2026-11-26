// SPDX-License-Identifier: MIT
//
// See reasoning.hpp.
#include "crucible/llm/reasoning.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <mutex>
#include <optional>

#include <common/chat.h>
#include <nlohmann/json.hpp>

#include "crucible/util/format.hpp"

namespace crucible::reasoning {
namespace {

using json = nlohmann::json;

/// `model` without whose it is and in lower case: "openai/gpt-5.5" is
/// "gpt-5.5", "models/gemini-2.5-pro" is "gemini-2.5-pro".
std::string bare_name(std::string_view model) {
    std::string name = format::to_lower(model);
    if (const std::size_t slash = name.rfind('/'); slash != std::string::npos) {
        name.erase(0, slash + 1);
    }
    return name;
}

/// Whether `name` starts with `prefix` as a whole part of it: "o1" is not
/// "o10", and "gpt-5" is "gpt-5-mini" but not "gpt-5.5".
bool named(std::string_view name, std::string_view prefix) {
    if (name.rfind(prefix, 0) != 0) {
        return false;
    }
    const std::size_t end = prefix.size();
    return name.size() == end || prefix.back() == '-' || name[end] == '-' || name[end] == ':'
        || name[end] == '@' || name[end] == '_';
}

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/// A profile from a list of levels, in scale order whatever order they came.
Profile with_levels(std::vector<std::string> levels, Thinking thinking, std::string standard = {}) {
    std::sort(levels.begin(), levels.end(), [](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    levels.erase(std::unique(levels.begin(), levels.end()), levels.end());
    Profile profile;
    profile.levels   = std::move(levels);
    profile.thinking = thinking;
    profile.standard = std::move(standard);
    profile.source   = "name";
    return profile;
}

}  // namespace

const std::vector<std::string>& scale() {
    static const std::vector<std::string> levels{"minimal", "low", "medium", "high", "xhigh", "max"};
    return levels;
}

int rank(std::string_view level) {
    const std::vector<std::string>& all = scale();
    const auto found = std::find(all.begin(), all.end(), level);
    return found == all.end() ? -1 : static_cast<int>(found - all.begin());
}

std::string_view thinking_word(Thinking thinking) {
    switch (thinking) {
        case Thinking::Always:   return "always";
        case Thinking::Optional: return "optional";
        case Thinking::None:     break;
    }
    return "none";
}

std::string nearest(const std::vector<std::string>& levels, std::string_view wanted) {
    if (wanted.empty() || levels.empty()) {
        return {};
    }
    if (std::find(levels.begin(), levels.end(), wanted) != levels.end()) {
        return std::string(wanted);
    }
    const int asked = rank(wanted);
    if (asked < 0) {
        return {};
    }
    // The nearest on the scale, and of two as near the higher: Qwen 3.8 reads
    // "high" as its "xhigh", and someone asking for more thinking than a model
    // has a level for is better given more than less.
    std::string best;
    int         gap = 1 << 20;
    for (const std::string& level : levels) {
        const int distance = std::abs(rank(level) - asked);
        if (distance < gap || (distance == gap && rank(level) > rank(best))) {
            gap  = distance;
            best = level;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// A model on this machine: what its template does with each setting
// ---------------------------------------------------------------------------
//
// Read by rendering rather than by reading. A template is a program, and what
// a program does with a variable is found by running it: Qwen 3.8's raises an
// error for any effort but low, medium and xhigh (and takes high as xhigh),
// Inkling 2 Mini's maps seven words to a number, and gpt-oss's writes any word
// it is given into its system message. Each level of the scale is rendered as
// `reasoning_effort`; one the template refuses is not its, and one written
// the same as a higher one is that one under another name. A template that
// writes them all the same does not read the variable at all.
//
// Thinking is three questions. Whether the model thinks at all is llama.cpp's
// to say, by whether the reply it expects has a thinking block. Whether it can
// be told not to is whether switching enable_thinking off changes what the
// template writes (Qwen's), whether "none" is an effort it takes apart from the
// others (Inkling's), or whether the template opens a thinking block itself
// at the start of the reply, which Crucible shuts when thinking is off
// (LFM2.5's; see LoadedModel::open_chat). Otherwise it thinks whatever it is
// told. gpt-oss's harmony format always reasons.

Profile from_template(const std::string& source) {
    Profile profile;
    if (source.empty()) {
        return profile;
    }
    common_chat_templates_ptr templates;
    try {
        templates = common_chat_templates_init(nullptr, source, "", "");
    } catch (const std::exception&) {
        return profile;
    }
    if (!templates) {
        return profile;
    }

    // One moment for every render, so a template that writes today's date
    // writes the same one each time.
    const auto now = std::chrono::system_clock::now();
    const auto render = [&](const std::optional<std::string>& effort, bool thinking) -> std::optional<common_chat_params> {
        common_chat_templates_inputs inputs;
        common_chat_msg              hello;
        hello.role    = "user";
        hello.content = "Hello";
        inputs.messages              = {hello};
        inputs.add_generation_prompt = true;
        inputs.enable_thinking       = thinking;
        inputs.reasoning_format      = COMMON_REASONING_FORMAT_AUTO;
        inputs.now                   = now;
        if (effort) {
            inputs.chat_template_kwargs["reasoning_effort"] = json(*effort).dump();
        }
        try {
            return common_chat_templates_apply(templates.get(), inputs);
        } catch (const std::exception&) {
            return std::nullopt;
        }
    };

    const std::optional<common_chat_params> plain = render(std::nullopt, true);
    if (!plain) {
        return profile;
    }
    const std::string written = plain->prompt.str();
    const bool        harmony = contains(source, "<|channel|>");

    // --- effort ------------------------------------------------------------
    std::map<std::string, std::string> as_level;
    std::vector<std::string>           tried = scale();
    tried.insert(tried.begin(), "none");
    for (const std::string& level : tried) {
        if (const std::optional<common_chat_params> made = render(level, true)) {
            as_level[level] = made->prompt.str();
        }
    }
    bool differs = false;
    for (const auto& [level, text] : as_level) {
        differs = differs || text != written;
    }
    bool none_apart = false;
    if (differs) {
        for (const std::string& level : scale()) {
            const auto found = as_level.find(level);
            if (found == as_level.end()) {
                continue;   // refused: not one of its levels
            }
            // Harmony writes any word into "Reasoning: ...", and gpt-oss was
            // trained on three of them.
            if (harmony && level != "low" && level != "medium" && level != "high") {
                continue;
            }
            // The same as a higher level, which is the one named.
            bool same_as_higher = false;
            for (int above = rank(level) + 1; above < static_cast<int>(scale().size()); ++above) {
                const auto higher = as_level.find(scale()[static_cast<std::size_t>(above)]);
                same_as_higher = same_as_higher || (higher != as_level.end() && higher->second == found->second);
            }
            if (!same_as_higher || harmony) {
                profile.levels.push_back(level);
            }
        }
        for (const std::string& level : profile.levels) {
            if (as_level[level] == written) {
                profile.standard = level;
            }
        }
        if (const auto none = as_level.find("none"); none != as_level.end() && none->second != written && !harmony) {
            none_apart = std::none_of(profile.levels.begin(), profile.levels.end(),
                                      [&](const std::string& level) { return as_level[level] == none->second; });
        }
    }

    // --- thinking -------------------------------------------------------------
    const bool thinks = plain->supports_thinking || harmony || none_apart || !profile.levels.empty();
    if (thinks) {
        const std::optional<common_chat_params> off = render(std::nullopt, false);
        const bool switched = off && off->prompt.str() != written;
        const bool opened   = plain->supports_thinking && !plain->thinking_start_tag.empty()
                           && plain->generation_prompt.find(plain->thinking_start_tag) != std::string::npos
                           && std::none_of(plain->thinking_end_tags.begin(), plain->thinking_end_tags.end(),
                                           [&](const std::string& close) {
                                               return !close.empty()
                                                   && plain->generation_prompt.find(
                                                          close, plain->generation_prompt.rfind(plain->thinking_start_tag))
                                                          != std::string::npos;
                                           });
        profile.thinking    = harmony ? Thinking::Always
                            : switched || none_apart || opened ? Thinking::Optional
                                                               : Thinking::Always;
        profile.off_is_none = !switched && none_apart;
    }
    profile.source = "template";
    return profile;
}

// ---------------------------------------------------------------------------
// Claude, as its Models API describes it
// ---------------------------------------------------------------------------

Profile from_anthropic(const json& capabilities) {
    Profile profile;
    profile.source = "provider";
    if (!capabilities.is_object()) {
        return profile;
    }
    const auto supported = [](const json& node) {
        if (!node.is_object()) {
            return false;
        }
        const auto flag = node.find("supported");
        return flag != node.end() && flag->is_boolean() && flag->get<bool>();
    };
    const auto at = [](const json& node, const char* key) -> const json& {
        static const json kNothing;
        if (!node.is_object()) {
            return kNothing;
        }
        const auto found = node.find(key);
        return found == node.end() ? kNothing : *found;
    };
    const json& effort = at(capabilities, "effort");
    if (supported(effort)) {
        for (const std::string& level : scale()) {
            if (supported(at(effort, level.c_str()))) {
                profile.levels.push_back(level);
            }
        }
    }
    const json& thinking = at(capabilities, "thinking");
    const json& types    = at(thinking, "types");
    const bool  adaptive = supported(at(types, "adaptive"));
    const bool  budget   = supported(at(types, "enabled"));
    const bool  off      = supported(at(types, "disabled")) || supported(at(types, "between_tools"));
    if (supported(thinking) && (adaptive || budget)) {
        profile.thinking = off ? Thinking::Optional : Thinking::Always;
        // A model with adaptive thinking thinks unasked; one with only a
        // budget to set (Claude 4.5's Sonnet and Haiku, and before) does not,
        // and its levels are how large a budget it is given.
        profile.thinks_unasked = adaptive;
        if (!adaptive && profile.levels.empty()) {
            profile.levels = {"minimal", "low", "medium", "high", "max"};
        }
    }
    return profile;
}

Profile claude_by_name(std::string_view model) {
    const std::string name = bare_name(model);
    const auto either = [&name](std::string_view dashed, std::string_view dotted) {
        return contains(name, dashed) || contains(name, dotted);
    };
    // platform.claude.com/docs/en/build-with-claude/effort, 2026-11: five
    // levels on the 5.x models and Opus 4.7 and 4.8; max but no xhigh on the
    // 4.6 models; low to high on Opus 4.5. Thinking cannot be turned off on
    // Opus 5.5 or the Fable and Mythos models, and can on the rest at high
    // and below.
    const std::vector<std::string> five{"low", "medium", "high", "xhigh", "max"};
    if (either("opus-5-5", "opus-5.5")) {
        return with_levels(five, Thinking::Always, "medium");
    }
    if (either("haiku-5-5", "haiku-5.5")) {
        return with_levels(five, Thinking::Optional, "medium");
    }
    if (contains(name, "fable") || contains(name, "mythos")) {
        return with_levels(five, Thinking::Always, "high");
    }
    if (either("4-6", "4.6")) {
        return with_levels({"low", "medium", "high", "max"}, Thinking::Optional, "high");
    }
    if (either("opus-4-5", "opus-4.5")) {
        return with_levels({"low", "medium", "high"}, Thinking::Optional, "high");
    }
    if (either("4-7", "4.7") || either("4-8", "4.8") || either("opus-5", "opus-5") || either("sonnet-5", "sonnet-5")
        || either("haiku-5", "haiku-5")) {
        return with_levels(five, Thinking::Optional, "high");
    }
    // Claude 4.5's Sonnet and Haiku and everything before: thinking with a
    // budget, switched on by asking, and the levels are how large a budget.
    Profile p = with_levels({"minimal", "low", "medium", "high", "max"}, Thinking::Optional);
    p.thinks_unasked = false;
    return p;
}

// ---------------------------------------------------------------------------
// Everybody else, by name
// ---------------------------------------------------------------------------

Profile from_name(std::string_view endpoint, std::string_view model) {
    const std::string name = bare_name(model);
    const std::string where = format::to_lower(endpoint);
    const auto none_switch = [](Profile p, bool unasked = true) {
        p.thinking       = Thinking::Optional;
        p.off_is_none    = true;
        p.thinks_unasked = unasked;
        return p;
    };

    // Claude through a gateway that speaks OpenAI's shape (OpenRouter's
    // "anthropic/claude-..."), which normalizes the effort.
    if (named(name, "claude-") || contains(name, "claude-")) {
        return claude_by_name(name);
    }

    // OpenAI. developers.openai.com/api/docs/guides/reasoning, 2026-11: none
    // to max, model by model.
    const std::vector<std::string> to_max{"low", "medium", "high", "xhigh", "max"};
    if (named(name, "gpt-6")) {
        return none_switch(with_levels(to_max, Thinking::Always, "medium"));
    }
    if (named(name, "gpt-5.6")) {
        return none_switch(with_levels(to_max, Thinking::Always, "medium"));
    }
    if (named(name, "gpt-5.5-pro") || named(name, "gpt-5.2-pro")) {
        return with_levels({"low", "medium", "high", "xhigh"}, Thinking::Always, "high");
    }
    if (named(name, "gpt-5.5")) {
        return none_switch(with_levels({"minimal", "low", "medium", "high", "xhigh"}, Thinking::Always, "medium"));
    }
    if (named(name, "gpt-5.4") || named(name, "gpt-5.3") || named(name, "gpt-5.2")) {
        return none_switch(with_levels({"low", "medium", "high", "xhigh"}, Thinking::Always, "medium"));
    }
    if (named(name, "gpt-5.1-codex-max")) {
        return with_levels({"low", "medium", "high", "xhigh"}, Thinking::Always);
    }
    if (named(name, "gpt-5.1-codex") || named(name, "gpt-5-codex")) {
        return with_levels({"low", "medium", "high"}, Thinking::Always);
    }
    if (named(name, "gpt-5.1")) {
        // Its default is no reasoning at all.
        return none_switch(with_levels({"low", "medium", "high"}, Thinking::Always), false);
    }
    if (named(name, "gpt-5-pro")) {
        return with_levels({"high"}, Thinking::Always, "high");
    }
    if (named(name, "gpt-5-chat")) {
        return {};
    }
    if (named(name, "gpt-5")) {
        return with_levels({"minimal", "low", "medium", "high"}, Thinking::Always, "medium");
    }
    if (named(name, "o1-mini")) {
        return with_levels({}, Thinking::Always);
    }
    if (named(name, "o1") || named(name, "o3") || named(name, "o4-mini") || named(name, "o3-mini")) {
        return with_levels({"low", "medium", "high"}, Thinking::Always, "medium");
    }
    if (name.rfind("gpt-4", 0) == 0 || name.rfind("gpt-3", 0) == 0 || named(name, "chatgpt-") || contains(name, "embedding")
        || contains(name, "tts") || contains(name, "whisper") || contains(name, "dall-e") || contains(name, "image")) {
        return {};
    }
    if (contains(where, "api.openai.com")) {
        // A model at OpenAI newer than this table: every chat model it has
        // released since GPT-5 reasons and takes these three. A refusal
        // teaches the request otherwise.
        return with_levels({"low", "medium", "high"}, Thinking::Always, "medium");
    }

    // gpt-oss, wherever it is served: Groq, Together, Fireworks, OpenRouter,
    // somebody's llama-server.
    if (contains(name, "gpt-oss")) {
        return with_levels({"low", "medium", "high"}, Thinking::Always, "medium");
    }

    // Gemini, at its OpenAI door. ai.google.dev/gemini-api/docs/openai,
    // 2026-11: none only on 2.5 Flash, minimal on some Flash models.
    if (named(name, "gemini-2.5-flash")) {
        return none_switch(with_levels({"minimal", "low", "medium", "high"}, Thinking::Always));
    }
    if (named(name, "gemini-2.5")) {
        return with_levels({"minimal", "low", "medium", "high"}, Thinking::Always);
    }
    if (named(name, "gemini-3") || named(name, "gemini-4") || named(name, "gemini-")
        || contains(where, "generativelanguage.googleapis.com")) {
        // And any model at Gemini's door: the door turns the three into the
        // model's own thinking level whatever it is called.
        return with_levels({"low", "medium", "high"}, Thinking::Always);
    }

    // xAI. docs.x.ai, 2026-11: Grok 3 Mini takes low and high; the Grok 4.x
    // that reason take low to high, xhigh from 4.6; the "non-reasoning"
    // variants do not think at all.
    if (contains(name, "non-reasoning")) {
        return {};
    }
    if (named(name, "grok-3-mini")) {
        return with_levels({"low", "high"}, Thinking::Always, "low");
    }
    if (named(name, "grok-4.6") || named(name, "grok-4.7") || named(name, "grok-5")) {
        return with_levels({"low", "medium", "high", "xhigh"}, Thinking::Always, "high");
    }
    if (named(name, "grok-4.") || contains(name, "fast-reasoning")) {
        return with_levels({"low", "medium", "high"}, Thinking::Always, "high");
    }
    if (named(name, "grok-4") || named(name, "grok-build")) {
        return with_levels({}, Thinking::Always);
    }

    // DeepSeek. api-docs.deepseek.com/guides/thinking_mode, 2026-11: thinking
    // switched by a field of its own, and an effort of low, high or max.
    if (contains(where, "deepseek.com") || named(name, "deepseek-v4") || named(name, "deepseek-flash")
        || named(name, "deepseek-pro")) {
        if (named(name, "deepseek-chat")) {
            return {};
        }
        if (named(name, "deepseek-reasoner")) {
            return with_levels({}, Thinking::Always);
        }
        return with_levels({"low", "high", "max"}, Thinking::Optional, "high");
    }

    // Mistral's reasoning models: none or high.
    if (named(name, "magistral")) {
        return none_switch(with_levels({"high"}, Thinking::Always, "high"));
    }

    // Kimi's thinking models think, and the others do not.
    if (named(name, "kimi-") && contains(name, "thinking")) {
        return with_levels({}, Thinking::Always);
    }

    // An open model with a thinking switch, served by somebody: Qwen 3 and
    // its kin, which the servers that run them can be told to switch off.
    if (named(name, "qwen3") || named(name, "qwq") || contains(name, "deepseek-r1")) {
        Profile p = with_levels({}, named(name, "qwen3") ? Thinking::Optional : Thinking::Always);
        return p;
    }
    return {};
}

Asked asked_of(const Config& config, const ModelParams& seat) {
    Asked asked;
    asked.effort   = seat.effort.empty() ? config.reasoning_effort : seat.effort;
    asked.thinking = seat.thinking.empty() ? config.thinking : seat.thinking;
    return asked;
}

json to_json(const Profile& profile) {
    return json{{"levels", profile.levels},
                {"standard", profile.standard},
                {"thinking", std::string(thinking_word(profile.thinking))},
                {"thinks_unasked", profile.thinks_unasked},
                {"source", profile.source}};
}

}  // namespace crucible::reasoning
