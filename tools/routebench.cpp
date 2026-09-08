// SPDX-License-Identifier: MIT
// crucible-routebench -- measure how well a model does the delegator's job.
//
// Routing quality is the single thing that decides whether Crucible sends your
// question to the right expert, and it is not obvious from a model's size or
// its benchmark scores. This loads one candidate delegator, routes a fixed set
// of prompts whose subject is not in doubt, and reports how many it got right.
//
//   crucible-routebench ~/.local/share/crucible/models/LFM2-1.2B-Q8_0.gguf
//
// Sampling is greedy, so repeated runs give identical results and a change in
// the score is a real change rather than sampling noise.

#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/routing/benchmark.hpp"
#include "crucible/routing/router.hpp"
#include <array>
#include <cstdlib>

using namespace crucible;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: crucible-routebench <router-model.gguf> [--gpu-layers N]\n");
        return 2;
    }

    float       calibration = ModelRouter::kCalibration;
    std::string explain;

    ModelParams params;
    params.path        = paths::expand_user(argv[1]).string();
    params.model       = params.path;
    params.n_ctx       = 4096;
    // Every layer on the GPU by default, which is what the app does. --gpu-layers 0
    // forces the processor, for measuring a machine with no GPU at all.
    params.n_gpu_layers = -1;
    params.temperature = 0.0F;  // greedy, so the number is reproducible
    params.max_tokens  = 16;

    bool quiet = false;
    for (int i = 2; i < argc; ++i) {
        if (std::string(argv[i]) == "--quiet") { quiet = true; }
    }
    for (int i = 2; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--gpu-layers") {
            params.n_gpu_layers = std::atoi(argv[i + 1]);
        }
        if (std::string(argv[i]) == "--calibration") {
            calibration = static_cast<float>(std::atof(argv[i + 1]));
        }
        if (std::string(argv[i]) == "--explain") {
            explain = argv[i + 1];
        }
    }

    ModelHost host(paths::log_file());
    std::string error;
    LoadedModel* model = host.acquire_router(params, [](float) {}, {}, error);
    if (model == nullptr) {
        std::printf("could not load: %s\n", error.c_str());
        return 1;
    }

    std::printf("%s\n%.2fB parameters, %.1f GB\n\n", params.path.c_str(),
                static_cast<double>(model->params()) / 1e9,
                static_cast<double>(model->bytes()) / (1024.0 * 1024.0 * 1024.0));

    // Every seat on the roster is scored, and the scorer can only answer with
    // one of them. This measures the delegator's job and nothing else.
    // Measured against the benchmark roster, which is what the cases are
    // written for. A user's own config may have added seats or taken some
    // away; scoring against that would be measuring their roster, not the
    // delegator.
    const auto roster = std::make_shared<const Roster>(benchmark_roster());

    ModelRouter router(*model, params, roster);
    router.set_calibration(calibration);

    // --explain dumps the arithmetic behind one decision, which is the only way
    // to tell a delegator that dislikes a subject from a calibration that is
    // taking it away.
    if (!explain.empty()) {
        router.route("warm up so the bias is measured", {});
        const std::vector<float> raw  = router.raw_scores(explain);
        const std::vector<float> bias = router.bias();
        const std::vector<std::string> labels = roster->router_labels();
        std::printf("%-14s %9s %9s %9s\n", "expert", "raw", "bias", "calibrated");
        for (std::size_t i = 0; i < labels.size() && i < raw.size(); ++i) {
            const double adjusted =
                static_cast<double>(raw[i]) -
                static_cast<double>(calibration) *
                    (i < bias.size() ? static_cast<double>(bias[i]) : 0.0);
            std::printf("%-14s %9.2f %9.2f %9.2f\n", labels[i].c_str(),
                        static_cast<double>(raw[i]),
                        i < bias.size() ? static_cast<double>(bias[i]) : 0.0, adjusted);
        }
        std::printf("\nprompt: %s\n", explain.c_str());
        return 0;
    }
    std::printf("calibration %.2f\n\n", static_cast<double>(calibration));
    int    correct  = 0;
    double total_ms = 0.0;

    // [wanted][got], plus the margins, for the breakdown below. Keyed by id
    // rather than sized by an enum, because there is no longer a compile-time
    // count of seats to size it by.
    std::map<ExpertId, std::map<ExpertId, int>> confusion;
    std::map<ExpertId, int> expected;
    std::map<ExpertId, int> chosen;

    for (const RouteCase& test : benchmark_cases()) {
        const auto start = std::chrono::steady_clock::now();
        const RouteDecision decision = router.route(std::string(test.prompt), {});
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        total_ms += ms;

        const ExpertId want(test.expect);
        const bool ok = decision.expert == want;
        correct += ok ? 1 : 0;
        ++expected[want];
        ++chosen[decision.expert];
        ++confusion[want][decision.expert];
        if (!quiet) {
            std::printf("%s  %-12s (want %-12s) conf %.2f  %4.0fms  %.44s\n",
                        ok ? "ok  " : "MISS",
                        expert_label(*roster, decision.expert).c_str(),
                        expert_label(*roster, want).c_str(),
                        static_cast<double>(decision.confidence), ms, std::string(test.prompt).c_str());
        }
    }

    // Per expert, because an average hides the failure that matters. A
    // delegator can score 85% while never once choosing one of the nine, and
    // that seat is then unreachable however good the model is.
    std::printf("\n%-14s %-8s %-8s  where the misses went\n", "expert", "found", "chosen");
    for (const Expert& expert : roster->experts()) {
        const ExpertId& id = expert.id;
        std::string went;
        for (const Expert& other : roster->experts()) {
            const int count = confusion[id][other.id];
            if (other.id != id && count > 0) {
                if (!went.empty()) {
                    went += ", ";
                }
                went += other.name + " x" + std::to_string(count);
            }
        }
        // "chosen" counts how often the delegator picked this expert for
        // anything at all. A zero there is a seat nothing can reach.
        std::printf("%-14s %d/%-6d %-8d  %s\n", expert.name.c_str(),
                    confusion[id][id], expected[id], chosen[id], went.c_str());
    }

    std::printf("\n%d/%zu correct (%.0f%%), %.0fms per route\n", correct, benchmark_cases().size(),
                100.0 * static_cast<double>(correct) / static_cast<double>(benchmark_cases().size()),
                total_ms / static_cast<double>(benchmark_cases().size()));
    // Nine seats, so anything near 11% is a model that is not reading the
    // prompt at all -- usually a chat template or prompt problem, not the model.
    return correct * 2 >= static_cast<int>(benchmark_cases().size()) ? 0 : 1;
}
