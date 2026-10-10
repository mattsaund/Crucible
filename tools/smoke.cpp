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

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

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

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 4 && std::string(argv[1]) == "--build") {
        return smoke_build(argv[2], argv[3], argc >= 5 ? argv[4] : "");
    }
    if (argc < 4) {
        std::printf("usage: crucible-smoke <delegator.gguf or -> <expert.gguf> \"a question\"\n"
                    "       crucible-smoke --build <expert.gguf> \"a directive\" [\"a question\"]\n");
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
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
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
    std::printf("%s: %s\n", turn.failed ? "failed" : "reply", turn.reply.c_str());
    engine.stop();
    return turn.failed || turn.reply.empty() ? 1 : 0;
}
