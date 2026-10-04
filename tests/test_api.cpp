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

#include <nlohmann/json.hpp>

#include "crucible/api/surface.hpp"

using namespace crucible;
using json = nlohmann::json;

namespace {

/// A surface with nothing behind it.
///
/// Most of the contract is testable this way, and the half that is not --
/// does submit actually reach llama.cpp -- is not a unit test's business.
api::Surface bare() { return api::Surface(api::Deps{}); }

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
    // interface believes it.
    for (const std::string& method : listed) {
        const json one = ask(surface, json{{"id", 1}, {"method", method}}.dump());
        const std::string error = one.value("error", std::string{});
        CHECK(error.find("no method called") == std::string::npos);
    }
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
    snapshot.seats  = {SeatState{SeatPhase::Active, 0.5F}};

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
