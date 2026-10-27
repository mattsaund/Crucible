// SPDX-License-Identifier: MIT
//
// The JSON door.
//
// Every interface Crucible grows -- the window, the TypeScript one, the Python
// orchestrator -- reaches the engine through this and nothing else, so its
// shape is a contract rather than an implementation detail. What is worth
// pinning down without a running engine is all of that contract except the
// part that moves models: the reply always has the same shape, enums cross as
// words, and a mistake comes back as a sentence rather than a crash.
#include "test_helpers.hpp"

#include <fstream>

#include <nlohmann/json.hpp>

#include "crucible/api/surface.hpp"
#include "crucible/lab/recipe.hpp"

using namespace crucible;
using json = nlohmann::json;

namespace {

/// A surface with nothing behind it.
///
/// Most of the contract is testable this way, and the half that is not --
/// does submit actually reach llama.cpp -- is not a unit test's business.
/// A surface standing on nothing: a host that owns no engine, no project and
/// no configuration, which is what every member of api::Host answers by
/// default.
api::Surface bare() {
    static api::Host nobody;
    return api::Surface(nobody);
}

json ask(api::Surface& surface, const std::string& request) {
    return json::parse(surface.handle(request), nullptr, false);
}

}  // namespace

TEST(a_reply_always_has_the_same_shape_even_for_nonsense) {
    api::Surface surface = bare();

    // A caller that has to parse one format for success and another for
    // "your JSON was broken" will get the second one wrong, because it only
    // ever sees it by accident.
    for (const std::string broken : {"", "not json", "[1,2,3]", "\"a string\"", "{"}) {
        const json reply = ask(surface, broken);
        CHECK(reply.is_object());
        CHECK(reply.contains("ok"));
        CHECK(reply["ok"] == false);
        CHECK(reply.contains("error"));
        CHECK(!reply["error"].get<std::string>().empty());
    }
}

TEST(the_id_comes_back_whatever_it_was) {
    api::Surface surface = bare();
    // Numbers and strings both, because the caller picks and this has no
    // business having an opinion about which.
    CHECK(ask(surface, R"({"id":7,"method":"ping"})")["id"] == 7);
    CHECK(ask(surface, R"({"id":"abc","method":"ping"})")["id"] == "abc");
    // And a request with no id is answered rather than refused: fire and
    // forget is a reasonable thing for an interface to do.
    const json reply = ask(surface, R"({"method":"ping"})");
    CHECK(reply["ok"] == true);
    CHECK(reply["id"].is_null());
}

TEST(ping_answers_before_the_engine_exists) {
    // What a caller sends to find out whether anything is listening. If this
    // needed a running engine it could not answer the question it is for.
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"id":1,"method":"ping"})");
    CHECK(reply["ok"] == true);
    CHECK(reply["result"]["version"] == std::string(CRUCIBLE_VERSION));
}

TEST(the_surface_lists_what_it_understands) {
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"id":1,"method":"methods"})");
    CHECK(reply["ok"] == true);

    const auto listed = reply["result"].get<std::vector<std::string>>();
    CHECK(!listed.empty());
    CHECK(listed == api::Surface::methods());

    // Every method it advertises has to be one it actually answers. A list
    // that drifts from the implementation is worse than no list, because an
    // interface believes it. Asked in a data folder of its own: trainer.remove
    // takes no parameters, and asked here it took away the training
    // environment of whoever ran the suite.
    const TempDir        temp;
    const ScopedDataHome scoped(temp.path());
    for (const std::string& method : listed) {
        const json one = ask(surface, json{{"id", 1}, {"method", method}}.dump());
        const std::string error = one.value("error", std::string{});
        CHECK(error.find("no method called") == std::string::npos);
    }
}

TEST(a_surface_with_no_window_does_not_start_another_crucible) {
    // The test above asks every method of a surface with nothing behind it,
    // and a restart there used to start the built Crucible -- a new window on
    // the desktop each time the suite ran, with no window here to close.
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"id":1,"method":"self.restart"})");
    CHECK(reply["ok"] == false);
    CHECK(reply.value("error", std::string{}).find("cannot restart") != std::string::npos);
}

TEST(a_surface_with_no_window_does_not_update_itself) {
    api::Surface surface = bare();
    const json reply = ask(surface, R"({"id":1,"method":"update.install"})");
    CHECK(reply["ok"] == false);
    CHECK(reply.value("error", std::string{}).find("cannot update itself") != std::string::npos);
    CHECK(ask(surface, R"({"id":1,"method":"update.progress"})")["result"]["phase"] == "idle");
}

TEST(an_unknown_method_names_itself_in_the_refusal) {
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"id":1,"method":"summon"})");
    CHECK(reply["ok"] == false);
    // The name is in the message because the caller's bug is almost always a
    // typo, and a bare "unknown method" makes them go and find which one.
    CHECK(reply["error"].get<std::string>().find("summon") != std::string::npos);
}

TEST(a_request_with_no_method_says_so) {
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"id":1,"params":{}})");
    CHECK(reply["ok"] == false);
}

TEST(everything_that_moves_a_model_refuses_without_an_engine) {
    api::Surface surface = bare();
    for (const std::string method : {"snapshot", "submit", "cancel", "release",
                                     "cook.start", "cook.stop", "edit.approve"}) {
        const json reply = ask(surface, json{{"id", 1}, {"method", method}}.dump());
        CHECK(reply["ok"] == false);
    }
}

TEST(a_snapshot_crosses_as_words_rather_than_numbers) {
    // An enum sent as an integer is a thing both sides have to agree about
    // forever, cannot be read in a log, and silently changes meaning when
    // somebody reorders the enum.
    Snapshot snapshot;
    snapshot.mood   = Mood::Talking;
    snapshot.status = "answering";
    snapshot.busy   = true;

    const json out = json::parse(api::snapshot_json(snapshot));
    CHECK(out["mood"] == "talking");
    CHECK(out["status"] == "answering");
    CHECK(out["busy"] == true);
    // Absent things are absent rather than null or empty-string sentinels.
    CHECK(!out.contains("resident"));
    CHECK(!out.contains("linked"));
    CHECK(out["experts"].is_array());
    CHECK(out["turns"].is_array());
}

TEST(a_turn_keeps_its_reasoning_apart_from_its_answer) {
    // Concatenating them is what made gpt-oss look broken in the window, and
    // an interface that received one field could not do anything else.
    Snapshot snapshot;
    Turn     turn;
    turn.prompt    = "why";
    turn.reply     = "because";
    turn.reasoning = "let me think";
    snapshot.turns.push_back(turn);

    const json out = json::parse(api::snapshot_json(snapshot))["turns"][0];
    CHECK(out["prompt"] == "why");
    CHECK(out["reply"] == "because");
    CHECK(out["reasoning"] == "let me think");

    // And a turn that did no thinking carries no empty field for it.
    Snapshot plain;
    Turn     quiet;
    quiet.reply = "yes";
    plain.turns.push_back(quiet);
    CHECK(!json::parse(api::snapshot_json(plain))["turns"][0].contains("reasoning"));
}

TEST(the_roster_and_its_seats_arrive_zipped) {
    // They are parallel arrays in the engine. Sending them that way would
    // make every interface zip them by index separately, and one of them
    // would eventually do it wrong on a roster that had just changed.
    auto roster  = std::make_shared<Roster>(Roster::bare());
    Expert expert;
    expert.name  = "Mathematics";
    expert.blurb = "algebra, calculus, proofs";   // a seat with no blurb is refused
    std::string error;
    CHECK(roster->add(expert, error));

    Snapshot snapshot;
    snapshot.roster = roster;
    snapshot.seats  = {SeatState{SeatPhase::Active, 0.5F, {}}};

    const json out = json::parse(api::snapshot_json(snapshot))["experts"][0];
    CHECK(out["name"] == "Mathematics");
    CHECK(out["phase"] == "active");
    CHECK(out["progress"] == 0.5F);
}

TEST(a_roster_longer_than_its_seat_list_does_not_read_off_the_end) {
    // The two are published together and should always match, but "should"
    // is doing a lot of work across a thread boundary, and the cost of being
    // wrong here is a read past the end of a vector.
    auto roster = std::make_shared<Roster>(Roster::bare());
    for (const char* name : {"Mathematics", "Physics"}) {
        Expert expert;
        expert.name  = name;
        expert.blurb = "what it is for";
        std::string error;
        CHECK(roster->add(expert, error));
    }

    Snapshot snapshot;
    snapshot.roster = roster;   // and no seats at all
    const json out  = json::parse(api::snapshot_json(snapshot));
    CHECK(out["experts"].size() == 2);
    CHECK(out["experts"][1]["phase"] == "unconfigured");
}

TEST(a_pending_edit_carries_both_sides_at_once) {
    // The interface draws them side by side. Fetching the second half in a
    // second request would let the file change between the two.
    Snapshot snapshot;
    auto     edit  = std::make_shared<PendingEdit>();
    edit->path     = "src/calc.py";
    edit->before   = "old";
    edit->after    = "new";
    snapshot.pending_edit = edit;

    const json out = json::parse(api::snapshot_json(snapshot))["pending_edit"];
    CHECK(out["path"] == "src/calc.py");
    CHECK(out["before"] == "old");
    CHECK(out["after"] == "new");
}

// ---------------------------------------------------------------------------
// The two long installs
//
// Compiling a runtime and building the trainer's Python are the two things an
// interface can start that take minutes. Both are optional -- a Surface built
// without them is what a test harness or a headless caller has -- so the
// method has to say so rather than crash, and it has to say so differently
// from "no such method".
// ---------------------------------------------------------------------------

TEST(the_installs_are_methods_whether_or_not_this_build_can_run_them) {
    api::Surface surface = bare();
    const json   listed  = ask(surface, R"({"method":"methods"})")["result"];

    for (const char* name : {"runtime.build", "runtime.cancel", "runtime.dismiss",
                             "runtime.progress", "trainer.install", "trainer.cancel",
                             "trainer.dismiss", "trainer.flavors"}) {
        bool found = false;
        for (const auto& entry : listed) {
            found = found || entry == name;
        }
        CHECK(found);
        if (!found) {
            std::printf("      %s is not in methods()\n", name);
        }
    }
}

TEST(a_surface_with_no_installer_says_so_rather_than_crashing) {
    api::Surface surface = bare();

    // The distinction that matters: "this build cannot" is a fact about the
    // caller's Surface, and "no such method" would send them looking for a
    // typo that is not there.
    for (const char* request : {R"({"method":"runtime.build","params":{"backend":"cuda"}})",
                                R"({"method":"runtime.cancel"})",
                                R"({"method":"runtime.dismiss"})",
                                R"({"method":"trainer.install"})",
                                R"({"method":"trainer.cancel"})",
                                R"({"method":"trainer.dismiss"})"}) {
        const json reply = ask(surface, request);
        CHECK(reply["ok"] == false);
        CHECK(reply["error"].get<std::string>().find("cannot") != std::string::npos);
    }
}

TEST(progress_is_readable_before_anything_has_started) {
    // The page asks for this on the way into the settings screen, before any
    // build exists. An error there would be an error on a screen that is
    // merely being opened.
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"method":"runtime.progress"})");
    CHECK(reply["ok"] == true);
    CHECK(reply["result"]["running"] == false);
    CHECK(reply["result"]["phase"] == "idle");
}

TEST(the_flavors_say_what_installing_would_cost) {
    // Somebody on a metered connection is entitled to know before it starts
    // rather than after, so the sizes are part of the offer and not a
    // footnote on the progress bar.
    api::Surface surface = bare();
    const json   out     = ask(surface, R"({"method":"trainer.flavors"})")["result"];

    CHECK(out.size() == 3);
    int suggested = 0;
    for (const auto& flavor : out) {
        CHECK(!flavor["id"].get<std::string>().empty());
        CHECK(!flavor["note"].get<std::string>().empty());
        CHECK(flavor["download"].get<std::uint64_t>() > 0);
        CHECK(flavor["installed"].get<std::uint64_t>() > 0);
        // What it unpacks to is larger than what comes down the wire. A
        // figure the other way round would be a wrong promise about disk.
        CHECK(flavor["installed"].get<std::uint64_t>()
              >= flavor["download"].get<std::uint64_t>());
        suggested += flavor["suggested"].get<bool>() ? 1 : 0;
    }
    // Exactly one, so the screen can mark it without choosing between two.
    CHECK(suggested == 1);
}

TEST(a_backend_that_does_not_exist_is_refused_by_name) {
    api::Surface surface = bare();
    const json   reply   = ask(surface, R"({"method":"runtime.build","params":{"backend":"xyz"}})");
    CHECK(reply["ok"] == false);
    // Not "this build cannot compile runtimes": the backend is the problem,
    // and saying the other thing sends them to check their build options.
    CHECK(reply["error"].get<std::string>().find("backend") != std::string::npos);
}

TEST(a_bench_result_is_posted_without_what_stays_on_this_machine) {
    const TempDir        temp;
    const ScopedDataHome scoped(temp.path());
    const std::filesystem::path bench   = temp.path() / "crucible" / "bench";
    const std::filesystem::path results = bench / "results";
    std::filesystem::create_directories(results);
    // A result as the runner keeps it: the board's fields, the names that are
    // free text and stay here, and then what is this machine's -- where the
    // model is, every answer, the worked scores.
    const json kept{
        {"schema", 1}, {"suite", "crucible-bench-1"}, {"size", "quick"}, {"crucible", "0.9.5"},
        {"llama", "b11846"}, {"date", "2026-10-26"},
        {"model", {{"file", "tiny.gguf"}, {"sha256", std::string(64, 'a')}, {"bytes", 1000}, {"name", "Tiny"},
                   {"arch", "llama"}, {"params", 1000000}, {"quant", "Q4_K_M"}}},
        {"settings", {{"context", 20480}, {"max_tokens", 16384}, {"temperature", 0}, {"effort", "default"}}},
        {"hardware", {{"backend", "CUDA"}, {"gpus", json::array({"A card"})}, {"gpu_count", 1}, {"memory_gb", 12.0}}},
        {"benchmarks", {{"gsm8k", {{"n", 50}, {"correct", 40}}}}},
        {"speed", {{"gen_tps", 100.5}}}, {"seconds", 90},
        {"id", "run-0"}, {"path", "/home/someone/models/tiny.gguf"}, {"stopped", false},
        {"skills", {{"math", 0.8}}}, {"overall", 0.8},
        {"items", {{"gsm8k", json::array({json{{"i", 1}, {"ok", true}, {"answer", "#### 4"}}})}}}};
    std::ofstream(results / "run-0.json") << kept.dump();
    // Where Hugging Face has the file, found before and kept by its hash, so
    // nothing here asks the network: under the name it has there.
    std::ofstream(bench / "public.json")
        << json{{std::string(64, 'a'), {{"repo", "someone/tiny-GGUF"}, {"file", "tiny-Q4_K_M.gguf"}}}}.dump();

    api::Surface surface = bare();
    const json reply = ask(surface, R"({"id":1,"method":"bench.posting","params":{"id":"run-0"}})");
    CHECK(reply["ok"] == true);
    CHECK(reply["result"]["public"] == true);
    CHECK(!reply["result"].contains("offline"));
    const json posting = reply["result"]["posting"];
    for (const char* mine : {"id", "path", "stopped", "skills", "overall", "items"}) {
        CHECK(!posting.contains(mine));
    }
    // The board's format exactly: no model name, no card names, and the file
    // as Hugging Face has it.
    CHECK((posting["model"] == json{{"file", "tiny-Q4_K_M.gguf"}, {"sha256", std::string(64, 'a')}, {"bytes", 1000},
                                     {"repo", "someone/tiny-GGUF"}, {"arch", "llama"}, {"params", 1000000},
                                     {"quant", "Q4_K_M"}}));
    CHECK((posting["hardware"] == json{{"backend", "CUDA"}, {"gpu_count", 1}, {"memory_gb", 12.0}}));
    CHECK((posting["benchmarks"] == json{{"gsm8k", {{"n", 50}, {"correct", 40}}}}));
    CHECK(reply["result"]["repo"] == "mattsaund/crucible-bench");

    // An id that could be a path is no result.
    CHECK(ask(surface, R"({"id":2,"method":"bench.posting","params":{"id":"../../config"}})")["ok"] == false);
    // The list leaves out every answer; the one result opened has them.
    const json list = ask(surface, R"({"id":3,"method":"bench.results"})");
    CHECK(list["result"]["results"].size() == 1);
    CHECK(!list["result"]["results"][0].contains("items"));
    CHECK(ask(surface, R"({"id":4,"method":"bench.result","params":{"id":"run-0"}})")["result"].contains("items"));
    CHECK(ask(surface, R"({"id":5,"method":"bench.delete","params":{"id":"run-0"}})")["ok"] == true);
    CHECK(!std::filesystem::exists(results / "run-0.json"));
}

TEST(a_bench_result_posts_only_names_of_the_boards_form) {
    const TempDir        temp;
    const ScopedDataHome scoped(temp.path());
    const std::filesystem::path results = temp.path() / "crucible" / "bench" / "results";
    std::filesystem::create_directories(results);
    // Names the board would turn away, card names from before results kept a
    // count, and no hash: nothing to look for on Hugging Face, so no network.
    const json kept{
        {"schema", 1}, {"suite", "crucible-bench-1"}, {"size", "quick"}, {"crucible", "0.9.5"},
        {"llama", "b11846"}, {"date", "2026-10-26"},
        {"model", {{"file", "my model.gguf"}, {"bytes", 1000}, {"name", "Mine"}, {"arch", "Llama 3"},
                   {"quant", "q4 k m"}}},
        {"settings", {{"context", 20480}, {"max_tokens", 16384}, {"temperature", 0}, {"effort", "default"}}},
        {"hardware", {{"backend", "MUSA"}, {"gpus", json::array({"One", "Two"})}, {"memory_gb", -1}}},
        {"benchmarks", {{"gsm8k", {{"n", 50}, {"correct", 40}}}}},
        {"id", "run-1"}};
    std::ofstream(results / "run-1.json") << kept.dump();

    api::Surface surface = bare();
    const json reply = ask(surface, R"({"id":1,"method":"bench.posting","params":{"id":"run-1"}})");
    CHECK(reply["ok"] == true);
    CHECK(reply["result"]["public"] == false);
    const json posting = reply["result"]["posting"];
    CHECK((posting["model"] == json{{"file", "my model.gguf"}, {"bytes", 1000}}));
    CHECK((posting["hardware"] == json{{"gpu_count", 2}}));
    // And it does not go.
    CHECK(ask(surface, R"({"id":2,"method":"bench.post","params":{"id":"run-1"}})")["ok"] == false);
}

TEST(a_bench_result_of_a_model_made_here_says_so_and_is_not_offered_to_the_board) {
    const TempDir        temp;
    const ScopedDataHome scoped(temp.path());
    const std::filesystem::path results = temp.path() / "crucible" / "bench" / "results";
    std::filesystem::create_directories(results);
    // A fine-tune the lab made, in the models folder.
    const std::filesystem::path made = temp.path() / "models" / "kitchen-Q4_K_M.gguf";
    std::filesystem::create_directories(made.parent_path());
    std::ofstream(made) << "GGUF";
    lab::Recipe recipe;
    recipe.name         = "Kitchen";
    recipe.id           = "kitchen";
    recipe.trained_path = made.string();
    recipe.stage        = lab::Stage::Finished;
    std::string error;
    CHECK(lab::save(recipe, error));
    // Its result, with a fingerprint Hugging Face would be asked about if
    // anything asked; and a result of some other file.
    const auto result = [](const std::string& id, const std::string& path) {
        return json{{"schema", 1}, {"suite", "crucible-bench-1"}, {"size", "quick"}, {"crucible", "0.9.5"},
                    {"llama", "b11846"}, {"date", "2026-10-26"},
                    {"model", {{"file", std::filesystem::path(path).filename().string()}, {"sha256", std::string(64, 'c')},
                               {"bytes", 4}}},
                    {"settings", {{"context", 20480}, {"max_tokens", 16384}, {"temperature", 0}, {"effort", "default"}}},
                    {"benchmarks", {{"gsm8k", {{"n", 50}, {"correct", 40}}}}},
                    {"id", id}, {"path", path}};
    };
    std::ofstream(results / "made-0.json") << result("made-0", made.string()).dump();
    std::ofstream(results / "other-0.json") << result("other-0", (temp.path() / "models" / "other.gguf").string()).dump();

    api::Surface surface = bare();
    const json list = ask(surface, R"({"id":1,"method":"bench.results"})")["result"]["results"];
    CHECK(list.size() == 2);
    for (const json& one : list) {
        CHECK(one.value("made", std::string()) == (one["id"] == "made-0" ? "fine-tune" : ""));
    }
    CHECK(ask(surface, R"({"id":2,"method":"bench.result","params":{"id":"made-0"}})")["result"]["made"] == "fine-tune");
    // Nobody is asked where it is public: it is not, and the dialog says why.
    const json posting = ask(surface, R"({"id":3,"method":"bench.posting","params":{"id":"made-0"}})");
    CHECK(posting["ok"] == true);
    CHECK(posting["result"]["public"] == false);
    CHECK(posting["result"]["made"] == "fine-tune");
    CHECK(!posting["result"]["posting"]["model"].contains("repo"));
    const json post = ask(surface, R"({"id":4,"method":"bench.post","params":{"id":"made-0"}})");
    CHECK(post["ok"] == false);
    CHECK(post["error"].get<std::string>().find("fine-tune") != std::string::npos);
}

TEST(the_page_is_sent_a_turn_it_already_has_as_a_stub) {
    Snapshot snapshot;
    Turn finished;
    finished.prompt = "is 7 prime?";
    finished.reply  = "Yes.";
    finished.revision = 11;
    Turn streaming;
    streaming.prompt = "and 9?";
    streaming.reply  = "No";
    streaming.streaming = true;
    streaming.revision = 12;
    snapshot.turns.push_back(finished);
    snapshot.turns.push_back(streaming);

    api::PageTurns page;
    const json first = json::parse(page.turns_json(snapshot.turns));
    CHECK(first[0]["reply"] == "Yes.");
    CHECK(first[1]["rev"] == 12);

    // The same turns again: both stubs. One changed: that one whole.
    const json again = json::parse(page.turns_json(snapshot.turns));
    CHECK(again[0]["stub"] == true);
    CHECK(again[0]["rev"] == 11);
    CHECK(!again[0].contains("reply"));
    Snapshot later;
    later.turns.push_back(finished);
    streaming.reply = "No: 3 x 3.";
    streaming.revision = 13;
    later.turns.push_back(streaming);
    const json next = json::parse(page.turns_json(later.turns));
    CHECK(next[0]["stub"] == true);
    CHECK(next[1]["reply"] == "No: 3 x 3.");

    // A turn made by hand has no revision, and is always sent whole.
    Snapshot hand;
    Turn plain;
    plain.prompt = "hi";
    hand.turns.push_back(plain);
    page.turns_json(hand.turns);
    CHECK(json::parse(page.turns_json(hand.turns))[0]["prompt"] == "hi");
}
