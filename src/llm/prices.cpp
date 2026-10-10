// SPDX-License-Identifier: MIT
#include "crucible/llm/prices.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <fstream>
#include <mutex>
#include <system_error>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/util/http.hpp"

namespace crucible::prices {
namespace {

using json = nlohmann::json;

constexpr const char* kSource =
    "https://raw.githubusercontent.com/BerriAI/litellm/main/model_prices_and_context_window.json";

/// A day: prices change with announcements, not by the hour.
constexpr std::int64_t kFresh = 24 * 60 * 60;

std::filesystem::path list_file() { return paths::data_dir() / "prices.json"; }

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

/// A name as names are compared when they are not spelled alike: "3.5" and
/// "3-5" are the same model, and so are "_" and "-".
std::string normal(std::string_view name) {
    std::string out = lower(name);
    std::replace(out.begin(), out.end(), '.', '-');
    std::replace(out.begin(), out.end(), '_', '-');
    return out;
}

/// `model` without a date or "-latest" on the end: claude-opus-4-5-20251101
/// is priced as claude-opus-4-5.
std::string undated(const std::string& model) {
    const auto digits = [](std::string_view text) {
        return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c); });
    };
    if (model.size() > 7 && model.compare(model.size() - 7, 7, "-latest") == 0) {
        return model.substr(0, model.size() - 7);
    }
    if (model.size() > 9 && model[model.size() - 9] == '-' && digits(std::string_view(model).substr(model.size() - 8))) {
        return model.substr(0, model.size() - 9);
    }
    if (model.size() > 11 && model[model.size() - 11] == '-' && model[model.size() - 6] == '-'
        && model[model.size() - 3] == '-') {
        return model.substr(0, model.size() - 11);   // -2025-11-01
    }
    return model;
}

json price_json(const std::string& provider, const Price& price) {
    return json{{"provider", provider}, {"input", price.input}, {"output", price.output},
                {"cache_read", price.cache_read}, {"cache_write", price.cache_write}};
}

/// The list as kept on disk, read again whenever the file changes.
struct Kept {
    std::mutex                      mutex;
    std::filesystem::file_time_type stamp{};
    detail::List                    list;
};

Kept& kept() {
    static Kept instance;
    return instance;
}

}  // namespace

namespace detail {

List reduce(std::string_view litellm) {
    List list;
    const json document = json::parse(litellm, nullptr, /*allow_exceptions=*/false);
    if (!document.is_object()) {
        return list;
    }
    for (const auto& [name, entry] : document.items()) {
        if (!entry.is_object()) {
            continue;
        }
        const std::string mode = entry.value("mode", std::string("chat"));
        const auto input  = entry.find("input_cost_per_token");
        const auto output = entry.find("output_cost_per_token");
        if ((mode != "chat" && mode != "responses") || input == entry.end() || output == entry.end()
            || !input->is_number() || !output->is_number()) {
            continue;
        }
        Price price;
        price.input  = input->get<double>();
        price.output = output->get<double>();
        if (const auto read = entry.find("cache_read_input_token_cost"); read != entry.end() && read->is_number()) {
            price.cache_read = read->get<double>();
        }
        if (const auto write = entry.find("cache_creation_input_token_cost"); write != entry.end() && write->is_number()) {
            price.cache_write = write->get<double>();
        }
        list[name] = {entry.value("litellm_provider", std::string()), price};
    }
    return list;
}

std::string provider_for(std::string_view endpoint) {
    std::string host = lower(endpoint);
    if (const std::size_t scheme = host.find("://"); scheme != std::string::npos) {
        host.erase(0, scheme + 3);
    }
    host = host.substr(0, host.find_first_of("/:?"));
    const auto is = [&host](std::string_view domain) {
        return host == domain
            || (host.size() > domain.size() && host.compare(host.size() - domain.size(), domain.size(), domain) == 0
                && host[host.size() - domain.size() - 1] == '.');
    };
    static const std::pair<const char*, const char*> kHosts[] = {
        {"api.anthropic.com", "anthropic"},
        {"api.openai.com", "openai"},
        {"generativelanguage.googleapis.com", "gemini"},
        {"api.deepseek.com", "deepseek"},
        {"openrouter.ai", "openrouter"},
        {"api.mistral.ai", "mistral"},
        {"api.x.ai", "xai"},
        {"api.groq.com", "groq"},
        {"api.together.xyz", "together_ai"},
        {"api.fireworks.ai", "fireworks_ai"},
        {"api.moonshot.ai", "moonshot"},
        {"api.moonshot.cn", "moonshot"},
        {"api.cerebras.ai", "cerebras"},
        {"api.perplexity.ai", "perplexity"},
    };
    for (const auto& [domain, provider] : kHosts) {
        if (is(domain)) {
            return provider;
        }
    }
    return {};
}

std::optional<Price> lookup(const List& list, const std::string& endpoint, const std::string& model) {
    const std::string provider = provider_for(endpoint);
    if (provider.empty() || model.empty()) {
        return std::nullopt;
    }
    const std::string bare = undated(model);
    // As LiteLLM spells it, with the provider in front or not.
    for (const std::string& name : {model, provider + "/" + model, bare, provider + "/" + bare}) {
        if (const auto found = list.find(name); found != list.end() && found->second.first == provider) {
            return found->second.second;
        }
    }
    // And spelled differently: claude-3.5-sonnet for claude-3-5-sonnet.
    const std::string wanted = normal(model);
    const std::string wanted_bare = normal(bare);
    for (const auto& [name, entry] : list) {
        if (entry.first != provider) {
            continue;
        }
        const std::size_t slash = name.find('/');
        const std::string listed = normal(slash == std::string::npos || name.compare(0, slash, provider) != 0
                                              ? name : name.substr(slash + 1));
        if (listed == wanted || listed == wanted_bare) {
            return entry.second;
        }
    }
    return std::nullopt;
}

}  // namespace detail

std::optional<Price> find(const std::string& endpoint, const std::string& model) {
    Kept& store = kept();
    const std::lock_guard<std::mutex> lock(store.mutex);
    std::error_code ec;
    const std::filesystem::file_time_type stamp = std::filesystem::last_write_time(list_file(), ec);
    if (ec) {
        return std::nullopt;
    }
    if (stamp != store.stamp) {
        store.stamp = stamp;
        store.list.clear();
        std::ifstream in(list_file());
        const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (document.is_object()) {
            for (const auto& [name, entry] : document.value("models", json::object()).items()) {
                Price price;
                price.input       = entry.value("input", 0.0);
                price.output      = entry.value("output", 0.0);
                price.cache_read  = entry.value("cache_read", 0.0);
                price.cache_write = entry.value("cache_write", 0.0);
                store.list[name]  = {entry.value("provider", std::string()), price};
            }
        }
    }
    return detail::lookup(store.list, endpoint, model);
}

double cost(const spend::Tokens& tokens, const Price& price) {
    // A list that does not price the cache gets the plain input price for it:
    // too high, where too low would be the worse surprise.
    const double read  = price.cache_read > 0 ? price.cache_read : price.input;
    const double write = price.cache_write > 0 ? price.cache_write : price.input;
    return static_cast<double>(tokens.input) * price.input + static_cast<double>(tokens.output) * price.output
         + static_cast<double>(tokens.cache_read) * read + static_cast<double>(tokens.cache_write) * write;
}

void refresh() {
    const std::int64_t now = static_cast<std::int64_t>(std::time(nullptr));
    {
        std::ifstream in(list_file());
        const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
        if (document.is_object() && now - document.value("fetched", std::int64_t{0}) < kFresh) {
            return;
        }
    }
    util::http::Request request;
    request.url             = kSource;
    request.timeout_seconds = 60;
    const util::http::Response response = util::http::send(request);
    if (!response.ok()) {
        return;
    }
    const detail::List list = detail::reduce(response.body);
    if (list.empty()) {
        return;
    }
    json models = json::object();
    for (const auto& [name, entry] : list) {
        models[name] = price_json(entry.first, entry.second);
    }
    const json document = {{"fetched", now}, {"source", kSource}, {"models", std::move(models)}};

    std::error_code ec;
    std::filesystem::create_directories(list_file().parent_path(), ec);
    std::filesystem::path fresh = list_file();
    fresh += ".new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        out << document.dump() << '\n';
        if (!out) {
            std::filesystem::remove(fresh, ec);
            return;
        }
    }
    std::filesystem::rename(fresh, list_file(), ec);
}

}  // namespace crucible::prices
