// SPDX-License-Identifier: MIT
//
// Routing: the roster, both routers, the policy that picks between them,
// and the naming rules a new expert has to satisfy.
#include "test_helpers.hpp"
#include "roster_fixture.hpp"

// ---------------------------------------------------------------------------
// The roster
// ---------------------------------------------------------------------------

TEST(the_fixture_roster_is_complete_and_unique) {
    const Roster roster = testing::sample_roster();
    CHECK_EQ(roster.size(), std::size_t{9});

    std::set<std::string> ids;
    std::set<std::string> tags;
    for (const Expert& expert : roster.experts()) {
        CHECK(!expert.id.empty());
        CHECK(!expert.name.empty());
        CHECK(!expert.blurb.empty());
        CHECK(!expert.tag.empty());
        CHECK(expert.tag.size() <= std::size_t{4});

        // Two seats sharing an id would make one of them unreachable by slash
        // command and would collide in the config map; two sharing a tag would
        // put the same chip on two rows of the panel.
        CHECK(ids.insert(expert.id).second);
        CHECK(tags.insert(expert.tag).second);
    }
}

TEST(every_shipped_expert_is_worked_and_none_is_a_catch_all) {
    const Roster roster = testing::sample_roster();
    for (const Expert& expert : roster.experts()) {
        // Two examples each and a keyword set each: both are what the two
        // routers actually read, and a seat missing either is a seat that
        // cannot be reached by that router.
        CHECK_EQ(expert.examples.size(), std::size_t{2});
        CHECK(!expert.keywords.empty());
    }

    // No built-in catch-all. There used to be a tenth seat the delegator was
    // forbidden from naming; a general-purpose expert is now something the user
    // adds and nominates in `routing.default_expert`.
    CHECK(!roster.find("fallback").has_value());
    CHECK_EQ(roster.size(), std::size_t{9});
}

TEST(roster_lookup_accepts_ids_tags_names_and_case) {
    const Roster roster = testing::sample_roster();
    const auto is = [&](const char* key, const char* id) {
        const std::optional<std::size_t> found = roster.find(key);
        return found.has_value() && roster.at(*found).id == id;
    };

    CHECK(is("physics",    "physics"));
    CHECK(is("PHYSICS",    "physics"));
    CHECK(is("PhYsIcS",    "physics"));
    CHECK(is("PHYS",       "physics"));
    CHECK(is("phys",       "physics"));
    CHECK(is("Physics",    "physics"));

    // Surrounding whitespace must not defeat a lookup: this is reached from a
    // typed slash command and from a config file someone hand-edited.
    CHECK(is("  biology ", "biology"));
    CHECK(is("BIO",        "biology"));

    CHECK(!roster.find("astrology").has_value());
    CHECK(!roster.find("").has_value());
    CHECK(!roster.find("   ").has_value());
}

TEST(every_shipped_expert_round_trips_through_its_own_strings) {
    const Roster roster = testing::sample_roster();
    for (const Expert& expert : roster.experts()) {
        const std::optional<std::size_t> by_id   = roster.find(expert.id);
        const std::optional<std::size_t> by_tag  = roster.find(expert.tag);
        const std::optional<std::size_t> by_name = roster.find(expert.name);
        CHECK(by_id.has_value() && roster.at(*by_id).id == expert.id);
        CHECK(by_tag.has_value() && roster.at(*by_tag).id == expert.id);
        CHECK(by_name.has_value() && roster.at(*by_name).id == expert.id);
    }
}

TEST(an_expert_needs_only_a_name_and_a_description) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name  = "Rust Async";
    expert.blurb = "tokio, futures, pinning, async traits, executor tuning";

    std::string error;
    CHECK(roster.add(expert, error));
    CHECK(error.empty());

    const std::optional<std::size_t> found = roster.find("rust-async");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& added = roster.at(*found);

    // Everything the user was not asked for is filled in.
    CHECK_EQ(added.id, std::string("rust-async"));
    CHECK_EQ(added.tag, std::string("RA"));   // initials, not the first four letters
    CHECK(!added.keywords.empty());

    // And the derived keywords are content words, not the whole description.
    CHECK(std::find(added.keywords.begin(), added.keywords.end(), "tokio")
          != added.keywords.end());
    CHECK(std::find(added.keywords.begin(), added.keywords.end(), "async")
          != added.keywords.end());
}

TEST(an_expert_without_a_description_is_refused) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name = "Vibes";

    // The blurb is the only thing the delegator routes on. A seat without one
    // is not a weak seat, it is an unreachable one, so this is refused at the
    // point of creation rather than accepted and left never chosen.
    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(!error.empty());
    CHECK(!roster.find("vibes").has_value());
}

TEST(a_name_that_is_already_taken_is_refused) {
    Roster roster = testing::sample_roster();
    Expert expert;
    expert.name  = "Physics";
    expert.blurb = "a second physics seat";

    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(error.find("Physics") != std::string::npos);
    CHECK_EQ(roster.size(), std::size_t{9});
}

TEST(a_name_with_nothing_to_slugify_is_refused) {
    Roster roster = Roster::bare();
    Expert expert;
    expert.name  = "!!!";
    expert.blurb = "punctuation only";

    std::string error;
    CHECK(!roster.add(expert, error));
    CHECK(!error.empty());
}

TEST(a_colliding_tag_is_broken_rather_than_duplicated) {
    Roster roster = testing::sample_roster();
    Expert expert;
    // "Mathematical Analysis" gives initials "MA", which is free -- so force
    // the collision with a single-word name whose first four letters are MATH.
    expert.name  = "Mathsy";
    expert.blurb = "a seat whose tag would otherwise clash with Mathematics";

    std::string error;
    CHECK(roster.add(expert, error));

    std::set<std::string> tags;
    for (const Expert& seat : roster.experts()) {
        // Two seats sharing a chip is a bug you only notice once the wrong one
        // lights up.
        CHECK(tags.insert(seat.tag).second);
    }
}

TEST(a_built_in_expert_can_be_ejected_like_any_other) {
    Roster roster = testing::sample_roster();
    std::string error;

    // The roster is the user's list. Nothing on it is protected.
    CHECK(roster.remove("chemistry", error));
    CHECK(!roster.find("chemistry").has_value());
    CHECK_EQ(roster.size(), std::size_t{8});

    // And removing something that is not there is an error, not a silent
    // success -- a typo in /ejectexpert should say so.
    CHECK(!roster.remove("chemistry", error));
    CHECK(!error.empty());
}

TEST(a_new_seat_goes_on_the_end_and_the_order_is_the_drawing_order) {
    Roster roster = testing::sample_roster();
    Expert expert;
    expert.name  = "Tax Law";
    expert.blurb = "deductions, filing, corporate structure, capital gains";

    std::string error;
    CHECK(roster.add(expert, error));

    // The expert panel draws the roster in order, so where a seat lands in the
    // list is where it lands on screen.
    CHECK_EQ(roster.at(roster.size() - 1).id, std::string("tax-law"));
    CHECK_EQ(roster.size(), std::size_t{10});
}

TEST(an_out_of_range_seat_reads_as_nobody) {
    const Roster roster = testing::sample_roster();
    // A handle can go stale between a seat being ejected and a turn in flight
    // noticing. Reading "nobody in particular" is true; reading whoever is at
    // index zero would attribute the work to Mathematics.
    CHECK(roster.at(9999).id.empty());
    CHECK(roster.at(9999).name.empty());
}

TEST(every_seat_can_be_ejected_including_the_last_one) {
    Roster roster = testing::sample_roster();
    std::string error;
    // Someone who ejects every expert meant to. An empty list is a state
    // the UI explains; quietly putting a seat back would be worse.
    while (roster.size() > 0) {
        CHECK(roster.remove(roster.at(0).id, error));
    }
    CHECK(roster.experts().empty());
    CHECK(roster.router_labels().empty());
    CHECK(roster.router_examples().empty());
}

TEST(expert_label_falls_back_to_the_id_it_was_given) {
    const Roster roster = testing::sample_roster();
    CHECK_EQ(expert_label(roster, "physics"), std::string("Physics"));
    // A session recorded against a seat that has since been ejected still has
    // to render. The id is the honest answer.
    CHECK_EQ(expert_label(roster, "rust-async"), std::string("rust-async"));
}

TEST(ids_and_tags_are_derived_the_way_the_dialog_promises) {
    CHECK_EQ(make_expert_id("Rust Async"),      std::string("rust-async"));
    CHECK_EQ(make_expert_id("  C++  Templates "), std::string("c-templates"));
    CHECK_EQ(make_expert_id("Physics"),         std::string("physics"));
    CHECK_EQ(make_expert_id("!!!"),             std::string(""));

    CHECK_EQ(make_expert_tag("Rust Async", {}), std::string("RA"));
    CHECK_EQ(make_expert_tag("Chemistry",  {}), std::string("CHEM"));
    // Four initials at most, so a long name still fits the chip.
    CHECK_EQ(make_expert_tag("One Two Three Four Five", {}), std::string("OTTF"));
}

TEST(derived_keywords_drop_the_words_that_would_match_everything) {
    const std::vector<std::string> words =
        derive_keywords("Tax Law", "how to handle the things that you should know about filing");

    // The keyword router scores by count of whole-word matches, so a list
    // holding "the" or "about" wins every prompt ever typed.
    for (const std::string& word : words) {
        CHECK(word.size() >= 4);
        CHECK(word != "the");
        CHECK(word != "about");
        CHECK(word != "should");
        CHECK(word != "handle");
        CHECK(word != "things");
    }
    CHECK(std::find(words.begin(), words.end(), "filing") != words.end());
}

TEST(worked_examples_are_parsed_out_of_whatever_the_model_replied_with) {
    // Every shape here is one a model actually produced when asked for two
    // questions and told not to decorate them.
    const std::vector<std::string> got = parse_examples(
        "Here are two questions:\n"
        "1. What preload should an M8 bolt take in aluminum?\n"
        "2) \"How do I damp a resonant bracket at 40Hz?\"\n"
        "- a third one that should not be kept\n");

    CHECK_EQ(got.size(), std::size_t{2});
    CHECK_EQ(got[0], std::string("What preload should an M8 bolt take in aluminum?"));
    CHECK_EQ(got[1], std::string("How do I damp a resonant bracket at 40Hz?"));
}

TEST(a_preamble_line_is_not_mistaken_for_a_question) {
    // A line ending in a colon is the model introducing its list, and a very
    // short line is not a question worth showing a delegator.
    const std::vector<std::string> got = parse_examples(
        "Sure, here you go:\nok\nWhat is the yield strength of 6061-T6 aluminum?\n");
    CHECK_EQ(got.size(), std::size_t{1});
    CHECK_EQ(got[0], std::string("What is the yield strength of 6061-T6 aluminum?"));
}

// ---------------------------------------------------------------------------
// Router labels
// ---------------------------------------------------------------------------

TEST(a_reasoning_format_is_asked_where_its_answer_actually_goes) {
    // The failure this exists for, and it is not a subtle one: a delegator
    // whose chat format opens the assistant turn with a channel marker was
    // being asked how likely each subject was as the very next token, at a
    // position where no word can occur at all. gpt-oss-20b scored 13% -- the
    // 11% that guessing gives -- and put 52 of 54 prompts in one seat.
    CHECK_EQ(answer_prefix("<|start|>user<|message|>hi<|end|><|start|>assistant"),
             "<|channel|>final<|message|>");

    // Formats whose assistant turn begins with the answer need nothing, which
    // is most of them.
    CHECK(answer_prefix("<|im_start|>assistant\n").empty());
    CHECK(answer_prefix("### Assistant:").empty());
    CHECK(answer_prefix("").empty());

    // And the header has to be at the end. One earlier in the conversation is a
    // turn that already happened.
    CHECK(answer_prefix("<|start|>assistant<|message|>hello<|return|>").empty());
}

TEST(router_labels_name_every_seat_in_roster_order) {
    const Roster roster = testing::sample_roster();
    const std::vector<std::string> labels = roster.router_labels();

    // The two are read in lockstep by ModelRouter: labels[i] is what it scores
    // for experts()[i]. A mismatch in length or order would route every prompt
    // to the wrong seat while looking entirely healthy.
    CHECK_EQ(labels.size(), roster.size());
    for (std::size_t i = 0; i < labels.size(); ++i) {
        CHECK_EQ(labels[i], roster.at(i).name);
    }
}

TEST(router_labels_carry_no_padding) {
    // A leading or trailing space changes how a label tokenizes, which would
    // score it at a position the model was never shown.
    for (const std::string& label : testing::sample_roster().router_labels()) {
        CHECK(!label.empty());
        CHECK(label.front() != ' ');
        CHECK(label.back() != ' ');
    }
}

TEST(the_delegator_can_only_answer_with_a_seat_that_exists) {
    const Roster roster = testing::sample_roster();
    const std::vector<std::string> labels = roster.router_labels();

    // Unrepresentable, not merely unlikely: the scorer compares exactly these
    // strings, so there is nowhere for an invented answer to come from.
    CHECK_EQ(labels.size(), roster.size());

    // And the worked examples answer with exactly those labels -- an example
    // that answered anything else would teach the model to produce a string
    // the scorer never asks about.
    for (const auto& [question, answer] : roster.router_examples()) {
        CHECK(std::find(labels.begin(), labels.end(), answer) != labels.end());
    }
}

TEST(router_system_prompt_describes_every_expert) {
    const Roster roster = testing::sample_roster();
    const std::string prompt = roster.router_system_prompt();
    for (const Expert& expert : roster.experts()) {
        CHECK(prompt.find(expert.name) != std::string::npos);
        CHECK(prompt.find(expert.blurb) != std::string::npos);
    }

    // The prompt must not offer a way to decline. An earlier version closed by
    // naming a catch-all and small models answered it for almost everything --
    // 16% accurate against 96% now -- so the wording that did it stays out, and
    // so does any seat the scorer cannot name.
    CHECK(prompt.find("fits no other") == std::string::npos);
    CHECK(prompt.find("Fallback")      == std::string::npos);
    CHECK(prompt.find("FALL")          == std::string::npos);
}

TEST(an_added_expert_reaches_the_delegator_without_anything_else_being_edited) {
    Roster roster = testing::sample_roster();
    Expert expert;
    expert.name     = "Tax Law";
    expert.blurb    = "deductions, filing, corporate structure, capital gains";
    expert.examples = {"can I deduct a home office", "when is capital gains tax due"};

    std::string error;
    CHECK(roster.add(expert, error));

    // This is the whole point of generating the delegator's inputs from the
    // list rather than storing them beside it: one call to add(), and the
    // labels, the system prompt and the worked examples all know about it.
    const std::vector<std::string> labels = roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Tax Law") != labels.end());
    CHECK(roster.router_system_prompt().find("deductions, filing") != std::string::npos);

    int examples = 0;
    for (const auto& [question, answer] : roster.router_examples()) {
        examples += answer == "Tax Law" ? 1 : 0;
    }
    CHECK_EQ(examples, 2);
}

TEST(an_ejected_expert_leaves_the_delegator_prompt_entirely) {
    Roster roster = testing::sample_roster();
    std::string error;
    CHECK(roster.remove("chemistry", error));

    const std::string prompt = roster.router_system_prompt();
    CHECK(prompt.find("Chemistry") == std::string::npos);
    CHECK(prompt.find("titration") == std::string::npos);

    const std::vector<std::string> labels = roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Chemistry") == labels.end());
    for (const auto& [question, answer] : roster.router_examples()) {
        CHECK(answer != "Chemistry");
    }
}

TEST(a_nominated_default_expert_catches_what_does_not_fit) {
    Config config;
    config.roster = testing::sample_roster();
    config.experts["physics"].model  = "p.gguf";
    config.experts["language"].model = "l.gguf";

    // No default nominated: an uncertain route is taken at face value, because
    // there is nothing better to do with it.
    RouteDecision proposed;
    proposed.expert     = "physics";
    proposed.confidence = 0.20F;
    proposed.source     = RouteSource::Model;
    RouteDecision out = apply_route_policy(proposed, config);
    CHECK_EQ(out.expert, ExpertId("physics"));
    CHECK(out.detail.find("undecided") != std::string::npos);

    // Nominate one, and it takes the uncertain route instead.
    config.routing.default_expert = "language";
    out = apply_route_policy(proposed, config);
    CHECK_EQ(out.expert, ExpertId("language"));
    CHECK(out.source == RouteSource::Fallback);
    CHECK(out.detail.find("Language") != std::string::npos);
}

TEST(a_default_expert_with_no_model_is_not_used) {
    Config config;
    config.roster = testing::sample_roster();
    config.experts["physics"].model = "p.gguf";
    // Nominated but never filled. Routing to it would turn a working prompt
    // into a "no model configured" failure one layer further down.
    config.routing.default_expert = "language";

    RouteDecision proposed;
    proposed.expert     = "chemistry";   // no model either
    proposed.confidence = 0.95F;
    proposed.source     = RouteSource::Model;

    const RouteDecision out = apply_route_policy(proposed, config);
    CHECK_EQ(out.expert, ExpertId("physics"));
    CHECK(out.detail.find("Chemistry has no model") != std::string::npos);
}

TEST(every_expert_gets_the_same_number_of_worked_examples) {
    const Roster roster = testing::sample_roster();
    const std::vector<std::pair<std::string, std::string>> examples = roster.router_examples();

    // The same number each, because a seat with more examples than its
    // neighbors is a seat the delegator is being nudged towards -- and the
    // nudge is invisible in the score until another seat stops being reachable.
    CHECK_EQ(examples.size(), roster.size() * 2);

    std::map<std::string, int> seen;
    const std::vector<std::string> labels = roster.router_labels();
    for (const auto& [question, answer] : examples) {
        CHECK(!question.empty());
        // The answer is exactly the label that gets scored, and nothing else:
        // an example demonstrating a longer answer would teach the delegator to
        // continue past the string the scorer measures.
        CHECK(std::find(labels.begin(), labels.end(), answer) != labels.end());
        ++seen[answer];
    }
    for (const Expert& expert : roster.experts()) {
        CHECK_EQ(seen[expert.name], 2);
    }
}

TEST(no_worked_example_is_a_benchmark_prompt) {
    // The examples go into the delegator's prompt and the benchmark measures
    // it. An example that is also a test case measures how well the prompt was
    // copied into the answer sheet, which is how a routing change can look like
    // an improvement while making nothing better.
    //
    // Paraphrases count, so this is not string equality. It is shared *rare*
    // words: two questions that both say "semicolon" and "comma" are the same
    // question, while two that both say "what is the difference between" merely
    // have the same shape. Rarity is measured against the benchmark itself, so
    // there is no list of stop words to keep up to date.
    std::map<std::string, int> appearances;
    const auto words = [](std::string_view text) {
        std::set<std::string> out;
        std::string word;
        for (const char c : text) {
            if (std::isalnum(static_cast<unsigned char>(c)) != 0) {
                word += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            } else if (!word.empty()) {
                out.insert(std::exchange(word, {}));
            }
        }
        if (!word.empty()) {
            out.insert(word);
        }
        return out;
    };

    for (const RouteCase& test : benchmark_cases()) {
        for (const std::string& word : words(test.prompt)) {
            ++appearances[word];
        }
    }

    for (const auto& [question, answer] : testing::sample_roster().router_examples()) {
        const std::set<std::string> example = words(question);
        for (const RouteCase& test : benchmark_cases()) {
            std::vector<std::string> rare;
            for (const std::string& word : words(test.prompt)) {
                if (appearances[word] <= 1 && example.count(word) > 0) {
                    rare.push_back(word);
                }
            }
            if (rare.size() >= 2) {
                std::printf("      example \"%s\"\n      reuses %s of benchmark \"%s\"\n",
                            question.c_str(), rare[0].c_str(), std::string(test.prompt).c_str());
            }
            CHECK(rare.size() < 2);
        }
    }
}

// ---------------------------------------------------------------------------
// Keyword router
// ---------------------------------------------------------------------------

TEST(keyword_router_picks_the_obvious_subject) {
    CHECK(route_of("compute the derivative of this polynomial") == "mathematics");
    CHECK(route_of("my code hits a segfault when I compile")    == "programming");
    CHECK(route_of("explain the lagrangian of this system")     == "physics");
    CHECK(route_of("balance this reaction and find the enthalpy") == "chemistry");
    CHECK(route_of("how does an enzyme change a protein")       == "biology");
    CHECK(route_of("what torque does this bearing take")        == "engineering");
    CHECK(route_of("is free will compatible with determinism")  == "philosophy");
    CHECK(route_of("how does migration reshape a community")    == "sociology");
    CHECK(route_of("proofread this paragraph for tone")         == "language");
}

TEST(keyword_router_names_nobody_when_nothing_matches) {
    KeywordRouter router(shipped());
    const RouteDecision decision = router.route("hello there", {});
    // An empty expert is what "no decision" means. Naming a seat here would be
    // inventing one, and the route policy is what decides where an undecided
    // prompt actually goes.
    CHECK(decision.expert.empty());
    CHECK(decision.source     == RouteSource::Fallback);
    CHECK(decision.confidence == 0.0F);
}

TEST(a_seat_with_no_keywords_never_wins_on_score) {
    // A prompt with real keywords goes to its expert.
    CHECK(route_of("compute the derivative of this polynomial") == "mathematics");
    CHECK(route_of("balance this reaction and find the enthalpy") == "chemistry");

    // A seat that gave the keyword scorer nothing to match on scores zero and
    // is never chosen by it -- which is the honest outcome, not a special case.
    Roster roster = testing::sample_roster();
    Expert quiet;
    quiet.name     = "Quiet";
    quiet.blurb    = "a seat with no keywords at all";
    quiet.keywords = {};  // add() would derive some, so they are cleared below
    std::string error;
    CHECK(roster.add(quiet, error));
    if (const std::optional<std::size_t> found = roster.find("quiet")) {
        Expert stripped = roster.at(*found);
        stripped.keywords.clear();
        CHECK(roster.update("quiet", stripped));
    }

    KeywordRouter router(std::make_shared<const Roster>(roster));
    CHECK(router.route("mmm", {}).expert.empty());
    CHECK(router.route("compute the derivative", {}).expert == "mathematics");
}

TEST(keyword_router_matches_whole_words_only) {
    // Keywords hide inside ordinary words: "ion" (Chemistry) sits in "question"
    // and "opinion", "cell" (Biology) sits in "excellent". None of them should
    // count, so this prompt matches nothing and nobody is named. Substring
    // matching would score Chemistry three times and route there.
    CHECK(route_of("an excellent question about your opinion").empty());

    // "gene" (Biology) hides inside both "generate" and "general".
    CHECK(route_of("generate a general overview").empty());

    // The same words as whole words must still match.
    CHECK(route_of("what is an ion")            == "chemistry");
    CHECK(route_of("describe a gene")           == "biology");
    CHECK(route_of("write a function to parse") == "programming");
}

TEST(keyword_router_confidence_reflects_ambiguity) {
    KeywordRouter router(shipped());
    const RouteDecision clear = router.route(
        "derivative integral polynomial theorem eigenvalue", {});
    const RouteDecision mixed = router.route(
        "derivative of the enzyme torque circuit", {});
    // A prompt pulling in four directions should not report the same certainty
    // as one that only ever points at maths.
    CHECK(clear.confidence > mixed.confidence);
    CHECK(clear.confidence <= 0.95F);
}

// ---------------------------------------------------------------------------
// Routing policy
//
// What happens to the delegator's answer. Extracted from the engine as a pure
// function precisely so these rules can be checked without loading a model.
// ---------------------------------------------------------------------------

namespace {

/// A config with the named seats filled, so policy tests read as a sentence.
/// The first is nominated as the default expert unless a test says otherwise --
/// that is the seat that plays the part the built-in Fallback used to.
Config config_with(std::initializer_list<ExpertId> filled) {
    Config config;
    // A fresh Config carries no experts at all now, so the roster these route
    // to is the test fixture rather than anything Crucible ships.
    config.roster = testing::sample_roster();
    for (const ExpertId& seat : filled) {
        config.experts[seat].model = "some-model.gguf";
    }
    return config;
}

RouteDecision proposal(ExpertId expert, float confidence, RouteSource source) {
    RouteDecision decision;
    decision.expert     = std::move(expert);
    decision.confidence = confidence;
    decision.source     = source;
    return decision;
}

}  // namespace

TEST(a_decision_nobody_made_names_nobody) {
    // A default-constructed decision is what the engine gets when the delegator
    // could not run -- no model assigned, or a load that failed. It must not
    // name a real expert: whatever it names is where the prompt goes, and
    // defaulting to a seat would send every such prompt there as though
    // something had chosen it.
    const RouteDecision nothing;
    CHECK(nothing.expert.empty());
    CHECK(nothing.source == RouteSource::Fallback);
    CHECK(nothing.confidence == 0.0F);
}

TEST(a_confident_route_to_a_filled_seat_stands) {
    const Config config = config_with({"physics", "language"});
    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Model);
}

TEST(an_unconfident_route_goes_to_the_nominated_default) {
    Config config = config_with({"physics", "language"});
    config.routing.min_confidence = 0.60F;
    config.routing.default_expert = "language";

    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.40F, RouteSource::Model), config);
    // Below the floor the delegator is treated as having made no decision.
    CHECK(out.expert == "language");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("undecided") != std::string::npos);
    CHECK(out.detail.find("Physics")   != std::string::npos);
}

TEST(a_pinned_route_ignores_the_confidence_floor) {
    Config config = config_with({"physics", "language"});
    config.routing.min_confidence = 0.99F;
    config.routing.default_expert = "language";

    // The user chose this expert; second-guessing them would be wrong even at
    // a confidence the model never reports.
    const RouteDecision out =
        apply_route_policy(proposal("physics", 1.0F, RouteSource::Forced), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Forced);
}

TEST(a_zero_floor_disables_the_confidence_check) {
    Config config = config_with({"physics", "language"});
    config.routing.min_confidence = 0.0F;
    config.routing.default_expert = "language";
    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.01F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
}

TEST(an_empty_seat_sends_work_to_the_nominated_default) {
    Config config = config_with({"physics", "language"});
    config.routing.default_expert = "language";
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "language");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("Chemistry has no model") != std::string::npos);
}

TEST(with_no_default_nominated_any_filled_seat_is_used) {
    // A partly-configured install should still answer rather than fail, and
    // should say plainly that it substituted.
    const Config config = config_with({"physics"});
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Fallback);
    CHECK(out.detail.find("used Physics") != std::string::npos);
}

TEST(with_nothing_configured_the_route_reports_it) {
    const Config config;
    const RouteDecision out =
        apply_route_policy(proposal("chemistry", 0.95F, RouteSource::Model), config);
    CHECK(out.detail.find("no experts have a model") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The blank slate
//
// A fresh install has no experts at all now, so "nothing on the roster" is the
// state every new user is in for their first few minutes rather than a corner
// nobody reaches. Everything that reads the roster has to survive it.
// ---------------------------------------------------------------------------

TEST(a_fresh_config_has_no_experts_at_all) {
    const Config fresh;
    CHECK(fresh.roster.empty());
    CHECK_EQ(fresh.roster.size(), std::size_t{0});
    CHECK(fresh.configured_experts().empty());
    // And nothing generated from it pretends otherwise.
    CHECK(fresh.roster.router_labels().empty());
    CHECK(fresh.roster.router_examples().empty());
}

TEST(the_keyword_router_names_nobody_on_an_empty_roster) {
    KeywordRouter router(std::make_shared<const Roster>(Roster::bare()));
    const RouteDecision out = router.route("why is the sky blue", {});
    CHECK(out.expert.empty());
}

TEST(an_empty_roster_routes_to_nobody_rather_than_failing) {
    const Config fresh;
    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.95F, RouteSource::Model), fresh);
    // Whatever it says, it must say something and it must not name a seat that
    // does not exist.
    CHECK(!out.detail.empty());
    CHECK(fresh.roster.find(out.expert) == std::nullopt);
}

TEST(the_delegator_prompt_survives_having_no_experts_to_describe) {
    const Roster bare = Roster::bare();
    // Generated rather than stored, so an empty roster must produce an empty or
    // harmless prompt rather than one describing experts that are not there.
    const std::string prompt = bare.router_system_prompt();
    CHECK(prompt.find("Mathematics") == std::string::npos);
    CHECK(prompt.find("Physics") == std::string::npos);
}

TEST(an_uncertain_route_with_no_default_is_taken_at_face_value) {
    // The old behavior sent this to a built-in Fallback seat that on most
    // installs had no model either, so the prompt failed instead of being
    // answered by the delegator's best guess. With nothing nominated, the guess
    // stands and the transcript records the doubt.
    Config config = config_with({"physics"});
    config.routing.min_confidence = 0.60F;

    const RouteDecision out =
        apply_route_policy(proposal("physics", 0.20F, RouteSource::Model), config);
    CHECK(out.expert == "physics");
    CHECK(out.source  == RouteSource::Model);
    CHECK(out.detail.find("undecided") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Module naming
//
// ggml opens a backend module by an exact file name, so Crucible's idea of what
// one is called has to agree with ggml's own down to the character.
// ---------------------------------------------------------------------------

TEST(a_conversation_only_re_reads_what_actually_changed) {
    using detail::reusable_prefix;

    // The ordinary case: the cache holds a prompt and the reply that followed
    // it, and the next turn is all of that plus a new question. Everything up
    // to the new question is already there.
    const std::vector<llama_token> after_turn_one{1, 2, 3, 4, 5};
    const std::vector<llama_token> turn_two{1, 2, 3, 4, 5, 6, 7};
    CHECK_EQ(reusable_prefix(after_turn_one, turn_two), std::size_t{5});

    // A different expert, or an edited history: nothing in common, so nothing
    // is kept.
    CHECK_EQ(reusable_prefix({1, 2, 3}, {9, 8, 7}), std::size_t{0});
    CHECK_EQ(reusable_prefix({}, {1, 2, 3}), std::size_t{0});

    // Diverging part way through keeps only what matched.
    CHECK_EQ(reusable_prefix({1, 2, 3, 4}, {1, 2, 9, 4}), std::size_t{2});

    // The cache holding more than the prompt asks for -- the reply is still in
    // there -- keeps the part that matches and no more.
    CHECK_EQ(reusable_prefix({1, 2, 3, 4, 5}, {1, 2, 3}), std::size_t{2});
}

TEST(a_prompt_is_never_reused_in_its_entirety) {
    using detail::reusable_prefix;

    // Sending the same prompt again is what pressing enter on an unchanged
    // line does. Reusing every token of it would leave llama_decode nothing to
    // decode and the sampler no logits to read, so one token is always held
    // back to be read again.
    CHECK_EQ(reusable_prefix({1, 2, 3}, {1, 2, 3}), std::size_t{2});
    CHECK_EQ(reusable_prefix({1}, {1}), std::size_t{0});
    CHECK_EQ(reusable_prefix({1, 2, 3, 4}, {1, 2, 3}), std::size_t{2});
}

TEST(cuda_targets_the_cards_that_are_there_and_nothing_else) {
    // This machine: an Ampere, an Ada and a Blackwell card, against a CUDA 12.0
    // toolkit that stops at Hopper. Real code for the two it can compile for,
    // and Hopper PTX for the Blackwell card to be compiled by the driver.
    const std::vector<int> toolkit_12_0{50, 52, 60, 61, 70, 75, 80, 86, 87, 89, 90};
    CHECK_EQ(cuda_architectures({89, 120, 86}, toolkit_12_0), "86-real;89-real;90-virtual");

    // One card, and a toolkit that knows it: one real architecture, and PTX at
    // the same level so a card added later still runs.
    CHECK_EQ(cuda_architectures({86}, toolkit_12_0), "86-real;86-virtual");

    // Duplicates are the ordinary case -- two identical cards -- and must not
    // produce the architecture twice.
    CHECK_EQ(cuda_architectures({86, 86, 89}, toolkit_12_0), "86-real;89-real;89-virtual");
}

TEST(cuda_architectures_declines_rather_than_guessing) {
    const std::vector<int> toolkit{75, 80, 86, 89, 90};

    // No driver, or no compiler: an empty answer means "use llama.cpp's
    // defaults", which is slow but always correct. Anything else here would be
    // a module the machine cannot run.
    CHECK(cuda_architectures({}, toolkit).empty());
    CHECK(cuda_architectures({86}, {}).empty());

    // A card older than the toolkit supports has nothing that can be emitted
    // for it, and inventing an architecture would not help.
    CHECK(cuda_architectures({61}, toolkit).empty());
}

TEST(module_names_match_the_convention_ggml_loads_by) {
#ifdef _WIN32
    CHECK(module_prefix() == "ggml-");
    CHECK(module_suffix() == ".dll");
#else
    // macOS is not the odd one out: CMake gives a MODULE library the .so
    // suffix there too, which is why ggml only special-cases Windows.
    CHECK(module_prefix() == "libggml-");
    CHECK(module_suffix() == ".so");
#endif
}

// ---------------------------------------------------------------------------
// Slash-command completion
//
// The menu and the gray suggestion after the cursor both come from these two
// functions, so what they refuse to offer matters as much as what they offer.
// ---------------------------------------------------------------------------

TEST(a_lone_slash_offers_every_command) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/", *shipped());
    CHECK(!matches.empty());
    CHECK(matches.size() == ui::all_commands(*shipped()).size());
}

TEST(typing_narrows_the_list_to_what_still_matches) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/re", *shipped());
    CHECK(matches.size() == 2);
    CHECK(matches[0].name == "resume");
    CHECK(matches[1].name == "release");
}

TEST(the_experts_are_completable_too) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/phy", *shipped());
    CHECK(matches.size() == 1);
    CHECK(matches[0].name == "physics");
    // An expert takes a prompt after it, so completing one leaves the cursor
    // ready to type rather than up against the end of the word.
    CHECK(ui::command_completion("/phy", matches[0]) == "sics ");
}

TEST(a_command_that_is_already_complete_is_not_suggested_back) {
    // Offering to complete "/help" to "/help" is noise, and it would put a
    // menu over the transcript for every finished command.
    CHECK(ui::command_matches("/help", *shipped()).empty());
}

TEST(nothing_is_offered_once_the_command_is_settled) {
    // A space means the user has moved on to the argument, and the menu gets
    // out of the way. This is over-determined -- typed_word() bails on a space
    // and no command name contains one, so prefix matching would reject these
    // anyway. Kept because it is the behavior a reader wants pinned, not
    // because either mechanism alone is in doubt.
    CHECK(ui::command_matches("/physics why is the sky blue", *shipped()).empty());
    CHECK(ui::command_matches("/help ", *shipped()).empty());
    CHECK(ui::command_matches("/re sume", *shipped()).empty());
}

TEST(ordinary_text_is_not_a_command) {
    CHECK(ui::command_matches("hello", *shipped()).empty());
    CHECK(ui::command_matches("", *shipped()).empty());
    CHECK(ui::command_matches("what about /help", *shipped()).empty());
}

TEST(completion_is_case_insensitive_like_the_commands_themselves) {
    const std::vector<ui::CommandInfo> matches = ui::command_matches("/RES", *shipped());
    CHECK(matches.size() == 1);
    CHECK(matches[0].name == "resume");
}

TEST(completion_returns_only_what_is_missing) {
    // The caller appends it and draws it in gray, so returning the whole
    // command would double the part already typed.
    const ui::CommandInfo resume{"resume", "", false};
    CHECK(ui::command_completion("/re", resume) == "sume");
    CHECK(ui::command_completion("/", resume) == "resume");
    // A choice that does not match what is typed completes to nothing rather
    // than replacing the line under the user.
    CHECK(ui::command_completion("/xyz", resume).empty());
}

TEST(every_command_the_menu_offers_has_something_to_say_about_itself) {
    // The summary is the whole right-hand column of the menu and of /help.
    for (const ui::CommandInfo& command : ui::all_commands(*shipped())) {
        CHECK(!command.name.empty());
        CHECK(!command.summary.empty());
    }
}
