// SPDX-License-Identifier: MIT
//
// The experts, and where each one is answered.
//
// A seat is a name, what it is for, and a model. The model is either a file on
// this machine or a name at a provider, and the second half of this file is
// about providers: adding one, asking what it offers, taking it away.
#include "methods.hpp"

#include <algorithm>
#include <cstdlib>

#include "crucible/llm/remote_model.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/util/format.hpp"

namespace crucible::api {
namespace {

// ---------------------------------------------------------------------------
// Experts
// ---------------------------------------------------------------------------

/// Point `params` at a model. The one place that knows what changes with it.
void seat(ModelParams& params, const Config& config, const std::string& model,
          const std::string& provider) {
    const bool was_remote = params.remote();
    params.model    = model;
    params.provider = model.empty() ? std::string() : provider;
    params.path.clear();   // re-resolved when the configuration is applied

    if (params.remote() && !was_remote && params.max_tokens == config.defaults.max_tokens) {
        // The default cap is sized for a model on a graphics card, where a
        // long reply is minutes of waiting. A provider's model counts its
        // thinking against the same number, and two thousand tokens of budget
        // is routinely spent before the answer starts. Left alone if somebody
        // set it on purpose.
        params.max_tokens = -1;
    } else if (!params.remote() && was_remote && params.max_tokens == -1) {
        params.max_tokens = config.defaults.max_tokens;
    }
}

/// The reason `provider` cannot answer for a seat, or empty.
std::string provider_problem(const Config& config, const std::string& provider) {
    if (provider.empty() || config.provider(provider) != nullptr) {
        return {};
    }
    return "there is no provider called \"" + provider + "\"";
}

/// A new seat, from the three things a person can say about one.
///
/// Everything else is worked out: the id and the chip from the name, the
/// keywords from the description, and the worked examples by the delegator
/// itself a moment later. The model may be left out and chosen afterwards --
/// an expert is worth naming before it is worth filling.
Reply expert_add(const json& params, Host& host) {
    Expert expert;
    expert.name  = format::trim(params.value("name", std::string{}));
    expert.blurb = format::trim(params.value("description", std::string{}));
    const auto model    = format::trim(params.value("model", std::string{}));
    const auto provider = params.value("provider", std::string{});

    Config edited = host.config();
    if (const std::string problem = provider_problem(edited, provider); !problem.empty()) {
        return bad(problem);
    }
    std::string error;
    if (!edited.roster.add(expert, error)) {
        // Said as the roster said it: a name already taken is fixed by
        // changing the name, and the sentence names which.
        return bad(error);
    }
    const ExpertId id = make_expert_id(expert.name);

    ModelParams seated = edited.defaults;
    seat(seated, edited, model, provider);
    edited.experts[id] = seated;

    if (const std::string refused = host.apply_config(std::move(edited)); !refused.empty()) {
        return bad(refused);
    }
    // So the delegator has something to route on besides the name. Queued
    // behind whatever the engine is doing, and a no-op with no delegator.
    if (host.engine() != nullptr) {
        host.engine()->write_examples(id);
    }
    host.say(expert.name + " has joined the experts");
    return good(json{{"id", id}});
}

/// Point an existing seat at a different model, here or elsewhere.
Reply expert_set(const json& params, Host& host) {
    const auto id = params.value("id", std::string{});
    Config edited = host.config();
    if (!edited.roster.find(id)) {
        return bad("there is no expert called \"" + id + "\"");
    }
    const auto model    = format::trim(params.value("model", std::string{}));
    const auto provider = params.value("provider", std::string{});
    if (const std::string problem = provider_problem(edited, provider); !problem.empty()) {
        return bad(problem);
    }

    // From what the seat has now, so its own sampling settings survive a
    // change of model. A seat that had nothing starts from the defaults.
    ModelParams seated = edited.experts.count(id) != 0 ? edited.experts[id] : edited.defaults;
    seat(seated, edited, model, provider);
    edited.experts[id] = seated;

    const std::string refused = host.apply_config(std::move(edited));
    return refused.empty() ? good() : bad(refused);
}

/// Take a seat off the roster.
///
/// The seat, not the model: a file stays on disk and a provider stays
/// configured. What goes is the name the delegator could route to.
Reply expert_remove(const json& params, Host& host) {
    const auto id = params.value("id", std::string{});
    Config edited = host.config();
    const std::optional<std::size_t> at = edited.roster.find(id);
    if (!at) {
        return bad("there is no expert called \"" + id + "\"");
    }
    const std::string name = edited.roster.at(*at).name;

    std::string error;
    if (!edited.roster.remove(id, error)) {
        return bad(error);
    }
    edited.experts.erase(id);
    if (edited.routing.default_expert == id) {
        // A default expert that is not on the roster is not a default expert.
        edited.routing.default_expert.clear();
    }
    if (const std::string refused = host.apply_config(std::move(edited)); !refused.empty()) {
        return bad(refused);
    }
    host.say(name + " has left the experts");
    return good();
}

// ---------------------------------------------------------------------------
// Providers
// ---------------------------------------------------------------------------

/// One provider, as the settings screen draws it.
///
/// The key itself is never in here. Where it comes from is, and whether there
/// is one -- which is everything the screen has a use for, and nothing a
/// screenshot of the screen would give away.
json provider_json(const Provider& provider, const Config& config) {
    json key{{"source", "none"}, {"present", false},
             {"variable", provider.conventional_key_variable()}};
    if (provider.api_key.rfind("env:", 0) == 0) {
        key["source"]   = "variable";
        key["variable"] = format::trim(provider.api_key.substr(4));
        key["present"]  = !provider.resolved_key().empty();
    } else if (!provider.api_key.empty()) {
        key["source"]  = "typed";
        key["present"] = true;
    } else if (!provider.resolved_key().empty()) {
        key["source"]  = "convention";
        key["present"] = true;
    }

    // Who would stop answering if this went away.
    json seats = json::array();
    for (const Expert& expert : config.roster.experts()) {
        if (config.expert(expert.id).provider == provider.id) {
            seats.push_back(expert.name);
        }
    }

    return json{{"id", provider.id},
                {"name", provider.label()},
                {"kind", provider.kind},
                {"base_url", provider.base_url},
                {"endpoint", provider.endpoint()},
                {"on_your_network", provider.on_your_network()},
                {"key", std::move(key)},
                {"models", provider.models},
                {"seats", std::move(seats)}};
}

Reply providers(const json&, const Scene& scene) {
    json listed = json::array();
    for (const Provider& provider : scene.config.providers) {
        listed.push_back(provider_json(provider, scene.config));
    }
    json known = json::array();
    for (const KnownService& service : known_services()) {
        known.push_back(json{{"name", service.name},
                             {"kind", service.kind},
                             {"base_url", service.base_url},
                             {"key_variable", service.key_variable},
                             {"note", service.note}});
    }
    return good(json{{"providers", std::move(listed)}, {"known", std::move(known)}});
}

/// Add a provider, or change one. `id` says which; without one it is new.
///
/// A field that is not sent is left as it was, which matters most for the
/// key: the screen never has it to send back, so "save" with the key box
/// untouched has to mean "the same key".
Reply provider_save(const json& params, Host& host) {
    Config edited = host.config();

    const auto id = params.value("id", std::string{});
    Provider*  target = nullptr;
    for (Provider& one : edited.providers) {
        if (one.id == id && !id.empty()) {
            target = &one;
        }
    }

    Provider fresh;
    if (target == nullptr) {
        if (!id.empty()) {
            return bad("there is no provider called \"" + id + "\"");
        }
        fresh.name = format::trim(params.value("name", std::string{}));
        fresh.id   = provider_id_from_name(fresh.name);
        if (fresh.id.empty()) {
            return bad("a provider needs a name");
        }
        if (edited.provider(fresh.id) != nullptr) {
            return bad("there is already a provider called " + fresh.name);
        }
        target = &fresh;
    } else if (params.contains("name")) {
        // The name it goes by. The id stays: seats refer to it.
        const std::string name = format::trim(params.value("name", std::string{}));
        if (!name.empty()) {
            target->name = name;
        }
    }

    if (params.contains("kind")) {
        target->kind = params.value("kind", std::string{});
    }
    if (target->kind != "anthropic" && target->kind != "openai") {
        return bad("a provider speaks either \"anthropic\" or \"openai\"");
    }
    if (params.contains("base_url")) {
        target->base_url = format::trim(params.value("base_url", std::string{}));
    }
    if (!target->base_url.empty() && target->base_url.rfind("https://", 0) != 0
        && target->base_url.rfind("http://", 0) != 0) {
        return bad("the address has to start with https:// or http://");
    }
    if (params.contains("api_key")) {
        target->api_key = format::trim(params.value("api_key", std::string{}));
    }
    if (params.contains("models") && params["models"].is_array()) {
        target->models.clear();
        for (const json& one : params["models"]) {
            if (one.is_string() && !one.get<std::string>().empty()) {
                target->models.push_back(one.get<std::string>());
            }
        }
    }

    const Provider saved = *target;
    if (target == &fresh) {
        edited.providers.push_back(fresh);
    }

    // Said rather than refused. Somebody running a model server across a VPN
    // knows what they are doing; somebody who typed http by mistake does not
    // know their key is about to cross the internet readable.
    std::string warning;
    if (saved.endpoint().rfind("http://", 0) == 0 && !saved.on_your_network()) {
        warning = "That address is not encrypted. The key and every prompt sent to it "
                  "cross the network readable by anything in between.";
    }

    const json out = provider_json(saved, edited);
    if (const std::string refused = host.apply_config(std::move(edited)); !refused.empty()) {
        return bad(refused);
    }
    json reply{{"provider", out}};
    if (!warning.empty()) {
        reply["warning"] = warning;
    }
    return good(std::move(reply));
}

Reply provider_remove(const json& params, Host& host) {
    const auto id = params.value("id", std::string{});
    Config edited = host.config();
    const Provider* found = edited.provider(id);
    if (found == nullptr) {
        return bad("there is no provider called \"" + id + "\"");
    }
    const std::string name = found->label();

    // Refused while anything is answered by it. Removing it would leave those
    // seats assigned to nothing, and the moment that is discovered is the
    // moment a prompt is routed to one.
    std::string using_it;
    for (const Expert& expert : edited.roster.experts()) {
        if (edited.expert(expert.id).provider == id) {
            using_it += (using_it.empty() ? "" : ", ") + expert.name;
        }
    }
    if (!using_it.empty()) {
        return bad(using_it + (using_it.find(", ") == std::string::npos ? " is" : " are")
                   + " answered by " + name + ". Point "
                   + (using_it.find(", ") == std::string::npos ? "it" : "them")
                   + " at another model first.");
    }

    edited.providers.erase(
        std::remove_if(edited.providers.begin(), edited.providers.end(),
                       [&id](const Provider& one) { return one.id == id; }),
        edited.providers.end());
    const std::string refused = host.apply_config(std::move(edited));
    return refused.empty() ? good() : bad(refused);
}

/// What a provider offers, asked live.
///
/// By id for one that is saved. With an address and a key for one that is
/// still being typed, so "does this key work" can be answered before anything
/// is written to the configuration.
Reply provider_models(const json& params, const Scene& scene) {
    Provider provider;
    if (const auto id = params.value("id", std::string{}); !id.empty()) {
        const Provider* saved = scene.config.provider(id);
        if (saved == nullptr) {
            return bad("there is no provider called \"" + id + "\"");
        }
        provider = *saved;
    } else {
        provider.name     = params.value("name", std::string("that provider"));
        provider.kind     = params.value("kind", std::string("openai"));
        provider.base_url = format::trim(params.value("base_url", std::string{}));
    }
    // A key typed and not yet saved wins over the one on file.
    if (params.contains("api_key")) {
        provider.api_key = format::trim(params.value("api_key", std::string{}));
    }
    if (provider.kind != "anthropic" && provider.kind != "openai") {
        return bad("a provider speaks either \"anthropic\" or \"openai\"");
    }

    std::string error;
    const std::vector<std::string> models = remote::list_models(provider, error);
    if (models.empty()) {
        return bad(error.empty() ? "it listed no models" : error);
    }
    return good(json{{"models", models}});
}

}  // namespace

void roster_methods(std::vector<Method>& table) {
    table.push_back({"expert.add",      nullptr, expert_add});
    table.push_back({"expert.set",      nullptr, expert_set});
    table.push_back({"expert.remove",   nullptr, expert_remove});
    table.push_back({"providers",       providers, nullptr});
    table.push_back({"provider.save",   nullptr, provider_save});
    table.push_back({"provider.remove", nullptr, provider_remove});
    table.push_back({"provider.models", provider_models, nullptr});
}

}  // namespace crucible::api
