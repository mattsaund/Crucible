// SPDX-License-Identifier: MIT
// crucible-smoke -- one prompt through the whole engine, with no window.
//
//   crucible-smoke <delegator.gguf or -> <expert.gguf> "a question"
//
// Everything a prompt typed into the window goes through, except the window:
// the orchestrator started on Crucible's Python (or CRUCIBLE_PYTHON), the
// route, the delegator and the expert loaded and freed, the answer generated
// in a scratch project folder. It prints the route and the reply, and exits
// non-zero when the turn failed -- so a platform's whole prompt path can be
// run where no window can be opened: a CI runner, or Windows code under Wine.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>

#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/routing/router.hpp"

using namespace crucible;

int main(int argc, char** argv) {
    if (argc < 4) {
        std::printf("usage: crucible-smoke <delegator.gguf or -> <expert.gguf> \"a question\"\n");
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
