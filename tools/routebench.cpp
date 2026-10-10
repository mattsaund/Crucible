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
//
// The routing itself is the orchestrator's, in Python, exactly as the app runs
// it: this loads the delegator, starts the orchestrator on Crucible's Python
// (or CRUCIBLE_PYTHON), and answers its calls to format and score with the
// model loaded here.

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
#include "crucible/lab/python.hpp"
#include "crucible/orchestra/link.hpp"
#include "crucible/routing/router.hpp"
#include <array>
#include <cstdlib>
#include <fstream>

using namespace crucible;

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: crucible-routebench <router-model.gguf> [--gpu-layers N]\n");
        return 2;
    }

    float       calibration = 0.25F;  // the orchestrator's own, routing.CALIBRATION
    std::string explain;
    // --cases FILE: a roster and the prompts to route against it, as JSON --
    // {"roster": [{"id", "name", "tag", "blurb", "keywords", "examples"}...],
    //  "cases": [["expected id", "prompt"], ...]} -- for a roster other than
    // the nine the built-in cases are written for. --method NAME is handed to
    // the orchestrator's router, which is how a routing idea is measured
    // against the shipped one without a rebuild.
    std::string cases_file;
    std::string method;

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
        if (std::string(argv[i]) == "--cases") {
            cases_file = argv[i + 1];
        }
        if (std::string(argv[i]) == "--method") {
            method = argv[i + 1];
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
    using json = nlohmann::json;
    json experts = json::array();
    std::vector<std::pair<std::string, std::string>> cases;
    if (cases_file.empty()) {
        for (const Expert& expert : benchmark_roster().experts()) {
            experts.push_back(json{{"id", expert.id}, {"name", expert.name}, {"tag", expert.tag},
                                   {"blurb", expert.blurb}, {"keywords", expert.keywords},
                                   {"examples", expert.examples}});
        }
        for (const RouteCase& test : benchmark_cases()) {
            cases.emplace_back(std::string(test.expect), std::string(test.prompt));
        }
    } else {
        json doc;
        try {
            std::ifstream in(cases_file);
            in >> doc;
            experts = doc.at("roster");
            for (const json& one : doc.at("cases")) {
                cases.emplace_back(one.at(0).get<std::string>(), one.at(1).get<std::string>());
            }
        } catch (const std::exception& e) {
            std::printf("could not read %s: %s\n", cases_file.c_str(), e.what());
            return 1;
        }
    }
    const auto name_of = [&experts](const std::string& id) {
        for (const json& one : experts) {
            if (one.value("id", "") == id) {
                return one.value("name", id);
            }
        }
        return id;
    };

    if (!lab::python::installed()) {
        std::printf("Crucible's Python is not installed: run crucible --install-python, or set "
                    "CRUCIBLE_PYTHON to a Python 3.10 or newer\n");
        return 1;
    }

    // The orchestrator's calls, answered with the model loaded above.
    const orchestra::Handler serve = [&](const std::string& method, const json& asked) -> json {
        if (method == "delegator.ready") {
            return json{{"available", true}, {"path", params.path}};
        }
        if (method == "delegator.format") {
            std::vector<ChatMessage> messages;
            for (const json& one : asked.value("messages", json::array())) {
                messages.push_back({one.value("role", "user"), one.value("content", "")});
            }
            return json{{"text", model->format_chat(messages, true)}};
        }
        if (method == "delegator.score") {
            std::vector<std::string> labels;
            for (const json& one : asked.value("labels", json::array())) {
                labels.push_back(one.get<std::string>());
            }
            json out = json::array();
            for (const float score : model->score_labels(asked.value("prompt", ""), labels, {})) {
                out.push_back(score <= kUnscored ? json(nullptr) : json(score));
            }
            return json{{"scores", out}, {"canceled", false}};
        }
        throw std::runtime_error("routebench has no " + method);
    };

    // Every seat is filled and there is no floor, so what comes back is the
    // delegator's choice and nothing the policy did to it.
    const auto request = [&](const std::string& prompt) {
        json seats = json::object();
        for (const json& expert : experts) {
            seats[expert.value("id", "")] = json{{"model", true}};
        }
        json out{{"prompt", prompt}, {"roster", experts}, {"seats", seats},
                 {"routing", {{"min_confidence", 0.0}, {"default_expert", ""}}},
                 {"calibration", calibration}};
        if (!method.empty()) {
            out["method"] = method;
        }
        return out;
    };

    orchestra::Link orchestrator;
    orchestrator.set_handler(serve);
    std::string started;
    if (!orchestrator.start(started)) {
        std::printf("the orchestrator would not start: %s\n", started.c_str());
        return 1;
    }

    if (!explain.empty()) {
        const json out = orchestrator.call("route.explain", request(explain));
        std::printf("%-14s %9s %9s %9s\n", "expert", "raw", "bias", "calibrated");
        for (std::size_t i = 0; i < out["labels"].size(); ++i) {
            std::printf("%-14s %9.2f %9.2f %9.2f\n", out["labels"][i].get<std::string>().c_str(),
                        out["raw"][i].get<double>(), out["bias"][i].get<double>(),
                        out["calibrated"][i].get<double>());
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

    for (const auto& [expect, prompt] : cases) {
        const auto start = std::chrono::steady_clock::now();
        const json answer = orchestrator.call("route", request(prompt));
        RouteDecision decision;
        decision.expert     = answer.value("expert", "");
        decision.confidence = answer.value("confidence", 0.0F);
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - start).count();
        total_ms += ms;

        const ExpertId want(expect);
        const bool ok = decision.expert == want;
        correct += ok ? 1 : 0;
        ++expected[want];
        ++chosen[decision.expert];
        ++confusion[want][decision.expert];
        if (!quiet) {
            std::printf("%s  %-12s (want %-12s) conf %.2f  %4.0fms  %.44s\n",
                        ok ? "ok  " : "MISS",
                        name_of(decision.expert).c_str(), name_of(want).c_str(),
                        static_cast<double>(decision.confidence), ms, prompt.c_str());
        }
    }

    // Per expert, because an average hides the failure that matters. A
    // delegator can score 85% while never once choosing one of the nine, and
    // that seat is then unreachable however good the model is.
    std::printf("\n%-14s %-8s %-8s  where the misses went\n", "expert", "found", "chosen");
    for (const json& expert : experts) {
        const std::string id = expert.value("id", "");
        std::string went;
        for (const json& other : experts) {
            const std::string other_id = other.value("id", "");
            const int count = confusion[id][other_id];
            if (other_id != id && count > 0) {
                if (!went.empty()) {
                    went += ", ";
                }
                went += name_of(other_id) + " x" + std::to_string(count);
            }
        }
        // "chosen" counts how often the delegator picked this expert for
        // anything at all. A zero there is a seat nothing can reach.
        std::printf("%-14s %d/%-6d %-8d  %s\n", name_of(id).c_str(),
                    confusion[id][id], expected[id], chosen[id], went.c_str());
    }

    std::printf("\n%d/%zu correct (%.0f%%), %.0fms per route\n", correct, cases.size(),
                100.0 * static_cast<double>(correct) / static_cast<double>(cases.size()),
                total_ms / static_cast<double>(cases.size()));
    // Anything near one in the number of seats is a model that is not reading
    // the prompt at all -- usually a chat template or prompt problem.
    return correct * 2 >= static_cast<int>(cases.size()) ? 0 : 1;
}
