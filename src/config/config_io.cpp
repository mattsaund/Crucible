// SPDX-License-Identifier: MIT
//
// Reading and writing config.json.
//
// Kept separate from the Config type itself so the rules about *what* a setting
// means stay in config.cpp, and the rules about how it survives a round trip
// to disk stay here.
//
// Loading is forgiving on purpose: a single mistyped field costs that field,
// not the other eight experts, and every problem is collected as a warning the
// UI can show rather than thrown.
#include "crucible/config/config.hpp"

#include <cmath>
#include <fstream>
#include <system_error>
#include <nlohmann/json.hpp>

#include "crucible/llm/model_catalog.hpp"
#include "crucible/config/paths.hpp"

namespace crucible {
namespace {

using json = nlohmann::json;

/// Read one optional field, leaving the destination untouched when the key is
/// absent or holds the wrong type. A malformed entry is reported and skipped
/// rather than aborting the load -- a typo in one expert should not stop the
/// other eight from working.
template <typename T>
void read_field(const json& obj, const char* key, T& dest,
                std::string_view context, std::vector<std::string>& warnings) {
    const auto it = obj.find(key);
    if (it == obj.end() || it->is_null()) {
        return;
    }
    try {
        dest = it->get<T>();
    } catch (const json::exception& e) {
        warnings.emplace_back(std::string(context) + "." + key + ": " + e.what()
                              + " (keeping the default)");
    }
}

void read_model_params(const json& obj, ModelParams& params,
                       std::string_view context, std::vector<std::string>& warnings) {
    read_field(obj, "model",          params.model,          context, warnings);
    read_field(obj, "n_gpu_layers",   params.n_gpu_layers,   context, warnings);
    read_field(obj, "main_gpu",       params.main_gpu,       context, warnings);
    read_field(obj, "split_mode",     params.split_mode,     context, warnings);
    read_field(obj, "tensor_split",   params.tensor_split,   context, warnings);
    read_field(obj, "n_ctx",          params.n_ctx,          context, warnings);
    read_field(obj, "n_batch",        params.n_batch,        context, warnings);
    read_field(obj, "n_threads",      params.n_threads,      context, warnings);
    read_field(obj, "flash_attn",     params.flash_attn,     context, warnings);
    read_field(obj, "temperature",    params.temperature,    context, warnings);
    read_field(obj, "top_p",          params.top_p,          context, warnings);
    read_field(obj, "top_k",          params.top_k,          context, warnings);
    read_field(obj, "min_p",          params.min_p,          context, warnings);
    read_field(obj, "repeat_penalty", params.repeat_penalty, context, warnings);
    read_field(obj, "repeat_last_n",  params.repeat_last_n,  context, warnings);
    read_field(obj, "max_tokens",     params.max_tokens,     context, warnings);
    read_field(obj, "seed",           params.seed,           context, warnings);
}


/// Identity fields for one seat.
///
/// Always written in full. This used to omit them for a seat that matched one
/// of the nine Crucible shipped, reconstructing it from the built-in table on
/// the way back in. There is no built-in table any more -- every expert is one
/// the user made -- so there is nothing to reconstruct from and the file has to
/// carry the whole seat.
void write_expert_identity(json& entry, const Expert& expert) {
    entry["name"]     = expert.name;
    entry["tag"]      = expert.tag;
    entry["blurb"]    = expert.blurb;
    entry["examples"] = expert.examples;
    entry["keywords"] = expert.keywords;
}

/// Rebuild one seat from its entry. Returns false when there is not enough to
/// work with, which is reported as a warning and skips that seat only.
bool read_expert_identity(const json& entry, const ExpertId& id, Expert& expert,
                          std::vector<std::string>& warnings) {
    expert    = Expert{};
    expert.id = id;

    read_field(entry, "name",     expert.name,     id, warnings);
    read_field(entry, "tag",      expert.tag,      id, warnings);
    read_field(entry, "blurb",    expert.blurb,    id, warnings);
    read_field(entry, "examples", expert.examples, id, warnings);
    read_field(entry, "keywords", expert.keywords, id, warnings);

    if (expert.name.empty()) {
        expert.name = id;
    }
    if (expert.blurb.empty()) {
        warnings.emplace_back(id + ": no \"blurb\", so the delegator has nothing to "
                                   "route on -- this expert was skipped");
        return false;
    }
    return true;
}

/// Round a float before serializing it.
///
/// A float widened to double prints as 0.05000000074505806, which is correct
/// and unreadable. The config is meant to be edited by hand, so trim the noise.
double tidy(float value) {
    return std::round(static_cast<double>(value) * 10000.0) / 10000.0;
}

/// `include_model` is false for the `defaults` block, which describes how to
/// load models rather than naming one.
json model_params_to_json(const ModelParams& params, bool include_model = true) {
    json out = json{
        {"n_gpu_layers",   params.n_gpu_layers},
        {"main_gpu",       params.main_gpu},
        {"split_mode",     params.split_mode},
        {"n_ctx",          params.n_ctx},
        {"n_batch",        params.n_batch},
        {"n_threads",      params.n_threads},
        {"flash_attn",     params.flash_attn},
        {"temperature",    tidy(params.temperature)},
        {"top_p",          tidy(params.top_p)},
        {"top_k",          params.top_k},
        {"min_p",          tidy(params.min_p)},
        {"repeat_penalty", tidy(params.repeat_penalty)},
        {"repeat_last_n",  params.repeat_last_n},
        {"max_tokens",     params.max_tokens},
    };
    if (include_model) {
        out["model"] = params.model;
    }
    return out;
}

}  // namespace

bool save_config(const Config& config, const std::filesystem::path& file) {
    // An array, not an object: the order seats are drawn in is the order they
    // are listed here, and a JSON object has no order to preserve.
    json experts = json::array();
    for (const Expert& seat : config.roster.experts()) {
        json entry = json::object();
        entry["id"] = seat.id;
        write_expert_identity(entry, seat);

        const ModelParams& params = config.expert(seat.id);
        entry["model"] = params.model;

        // Only write fields that differ from `defaults`. Round-tripping every
        // field would turn a ten-line config into a hundred-line one the first
        // time the user saved from the settings screen.
        const ModelParams& base = config.defaults;
        if (params.n_ctx          != base.n_ctx)          { entry["n_ctx"]          = params.n_ctx; }
        if (params.n_batch        != base.n_batch)        { entry["n_batch"]        = params.n_batch; }
        if (params.n_threads      != base.n_threads)      { entry["n_threads"]      = params.n_threads; }
        if (params.n_gpu_layers   != base.n_gpu_layers)   { entry["n_gpu_layers"]   = params.n_gpu_layers; }
        if (params.main_gpu       != base.main_gpu)       { entry["main_gpu"]       = params.main_gpu; }
        if (params.split_mode     != base.split_mode)     { entry["split_mode"]     = params.split_mode; }
        if (params.flash_attn     != base.flash_attn)     { entry["flash_attn"]     = params.flash_attn; }
        if (params.temperature    != base.temperature)    { entry["temperature"]    = params.temperature; }
        if (params.top_p          != base.top_p)          { entry["top_p"]          = params.top_p; }
        if (params.top_k          != base.top_k)          { entry["top_k"]          = params.top_k; }
        if (params.min_p          != base.min_p)          { entry["min_p"]          = params.min_p; }
        if (params.repeat_penalty != base.repeat_penalty) { entry["repeat_penalty"] = tidy(params.repeat_penalty); }
        if (params.repeat_last_n  != base.repeat_last_n)  { entry["repeat_last_n"]  = params.repeat_last_n; }
        if (params.max_tokens     != base.max_tokens)     { entry["max_tokens"]     = params.max_tokens; }

        experts.push_back(std::move(entry));
    }

    const json doc{
        {"$schema_note",
         "Crucible config. Models live in \"models_dir\"; each expert names a file "
         "inside it. An absolute or ~-path is also accepted. Anything an expert "
         "leaves out is inherited from \"defaults\". Experts are listed in the order "
         "they are drawn. Crucible ships none: add one with /newexpert, or write an "
         "entry with an \"id\", a \"name\" and a \"blurb\". Editable in the app "
         "with /settings."},
        {"models_dir",    config.models_dir},
        {"system_prompt", config.system_prompt},
        {"reasoning_effort", config.reasoning_effort},
        {"router",        model_params_to_json(config.router)},
        {"defaults",      model_params_to_json(config.defaults, /*include_model=*/false)},
        {"experts",       experts},
        {"gpu", json{
            {"mode",      config.gpu.mode},
            {"priority",  config.gpu.priority},
            {"main_gpu",  config.gpu.main_gpu},
            {"gpu_only",  config.gpu.gpu_only},
            {"vram_only", config.gpu.vram_only},
        }},
        {"routing", json{
            {"min_confidence",       tidy(config.routing.min_confidence)},
            {"default_expert",       config.routing.default_expert},
            {"keep_delegator_loaded", config.routing.keep_delegator_loaded},
        }},
        {"tools", json{
            {"web_search",      config.tools.web_search},
            {"search_provider", config.tools.search_provider},
            {"search_endpoint", config.tools.search_endpoint},
            {"search_api_key",  config.tools.search_api_key},
            {"search_results",  config.tools.search_results},
            {"search_timeout",  config.tools.search_timeout},
            {"search_rounds",   config.tools.search_rounds},
            {"auto_edits",       config.tools.auto_edits},
            {"overflow",         config.tools.overflow},
            {"workshop_timeout", config.tools.workshop_timeout},
        }},
        {"ui", json{
            {"animation_ms",    config.ui.animation_ms},
            {"show_experts", config.ui.show_experts},
            {"show_reasoning", config.ui.show_reasoning},
            {"unicode",         config.ui.unicode},
        }},
    };

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    // Write to a sibling temp file and rename, so an interrupted save cannot
    // leave the user with a truncated config and no way back into the app.
    const std::filesystem::path temp = file.string() + ".tmp";
    {
        std::ofstream out(temp);
        if (!out) {
            return false;
        }
        out << doc.dump(2) << '\n';
        if (!out.good()) {
            return false;
        }
    }

    std::filesystem::rename(temp, file, ec);
    if (ec) {
        // Rename can fail across filesystems; fall back to a direct write.
        std::ofstream out(file);
        if (!out) {
            std::filesystem::remove(temp, ec);
            return false;
        }
        out << doc.dump(2) << '\n';
        std::filesystem::remove(temp, ec);
        return out.good();
    }
    return true;
}

bool save_config(const Config& config) {
    return save_config(config, paths::config_file());
}

void write_default_config(const std::filesystem::path& file) {
    Config defaults;

    // An empty list, written explicitly rather than left out.
    //
    // Crucible ships no experts, and the key being present and empty is what
    // says so: the reader takes an "experts" key at its word, so this is a
    // blank roster the user fills rather than an omission it might one day be
    // tempted to fill for them.
    json experts = json::array();
    for (const Expert& seat : defaults.roster.experts()) {
        json entry = json::object();
        entry["id"] = seat.id;
        entry["model"] = "";
        experts.push_back(std::move(entry));
    }

    ModelParams router_defaults;
    router_defaults.n_ctx       = 4096;
    router_defaults.temperature = 0.0F;   // greedy: routing wants determinism
    router_defaults.max_tokens  = 16;

    const json doc{
        {"$schema_note",
         "Crucible config. Drop your GGUF files in \"models_dir\" and name one per "
         "expert below. An absolute or ~-path also works. Anything an expert "
         "leaves out is inherited from \"defaults\". Experts are listed in the order "
         "they are drawn. Crucible ships none: add one with /newexpert, or write an "
         "entry with an \"id\", a \"name\" and a \"blurb\". Editable in the app "
         "with /settings."},
        {"models_dir", paths::models_dir().string()},
        {"system_prompt", defaults.system_prompt},
        {"reasoning_effort", defaults.reasoning_effort},
        {"router", model_params_to_json(router_defaults)},
        {"defaults", model_params_to_json(defaults.defaults, /*include_model=*/false)},
        {"experts", experts},
        {"gpu", json{
            {"mode",      defaults.gpu.mode},
            {"priority",  defaults.gpu.priority},
            {"main_gpu",  defaults.gpu.main_gpu},
            {"gpu_only",  defaults.gpu.gpu_only},
            {"vram_only", defaults.gpu.vram_only},
        }},
        {"routing", json{
            {"min_confidence",       defaults.routing.min_confidence},
            {"default_expert",       defaults.routing.default_expert},
            {"keep_delegator_loaded", defaults.routing.keep_delegator_loaded},
        }},
        {"tools", json{
            {"web_search",      defaults.tools.web_search},
            {"search_provider", defaults.tools.search_provider},
            {"search_endpoint", defaults.tools.search_endpoint},
            {"search_api_key",  defaults.tools.search_api_key},
            {"search_results",  defaults.tools.search_results},
            {"search_timeout",  defaults.tools.search_timeout},
            {"search_rounds",   defaults.tools.search_rounds},
            {"auto_edits",       defaults.tools.auto_edits},
            {"overflow",         defaults.tools.overflow},
            {"workshop_timeout", defaults.tools.workshop_timeout},
        }},
        {"ui", json{
            {"animation_ms",    defaults.ui.animation_ms},
            {"show_experts", defaults.ui.show_experts},
            {"show_reasoning", defaults.ui.show_reasoning},
            {"unicode",         defaults.ui.unicode},
        }},
    };

    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);

    std::ofstream out(file);
    if (out) {
        out << doc.dump(2) << '\n';
    }
}

Config load_config(const std::filesystem::path& file, std::vector<std::string>& warnings) {
    Config config;

    if (!std::filesystem::exists(file)) {
        write_default_config(file);
        // A freshly written default has no models in it; the UI turns that into
        // a "point me at some GGUFs" screen rather than an error.
        config.router.n_ctx       = 4096;
        config.router.temperature = 0.0F;   // greedy: same prompt, same expert
        config.router.max_tokens  = 16;
        return config;
    }

    json doc;
    try {
        std::ifstream in(file);
        in >> doc;
    } catch (const json::exception& e) {
        warnings.emplace_back("could not parse " + file.string() + ": " + e.what()
                              + " -- falling back to built-in defaults");
        return config;
    }

    if (!doc.is_object()) {
        warnings.emplace_back(file.string() + " is not a JSON object -- using defaults");
        return config;
    }

    read_field(doc, "system_prompt", config.system_prompt, "config", warnings);
    read_field(doc, "reasoning_effort", config.reasoning_effort, "config", warnings);
    read_field(doc, "models_dir",    config.models_dir,    "config", warnings);

    if (const auto it = doc.find("defaults"); it != doc.end() && it->is_object()) {
        read_model_params(*it, config.defaults, "defaults", warnings);
    }

    config.router = config.defaults;
    config.router.model.clear();
    config.router.path.clear();
    config.router.n_ctx       = 4096;
    config.router.temperature = 0.0F;   // greedy: same prompt, same expert
    config.router.max_tokens  = 16;
    if (const auto it = doc.find("router"); it != doc.end() && it->is_object()) {
        read_model_params(*it, config.router, "router", warnings);
    }

    // The roster comes from the file when the file has one, and from the
    // built-in defaults when it does not. An "experts" key that is present but
    // empty is taken at its word: someone who deleted every seat wanted every
    // seat deleted, and silently restoring nine of them would be worse than an
    // empty expert list the UI can explain.
    if (const auto experts = doc.find("experts"); experts != doc.end()) {
        config.roster = Roster::bare();

        // An array is the format Crucible writes, because it preserves the
        // order the seats are drawn in. An object is accepted too: it is what a
        // hand-written file most naturally looks like, and it costs four lines
        // to read.
        std::vector<std::pair<ExpertId, const json*>> entries;
        if (experts->is_array()) {
            for (const json& entry : *experts) {
                if (!entry.is_object()) {
                    continue;
                }
                ExpertId id;
                read_field(entry, "id", id, "experts", warnings);
                if (id.empty()) {
                    warnings.emplace_back("experts: an entry with no \"id\" was skipped");
                    continue;
                }
                entries.emplace_back(std::move(id), &entry);
            }
        } else if (experts->is_object()) {
            for (const auto& [id, entry] : experts->items()) {
                if (entry.is_object()) {
                    entries.emplace_back(id, &entry);
                }
            }
        } else {
            warnings.emplace_back("experts: expected a list of experts -- using the "
                                  "built-in ones");
            config.roster = Roster::bare();
        }

        for (const auto& [id, entry] : entries) {
            ModelParams params = config.defaults;
            params.model.clear();
            read_model_params(*entry, params, id, warnings);
            params.inherit_from(config.defaults);
            config.experts[id] = std::move(params);

            Expert expert;
            if (!read_expert_identity(*entry, id, expert, warnings)) {
                continue;
            }
            std::string error;
            if (!config.roster.add(std::move(expert), error)) {
                warnings.emplace_back("experts: " + error);
            }
        }
    }

    if (const auto gpu = doc.find("gpu"); gpu != doc.end() && gpu->is_object()) {
        read_field(*gpu, "mode",      config.gpu.mode,      "gpu", warnings);
        read_field(*gpu, "main_gpu",  config.gpu.main_gpu,  "gpu", warnings);
        read_field(*gpu, "gpu_only",  config.gpu.gpu_only,  "gpu", warnings);
        read_field(*gpu, "vram_only", config.gpu.vram_only, "gpu", warnings);
        if (const auto priority = gpu->find("priority");
            priority != gpu->end() && priority->is_array()) {
            config.gpu.priority.clear();
            for (const json& index : *priority) {
                if (index.is_number_integer()) {
                    config.gpu.priority.push_back(index.get<int>());
                }
            }
        }
    }

    if (const auto routing = doc.find("routing"); routing != doc.end() && routing->is_object()) {
        read_field(*routing, "min_confidence",       config.routing.min_confidence,
                   "routing", warnings);
        read_field(*routing, "keep_delegator_loaded", config.routing.keep_delegator_loaded,
                   "routing", warnings);
        read_field(*routing, "default_expert",       config.routing.default_expert,
                   "routing", warnings);
    }

    if (const auto tools = doc.find("tools"); tools != doc.end() && tools->is_object()) {
        read_field(*tools, "web_search",      config.tools.web_search,      "tools", warnings);
        read_field(*tools, "search_provider", config.tools.search_provider, "tools", warnings);
        read_field(*tools, "search_endpoint", config.tools.search_endpoint, "tools", warnings);
        read_field(*tools, "search_api_key",  config.tools.search_api_key,  "tools", warnings);
        read_field(*tools, "search_results",  config.tools.search_results,  "tools", warnings);
        read_field(*tools, "search_timeout",  config.tools.search_timeout,  "tools", warnings);
        read_field(*tools, "search_rounds",   config.tools.search_rounds,   "tools", warnings);
        read_field(*tools, "auto_edits",       config.tools.auto_edits,       "tools", warnings);
        read_field(*tools, "overflow",         config.tools.overflow,         "tools", warnings);
        read_field(*tools, "workshop_timeout", config.tools.workshop_timeout, "tools", warnings);
    }

    if (const auto ui = doc.find("ui"); ui != doc.end() && ui->is_object()) {
        read_field(*ui, "animation_ms",    config.ui.animation_ms,    "ui", warnings);
        read_field(*ui, "show_experts", config.ui.show_experts, "ui", warnings);
        read_field(*ui, "show_reasoning", config.ui.show_reasoning, "ui", warnings);
        read_field(*ui, "unicode",         config.ui.unicode,         "ui", warnings);
    }

    // Resolve every reference once, here, so nothing downstream has to think
    // about the models directory, `~`, or relative paths.
    config.resolve_models();

    // Warn about files that are not there, but keep them configured: the user
    // may be mid-download, or about to point the models directory elsewhere.
    const auto check = [&](const ModelParams& params, std::string_view label) {
        if (!params.model.empty() && !std::filesystem::exists(params.path)) {
            warnings.emplace_back(std::string(label) + ": model not found at " + params.path);
        }
    };
    check(config.router, "router");
    for (const Expert& seat : config.roster.experts()) {
        check(config.expert(seat.id), seat.id);
    }

    return config;
}

Config load_config(std::vector<std::string>& warnings) {
    return load_config(paths::config_file(), warnings);
}

}  // namespace crucible
