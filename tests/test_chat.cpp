// SPDX-License-Identifier: MIT
//
// A chat turn, end to end: the engine, the orchestrator, and a provider that
// answers from a script.
//
// A turn is what Crucible is used for most, and the thing with the most ways
// to end: an answer, a tool and then an answer, an edit the person declined, a
// search, a provider that refused, a Stop. Each leaves the transcript in a
// shape of its own, and the shape is what is pinned here -- the reply, the
// actions above it, the reasoning kept apart, what the next turn remembers --
// along with what the expert was sent each round, which the provider writes
// down as it is asked.
//
// No weights. The expert is a provider on this machine, scripted round by
// round (see scripted_provider.py), so every way a turn can go is one a test
// can walk, in a second. The orchestrator runs as it does under the window,
// on a Python from CRUCIBLE_PYTHON or PATH; with neither, these are skipped.
#include "test_helpers.hpp"

#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "crucible/engine/engine.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/subprocess.hpp"

namespace {

using json = nlohmann::json;

/// What the system prompt starts with, so a test can find it.
constexpr const char* kSystem = "You are a careful expert.";

/// A Python for the orchestrator and the provider, or empty.
std::string test_python() {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return chosen;
    }
    for (const char* name : {"python3", "python"}) {
        if (util::on_path(name)) {
            return name;
        }
    }
    return {};
}

bool has(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void write_file(const std::filesystem::path& file, const std::string& text) {
    std::filesystem::create_directories(file.parent_path());
    std::ofstream(file, std::ios::binary) << text;
}

std::string read_file(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// CRUCIBLE_PYTHON for as long as this lives: the engine starts its
/// orchestrator on it.
class ScopedPython {
public:
    explicit ScopedPython(const std::string& python) {
        if (const char* now = std::getenv("CRUCIBLE_PYTHON"); now != nullptr) {
            previous_ = now;
            had_      = true;
        }
        set_env("CRUCIBLE_PYTHON", python);
    }
    ~ScopedPython() {
        if (had_) {
            set_env("CRUCIBLE_PYTHON", previous_);
        } else {
            unset_env("CRUCIBLE_PYTHON");
        }
    }
    ScopedPython(const ScopedPython&)            = delete;
    ScopedPython& operator=(const ScopedPython&) = delete;

private:
    std::string previous_;
    bool        had_ = false;
};

/// scripted_provider.py, running for as long as this lives.
class ScriptedServer {
public:
    ScriptedServer(const std::filesystem::path& dir, const json& script, const std::string& python)
        : log_(dir / "requests.jsonl") {
        const std::filesystem::path file = dir / "script.json";
        write_file(file, script.dump());
        const std::filesystem::path source =
            std::filesystem::path(CRUCIBLE_SOURCE_DIR) / "tests" / "scripted_provider.py";
        util::Subprocess::Streams streams;
        streams.input  = true;
        streams.errors = dir / "provider.log";
        std::string error;
        if (child_.start({python, "-u", source.string(), file.string(), log_.string()}, {}, {}, error,
                         streams)) {
            std::string line;
            if (child_.read_line(line)) {
                port_ = format::trim(line);
            }
        }
    }
    ~ScriptedServer() {
        child_.close_input();
        child_.wait();
    }
    ScriptedServer(const ScriptedServer&)            = delete;
    ScriptedServer& operator=(const ScriptedServer&) = delete;

    bool        ok() const { return !port_.empty(); }
    std::string url() const { return "http://127.0.0.1:" + port_; }

    /// What each round was sent, in order: the request bodies.
    std::vector<json> requests() const { return entries("body"); }

    /// What was searched for, in order.
    std::vector<std::string> searches() const {
        std::vector<std::string> out;
        for (const json& entry : entries("search")) {
            out.push_back(entry.get<std::string>());
        }
        return out;
    }

private:
    std::vector<json> entries(const char* key) const {
        std::vector<json> out;
        std::ifstream in(log_);
        std::string line;
        while (std::getline(in, line)) {
            const json entry = json::parse(line, nullptr, false);
            if (entry.is_object() && entry.contains(key)) {
                out.push_back(entry[key]);
            }
        }
        return out;
    }

    util::Subprocess      child_;
    std::filesystem::path log_;
    std::string           port_;
};

/// An engine whose experts are the scripted provider, in a project of its own.
///
/// Two seats, Programming and Writing, each on a model of its own name so a
/// request says which was asked. `adjust` changes the settings before the
/// engine starts.
class Rig {
public:
    explicit Rig(const json& script, const std::function<void(Config&)>& adjust = {},
                 bool with_project = true) {
        const std::string python = test_python();
        if (python.empty()) {
            return;
        }
        home_     = std::make_unique<ScopedDataHome>(dir_.path() / "data");
        chosen_   = std::make_unique<ScopedPython>(python);
        provider_ = std::make_unique<ScriptedServer>(dir_.path(), script, python);
        if (!provider_->ok()) {
            return;
        }
        project_ = dir_.path() / "project";
        std::filesystem::create_directories(project_);

        Config config;
        config.system_prompt = kSystem;
        crucible::Provider scripted;
        scripted.id       = "scripted";
        scripted.name     = "Scripted";
        scripted.kind     = "openai";
        scripted.base_url = provider_->url() + "/v1";
        config.providers.push_back(scripted);
        std::string error;
        for (const auto& [name, keyword] :
             {std::pair<const char*, const char*>{"Programming", "python"}, {"Writing", "readme"}}) {
            Expert seat;
            seat.name     = name;
            seat.blurb    = std::string(name) + ", all of it";
            seat.keywords = {keyword};
            config.roster.add(seat, error);
        }
        config.experts["programming"].model    = "programming-model";
        config.experts["programming"].provider = "scripted";
        config.experts["writing"].model        = "writing-model";
        config.experts["writing"].provider     = "scripted";
        config.tools.search_provider = "searxng";
        config.tools.search_endpoint = provider_->url();
        if (adjust) {
            adjust(config);
        }
        config.resolve_models();

        engine_ = std::make_unique<Engine>(config, state_, [this] { woken(); });
        if (with_project) {
            engine_->set_project(project_, dir_.path() / "history");
        }
        engine_->start();
    }

    ~Rig() {
        if (engine_) {
            engine_->stop();
        }
        engine_.reset();
    }

    Rig(const Rig&)            = delete;
    Rig& operator=(const Rig&) = delete;

    bool ready() const { return engine_ != nullptr; }

    /// Ask, and wait for the turn to be over: finished, and the engine idle.
    /// `meanwhile` sees every snapshot taken while it waits.
    Turn ask(const std::string& prompt, std::optional<ExpertId> pinned = std::nullopt,
             std::vector<attach::Attachment> attachments = {},
             const std::function<void(const Snapshot&)>& meanwhile = {}) {
        const std::size_t before = state_.snapshot().turns.size();
        engine_->submit(prompt, std::move(pinned), std::move(attachments));
        const auto started = std::chrono::steady_clock::now();
        for (;;) {
            const Snapshot now = state_.snapshot();
            if (meanwhile) {
                meanwhile(now);
            }
            if (now.turns.size() > before && !now.turns.back().streaming && !engine_->is_busy()) {
                return now.turns.back();
            }
            if (std::chrono::steady_clock::now() - started > std::chrono::seconds(90)) {
                harness::report_failure(__FILE__, __LINE__, "the turn did not finish: " + prompt);
                engine_->cancel();
                return now.turns.empty() ? Turn{} : now.turns.back();
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }

    Engine&                      engine() { return *engine_; }
    Snapshot                     snapshot() const { return state_.snapshot(); }
    const std::filesystem::path& project() const { return project_; }
    std::vector<json>            requests() const { return provider_->requests(); }
    std::vector<std::string>     searches() const { return provider_->searches(); }

    /// Every status line a wake found, without repeats in a row.
    std::vector<std::string> statuses() const {
        const std::lock_guard<std::mutex> lock(seen_mutex_);
        return seen_;
    }

private:
    void woken() {
        const Snapshot now = state_.snapshot();
        const std::lock_guard<std::mutex> lock(seen_mutex_);
        if (seen_.empty() || seen_.back() != now.status) {
            seen_.push_back(now.status);
        }
    }

    TempDir                         dir_;
    std::unique_ptr<ScopedDataHome> home_;
    std::unique_ptr<ScopedPython>   chosen_;
    std::unique_ptr<ScriptedServer> provider_;
    std::filesystem::path           project_;
    AppState                        state_;
    mutable std::mutex              seen_mutex_;
    std::vector<std::string>        seen_;
    std::unique_ptr<Engine>         engine_;
};

void skipped() { std::printf("      (no Python to run the orchestrator on; skipped)\n"); }

json reply(const std::string& content) { return json{{"content", content}}; }

json script(std::initializer_list<json> replies) {
    json out = json::array();
    for (const json& one : replies) {
        out.push_back(one);
    }
    return json{{"replies", out}};
}

/// The text of a message as a request carried it: its content, or the text
/// part of one sent with pictures.
std::string text_of(const json& message) {
    const json& content = message["content"];
    if (content.is_string()) {
        return content.get<std::string>();
    }
    std::string text;
    for (const json& part : content) {
        if (part.value("type", "") == "text") {
            text += part.value("text", "");
        }
    }
    return text;
}

/// How many pictures a message carried.
std::size_t pictures_in(const json& message) {
    std::size_t count = 0;
    if (message["content"].is_array()) {
        for (const json& part : message["content"]) {
            count += part.value("type", "") == "image_url" ? 1 : 0;
        }
    }
    return count;
}

/// Whether `wanted` appear in `seen` in this order, others between allowed.
bool in_order(const std::vector<std::string>& seen, const std::vector<std::string>& wanted) {
    std::size_t at = 0;
    for (const std::string& one : seen) {
        if (at < wanted.size() && one == wanted[at]) {
            ++at;
        }
    }
    return at == wanted.size();
}

}  // namespace

// ---------------------------------------------------------------------------
// One round

TEST(a_turn_with_nothing_to_look_up_is_one_round) {
    Rig rig(script({reply("Paris is the capital of France.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("What is the capital of France?");
    CHECK_EQ(turn.reply, std::string("Paris is the capital of France."));
    CHECK(!turn.failed);
    CHECK(!turn.canceled);
    CHECK(turn.actions.empty());
    CHECK(turn.route.has_value());
    CHECK_EQ(turn.route ? turn.route->expert : std::string(), std::string("programming"));
    CHECK(turn.output_tokens > 0);

    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    const json& messages = requests.at(0)["messages"];
    CHECK_EQ(requests.at(0)["model"].get<std::string>(), std::string("programming-model"));
    CHECK_EQ(messages.size(), std::size_t{2});
    // The configured prompt first, then how replies are drawn, then the
    // tools a trusted folder offers -- and nothing that only a cook is told.
    const std::string system = text_of(messages.front());
    CHECK_EQ(messages.front()["role"].get<std::string>(), std::string("system"));
    CHECK(system.rfind(kSystem, 0) == 0);
    CHECK(has(system, "Replies are shown as Markdown. Write math as LaTeX"));
    CHECK(has(system, "You can act on the project, not only describe it."));
    CHECK(has(system, "When you have finished, write the answer for the person who asked"));
    CHECK(!has(system, "HANDOFF:"));
    CHECK(!has(system, "You can look things up on the internet."));
    CHECK_EQ(text_of(messages.back()), std::string("What is the capital of France?"));

    // Over, and said to be: nobody linked, nothing on the status line.
    const Snapshot after = rig.snapshot();
    CHECK(after.mood == Mood::Idle);
    CHECK_EQ(after.status, std::string());
    CHECK(!after.linked.has_value());
    CHECK_EQ(after.context_size, 128000);
    CHECK(after.context_used > 0);
    CHECK(in_order(rig.statuses(), {"Crucible is reading the prompt", "Programming is reading",
                                    "Programming is answering", ""}));
}

TEST(a_turns_reasoning_is_kept_apart_from_its_reply_and_from_the_next_turn) {
    Rig rig(script({json{{"reasoning", "Six sevens are forty-two."}, {"content", "42."}},
                    reply("<think>carry the one</think>Twelve."), reply("Noted.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    // On a channel of its own, as a provider sends it.
    const Turn first = rig.ask("What is six times seven?");
    CHECK_EQ(first.reasoning, std::string("Six sevens are forty-two."));
    CHECK_EQ(first.reply, std::string("42."));
    // And between markers in the answer, as an open model writes it.
    const Turn second = rig.ask("And seven plus five?");
    CHECK_EQ(second.reasoning, std::string("carry the one"));
    CHECK_EQ(second.reply, std::string("Twelve."));

    // The history is what was said, not what was thought.
    rig.ask("Thanks.");
    const json messages = rig.requests().at(2)["messages"];
    CHECK_EQ(messages.size(), std::size_t{6});
    CHECK_EQ(text_of(messages[1]), std::string("What is six times seven?"));
    CHECK_EQ(text_of(messages[2]), std::string("42."));
    CHECK_EQ(text_of(messages[3]), std::string("And seven plus five?"));
    CHECK_EQ(text_of(messages[4]), std::string("Twelve."));
    CHECK_EQ(text_of(messages[5]), std::string("Thanks."));
}

// ---------------------------------------------------------------------------
// Tools

TEST(a_turn_reads_a_file_and_answers_from_what_it_read) {
    Rig rig(script({reply("Let me look.\nREAD: notes.txt"), reply("It is PERIWINKLE.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    write_file(rig.project() / "notes.txt", "The code word is PERIWINKLE.\n");
    const Turn turn = rig.ask("What is the code word in notes.txt?");

    // The protocol line comes off; the prose before it stays, a paragraph
    // above the answer.
    CHECK_EQ(turn.reply, std::string("Let me look.\n\nIt is PERIWINKLE."));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    if (!turn.actions.empty()) {
        CHECK(turn.actions[0].summary.rfind("read notes.txt", 0) == 0);
        CHECK_EQ(turn.actions[0].language, std::string());
    }

    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    const json& second = requests.at(1)["messages"];
    CHECK_EQ(second.size(), std::size_t{4});
    CHECK_EQ(second[2]["role"].get<std::string>(), std::string("assistant"));
    CHECK_EQ(text_of(second[2]), std::string("Let me look.\nREAD: notes.txt"));
    CHECK_EQ(second[3]["role"].get<std::string>(), std::string("user"));
    CHECK(has(text_of(second[3]), "The code word is PERIWINKLE."));
    CHECK(!has(text_of(second[3]), "last action"));

    // The status line says what is being done, by its verb and its argument.
    CHECK(in_order(rig.statuses(), {"Programming is reading", "Programming is answering",
                                    "read notes.txt", "Programming is answering", ""}));
}

TEST(a_turn_is_not_run_the_same_call_twice) {
    Rig rig(script({reply("READ: notes.txt"), reply("READ: notes.txt"), reply("Done.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    write_file(rig.project() / "notes.txt", "one line\n");
    const Turn turn = rig.ask("Read notes.txt.");
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    CHECK_EQ(turn.reply, std::string("Done."));
    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{3});
    const json& third = requests.at(2)["messages"];
    CHECK_EQ(text_of(third.back()),
             std::string("You already did exactly that this turn, and its result is above. Use it, "
                         "and if it told you what you needed, answer now."));
}

TEST(a_turn_asks_before_it_writes_and_a_no_leaves_the_file_alone) {
    Rig rig(script({reply("Here it is.\nWRITE: out.txt\n```\nhello\n```"), reply("Left it alone.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    std::shared_ptr<const PendingEdit> asked;
    std::string                        shown;
    const Turn turn = rig.ask("Write hello to out.txt.", std::nullopt, {}, [&](const Snapshot& now) {
        if (now.pending_edit && !asked) {
            asked = now.pending_edit;
            shown = now.turns.back().reply;
            CHECK(has(now.status, "waiting on you: out.txt"));
            rig.engine().approve_edit(false);
        }
    });
    CHECK(asked != nullptr);
    if (asked) {
        CHECK_EQ(asked->path, std::string("out.txt"));
        CHECK_EQ(asked->before, std::string());
        CHECK_EQ(asked->after, std::string("hello\n"));
    }
    // The file is not on screen twice while the person decides: the raw
    // call is off the reply before the question goes up.
    CHECK_EQ(shown, std::string("Here it is."));
    CHECK(!std::filesystem::exists(rig.project() / "out.txt"));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    if (!turn.actions.empty()) {
        CHECK_EQ(turn.actions[0].summary, std::string("declined the edit to out.txt"));
    }
    // What was said about a change that did not happen goes with it.
    CHECK_EQ(turn.reply, std::string("Left it alone."));
    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{2});
    CHECK_EQ(text_of(requests.at(1)["messages"].back()),
             std::string("The user declined that edit; the file is unchanged. Do not try the same "
                         "write again. Either explain what you were going to change and why, or "
                         "propose something different."));
    CHECK(rig.snapshot().pending_edit == nullptr);
}

TEST(a_turn_writes_when_the_person_says_yes) {
    Rig rig(script({reply("Here it is.\nWRITE: out.txt\n```\nhello\n```"), reply("Written.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    bool asked = false;
    const Turn turn = rig.ask("Write hello to out.txt.", std::nullopt, {}, [&](const Snapshot& now) {
        if (now.pending_edit && !asked) {
            asked = true;
            rig.engine().approve_edit(true);
        }
    });
    CHECK(asked);
    CHECK_EQ(read_file(rig.project() / "out.txt"), std::string("hello\n"));
    CHECK_EQ(turn.reply, std::string("Here it is.\n\nWritten."));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    if (!turn.actions.empty()) {
        // The diff, colored as the file it is.
        CHECK_EQ(turn.actions[0].language, std::string("out.txt"));
        CHECK(has(turn.actions[0].body, "hello"));
    }
}

TEST(a_turn_writes_without_asking_when_auto_is_on_and_an_edit_that_cannot_apply_is_not_asked) {
    Rig rig(script({reply("WRITE: out.txt\n```\nhello\n```"),
                    reply("EDIT: out.txt\n<<<<<<< SEARCH\nnot in it\n=======\nbye\n>>>>>>> REPLACE"),
                    reply("Done.")}),
            [](Config& config) { config.tools.auto_edits = true; });
    if (!rig.ready()) {
        skipped();
        return;
    }
    bool asked = false;
    const Turn turn = rig.ask("Write hello to out.txt.", std::nullopt, {},
                              [&](const Snapshot& now) { asked = asked || now.pending_edit != nullptr; });
    CHECK(!asked);
    CHECK_EQ(read_file(rig.project() / "out.txt"), std::string("hello\n"));
    CHECK_EQ(turn.actions.size(), std::size_t{2});
    CHECK_EQ(turn.reply, std::string("Done."));
}

TEST(an_edit_that_would_not_apply_is_run_rather_than_asked_about) {
    Rig rig(script({reply("EDIT: notes.txt\n<<<<<<< SEARCH\nnot in it\n=======\nbye\n>>>>>>> REPLACE"),
                    reply("Could not.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    write_file(rig.project() / "notes.txt", "one line\n");
    bool asked = false;
    const Turn turn = rig.ask("Change notes.txt.", std::nullopt, {},
                              [&](const Snapshot& now) { asked = asked || now.pending_edit != nullptr; });
    CHECK(!asked);
    CHECK_EQ(read_file(rig.project() / "notes.txt"), std::string("one line\n"));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    CHECK_EQ(turn.reply, std::string("Could not."));
}

TEST(a_turn_out_of_rounds_says_so_and_remembers_what_the_expert_said) {
    // Twelve different reads, one a round: the most a turn may take.
    json replies = json::array();
    for (int i = 0; i < 12; ++i) {
        replies.push_back(reply("READ: f" + std::to_string(i) + ".txt"));
    }
    replies.push_back(reply("Fine."));
    Rig rig(json{{"replies", replies}});
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Read everything.");
    CHECK_EQ(turn.actions.size(), std::size_t{11});
    // A call left where the answer goes is never shown as the answer.
    CHECK_EQ(turn.reply, std::string("(ran out of turns before finishing. Ask again to carry on)"));
    CHECK(!turn.failed);

    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{12});
    const std::string last = "That was the last action available this turn. Write the answer now from what you have.";
    CHECK(!has(text_of(requests.at(10)["messages"].back()), last));
    CHECK(has(text_of(requests.at(11)["messages"].back()), last));

    // The history keeps what the expert said, not what was said about it.
    rig.ask("Well?");
    const json next = rig.requests().at(12)["messages"];
    CHECK_EQ(next.size(), std::size_t{4});
    CHECK_EQ(text_of(next[2]), std::string("READ: f11.txt"));
}

TEST(a_turn_without_a_project_has_no_tools_and_one_round) {
    Rig rig(script({reply("READ: notes.txt")}), {}, /*with_project=*/false);
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Read notes.txt.");
    CHECK(turn.actions.empty());
    CHECK_EQ(turn.reply, std::string("(ran out of turns before finishing. Ask again to carry on)"));
    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{1});
    CHECK(!has(text_of(requests.at(0)["messages"].front()), "You can act on the project"));
}

TEST(a_picture_a_tool_read_goes_to_a_model_that_sees_and_stays_for_the_turn) {
    Rig rig(script({reply("READ: chart.png"), reply("LIST: ."), reply("A chart.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    write_file(rig.project() / "chart.png", std::string("\x89PNG\r\n\x1a\n-not-really-a-png", 25));
    const Turn turn = rig.ask("What is in chart.png?");
    CHECK_EQ(turn.reply, std::string("A chart."));
    CHECK_EQ(turn.actions.size(), std::size_t{2});
    if (!turn.actions.empty()) {
        CHECK(has(turn.actions[0].picture, "chart.png"));
    }
    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{3});
    const json& second = requests.at(1)["messages"];
    CHECK_EQ(pictures_in(second.back()), std::size_t{1});
    CHECK(!has(text_of(second.back()), "cannot see the picture"));
    // Sent again with every round after it, in the place it was handed back.
    const json& third = requests.at(2)["messages"];
    CHECK_EQ(pictures_in(third[third.size() - 3]), std::size_t{1});
    CHECK_EQ(pictures_in(third.back()), std::size_t{0});
}

// ---------------------------------------------------------------------------
// Search

TEST(a_turn_searches_and_is_told_when_it_asks_for_the_same_search_again) {
    Rig rig(json{{"replies", {reply("SEARCH: capital of France"), reply("SEARCH: capital of France"),
                              reply("Paris.")}},
                 {"results", {{{"title", "Paris"}, {"url", "https://example.org/paris"},
                               {"content", "Paris is the capital of France."}}}}},
            [](Config& config) { config.tools.web_search = true; });
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("What is the capital of France?");
    CHECK_EQ(turn.reply, std::string("Paris."));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    if (!turn.actions.empty()) {
        CHECK_EQ(turn.actions[0].summary, std::string("searched \"capital of France\": 1 result from searxng"));
    }
    CHECK_EQ(rig.searches().size(), std::size_t{1});

    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{3});
    CHECK(has(text_of(requests.at(0)["messages"].front()), "You can look things up on the internet."));
    const std::string found = text_of(requests.at(1)["messages"].back());
    CHECK(found.rfind("Web search results for \"capital of France\":", 0) == 0);
    CHECK(has(found, "https://example.org/paris"));
    CHECK(!has(found, "This was the last search available."));
    CHECK_EQ(text_of(requests.at(2)["messages"].back()),
             std::string("You have already searched for \"capital of France\" and been given the "
                         "results above.\n\nThis was the last search available. Answer now from what "
                         "you have, and say plainly if it is not enough."));
    CHECK(in_order(rig.statuses(), {"searching for \"capital of France\"",
                                    "already searched for \"capital of France\""}));
}

TEST(a_turn_that_keeps_searching_is_stopped_and_says_why) {
    // No project: the search's own rounds, Settings' two and the answer.
    Rig rig(json{{"replies", {reply("SEARCH: one"), reply("SEARCH: two"), reply("SEARCH: three")}},
                 {"results", json::array()}},
            [](Config& config) { config.tools.web_search = true; }, /*with_project=*/false);
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Find things.");
    CHECK_EQ(turn.reply, std::string("(kept searching instead of answering. Raise \"Searches per "
                                     "prompt\" in Settings, Tools)"));
    CHECK_EQ(turn.actions.size(), std::size_t{2});
    if (turn.actions.size() == 2) {
        CHECK_EQ(turn.actions[0].summary, std::string("searched \"one\": nothing found for \"one\""));
    }
    const std::vector<json> requests = rig.requests();
    CHECK_EQ(requests.size(), std::size_t{3});
    CHECK(has(text_of(requests.at(2)["messages"].back()), "This was the last search available."));
    CHECK(!has(text_of(requests.at(1)["messages"].back()), "This was the last search available."));
}

// ---------------------------------------------------------------------------
// Turns that end without an answer

TEST(a_turn_with_no_answer_says_which_setting_to_look_at) {
    Rig rig(script({json{{"reasoning", "Let me think about this at length."}, {"finish", "length"}},
                    json{{"content", ""}, {"tokens", 3}}}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn thought = rig.ask("A hard question.");
    CHECK_EQ(thought.reply, std::string("(thought until \"Longest reply\" ran out. Raise it in Settings, "
                                        "Generation, or lower the effort)"));
    CHECK(!thought.failed);
    const Turn quiet = rig.ask("Another.");
    CHECK_EQ(quiet.reply, std::string("(stopped without answering)"));
    // Neither is history: there was nothing said to remember.
    rig.ask("Hello?");
    CHECK_EQ(rig.requests().at(2)["messages"].size(), std::size_t{2});
}

TEST(a_provider_that_fails_fails_the_turn_and_it_is_not_remembered) {
    Rig rig(script({json{{"status", 500}, {"error", "the scripted server fell over"}},
                    reply("Fine now.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn failed = rig.ask("Hello.");
    CHECK(failed.failed);
    CHECK(has(failed.reply, "the scripted server fell over"));
    CHECK(has(failed.reply, "Scripted is having trouble (HTTP 500)"));
    CHECK(rig.snapshot().mood == Mood::Error);
    CHECK(!rig.snapshot().linked.has_value());

    const Turn next = rig.ask("Hello again.");
    CHECK_EQ(next.reply, std::string("Fine now."));
    CHECK_EQ(rig.requests().at(1)["messages"].size(), std::size_t{2});
}

TEST(an_answer_cut_short_keeps_what_came_and_says_why) {
    Rig rig(script({json{{"content", "Half an ans"}, {"error", "overloaded"}}}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Tell me everything.");
    CHECK(turn.failed);
    CHECK_EQ(turn.reply, std::string("Half an ans"));
    CHECK_EQ(turn.actions.size(), std::size_t{1});
    if (!turn.actions.empty()) {
        CHECK(turn.actions[0].summary.rfind("cut short: ", 0) == 0);
        CHECK(has(turn.actions[0].summary, "overloaded"));
    }
}

TEST(a_refusal_takes_back_what_had_streamed) {
    Rig rig(script({json{{"content", "Sure, here is"}, {"finish", "refusal"}}}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Something it will not do.");
    CHECK(turn.failed);
    CHECK_EQ(turn.reply, std::string("programming-model declined this request"));
    CHECK(turn.actions.empty());
}

TEST(stop_ends_a_turn_mid_answer_and_it_is_not_remembered) {
    Rig rig(script({json{{"content", "one two three four five six seven eight"}, {"pieces", 8},
                         {"pause", 0.3}},
                    reply("Fine.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    bool stopped = false;
    const Turn turn = rig.ask("Count.", std::nullopt, {}, [&](const Snapshot& now) {
        if (!stopped && !now.turns.empty() && !now.turns.back().reply.empty()) {
            stopped = true;
            rig.engine().cancel();
        }
    });
    CHECK(stopped);
    CHECK(turn.canceled);
    CHECK(!turn.failed);
    CHECK(!turn.reply.empty());
    CHECK(std::string("one two three four five six seven eight").rfind(turn.reply, 0) == 0);
    CHECK_EQ(rig.snapshot().status, std::string("canceled"));
    rig.ask("Again.");
    CHECK_EQ(rig.requests().at(1)["messages"].size(), std::size_t{2});
}

// ---------------------------------------------------------------------------
// Who answers, and what fits

TEST(a_pinned_turn_goes_to_its_expert_and_an_attachment_steers_the_route) {
    Rig rig(script({reply("Writing here."), reply("Read it."), reply("Programming here.")}));
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn pinned = rig.ask("Who is this?", ExpertId("writing"));
    CHECK(pinned.route && pinned.route->source == RouteSource::Forced);
    CHECK_EQ(rig.requests().at(0)["model"].get<std::string>(), std::string("writing-model"));

    // The names of what was attached are part of what is routed on, and
    // what it says goes above the question.
    write_file(rig.project() / "readme.md", "# The attached text\n");
    attach::Attachment one;
    one.path  = (rig.project() / "readme.md").string();
    one.name  = "readme.md";
    one.kind  = attach::kind_of(one.path);
    one.label = attach::label_for(one.path);
    const Turn routed = rig.ask("What does this say?", std::nullopt, {one});
    CHECK(routed.route && routed.route->expert == "writing");
    CHECK(routed.route && routed.route->source == RouteSource::Keyword);
    CHECK_EQ(routed.attachments.size(), std::size_t{1});
    const json asked = rig.requests().at(1)["messages"].back();
    CHECK(has(text_of(asked), "# The attached text"));
    CHECK(text_of(asked).size() > std::string("What does this say?").size());
    CHECK(text_of(asked).rfind("What does this say?") == text_of(asked).size() - 19);

    // Nothing attached, nothing matched: the first seat with a model.
    const Turn fallback = rig.ask("What does this say?");
    CHECK(fallback.route && fallback.route->expert == "programming");
}

TEST(a_turn_with_no_model_to_answer_says_so) {
    Rig rig(script({}), [](Config& config) {
        config.experts["programming"].model.clear();
        config.experts["programming"].provider.clear();
        config.experts["writing"].model.clear();
        config.experts["writing"].provider.clear();
    });
    if (!rig.ready()) {
        skipped();
        return;
    }
    const Turn turn = rig.ask("Anyone?");
    CHECK(turn.failed);
    CHECK_EQ(turn.reply, std::string("No expert has a model. Add one in Settings, Experts."));
    CHECK(rig.snapshot().mood == Mood::Error);
    CHECK_EQ(rig.snapshot().status, std::string("no experts configured"));
    CHECK(rig.requests().empty());
}

TEST(a_conversation_too_long_for_the_context_is_trimmed_or_refused_as_set) {
    // About fifty thousand tokens an exchange, by the estimate a provider's
    // model is counted with, against the ninety-six thousand of a 128k
    // context a prompt may take.
    const auto history = [] {
        std::vector<ChatMessage> out;
        for (int i = 0; i < 3; ++i) {
            out.push_back({"user", "question " + std::to_string(i) + " " + std::string(150000, 'x')});
            out.push_back({"assistant", "answer " + std::to_string(i)});
        }
        return out;
    };
    {
        Rig rig(script({reply("Fits now.")}));
        if (!rig.ready()) {
            skipped();
            return;
        }
        rig.engine().restore_history(history());
        const Turn turn = rig.ask("And now?");
        CHECK_EQ(turn.reply, std::string("Fits now."));
        CHECK_EQ(turn.actions.size(), std::size_t{1});
        if (!turn.actions.empty()) {
            CHECK_EQ(turn.actions[0].summary,
                     std::string("dropped 2 earlier exchanges to stay inside the context"));
        }
        const json messages = rig.requests().at(0)["messages"];
        CHECK_EQ(messages.size(), std::size_t{4});
        CHECK(text_of(messages[1]).rfind("question 2 ", 0) == 0);
        CHECK(rig.snapshot().context_used <= 96000);
    }
    {
        Rig rig(script({reply("Never asked.")}), [](Config& config) { config.tools.overflow = "stop"; });
        rig.engine().restore_history(history());
        const Turn turn = rig.ask("And now?");
        CHECK(turn.failed);
        CHECK(turn.reply.rfind("this conversation no longer fits: ", 0) == 0);
        CHECK(has(turn.reply, " of 128000 tokens. Raise \"Context\" in Settings, Generation, or start a new chat."));
        CHECK_EQ(rig.snapshot().status, std::string("context full"));
        CHECK(rig.requests().empty());
    }
}
