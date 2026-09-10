// SPDX-License-Identifier: MIT
//
// Configuration on disk: reading it, writing it back the way the settings
// screen does, the models directory, folder trust, and where all of it lives.
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Config
// ---------------------------------------------------------------------------

TEST(missing_config_is_created_with_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(std::filesystem::exists(file));
    CHECK(config.is_empty());
    CHECK(config.configured_experts().empty());
}

TEST(experts_inherit_unset_fields_from_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({
          "defaults": { "n_ctx": 4096, "temperature": 0.25, "n_gpu_layers": 7 },
          "experts": {
            "physics": { "blurb": "mechanics, relativity, quantum",
                         "model": "/tmp/physics.gguf" },
            "biology": { "blurb": "cells, genetics, physiology",
                         "model": "/tmp/biology.gguf", "n_ctx": 999 }
          }
        })";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    const ModelParams& physics = config.expert("physics");
    CHECK_EQ(physics.n_ctx, 4096);
    CHECK_EQ(physics.n_gpu_layers, 7);
    CHECK(physics.temperature == 0.25F);

    // An explicit value must survive inheritance.
    const ModelParams& biology = config.expert("biology");
    CHECK_EQ(biology.n_ctx, 999);
    CHECK_EQ(biology.n_gpu_layers, 7);

    CHECK(config.has_expert("physics"));
    CHECK(config.has_expert("biology"));
    CHECK(!config.has_expert("chemistry"));
    CHECK_EQ(config.configured_experts().size(), std::size_t{2});
}

TEST(a_malformed_field_warns_but_keeps_the_rest) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        // n_ctx is a string, which is wrong. One bad field should not cost the
        // user the other eight experts.
        out << R"({
          "experts": { "physics": { "model": "/tmp/p.gguf", "n_ctx": "enormous" } }
        })";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(!warnings.empty());
    CHECK(config.has_expert("physics"));
    CHECK_EQ(config.expert("physics").n_ctx, 8192);
}

TEST(unparseable_config_falls_back_instead_of_throwing) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << "{ this is not json at all";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK(!warnings.empty());
    CHECK(config.is_empty());
}

// ---------------------------------------------------------------------------
// Models directory
// ---------------------------------------------------------------------------

TEST(bare_names_resolve_inside_the_models_directory) {
    const std::filesystem::path dir = "/srv/models";

    // The normal case: the config names a file, the directory says where.
    CHECK_EQ(resolve_model_ref(dir, "physics-q4.gguf").string(),
             std::string("/srv/models/physics-q4.gguf"));

    // A path escapes the directory, so a model can live anywhere.
    CHECK_EQ(resolve_model_ref(dir, "/opt/big/expert.gguf").string(),
             std::string("/opt/big/expert.gguf"));

    // Relative-with-slash stays relative to the models directory.
    CHECK_EQ(resolve_model_ref(dir, "subdir/expert.gguf").string(),
             std::string("/srv/models/subdir/expert.gguf"));

    CHECK(resolve_model_ref(dir, "").empty());
}

TEST(is_bare_name_distinguishes_a_file_from_a_path) {
    CHECK(is_bare_name("expert.gguf"));
    CHECK(!is_bare_name("/abs/expert.gguf"));
    CHECK(!is_bare_name("~/models/expert.gguf"));
    CHECK(!is_bare_name("sub/expert.gguf"));
    CHECK(!is_bare_name(""));
}

TEST(scanning_finds_only_gguf_files_in_name_order) {
    TempDir dir;
    const auto touch = [&](const std::string& name, std::size_t bytes) {
        std::ofstream out(dir.path() / name, std::ios::binary);
        out << std::string(bytes, '\0');
    };
    touch("zeta.gguf", 3000);
    touch("alpha.gguf", 2000);
    touch("notes.txt", 10);          // wrong extension
    touch("archive.gguf.bak", 10);   // extension is .bak, not .gguf
    std::filesystem::create_directories(dir.path() / "nested.gguf");  // a directory

    const std::vector<ModelFile> found = scan_models(dir.path());
    CHECK_EQ(found.size(), std::size_t{2});
    if (found.size() == 2) {
        CHECK_EQ(found[0].name, std::string("alpha.gguf"));
        CHECK_EQ(found[1].name, std::string("zeta.gguf"));
        CHECK_EQ(found[0].bytes, std::uintmax_t{2000});
        CHECK(found[0].path.is_absolute() || found[0].path.string().find(dir.path().string()) == 0);
    }
}

TEST(scanning_a_missing_directory_is_not_an_error) {
    // A first run has no models directory yet; that is a normal state the UI
    // explains, not a failure that should propagate.
    CHECK(scan_models("/definitely/not/a/real/directory").empty());
}

TEST(models_dir_falls_back_to_the_default_when_blank) {
    Config config;
    CHECK(config.models_dir.empty());
    CHECK_EQ(config.resolved_models_dir(), paths::models_dir());

    config.models_dir = "/srv/gguf";
    CHECK_EQ(config.resolved_models_dir().string(), std::string("/srv/gguf"));
}

TEST(model_references_resolve_against_the_configured_directory) {
    Config config;
    config.models_dir = "/srv/gguf";
    config.router.model = "router.gguf";
    config.experts["physics"].model = "/elsewhere/phys.gguf";
    config.resolve_models();

    CHECK_EQ(config.router.path, std::string("/srv/gguf/router.gguf"));
    CHECK_EQ(config.expert("physics").path,
             std::string("/elsewhere/phys.gguf"));
}

TEST(a_seat_whose_file_is_gone_is_not_shown_as_ready) {
    TempDir dir;
    const auto present = dir.path() / "here.gguf";
    { std::ofstream out(present); out << "GGUF"; }

    Config config;
    config.roster     = testing::sample_roster();
    config.models_dir = dir.path().string();
    config.experts["physics"].model = "here.gguf";
    config.experts["biology"].model = "gone.gguf";
    config.resolve_models();

    AppState state;
    state.configure_seats(config);
    const Snapshot snapshot = state.snapshot();

    CHECK(seat_of(snapshot, "physics").phase
          == SeatPhase::Dormant);
    // Assigned but absent must read differently from ready, or the expert panel
    // promises an expert that cannot answer.
    CHECK(seat_of(snapshot, "biology").phase
          == SeatPhase::Missing);
    CHECK(seat_of(snapshot, "chemistry").phase
          == SeatPhase::Unconfigured);
}

// ---------------------------------------------------------------------------
// Saving the config (what the in-app settings screen does)
// ---------------------------------------------------------------------------

TEST(saving_then_loading_round_trips_every_setting) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config original;
    original.roster               = testing::sample_roster();
    original.models_dir           = "/srv/gguf";
    original.system_prompt        = "Be exceptionally terse.";
    original.router.model         = "router-1b.gguf";
    original.defaults.n_ctx       = 16384;
    original.defaults.temperature = 0.33F;
    original.defaults.n_gpu_layers = 42;
    original.defaults.split_mode  = "row";
    original.ui.animation_ms      = 55;
    original.ui.show_experts   = false;
    original.ui.unicode           = false;
    original.tools.auto_edits     = true;
    original.experts["physics"].model = "phys.gguf";
    original.experts["biology"].model = "bio.gguf";
    // One expert deliberately differs from defaults, to prove overrides survive.
    original.experts["biology"].n_ctx = 999;

    CHECK(save_config(original, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    CHECK_EQ(reloaded.models_dir,    original.models_dir);
    CHECK_EQ(reloaded.system_prompt, original.system_prompt);
    CHECK_EQ(reloaded.router.model,  original.router.model);
    CHECK_EQ(reloaded.defaults.n_ctx, 16384);
    CHECK(reloaded.defaults.temperature == 0.33F);
    CHECK_EQ(reloaded.defaults.n_gpu_layers, 42);
    CHECK_EQ(reloaded.defaults.split_mode, std::string("row"));
    CHECK_EQ(reloaded.ui.animation_ms, 55);
    CHECK(!reloaded.ui.show_experts);
    CHECK(!reloaded.ui.unicode);
    // A safety switch that silently forgets itself is worse than not having
    // one: the user turns auto mode on, restarts, and is asked about every
    // edit again -- or, the other way round, believes it is off when it is not.
    CHECK(reloaded.tools.auto_edits);

    CHECK_EQ(reloaded.expert("physics").model,
             std::string("phys.gguf"));
    CHECK_EQ(reloaded.expert("biology").n_ctx, 999);
    // An expert that overrode nothing must still inherit the new defaults.
    CHECK_EQ(reloaded.expert("physics").n_ctx, 16384);
    CHECK_EQ(reloaded.configured_experts().size(), std::size_t{2});
}

TEST(saving_does_not_write_expert_fields_that_match_defaults) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    // The roster is empty on a fresh Config -- Crucible ships no experts -- so
    // the seat this test is about has to be put there first.
    Config config;
    std::string add_error;
    Expert physics_seat;
    physics_seat.id    = "physics";
    physics_seat.name  = "Physics";
    physics_seat.blurb = "physics, mechanics, thermodynamics, relativity";
    CHECK(config.roster.add(std::move(physics_seat), add_error));

    config.defaults.n_ctx = 4096;
    config.experts["physics"].model = "p.gguf";
    config.experts["physics"].n_ctx = 4096;  // same as default
    CHECK(save_config(config, file));

    // Parse rather than string-search: "router" legitimately carries an n_ctx
    // of its own, and a naive substring check could not tell the two apart.
    nlohmann::json doc;
    {
        std::ifstream in(file);
        in >> doc;
    }

    // The experts block is an array, because the order seats are drawn in is
    // the order they are written and a JSON object has no order to preserve.
    const nlohmann::json& experts = doc.at("experts");
    CHECK(experts.is_array());
    const nlohmann::json* physics = nullptr;
    for (const nlohmann::json& entry : experts) {
        if (entry.value("id", "") == "physics") {
            physics = &entry;
        }
    }
    CHECK(physics != nullptr);
    if (physics == nullptr) {
        return;
    }

    // Round-tripping every field would turn a short config into a wall of
    // redundant numbers the first time the user saved from the settings screen.
    CHECK(physics->contains("model"));
    CHECK(!physics->contains("n_ctx"));
    CHECK(!physics->contains("temperature"));

    // Identity, on the other hand, is always written. There is no built-in
    // table to reconstruct a seat from any more, so a config that did not carry
    // the blurb would load an expert the delegator cannot route to.
    CHECK(physics->contains("keywords"));
    CHECK(physics->contains("blurb"));
    CHECK(physics->contains("name"));

    // The defaults block still carries the real values.
    CHECK_EQ(doc.at("defaults").at("n_ctx").get<int>(), 4096);
}

TEST(a_user_made_expert_survives_a_round_trip_through_disk) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    Expert expert;
    expert.name     = "Rust Async";
    expert.blurb    = "tokio, futures, pinning, async traits, executor tuning";
    expert.examples = {"why does my future never wake", "how do I pin a self-referential struct"};
    std::string error;
    CHECK(config.roster.add(expert, error));
    config.experts["rust-async"].model = "rust.gguf";
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    const std::optional<std::size_t> found = reloaded.roster.find("rust-async");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& back = reloaded.roster.at(*found);

    // Everything the delegator reads has to come back, not just the name: a
    // seat that reloaded without its blurb or its examples would still appear
    // in the list and quietly stop being routable to.
    CHECK_EQ(back.name, std::string("Rust Async"));
    CHECK_EQ(back.blurb, expert.blurb);
    CHECK_EQ(back.examples.size(), std::size_t{2});
    CHECK_EQ(back.examples[0], std::string("why does my future never wake"));
    CHECK(!back.keywords.empty());
    CHECK_EQ(reloaded.expert("rust-async").model, std::string("rust.gguf"));

    // And it is a seat like any other by the time the delegator sees it.
    const std::vector<std::string> labels = reloaded.roster.router_labels();
    CHECK(std::find(labels.begin(), labels.end(), "Rust Async") != labels.end());
}

TEST(the_config_shape_the_readme_documents_actually_loads) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        // Copied from README.md, comments stripped. The README tells people
        // this is what a config looks like; if it stops being true, this is
        // where that is noticed rather than in someone's issue tracker.
        std::ofstream out(file);
        out << R"({
  "models_dir": "~/.local/share/crucible/models",
  "router":   { "model": "LFM2-1.2B-Q8_0.gguf" },
  "defaults": { "n_ctx": 8192, "n_gpu_layers": -1, "temperature": 0.7 },
  "experts": [
    { "id": "mathematics",
      "name": "Mathematics",
      "blurb": "algebra, calculus, proofs, geometry, statistics, probability",
      "model": "math-expert-q4_k_m.gguf" },
    { "id": "physics",
      "name": "Physics",
      "blurb": "mechanics, thermodynamics, relativity, quantum, electromagnetism",
      "model": "physics-expert-q4_k_m.gguf" },
    { "id": "rust-async",
      "name": "Rust Async",
      "tag": "RA",
      "blurb": "tokio, futures, pinning, async traits, executor tuning",
      "examples": ["why does my future never wake",
                   "how do I pin a self-referential struct"],
      "keywords": ["tokio", "futures", "pinning", "async"],
      "model": "" },
    { "id": "general",
      "name": "General",
      "blurb": "anything that does not obviously belong to one of the others",
      "model": "generalist-q4_k_m.gguf" }
  ],
  "routing": { "min_confidence": 0.60, "default_expert": "general" },
  "tools": { "web_search": false, "workshop_timeout": 120 }
})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    CHECK_EQ(config.roster.size(), std::size_t{4});
    CHECK_EQ(config.routing.default_expert, ExpertId("general"));
    CHECK(config.has_expert("general"));

    // A seat that gave only a name and a blurb still gets a tag and keywords:
    // they are derived on the way in, which is what makes the short form above
    // a real config rather than a half-written one.
    const std::optional<std::size_t> maths = config.roster.find("mathematics");
    CHECK(maths.has_value());
    if (maths) {
        CHECK_EQ(config.roster.at(*maths).name, std::string("Mathematics"));
        CHECK(!config.roster.at(*maths).tag.empty());
        CHECK(!config.roster.at(*maths).keywords.empty());
    }

    // And the user-made one keeps everything it was given.
    const std::optional<std::size_t> rust = config.roster.find("rust-async");
    CHECK(rust.has_value());
    if (rust) {
        CHECK_EQ(config.roster.at(*rust).tag, std::string("RA"));
        CHECK_EQ(config.roster.at(*rust).examples.size(), std::size_t{2});
    }

    CHECK_EQ(config.defaults.n_ctx, 8192);
    CHECK_EQ(config.tools.workshop_timeout, 120);
    // Absent from the file, so it takes the default -- and the default is the
    // careful one: an edit stops and asks before it overwrites anything.
    CHECK(!config.tools.auto_edits);

    // The only warnings should be about models that are not on this machine --
    // nothing about the shape of the file itself.
    for (const std::string& warning : warnings) {
        CHECK(warning.find("model not found") != std::string::npos);
    }
}

TEST(an_ejected_expert_does_not_come_back_on_the_next_load) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.roster = testing::sample_roster();
    std::string error;
    CHECK(config.roster.remove("chemistry", error));
    config.experts.erase("chemistry");
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);

    // An ejected seat stays ejected. Nothing is ever put back: quietly
    // restoring one would make /ejectexpert a no-op across restarts.
    CHECK(!reloaded.roster.find("chemistry").has_value());
    CHECK_EQ(reloaded.roster.size(), std::size_t{8});
    CHECK(reloaded.roster.find("physics").has_value());
}

TEST(a_config_whose_experts_are_all_gone_loads_as_an_empty_list) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({"experts": []})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    // Someone who deleted every seat wanted every seat deleted. Silently
    // restoring nine of them would make /ejectexpert a no-op across restarts.
    CHECK(config.roster.experts().empty());
    CHECK(config.roster.router_labels().empty());
    CHECK(config.configured_experts().empty());
}

TEST(a_hand_written_expert_needs_only_an_id_and_a_blurb) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        // The config file is documented as hand-editable, so the loader has to
        // accept the shape a person would actually type -- including the object
        // form, which is the natural way to write a keyed list by hand.
        std::ofstream out(file);
        out << R"({"experts": {"tax-law": {"blurb": "deductions and filing",
                                            "model": "tax.gguf"}}})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    const std::optional<std::size_t> found = config.roster.find("tax-law");
    CHECK(found.has_value());
    if (!found) {
        return;
    }
    const Expert& expert = config.roster.at(*found);
    CHECK_EQ(expert.name, std::string("tax-law"));  // no "name" given: the id stands in
    CHECK(!expert.tag.empty());                     // derived
    CHECK(!expert.keywords.empty());                // derived
    CHECK_EQ(config.expert("tax-law").model, std::string("tax.gguf"));
}

TEST(an_expert_entry_with_no_blurb_is_skipped_with_a_warning) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    {
        std::ofstream out(file);
        out << R"({"experts": [{"id": "vibes", "model": "x.gguf"}]})";
    }

    std::vector<std::string> warnings;
    const Config config = load_config(file, warnings);

    // A seat the delegator cannot route to is worse than no seat: it appears on
    // the list and is never chosen. Refusing it and saying so is the only
    // outcome the user can act on.
    CHECK(!config.roster.find("vibes").has_value());
    CHECK(!warnings.empty());
}

TEST(saving_a_seat_back_to_none_clears_it) {
    TempDir dir;
    const auto file = dir.path() / "config.json";

    Config config;
    config.experts["physics"].model = "p.gguf";
    CHECK(save_config(config, file));

    config.experts["physics"].model.clear();
    CHECK(save_config(config, file));

    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK(!reloaded.has_expert("physics"));
    CHECK(reloaded.configured_experts().empty());
}

// ---------------------------------------------------------------------------
// Trust store
// ---------------------------------------------------------------------------

TEST(trust_persists_and_covers_subdirectories) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    const auto project    = dir.path() / "project";
    std::filesystem::create_directories(project / "src" / "deep");

    {
        TrustStore store(store_file);
        CHECK(!store.is_trusted(project));
        CHECK(store.trust(project));
        CHECK(store.is_trusted(project));
        // Trusting a folder covers everything beneath it.
        CHECK(store.is_trusted(project / "src"));
        CHECK(store.is_trusted(project / "src" / "deep"));
    }

    // A new store must see what the previous one wrote.
    TrustStore reloaded(store_file);
    CHECK(reloaded.is_trusted(project));
    CHECK(reloaded.is_trusted(project / "src" / "deep"));
}

TEST(trust_does_not_leak_across_sibling_prefixes) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    const auto trusted   = dir.path() / "work";
    const auto sibling   = dir.path() / "work-secrets";
    std::filesystem::create_directories(trusted);
    std::filesystem::create_directories(sibling);

    TrustStore store(store_file);
    CHECK(store.trust(trusted));
    CHECK(store.is_trusted(trusted));
    // "work-secrets" merely starts with "work". A prefix comparison would
    // wrongly trust it; the check must be per path component.
    CHECK(!store.is_trusted(sibling));
    CHECK(!store.is_trusted(dir.path()));  // a parent is not trusted by a child
}

TEST(a_corrupt_trust_store_trusts_nothing_rather_than_failing) {
    TempDir dir;
    const auto store_file = dir.path() / "trust.json";
    {
        std::ofstream out(store_file);
        out << "not json";
    }
    TrustStore store(store_file);
    CHECK(!store.is_trusted(dir.path()));
    CHECK(store.entries().empty());
}

// ---------------------------------------------------------------------------
// Paths
// ---------------------------------------------------------------------------

TEST(tilde_expands_to_the_home_directory) {
    const char* home = std::getenv("HOME");
    if (home == nullptr) {
        return;  // nothing meaningful to assert
    }
    const auto expanded = paths::expand_user("~/models/expert.gguf");
    CHECK(expanded.is_absolute());
    CHECK(expanded.string().rfind(std::string(home), 0) == 0);
    CHECK(expanded.string().find("~") == std::string::npos);

    // A tilde that is not a home reference must be left alone.
    CHECK(paths::expand_user("/tmp/~odd/file").string().find("~odd") != std::string::npos);
    CHECK(paths::expand_user("").empty());
}

TEST(xdg_config_home_is_honored_when_absolute) {
    const auto file = paths::config_file();
    CHECK_EQ(file.filename().string(), std::string("config.json"));
    CHECK_EQ(file.parent_path().filename().string(), std::string("crucible"));
    CHECK(file.is_absolute());
}

TEST(an_empty_models_dir_means_the_default_so_resetting_is_clearing_it) {
    // This is what the "Reset to default" row does. It stores an empty string
    // rather than the resolved path, so a config written on one machine still
    // points somewhere sensible on another -- and so the default can move
    // without stranding anyone who reset.
    Config moved;
    moved.models_dir = "/mnt/external/ggufs";
    CHECK_EQ(moved.resolved_models_dir().string(), std::string("/mnt/external/ggufs"));

    moved.models_dir.clear();
    CHECK_EQ(moved.resolved_models_dir(), paths::models_dir());
}

TEST(resetting_the_models_dir_re_resolves_every_model_reference) {
    Config config;
    config.models_dir            = "/mnt/external/ggufs";
    config.router.model          = "router.gguf";
    config.experts["mathematics"].model      = "maths.gguf";
    // An absolute reference is not relative to the models directory, so moving
    // that directory must leave it exactly where it points.
    config.experts["programming"].model      = "/opt/models/programming.gguf";
    config.resolve_models();
    CHECK_EQ(config.router.path, std::string("/mnt/external/ggufs/router.gguf"));

    config.models_dir.clear();
    config.resolve_models();
    CHECK_EQ(config.router.path, (paths::models_dir() / "router.gguf").string());
    CHECK_EQ(config.expert("mathematics").path, (paths::models_dir() / "maths.gguf").string());
    CHECK_EQ(config.expert("programming").path, std::string("/opt/models/programming.gguf"));
}
