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
    // success -- a typo in the id should say so.
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

    // The side menu draws the roster in order, so where a seat lands in the
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

// ---------------------------------------------------------------------------
// The worked examples the delegator is shown
//
// The delegator's prompt is built by the orchestrator (see
// scripts/orchestrator/crucible_orchestrator/routing.py); what it is built from
// is the roster, and these hold the roster's examples to what that needs.
// ---------------------------------------------------------------------------

TEST(every_expert_gets_the_same_number_of_worked_examples) {
    const Roster roster = testing::sample_roster();
    // The same number each, because a seat with more examples than its
    // neighbors is a seat the delegator is being nudged towards -- and the
    // nudge is invisible in the score until another seat stops being reachable.
    for (const Expert& expert : roster.experts()) {
        CHECK_EQ(expert.examples.size(), std::size_t{2});
        for (const std::string& question : expert.examples) {
            CHECK(!question.empty());
        }
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

    std::vector<std::string> questions;
    const Roster sample = testing::sample_roster();
    for (const Expert& expert : sample.experts()) {
        questions.insert(questions.end(), expert.examples.begin(), expert.examples.end());
    }
    for (const std::string& question : questions) {
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

// ---------------------------------------------------------------------------
// Routing policy
//
// What happens to the delegator's answer. Extracted from the engine as a pure
// function precisely so these rules can be checked without loading a model.
// ---------------------------------------------------------------------------

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
    // ggml's own loader: ggml-*.dll on Windows, libggml-*.so everywhere else --
    // macOS included, where CMake names a MODULE library .so, not .dylib.
#if defined(_WIN32)
    CHECK(module_prefix() == "ggml-");
    CHECK(module_suffix() == ".dll");
#else
    CHECK(module_prefix() == "libggml-");
    CHECK(module_suffix() == ".so");
#endif
#endif
}
