// SPDX-License-Identifier: MIT
//
// The Config type's own behavior: inheritance from defaults, and resolving
// model references against the models directory.
//
// Anything to do with the file on disk lives in config_io.cpp.
#include "crucible/config/config.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "crucible/config/paths.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/util/format.hpp"

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
    // side menu draws them in a different order is a small but constant
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
        // A provider's model is a name, not a file. Resolving it against the
        // models directory would produce a path to nothing, and every check
        // for "is the file there" would then report a cloud seat as missing.
        params.path = params.remote() ? std::string()
                                      : resolve_model_ref(dir, params.model).string();
    }
}

const Provider* Config::provider(std::string_view id) const {
    if (id.empty()) {
        return nullptr;
    }
    for (const Provider& one : providers) {
        if (one.id == id) {
            return &one;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Provider
// ---------------------------------------------------------------------------

namespace {

/// The host out of a URL, lower case: "https://API.Deepseek.com/v1" gives
/// "api.deepseek.com".
std::string host_of(std::string_view url) {
    const std::size_t scheme = url.find("://");
    std::string_view  rest   = scheme == std::string_view::npos ? url : url.substr(scheme + 3);
    // An IPv6 address is written in brackets precisely because it is full of
    // the colons that would otherwise end the host.
    if (!rest.empty() && rest.front() == '[') {
        const std::size_t close = rest.find(']');
        rest = rest.substr(0, close == std::string_view::npos ? rest.size() : close + 1);
    } else {
        rest = rest.substr(0, rest.find_first_of("/:?#"));
    }
    return format::to_lower(rest);
}

}  // namespace

std::string Provider::endpoint() const {
    std::string url = base_url;
    if (url.empty()) {
        url = kind == "anthropic" ? "https://api.anthropic.com" : "https://api.openai.com/v1";
    }
    while (!url.empty() && url.back() == '/') {
        url.pop_back();
    }
    return url;
}

const std::vector<KnownService>& known_services() {
    static const std::vector<KnownService> services = {
        {"Anthropic", "anthropic", "", "anthropic.com", "ANTHROPIC_API_KEY",
         "Claude, through its own API."},
        {"OpenAI", "openai", "https://api.openai.com/v1", "openai.com", "OPENAI_API_KEY", ""},
        {"Google Gemini", "openai",
         "https://generativelanguage.googleapis.com/v1beta/openai",
         "generativelanguage.googleapis", "GEMINI_API_KEY", ""},
        {"DeepSeek", "openai", "https://api.deepseek.com/v1", "deepseek.com",
         "DEEPSEEK_API_KEY", ""},
        {"Moonshot (Kimi)", "openai", "https://api.moonshot.ai/v1", "moonshot.",
         "MOONSHOT_API_KEY", ""},
        {"Cloudflare Workers AI", "openai",
         "https://api.cloudflare.com/client/v4/accounts/YOUR_ACCOUNT_ID/ai/v1",
         "cloudflare.com", "CLOUDFLARE_API_TOKEN",
         "Put your account id in the address where it says YOUR_ACCOUNT_ID."},
        {"OpenRouter", "openai", "https://openrouter.ai/api/v1", "openrouter.ai",
         "OPENROUTER_API_KEY", "One key for most of the hosted models there are."},
        {"Groq", "openai", "https://api.groq.com/openai/v1", "groq.com", "GROQ_API_KEY", ""},
        {"Mistral", "openai", "https://api.mistral.ai/v1", "mistral.ai", "MISTRAL_API_KEY", ""},
        {"xAI", "openai", "https://api.x.ai/v1", "x.ai", "XAI_API_KEY", ""},
        {"Together", "openai", "https://api.together.xyz/v1", "together.",
         "TOGETHER_API_KEY", ""},
        {"Ollama", "openai", "http://localhost:11434/v1", "localhost:11434", "",
         "A server on this machine or your network. It wants no key."},
        {"LM Studio", "openai", "http://localhost:1234/v1", "localhost:1234", "",
         "A server on this machine or your network. It wants no key."},
        {"llama.cpp server", "openai", "http://localhost:8080/v1", "localhost:8080", "",
         "A server on this machine or your network. It wants no key."},
    };
    return services;
}

std::string Provider::conventional_key_variable() const {
    if (kind == "anthropic") {
        return "ANTHROPIC_API_KEY";
    }
    // By where it points rather than by what it was named: the name is the
    // user's to choose, and the address is the fact.
    const std::string host = host_of(endpoint());
    for (const KnownService& known : known_services()) {
        if (known.kind == "openai" && !known.key_variable.empty()
            && host.find(known.host_part) != std::string::npos) {
            return std::string(known.key_variable);
        }
    }
    return {};
}

bool Provider::on_your_network() const {
    const std::string host = host_of(endpoint());
    const auto starts = [&host](std::string_view prefix) { return host.rfind(prefix, 0) == 0; };
    if (host == "localhost" || starts("127.") || starts("10.") || starts("192.168.")
        || starts("169.254.") || host == "[::1]" || starts("[fe80:") || starts("[fc")
        || starts("[fd")) {
        return true;
    }
    // 172.16.0.0 through 172.31.255.255, which is the one private range that
    // is not a whole first octet.
    if (starts("172.")) {
        const int second = std::atoi(host.c_str() + 4);
        if (second >= 16 && second <= 31) {
            return true;
        }
    }
    const auto ends = [&host](std::string_view suffix) {
        return host.size() >= suffix.size()
            && host.compare(host.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    // A bare name with no dot in it is a machine on the local network; nothing
    // on the internet is called that.
    return ends(".local") || ends(".lan") || ends(".home.arpa")
        || (!host.empty() && host.find('.') == std::string::npos);
}

std::string Provider::resolved_key() const {
    const auto from_environment = [](const std::string& variable) -> std::string {
        if (variable.empty()) {
            return {};
        }
        const char* value = std::getenv(variable.c_str());
        return value == nullptr ? std::string() : std::string(value);
    };
    if (api_key.rfind("env:", 0) == 0) {
        return from_environment(format::trim(api_key.substr(4)));
    }
    if (!api_key.empty()) {
        return api_key;
    }
    return from_environment(conventional_key_variable());
}

std::string provider_id_from_name(std::string_view name) {
    std::string id;
    for (const char raw : name) {
        const auto ch = static_cast<unsigned char>(raw);
        if (std::isalnum(ch) != 0) {
            id += static_cast<char>(std::tolower(ch));
        } else if (!id.empty() && id.back() != '-') {
            id += '-';
        }
    }
    while (!id.empty() && id.back() == '-') {
        id.pop_back();
    }
    return id;
}


}  // namespace crucible
