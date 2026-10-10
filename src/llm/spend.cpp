// SPDX-License-Identifier: MIT
#include "crucible/llm/spend.hpp"

#include <algorithm>
#include <charconv>
#include <ctime>
#include <fstream>
#include <iterator>
#include <system_error>

#include <nlohmann/json.hpp>

#include "crucible/config/config.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::spend {
namespace {

using json = nlohmann::json;

std::string this_month() {
    const std::tm now = util::local_time(std::time(nullptr));
    char text[16] = {};
    std::strftime(text, sizeof(text), "%Y-%m", &now);
    return text;
}

bool whole_number(const std::string& text, std::int64_t& out) {
    const char* end = text.data() + text.size();
    const auto parsed = std::from_chars(text.data(), end, out);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

json tokens_json(const Tokens& tokens) {
    return json{{"input", tokens.input}, {"cache_read", tokens.cache_read},
                {"cache_write", tokens.cache_write}, {"output", tokens.output},
                {"requests", tokens.requests}};
}

Tokens tokens_from(const json& node) {
    Tokens tokens;
    tokens.input       = node.value("input", std::uint64_t{0});
    tokens.cache_read  = node.value("cache_read", std::uint64_t{0});
    tokens.cache_write = node.value("cache_write", std::uint64_t{0});
    tokens.output      = node.value("output", std::uint64_t{0});
    tokens.requests    = node.value("requests", std::uint64_t{0});
    return tokens;
}

}  // namespace

void Tokens::add(const Tokens& other) {
    input       += other.input;
    cache_read  += other.cache_read;
    cache_write += other.cache_write;
    output      += other.output;
    requests    += other.requests;
}

std::vector<Limit> limits_from(const std::vector<util::http::Header>& headers) {
    const auto header = [&headers](const std::string& name) {
        for (const util::http::Header& one : headers) {
            if (one.name == name) {
                return one.value;
            }
        }
        return std::string();
    };
    std::vector<Limit> limits;
    const auto take = [&limits](std::string what, const std::string& limit, const std::string& remaining,
                                std::string resets) {
        Limit one;
        if (whole_number(limit, one.limit) && whole_number(remaining, one.remaining) && one.limit > 0) {
            one.what   = std::move(what);
            one.resets = std::move(resets);
            limits.push_back(std::move(one));
        }
    };
    // Anthropic: anthropic-ratelimit-<what>-limit, -remaining, -reset.
    for (const char* what : {"requests", "input-tokens", "output-tokens", "tokens"}) {
        const std::string base = std::string("anthropic-ratelimit-") + what;
        std::string name = what;
        std::replace(name.begin(), name.end(), '-', ' ');
        take(name, header(base + "-limit"), header(base + "-remaining"), header(base + "-reset"));
    }
    // OpenAI's, and the shape most others copied: x-ratelimit-limit-<what>.
    for (const char* what : {"requests", "tokens"}) {
        take(what, header(std::string("x-ratelimit-limit-") + what),
             header(std::string("x-ratelimit-remaining-") + what),
             header(std::string("x-ratelimit-reset-") + what));
    }
    return limits;
}

Ledger::Ledger(std::filesystem::path file) : file_(std::move(file)), month_(this_month()) {
    load();
}

void Ledger::record(const Provider& provider, const std::string& model, const Tokens& used,
                    int prompt, int context, const std::vector<util::http::Header>& headers) {
    const std::lock_guard<std::mutex> lock(mutex_);
    // A session that runs over the turn of a month starts the new one at nothing.
    if (const std::string now = this_month(); now != month_) {
        month_ = now;
        for (Model& each : models_) {
            each.month = {};
        }
    }
    auto found = std::find_if(models_.begin(), models_.end(), [&](const Model& each) {
        return each.provider_id == provider.id && each.model == model;
    });
    if (found == models_.end()) {
        Model fresh;
        fresh.provider_id = provider.id;
        fresh.model       = model;
        models_.push_back(std::move(fresh));
        found = std::prev(models_.end());
    }
    Model& entry = *found;
    // Named as the provider is now, in case it was renamed since last month.
    entry.provider = provider.label();
    entry.kind     = provider.kind;
    entry.endpoint = provider.endpoint();
    entry.session.add(used);
    entry.month.add(used);
    if (prompt > 0) {
        entry.prompt = prompt;
    }
    if (context > 0) {
        entry.context = context;
    }
    entry.last = static_cast<std::int64_t>(std::time(nullptr));
    if (std::vector<Limit> limits = limits_from(headers); !limits.empty()) {
        entry.limits = std::move(limits);
    }
    save();
}

std::vector<Model> Ledger::models() const {
    std::vector<Model> out;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        out = models_;
    }
    std::stable_sort(out.begin(), out.end(), [](const Model& a, const Model& b) { return a.last > b.last; });
    return out;
}

void Ledger::load() {
    if (file_.empty()) {
        return;
    }
    std::ifstream in(file_);
    const json document = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (!document.is_object()) {
        return;
    }
    const json& month = document.value("months", json::object()).value(month_, json::array());
    for (const json& entry : month) {
        if (!entry.is_object()) {
            continue;
        }
        Model model;
        model.provider_id = entry.value("provider_id", std::string());
        model.provider    = entry.value("provider", std::string());
        model.kind        = entry.value("kind", std::string());
        model.endpoint    = entry.value("endpoint", std::string());
        model.model       = entry.value("model", std::string());
        model.last        = entry.value("last", std::int64_t{0});
        model.month       = tokens_from(entry);
        if (!model.provider_id.empty() && !model.model.empty()) {
            models_.push_back(std::move(model));
        }
    }
}

void Ledger::save() const {
    if (file_.empty()) {
        return;
    }
    // The other months as they were: this only ever writes the current one.
    json document;
    {
        std::ifstream in(file_);
        document = json::parse(in, nullptr, /*allow_exceptions=*/false);
    }
    if (!document.is_object()) {
        document = json::object();
    }
    json month = json::array();
    for (const Model& model : models_) {
        if (model.month.requests == 0) {
            continue;
        }
        json entry = tokens_json(model.month);
        entry["provider_id"] = model.provider_id;
        entry["provider"]    = model.provider;
        entry["kind"]        = model.kind;
        entry["endpoint"]    = model.endpoint;
        entry["model"]       = model.model;
        entry["last"]        = model.last;
        month.push_back(std::move(entry));
    }
    document["months"][month_] = std::move(month);

    std::error_code ec;
    std::filesystem::create_directories(file_.parent_path(), ec);
    std::filesystem::path fresh = file_;
    fresh += ".new";
    {
        std::ofstream out(fresh, std::ios::binary | std::ios::trunc);
        out << document.dump(2, ' ', false, json::error_handler_t::replace) << '\n';
        if (!out) {
            std::filesystem::remove(fresh, ec);
            return;
        }
    }
    std::filesystem::rename(fresh, file_, ec);
}

}  // namespace crucible::spend
