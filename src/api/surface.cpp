// SPDX-License-Identifier: MIT
//
// The envelope, and the table behind it.
//
// What a method *does* is in the methods_*.cpp files beside this one. This
// file is only the part that is the same for all of them: reading a request,
// finding the method it names, and wrapping what comes back.
#include "crucible/api/surface.hpp"
#include "crucible/app/setup.hpp"

#include "crucible/config/paths.hpp"
#include "crucible/session/store.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>

#include "crucible/llm/prices.hpp"
#include "crucible/llm/spend.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/resources.hpp"
#include "methods.hpp"

namespace crucible::api {
namespace {

json ok(const json& id, json result) {
    return json{{"id", id}, {"ok", true}, {"result", std::move(result)}};
}

json fail(const json& id, std::string message) {
    return json{{"id", id}, {"ok", false}, {"error", std::move(message)}};
}

/// Serialized so that it cannot throw. A model's reply can hold a byte that is
/// not UTF-8 -- half a character at the end of a stopped generation is enough
/// -- and it travels through here inside a snapshot.
std::string text_of(const json& document) {
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

/// This machine's memory, asked once: the models' share of it is what the
/// right-hand panel draws, and asking costs a program run on a Mac.
std::uint64_t memory_total() {
    static const std::uint64_t total = [] {
        std::uint64_t used = 0;
        std::uint64_t all  = 0;
        util::system_memory(used, all);
        return all;
    }();
    return total;
}

/// Every method there is, built once.
const std::vector<Method>& table() {
    static const std::vector<Method> methods = [] {
        std::vector<Method> out;
        conversation_methods(out);
        project_methods(out);
        roster_methods(out);
        settings_methods(out);
        install_methods(out);
        lab_methods(out);
        source_methods(out);
        return out;
    }();
    return methods;
}

const Method* find(std::string_view name) {
    for (const Method& method : table()) {
        if (method.name == name) {
            return &method;
        }
    }
    return nullptr;
}

}  // namespace

/// A request with everything decided about it except the answer.
struct Prepared::Impl {
    json          id;
    json          params = json::object();
    const Method* method = nullptr;

    /// Set when the request could be answered without running anything: it
    /// was malformed, or named nothing, or asked one of the two questions
    /// that are about the surface itself.
    std::string answered;

    /// What a lookup may know. Filled in only for one.
    Scene scene;
};

std::vector<std::string> Surface::methods() {
    std::vector<std::string> names{"ping", "methods"};
    for (const Method& method : table()) {
        names.emplace_back(method.name);
    }
    return names;
}

Prepared Surface::prepare(std::string_view request) {
    auto     impl = std::make_shared<Prepared::Impl>();
    Prepared prepared;

    const json parsed = json::parse(request, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        impl->answered = text_of(fail(nullptr, "not a request object"));
        prepared.impl  = std::move(impl);
        return prepared;
    }

    impl->id = parsed.contains("id") ? parsed["id"] : json(nullptr);
    if (parsed.contains("params") && parsed["params"].is_object()) {
        impl->params = parsed["params"];
    }
    const std::string name = parsed.value("method", std::string{});

    if (name.empty()) {
        impl->answered = text_of(fail(impl->id, "no method named"));
    } else if (name == "ping") {
        // These two answer before anything else is looked at, because they
        // are what a caller asks to find out whether anything is listening at
        // all. A door that cannot say "open" until the house is finished is
        // not much of a door.
        impl->answered = text_of(ok(impl->id, json{{"version", CRUCIBLE_VERSION}}));
    } else if (name == "methods") {
        impl->answered = text_of(ok(impl->id, methods()));
    } else if (const Method* method = find(name); method == nullptr) {
        // Whether a name exists is a fact about this surface and not about
        // what is running behind it -- a caller with a typo told "the engine
        // is not running" goes and debugs the wrong thing entirely.
        impl->answered = text_of(fail(impl->id, "no method called '" + name + "'"));
    } else {
        impl->method = method;
        if (method->lookup != nullptr) {
            // The copy a lookup works from, taken here because here is the
            // session's thread and the lookup may not be.
            impl->scene.config  = host_.config();
            impl->scene.project = host_.project_root();
            prepared.background = true;
        }
    }
    prepared.impl = std::move(impl);
    return prepared;
}

std::string Surface::run(const Prepared& prepared) {
    if (!prepared.impl) {
        return text_of(fail(nullptr, "not a request object"));
    }
    const Prepared::Impl& call = *prepared.impl;
    if (!call.answered.empty()) {
        return call.answered;
    }

    // "Never throws" is a promise about this function, and the methods are
    // written to keep it themselves -- but they call a JSON library that
    // throws on a wrong type and a filesystem library that throws on a
    // vanished directory, and the caller on the other side of this is a
    // webview that would rather have a sentence.
    Reply reply;
    try {
        reply = call.method->lookup != nullptr
                    ? call.method->lookup(call.params, call.scene)
                    : call.method->action(call.params, host_);
    } catch (const std::exception& e) {
        reply = bad(std::string(call.method->name) + " failed: " + e.what());
    } catch (...) {
        reply = bad(std::string(call.method->name) + " failed");
    }
    return text_of(reply.error.empty() ? ok(call.id, std::move(reply.result))
                                       : fail(call.id, std::move(reply.error)));
}

std::string Surface::handle(std::string_view request) {
    return run(prepare(request));
}

std::string Surface::snapshot() {
    AppState* state = host_.state();
    if (state == nullptr) {
        return "{}";
    }
    json out = snapshot_to_json(state->snapshot());

    // --- what the session knows and the engine does not --------------------
    //
    // Sent with every snapshot rather than asked for, because each is drawn
    // somewhere that is always on screen: the top bar names the project, the
    // side menu names each seat's model, the gear carries the update dot.

    const std::filesystem::path root   = host_.project_root();
    const std::filesystem::path asking = host_.pending_trust();
    json project{{"open", !root.empty()},
                 {"root", root.string()},
                 // With the home directory written as ~, which is how the top
                 // bar shows it: the part of a path that differs between two
                 // projects is the end, and this leaves room for it.
                 {"display", root.empty() ? std::string() : format::short_path(root)},
                 {"name", root.filename().empty() ? root.string() : root.filename().string()}};
    if (is_scratch(root)) {
        project["name"]       = "Scratchpad";
        project["scratchpad"] = true;
    }
    if (root.empty()) {
        // A chat with no project has no folder until its first message makes
        // one; until then the bar says where that folder will be.
        project["scratch_display"] = format::short_path(paths::scratchpad_dir());
    }
    if (!asking.empty()) {
        project["pending_trust"]   = asking.string();
        project["pending_display"] = format::short_path(asking);
    }
    out["project"] = std::move(project);

    const Config config = host_.config();
    if (out.contains("experts")) {
        for (json& expert : out["experts"]) {
            const std::string  id     = expert.value("id", std::string{});
            const ModelParams& params = config.expert(id);
            // The model's own name: a file name for one that is here, the
            // provider's name for it otherwise. Never the directory.
            expert["model"] = params.remote()
                                  ? params.model
                                  : std::filesystem::path(params.model).filename().string();
            if (config.routing.default_expert == id) {
                expert["default"] = true;
            }
        }
    }
    out["delegator"] = json{
        {"model", std::filesystem::path(config.router.model).filename().string()},
        {"stays_loaded", config.routing.keep_delegator_loaded},
    };
    out["auto_edits"]     = config.tools.auto_edits;
    out["reasoning_effort"] = config.reasoning_effort;
    out["session"]          = host_.session_id();
    out["session_name"]     = host_.session_name();

    // What Crucible is fetching for itself, while it is, and anything that
    // failed until it is retried. See app/setup.hpp.
    if (Setup* setup = host_.setup()) {
        const std::vector<Setup::Item> items = setup->items();
        if (!items.empty()) {
            json list = json::array();
            for (const Setup::Item& item : items) {
                const char* phase = item.state == Setup::Item::State::Working ? "working"
                                  : item.state == Setup::Item::State::Done    ? "done"
                                  : item.state == Setup::Item::State::Failed  ? "failed"
                                                                              : "waiting";
                json one{{"id", item.id}, {"label", item.label}, {"state", phase},
                         {"detail", item.detail}, {"download", item.download}};
                if (item.progress >= 0.0F) {
                    one["progress"] = item.progress;
                }
                list.push_back(std::move(one));
            }
            out["setup"] = json{{"running", setup->running()}, {"items", list}};
        }
    }

    // The frontier models: what each has been asked, what that cost at list
    // prices, and what its provider says is left of its limits. See
    // llm/spend.hpp and llm/prices.hpp.
    if (const Engine* engine = host_.engine()) {
        const auto tokens = [](const spend::Tokens& t) {
            return json{{"input", t.input}, {"cache_read", t.cache_read}, {"cache_write", t.cache_write},
                        {"output", t.output}, {"requests", t.requests}};
        };
        json frontier = json::array();
        for (const spend::Model& model : engine->spending()) {
            json one{{"provider", model.provider}, {"model", model.model}, {"last", model.last},
                     {"prompt", model.prompt}, {"context", model.context},
                     {"session", tokens(model.session)}, {"month", tokens(model.month)}};
            if (const std::optional<prices::Price> price = prices::find(model.endpoint, model.model)) {
                one["session"]["cost"] = prices::cost(model.session, *price);
                one["month"]["cost"]   = prices::cost(model.month, *price);
            }
            json limits = json::array();
            for (const spend::Limit& limit : model.limits) {
                limits.push_back(json{{"what", limit.what}, {"limit", limit.limit},
                                      {"remaining", limit.remaining}, {"resets", limit.resets}});
            }
            one["limits"] = std::move(limits);
            frontier.push_back(std::move(one));
        }
        out["frontier"] = std::move(frontier);
    }
    out["memory_total"] = memory_total();

    const update::State newer = host_.update();
    out["version"] = CRUCIBLE_VERSION;
    if (update::newer_than_this(newer)) {
        out["update"] = json{{"latest", newer.latest},
                             {"page", newer.page.empty() ? update::releases_url() : newer.page},
                             {"command", std::string(update::update_command())}};
    }
    return text_of(out);
}

}  // namespace crucible::api
