// SPDX-License-Identifier: MIT
// crucible-smoke -- one prompt through the whole engine, with no window.
//
//   crucible-smoke <delegator.gguf or -> <expert.gguf> "a question"
//   crucible-smoke --build <expert.gguf> "a directive" ["a question"]
//
// Everything a prompt typed into the window goes through, except the window:
// the orchestrator started on Crucible's Python (or CRUCIBLE_PYTHON), the
// route, the delegator and the expert loaded and freed, the answer generated
// in a scratch project folder. It prints the route and the reply, and exits
// non-zero when the turn failed -- so a platform's whole prompt path can be
// run where no window can be opened: a CI runner, or Windows code under Wine.
//
// With --build it runs a build of the directive instead, with up to three
// agents, and -- when a question is given -- asks it in the chat while the
// build is working: the two at once, through the same engine, which is the
// path a person takes who chats while a build runs. It prints the tasks and
// the reply, and exits non-zero when the build failed outright or the chat
// turn did.
//
//   crucible-smoke --bench <model.gguf> [quick|standard|full] [mmlu_pro,arc,...] [seconds]
//
// With --bench it runs the model through the Bench the way the Bench tab
// does, and prints how far it has got every ten seconds and the result at
// the end: how fast a run goes on this machine, timed without a window.
// Given seconds, it presses Stop after that long, and what was answered by
// then is kept as a stopped run's is: a sample of a model too slow to run
// through whole.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <nlohmann/json.hpp>

#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/routing/router.hpp"

using namespace crucible;

namespace {

/// A build of `directive` on `expert`, with `question` asked in the chat
/// while it works when there is one. See the file comment.
int smoke_build(const std::string& expert, const std::string& directive, const std::string& question) {
    Config config;
    config.routing.keep_delegator_loaded = false;
    config.build.confirm_plan = false;
    config.build.agents       = 3;
    config.tools.auto_edits   = true;
    std::string error;
    for (const auto& [name, blurb] :
         {std::pair<const char*, const char*>{"Programming", "Code, programs, scripts and tests"},
          {"Writing", "Documentation, READMEs and prose"}}) {
        Expert seat;
        seat.name  = name;
        seat.blurb = blurb;
        if (!config.roster.add(seat, error)) {
            std::printf("could not add %s: %s\n", name, error.c_str());
            return 1;
        }
    }
    for (const Expert& seat : config.roster.experts()) {
        config.experts[seat.id].model = std::filesystem::absolute(expert).string();
    }
    config.resolve_models();

    const std::filesystem::path project = paths::data_dir() / "smoke-build";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "smoke-build-history");
    engine.start();
    if (const std::string refused = engine.start_cook(directive, 0, project, {}, std::nullopt, "build");
        !refused.empty()) {
        std::printf("refused: %s\n", refused.c_str());
        return 1;
    }

    // The question, once the build has its plan: asked while its agents work.
    bool asked = question.empty();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        if (!asked && now.cook && !now.cook->tasks.empty()) {
            std::printf("asking the chat while the build works: %s\n", question.c_str());
            engine.submit(question);
            asked = true;
        }
        const bool chat_done = question.empty()
                            || (!now.turns.empty() && !now.turns.back().streaming && !engine.is_busy());
        if (asked && chat_done && !engine.cooking()) {
            break;
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(30)) {
            std::printf("timed out\n");
            engine.cancel_cook();
            engine.stop();
            return 1;
        }
    }
    const Snapshot after = state.snapshot();
    int failed = 0;
    if (after.cook) {
        std::printf("build: %s -- %s\n", std::string(cook_state_name(after.cook->state)).c_str(),
                    after.cook->outcome.substr(0, 400).c_str());
        for (const CookTask& task : after.cook->tasks) {
            std::printf("  task %d %-10s %s (%s)\n", task.index + 1, task.state.c_str(),
                        task.title.c_str(), task.expert.c_str());
        }
        failed = after.cook->state == CookState::Failed ? 1 : 0;
    }
    if (!question.empty() && !after.turns.empty()) {
        const Turn& turn = after.turns.back();
        std::printf("chat %s: %s\n", turn.failed ? "failed" : "reply", turn.reply.substr(0, 400).c_str());
        failed = failed || turn.failed || turn.reply.empty() ? 1 : 0;
    }
    engine.stop();
    return failed;
}

/// `model` through the Bench at `size`, on the benchmarks named in `only`
/// (comma separated) or every one, stopped after `stop_after` seconds when
/// that is more than none. See the file comment.
int smoke_bench(const std::string& model, const std::string& size, const std::string& only, double stop_after) {
    using nlohmann::json;
    Config config;
    config.routing.keep_delegator_loaded = false;
    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.start();

    json benchmarks = json::array();
    std::stringstream names(only);
    for (std::string name; std::getline(names, name, ',');) {
        if (!name.empty()) {
            benchmarks.push_back(name);
        }
    }
    const std::string path = std::filesystem::absolute(model).string();
    if (const std::string refused = engine.start_bench(json{
            {"models", json::array({json{{"path", path}, {"name", std::filesystem::path(path).filename().string()}}})},
            {"benchmarks", benchmarks}, {"size", size}, {"effort", ""}});
        !refused.empty()) {
        std::printf("refused: %s\n", refused.c_str());
        return 1;
    }

    // Done is what the run says, once the engine has let it go.
    const auto            started = std::chrono::steady_clock::now();
    auto                  said    = started;
    json                  last;
    std::set<std::string> finished;
    bool                  pressed = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        if (now.bench) {
            last = *now.bench;
        }
        if (!engine.benching() && last.is_object() && !last.value("running", true)) {
            break;
        }
        const auto at = std::chrono::steady_clock::now();
        if (stop_after > 0 && !pressed && std::chrono::duration<double>(at - started).count() >= stop_after) {
            pressed = true;
            std::printf("%6.1f s  stop\n", std::chrono::duration<double>(at - started).count());
            engine.stop_bench();
        }
        // When each benchmark was done, to the quarter second.
        for (const json& entry : last.is_object() ? last.value("models", json::array()) : json::array()) {
            const json benches = entry.value("benchmarks", json::object());
            for (const auto& [id, progress] : benches.items()) {
                if (progress.value("n", 0) > 0 && progress.value("done", 0) == progress.value("n", 0)
                    && finished.insert(id).second) {
                    std::printf("%6.1f s  %s done\n", std::chrono::duration<double>(at - started).count(), id.c_str());
                    std::fflush(stdout);
                }
            }
        }
        if (at - said >= std::chrono::seconds(10) && last.is_object()) {
            said = at;
            int done = 0;
            int correct = 0;
            std::string where;
            for (const json& entry : last.value("models", json::array())) {
                // Held here: items() of the temporary value() returns would outlive it.
                const json benches = entry.value("benchmarks", json::object());
                for (const auto& [id, progress] : benches.items()) {
                    done += progress.value("done", 0);
                    correct += progress.value("correct", 0);
                }
                where = entry.value("state", std::string()) + " " + entry.value("current", std::string());
            }
            const double seconds = std::chrono::duration<double>(at - started).count();
            std::printf("%6.0f s  %s  %d done, %d right\n", seconds, where.c_str(), done, correct);
            std::fflush(stdout);
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    int failed = last.contains("error") ? 1 : 0;
    if (failed != 0) {
        std::printf("error: %s\n", last["error"].dump().c_str());
    }
    for (const json& entry : last.value("models", json::array())) {
        std::printf("%s: %s, loaded in %ld ms, %s\n", entry.value("name", std::string()).c_str(),
                    entry.value("state", std::string()).c_str(), entry.value("load_ms", 0L),
                    entry.value("saved", std::string()).c_str());
        const json benches = entry.value("benchmarks", json::object());
        for (const auto& [id, progress] : benches.items()) {
            std::printf("  %-11s %3d of %3d, %3d right%s\n", id.c_str(), progress.value("done", 0),
                        progress.value("n", 0), progress.value("correct", 0),
                        progress.contains("error") ? (", " + progress["error"].dump()).c_str() : "");
        }
        if (entry.value("state", std::string()) != "done") {
            failed = 1;
        }
    }
    std::printf("%.0f s in all\n", seconds);
    engine.stop();
    return failed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--build") {
        return smoke_build(argv[2], argv[3], argc >= 5 ? argv[4] : "");
    }
    if (argc >= 3 && std::string(argv[1]) == "--bench") {
        return smoke_bench(argv[2], argc >= 4 ? argv[3] : "quick", argc >= 5 ? argv[4] : "",
                           argc >= 6 ? std::atof(argv[5]) : 0.0);
    }
    if (argc < 4) {
        std::printf("usage: crucible-smoke <delegator.gguf or -> <expert.gguf> \"a question\"\n"
                    "       crucible-smoke --build <expert.gguf> \"a directive\" [\"a question\"]\n"
                    "       crucible-smoke --bench <model.gguf> [quick|standard|full] [mmlu_pro,arc,...] [seconds]\n");
        return 2;
    }
    const std::string delegator = argv[1];
    const std::string expert    = argv[2];
    const std::string prompt    = argv[3];

    Config config;
    if (delegator != "-") {
        config.router.model = std::filesystem::absolute(delegator).string();
    }
    config.routing.keep_delegator_loaded = false;
    std::string error;
    for (const auto& [name, blurb] :
         {std::pair<const char*, const char*>{"Mathematics", "Proofs, algebra, calculus and arithmetic"},
          {"Chemistry", "Reactions, bonding, acids and the periodic table"}}) {
        Expert seat;
        seat.name  = name;
        seat.blurb = blurb;
        if (!config.roster.add(seat, error)) {
            std::printf("could not add %s: %s\n", name, error.c_str());
            return 1;
        }
    }
    for (const Expert& seat : config.roster.experts()) {
        config.experts[seat.id].model = std::filesystem::absolute(expert).string();
        // One seed rather than a new one each run, so the same prompt gets
        // the same reply from the same build: what lets two builds' answers
        // be compared word for word.
        config.experts[seat.id].seed = 1234;
    }
    config.resolve_models();

    const std::filesystem::path project = paths::data_dir() / "smoke-project";
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "smoke-history");
    engine.start();
    engine.submit(prompt);

    // The turn is over when it has stopped streaming and the engine is idle.
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<const PendingEdit> allowed;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        // A write waits for a yes, and there is nobody at a window to give
        // it: it is given here, as a person would, and said.
        if (now.pending_edit && now.pending_edit != allowed) {
            allowed = now.pending_edit;
            std::printf("edit: %s (allowed)\n", allowed->path.c_str());
            engine.approve_edit(true);
        }
        const bool done = !now.turns.empty() && !now.turns.back().streaming && !engine.is_busy();
        if (done) {
            break;
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
            std::printf("timed out\n");
            engine.stop();
            return 1;
        }
    }
    const Snapshot after = state.snapshot();
    const Turn& turn = after.turns.back();
    if (turn.route) {
        std::printf("route: %s (%s, %.2f) %s\n", turn.route->expert.c_str(),
                    std::string(route_source_name(turn.route->source)).c_str(),
                    static_cast<double>(turn.route->confidence), turn.route->detail.c_str());
    }
    // What the turn did besides talk -- a file read, a command run -- so a
    // turn that used a tool can be told from one that did not.
    for (const TurnAction& action : turn.actions) {
        std::printf("action: %s\n", action.summary.c_str());
    }
    std::printf("%s: %s\n", turn.failed ? "failed" : "reply", turn.reply.c_str());
    engine.stop();
    return turn.failed || turn.reply.empty() ? 1 : 0;
}
