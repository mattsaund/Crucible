// SPDX-License-Identifier: MIT
//
// The Config type's own behavior: inheritance from defaults, and resolving
// model references against the models directory.
//
// Anything to do with the file on disk lives in config_io.cpp.
#include "crucible/config/config.hpp"

#include "crucible/config/paths.hpp"
#include "crucible/llm/model_catalog.hpp"

namespace crucible {

std::string_view overflow_id(Overflow policy) {
    switch (policy) {
        case Overflow::RollingWindow:  return "rolling";
        case Overflow::TruncateMiddle: return "middle";
        case Overflow::StopAtLimit:    return "stop";
    }
    return "rolling";
}

Overflow overflow_from_id(std::string_view id) {
    if (id == "middle") { return Overflow::TruncateMiddle; }
    if (id == "stop")   { return Overflow::StopAtLimit; }
    return Overflow::RollingWindow;
}


void ModelParams::inherit_from(const ModelParams& base) {
    const ModelParams pristine;  // a field still equal to this was never set

    if (n_gpu_layers   == pristine.n_gpu_layers)   { n_gpu_layers   = base.n_gpu_layers; }
    if (main_gpu       == pristine.main_gpu)       { main_gpu       = base.main_gpu; }
    if (split_mode     == pristine.split_mode)     { split_mode     = base.split_mode; }
    if (tensor_split.empty())                      { tensor_split   = base.tensor_split; }
    if (n_ctx          == pristine.n_ctx)          { n_ctx          = base.n_ctx; }
    if (n_batch        == pristine.n_batch)        { n_batch        = base.n_batch; }
    if (n_threads      == pristine.n_threads)      { n_threads      = base.n_threads; }
    if (flash_attn     == pristine.flash_attn)     { flash_attn     = base.flash_attn; }
    if (temperature    == pristine.temperature)    { temperature    = base.temperature; }
    if (top_p          == pristine.top_p)          { top_p          = base.top_p; }
    if (top_k          == pristine.top_k)          { top_k          = base.top_k; }
    if (min_p          == pristine.min_p)          { min_p          = base.min_p; }
    if (repeat_penalty == pristine.repeat_penalty) { repeat_penalty = base.repeat_penalty; }
    if (repeat_last_n  == pristine.repeat_last_n)  { repeat_last_n  = base.repeat_last_n; }
    if (max_tokens     == pristine.max_tokens)     { max_tokens     = base.max_tokens; }
    if (seed           == pristine.seed)           { seed           = base.seed; }
}

const ModelParams& Config::expert(const ExpertId& id) const {
    // A seat with nothing assigned reads as an unfilled entry rather than as an
    // absent one, so every caller can ask for parameters without first asking
    // whether there are any. `model` is empty in that entry, which is the same
    // test `has_expert` makes.
    static const ModelParams kUnfilled;
    const auto found = experts.find(id);
    return found == experts.end() ? kUnfilled : found->second;
}

bool Config::has_expert(const ExpertId& id) const {
    return !expert(id).model.empty();
}

std::vector<ExpertId> Config::configured_experts() const {
    // Roster order, not map order. The map is sorted by id, and a list of
    // experts that reads "Biology, Chemistry, Engineering..." when the
    // expert panel draws them in a different order is a small but constant
    // friction.
    std::vector<ExpertId> found;
    for (const Expert& seat : roster.experts()) {
        if (has_expert(seat.id)) {
            found.push_back(seat.id);
        }
    }
    return found;
}

bool Config::is_empty() const {
    return router.model.empty() && configured_experts().empty();
}

std::filesystem::path Config::resolved_models_dir() const {
    if (models_dir.empty()) {
        return paths::models_dir();
    }
    return paths::expand_user(models_dir);
}

void Config::resolve_models() {
    const std::filesystem::path dir = resolved_models_dir();
    router.path = resolve_model_ref(dir, router.model).string();
    for (auto& [id, params] : experts) {
        params.path = resolve_model_ref(dir, params.model).string();
    }
}


}  // namespace crucible
