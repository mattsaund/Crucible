// SPDX-License-Identifier: MIT
//
// See starter.hpp.
#include "crucible/app/starter.hpp"

#include <array>

#include "crucible/llm/model_catalog.hpp"

namespace crucible::starter {

std::string Model::url() const { return "https://huggingface.co/" + repo + "/resolve/main/" + file; }

Model for_memory(std::uint64_t memory) {
    // Open models that write code, as GGUF at four bits a weight, measured
    // on 2026-10-10, and every one Apache-2.0: what somebody's business is
    // built on has to be theirs to use. Largest first; the first whose file
    // is under half the machine's memory is the one. A thirty-billion-
    // parameter model that runs three billion at a time is first because it
    // is as quick as a small one and writes like a large one, where the
    // memory holds it. Qwen2.5 Coder at 3B is not here -- its license is
    // research only -- so Qwen3's general 4B stands between 7B and 1.5B.
    static const std::array<Model, 5> kList{{
        {"unsloth/Qwen3-Coder-30B-A3B-Instruct-GGUF", "Qwen3-Coder-30B-A3B-Instruct-Q4_K_M.gguf",
         "Qwen3 Coder 30B", 18556689568ULL},
        {"bartowski/Qwen2.5-Coder-14B-Instruct-GGUF", "Qwen2.5-Coder-14B-Instruct-Q4_K_M.gguf",
         "Qwen2.5 Coder 14B", 8988111072ULL},
        {"bartowski/Qwen2.5-Coder-7B-Instruct-GGUF", "Qwen2.5-Coder-7B-Instruct-Q4_K_M.gguf",
         "Qwen2.5 Coder 7B", 4683074336ULL},
        {"unsloth/Qwen3-4B-Instruct-2507-GGUF", "Qwen3-4B-Instruct-2507-Q4_K_M.gguf",
         "Qwen3 4B", 2497281120ULL},
        {"bartowski/Qwen2.5-Coder-1.5B-Instruct-GGUF", "Qwen2.5-Coder-1.5B-Instruct-Q4_K_M.gguf",
         "Qwen2.5 Coder 1.5B", 986048800ULL},
    }};
    for (const Model& model : kList) {
        if (model.bytes * 2 <= memory) {
            return model;
        }
    }
    return kList.back();
}

bool needed(const Config& config) {
    if (!config.providers.empty()) {
        return false;
    }
    for (const Expert& expert : config.roster.experts()) {
        if (config.has_expert(expert.id)) {
            return false;
        }
    }
    return scan_models(config.resolved_models_dir()).empty();
}

void seat(Config& config, const std::string& file) {
    if (config.roster.empty()) {
        Expert programming;
        programming.name  = "Programming";
        programming.blurb = "Writes, fixes and explains software: apps, websites, scripts and their tests";
        std::string error;
        config.roster.add(programming, error);
    }
    for (const Expert& expert : config.roster.experts()) {
        ModelParams& params = config.experts[expert.id];
        if (params.model.empty()) {
            params.model    = file;
            params.provider.clear();
        }
    }
    if (config.build.worker_model.empty()) {
        config.build.worker_model = file;
        config.build.worker_provider.clear();
    }
    if (config.routing.default_expert.empty() && !config.roster.empty()) {
        config.routing.default_expert = config.roster.experts().front().id;
    }
    config.resolve_models();
}

}  // namespace crucible::starter
