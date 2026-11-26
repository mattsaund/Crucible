// SPDX-License-Identifier: MIT
//
// How hard a model thinks, and whether: what each one takes, and what it is
// sent. See llm/reasoning.hpp.
//
// The templates below are cut down from the real ones they are named after,
// each to the part that decides: Qwen 3.8's refuses an effort it does not
// know and reads "high" as "xhigh", Inkling 2 Mini's maps seven words to a
// number, gpt-oss's writes whatever it is given into its system message, and
// LFM2.5's opens a thinking block at the start of every reply.
#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include "crucible/llm/reasoning.hpp"
#include "crucible/llm/remote_model.hpp"

namespace {

using crucible::reasoning::Profile;
using crucible::reasoning::Thinking;
using json = nlohmann::json;
namespace wire = crucible::remote::detail;

/// ChatML, as most open models' templates are written.
constexpr const char* kPlain = R"({%- for message in messages -%}
{{- '<|im_start|>' + message.role + '\n' + message.content + '<|im_end|>\n' -}}
{%- endfor -%}
{%- if add_generation_prompt -%}{{- '<|im_start|>assistant\n' -}}{%- endif -%})";

/// Qwen 3.8's: a switch for thinking, and three efforts, "high" read as
/// "xhigh" and anything else refused.
constexpr const char* kQwen = R"({%- if enable_thinking is undefined or enable_thinking is true %}
    {%- set effort = reasoning_effort|default('xhigh') %}
    {%- if effort == 'high' %}{%- set effort = 'xhigh' %}{%- endif %}
    {%- if effort not in ('xhigh', 'medium', 'low') %}
        {{- raise_exception('Unexpected reasoning effort ' ~ effort) }}
    {%- endif %}
    {{- '<|im_start|>system\nReasoning effort: ' + effort + '<|im_end|>\n' }}
{%- endif %}
{%- for message in messages -%}
{{- '<|im_start|>' + message.role + '\n' + message.content + '<|im_end|>\n' -}}
{%- endfor -%}
{%- if add_generation_prompt -%}{{- '<|im_start|>assistant\n' -}}
{%- if enable_thinking is defined and enable_thinking is false %}{{- '<think>\n\n</think>\n\n' }}{%- endif %}
{%- endif -%})";

/// Inkling 2 Mini's: seven words to a number, "none" among them.
constexpr const char* kInkling = R"({%- set effort_map = {"none": 0.0, "minimal": 0.1, "low": 0.2, "medium": 0.7, "high": 0.9, "xhigh": 0.99, "max": 0.99} -%}
{%- set eff = reasoning_effort if reasoning_effort is defined and reasoning_effort is not none else 0.9 -%}
{%- if eff is string -%}
  {%- if eff not in effort_map -%}{{- raise_exception("Unknown reasoning_effort: " ~ eff) -}}{%- endif -%}
  {%- set num = effort_map[eff] -%}
{%- else -%}{%- set num = eff | float -%}{%- endif -%}
{{- '<|im_start|>system\nThinking effort level: ' ~ num ~ '<|im_end|>\n' -}}
{%- for message in messages -%}
{{- '<|im_start|>' + message.role + '\n' + message.content + '<|im_end|>\n' -}}
{%- endfor -%}
{%- if add_generation_prompt -%}{{- '<|im_start|>assistant\n' -}}{%- endif -%})";

/// A template that writes any word into its system message, as gpt-oss's
/// harmony format does.
constexpr const char* kHarmony = R"({{- '<|start|>system<|message|>Reasoning: ' + (reasoning_effort|default('medium')) + '<|end|>' -}}
{%- for message in messages -%}
{{- '<|start|>' + message.role + '<|message|>' + message.content + '<|end|>' -}}
{%- endfor -%}
{%- if add_generation_prompt -%}{{- '<|start|>assistant<|channel|>' -}}{%- endif -%})";

}  // namespace

TEST(the_nearest_level_a_model_takes_stands_in_for_one_it_does_not) {
    using crucible::reasoning::nearest;
    const std::vector<std::string> gpt_oss{"low", "medium", "high"};
    CHECK_EQ(nearest(gpt_oss, "medium"), std::string("medium"));
    CHECK_EQ(nearest(gpt_oss, "max"), std::string("high"));
    CHECK_EQ(nearest(gpt_oss, "xhigh"), std::string("high"));
    CHECK_EQ(nearest(gpt_oss, "minimal"), std::string("low"));
    // Nothing asked, a model with no levels, or a word that is not one.
    CHECK_EQ(nearest(gpt_oss, ""), std::string());
    CHECK_EQ(nearest({}, "high"), std::string());
    CHECK_EQ(nearest(gpt_oss, "none"), std::string());
    // Of two as near, the higher: Qwen 3.8 reads "high" as its "xhigh".
    CHECK_EQ(nearest({"low", "medium", "xhigh"}, "high"), std::string("xhigh"));
}

TEST(a_template_that_reads_no_effort_and_has_no_thinking_offers_neither) {
    const Profile profile = crucible::reasoning::from_template(kPlain);
    CHECK(profile.levels.empty());
    CHECK(profile.thinking == Thinking::None);
    CHECK(!crucible::reasoning::from_template("").takes_effort());
}

TEST(qwens_template_offers_its_three_efforts_and_a_switch) {
    const Profile profile = crucible::reasoning::from_template(kQwen);
    // "high" is written as "xhigh", so it is that level by another name, and
    // the rest are refused.
    CHECK_EQ(profile.levels.size(), std::size_t{3});
    if (profile.levels.size() == 3) {
        CHECK_EQ(profile.levels[0], std::string("low"));
        CHECK_EQ(profile.levels[1], std::string("medium"));
        CHECK_EQ(profile.levels[2], std::string("xhigh"));
    }
    CHECK_EQ(profile.standard, std::string("xhigh"));
    CHECK(profile.thinking == Thinking::Optional);
    CHECK(!profile.off_is_none);
}

TEST(inklings_template_offers_every_level_and_takes_none_for_no_thinking) {
    const Profile profile = crucible::reasoning::from_template(kInkling);
    // xhigh and max are the same number there, and max is the one named.
    const std::vector<std::string> expected{"minimal", "low", "medium", "high", "max"};
    CHECK(profile.levels == expected);
    CHECK_EQ(profile.standard, std::string("high"));
    CHECK(profile.thinking == Thinking::Optional);
    CHECK(profile.off_is_none);
}

TEST(a_harmony_template_offers_gpt_oss_three_and_always_thinks) {
    const Profile profile = crucible::reasoning::from_template(kHarmony);
    const std::vector<std::string> expected{"low", "medium", "high"};
    CHECK(profile.levels == expected);
    CHECK_EQ(profile.standard, std::string("medium"));
    CHECK(profile.thinking == Thinking::Always);
}

TEST(claudes_models_api_says_which_levels_and_whether_thinking_switches) {
    const json opus = json::parse(R"({"effort": {"supported": true, "low": {"supported": true},
        "medium": {"supported": true}, "high": {"supported": true}, "xhigh": {"supported": true},
        "max": {"supported": true}},
        "thinking": {"supported": true, "types": {"adaptive": {"supported": true},
        "disabled": {"supported": false}, "between_tools": {"supported": false}, "enabled": {"supported": false}}}})");
    const Profile always = crucible::reasoning::from_anthropic(opus);
    CHECK_EQ(always.levels.size(), std::size_t{5});
    CHECK(always.thinking == Thinking::Always);

    const json haiku = json::parse(R"({"effort": {"supported": true, "low": {"supported": true},
        "medium": {"supported": true}, "high": {"supported": true}, "xhigh": null, "max": {"supported": false}},
        "thinking": {"supported": true, "types": {"adaptive": {"supported": true}, "disabled": {"supported": true}}}})");
    const Profile switched = crucible::reasoning::from_anthropic(haiku);
    const std::vector<std::string> three{"low", "medium", "high"};
    CHECK(switched.levels == three);
    CHECK(switched.thinking == Thinking::Optional);
    CHECK(switched.thinks_unasked);

    // A model that thinks only on a budget it is given: the levels are how
    // large a budget, and it does not think unasked.
    const json budget = json::parse(R"({"effort": {"supported": false},
        "thinking": {"supported": true, "types": {"enabled": {"supported": true}, "disabled": {"supported": true}}}})");
    const Profile asked = crucible::reasoning::from_anthropic(budget);
    CHECK(!asked.levels.empty());
    CHECK(asked.thinking == Thinking::Optional);
    CHECK(!asked.thinks_unasked);
}

TEST(models_are_known_by_name_where_nothing_describes_them) {
    using crucible::reasoning::from_name;
    const Profile gpt = from_name("https://api.openai.com/v1", "gpt-5.5");
    CHECK(gpt.takes_effort());
    CHECK(gpt.off_is_none);
    CHECK(gpt.can_switch());
    CHECK(std::find(gpt.levels.begin(), gpt.levels.end(), "xhigh") != gpt.levels.end());
    CHECK(from_name("https://api.openai.com/v1", "gpt-4o").levels.empty());
    // gpt-oss wherever it is, through whoever serves it.
    CHECK_EQ(from_name("https://api.groq.com/openai/v1", "openai/gpt-oss-120b").levels.size(), std::size_t{3});
    // DeepSeek switches thinking with a field of its own.
    const Profile deepseek = from_name("https://api.deepseek.com/v1", "deepseek-flash");
    CHECK(deepseek.can_switch());
    CHECK(!deepseek.off_is_none);
    // A model nothing is known about is offered nothing.
    CHECK(from_name("http://192.168.1.9:8080/v1", "my-finetune").levels.empty());
    CHECK(from_name("http://192.168.1.9:8080/v1", "my-finetune").thinking == Thinking::None);
}

TEST(a_level_a_provider_refused_with_the_ones_it_takes_is_not_asked_for_again) {
    crucible::remote::Quirks quirks;
    CHECK(wire::adapt(quirks, "openai",
                      "Unsupported value: 'reasoning_effort' does not support 'xhigh' with this model. Supported "
                      "values are: 'low', 'medium', and 'high'."));
    const std::vector<std::string> three{"low", "medium", "high"};
    CHECK(quirks.efforts == three);
    CHECK(quirks.no_none);
    CHECK(!quirks.no_effort);
    // The same refusal again teaches nothing new, and the field goes.
    CHECK(wire::adapt(quirks, "openai",
                      "Unsupported value: 'reasoning_effort' does not support 'xhigh' with this model. Supported "
                      "values are: 'low', 'medium', and 'high'."));
    CHECK(quirks.no_effort);

    crucible::remote::Quirks gemini;
    CHECK(wire::adapt(gemini, "openai", "Invalid reasoning effort: valid values are high, low, medium, none"));
    CHECK(!gemini.no_none);
    CHECK_EQ(gemini.efforts.size(), std::size_t{3});
}

TEST(a_request_carries_the_level_the_model_takes_and_thinking_only_where_it_switches) {
    const std::vector<crucible::ChatMessage> messages{{"user", "Hello"}};
    crucible::ModelParams params;
    const auto body = [&](std::string_view endpoint, std::string_view model, std::string_view effort,
                          std::string_view thinking, const crucible::remote::Quirks& quirks = {}) {
        const crucible::ChatRequest request{messages, params, effort, thinking};
        return json::parse(wire::openai_body(model, request, {}, quirks, endpoint));
    };
    // OpenAI: the level nearest, and "none" for no thinking.
    CHECK_EQ(body("https://api.openai.com/v1", "gpt-5.5", "max", "")["reasoning_effort"].get<std::string>(),
             std::string("xhigh"));
    CHECK_EQ(body("https://api.openai.com/v1", "gpt-5.5", "high", "off")["reasoning_effort"].get<std::string>(),
             std::string("none"));
    // One that cannot be switched off is not told to.
    CHECK(!body("https://api.openai.com/v1", "o3", "", "off").contains("reasoning_effort"));
    // DeepSeek's switch, and the effort beside it while it thinks.
    const json deep = body("https://api.deepseek.com/v1", "deepseek-flash", "medium", "on");
    CHECK_EQ(deep["thinking"]["type"].get<std::string>(), std::string("enabled"));
    CHECK_EQ(deep["reasoning_effort"].get<std::string>(), std::string("high"));
    const json quiet = body("https://api.deepseek.com/v1", "deepseek-flash", "medium", "off");
    CHECK_EQ(quiet["thinking"]["type"].get<std::string>(), std::string("disabled"));
    CHECK(!quiet.contains("reasoning_effort"));
    // OpenRouter's one object.
    CHECK_EQ(body("https://openrouter.ai/api/v1", "openai/gpt-5.5", "low", "")["reasoning"]["effort"].get<std::string>(),
             std::string("low"));
    CHECK(!body("https://openrouter.ai/api/v1", "openai/gpt-5.5", "", "off")["reasoning"]["enabled"].get<bool>());
    // A Qwen on somebody's llama-server, switched through its template.
    CHECK(!body("http://192.168.1.9:8080/v1", "qwen3-32b", "", "off")["chat_template_kwargs"]["enable_thinking"]
               .get<bool>());
    // A level the provider refused, and the ones it said it takes.
    crucible::remote::Quirks learned;
    learned.efforts = {"low", "medium", "high"};
    CHECK_EQ(body("https://api.openai.com/v1", "gpt-6-astra", "max", "", learned)["reasoning_effort"].get<std::string>(),
             std::string("high"));
}

TEST(a_claude_is_switched_off_and_its_effort_kept_to_what_it_takes_without_thinking) {
    crucible::remote::ModelFacts facts;
    facts.effort            = true;
    facts.adaptive_thinking = true;
    facts.thinking_off      = true;
    facts.described         = true;
    facts.reasoning.levels  = {"low", "medium", "high", "xhigh", "max"};
    facts.reasoning.thinking = Thinking::Optional;
    const std::vector<crucible::ChatMessage> messages{{"user", "Hello"}};
    crucible::ModelParams params;
    const auto body = [&](std::string_view effort, std::string_view thinking) {
        const crucible::ChatRequest request{messages, params, effort, thinking};
        return json::parse(wire::anthropic_body("claude-haiku-5-5", request, facts, {}));
    };
    const json on = body("max", "");
    CHECK_EQ(on["thinking"]["type"].get<std::string>(), std::string("adaptive"));
    CHECK_EQ(on["output_config"]["effort"].get<std::string>(), std::string("max"));
    // Thinking off is refused above high, so the effort comes down to it.
    const json off = body("max", "off");
    CHECK_EQ(off["thinking"]["type"].get<std::string>(), std::string("disabled"));
    CHECK_EQ(off["output_config"]["effort"].get<std::string>(), std::string("high"));

    // A Claude that thinks on a budget is given one sized by the level, and
    // only when thinking is asked for.
    crucible::remote::ModelFacts older;
    older.thinking_budget = true;
    older.thinking_off    = true;
    older.described       = true;
    older.reasoning       = crucible::reasoning::from_anthropic(json::parse(
        R"({"thinking": {"supported": true, "types": {"enabled": {"supported": true}, "disabled": {"supported": true}}}})"));
    // A provider's seat sends no cap of its own (see expert.set), and the
    // budget is part of the reply's.
    params.max_tokens = -1;
    const crucible::ChatRequest asked{messages, params, "high", "on"};
    const json budget = json::parse(wire::anthropic_body("claude-sonnet-4-5", asked, older, {}));
    CHECK_EQ(budget["thinking"]["type"].get<std::string>(), std::string("enabled"));
    CHECK_EQ(budget["thinking"]["budget_tokens"].get<int>(), 16000);
    CHECK(!budget.contains("output_config"));
    const crucible::ChatRequest unasked{messages, params, "high", ""};
    CHECK(!json::parse(wire::anthropic_body("claude-sonnet-4-5", unasked, older, {})).contains("thinking"));
}
