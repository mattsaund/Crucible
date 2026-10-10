// SPDX-License-Identifier: MIT
//
// The configuration, and the machine it is a configuration of.
#include "methods.hpp"

#include "crucible/lab/python.hpp"

#include "crucible/llm/mlx_server.hpp"

#include <algorithm>
#include <fstream>
#include <mutex>

#include "crucible/app/self_source.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/config/trust.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"

namespace crucible::api {
namespace {

/// Merge `patch` into `into`, object by object.
///
/// Objects are merged so that {"gpu": {"mode": "single"}} changes the mode
/// and leaves the rest of the gpu block alone. Everything else replaces --
/// an array is a whole value, and a patch that meant to append would have to
/// say so, which nothing here needs.
void merge(json& into, const json& patch) {
    if (!patch.is_object() || !into.is_object()) {
        into = patch;
        return;
    }
    for (const auto& [key, value] : patch.items()) {
        if (value.is_object() && into.contains(key) && into[key].is_object()) {
            merge(into[key], value);
        } else {
            into[key] = value;
        }
    }
}

/// True when `key` is the secret itself rather than the name of where to find
/// it. "env:NAME" is not a secret; what NAME holds is.
bool is_secret(const json& key) {
    return key.is_string() && !key.get<std::string>().empty()
        && key.get<std::string>().rfind("env:", 0) != 0;
}

/// The running configuration as a document.
///
/// Through the one serializer in config_io.cpp, which is the only thing that
/// should know its shape.
json document_of(const Config& config) {
    return json::parse(config_to_json_text(config), nullptr, false);
}

Reply config(const json&, Host& host) {
    json document = document_of(host.config());
    if (!document.is_object()) {
        return good(json::object());
    }
    // A provider's key does not travel. The screen that edits providers has
    // no use for it -- it can say that there is one without holding it -- and
    // a secret that is never sent cannot end up in a log of what was.
    if (document.contains("providers") && document["providers"].is_array()) {
        for (json& provider : document["providers"]) {
            if (provider.is_object() && provider.contains("api_key")
                && is_secret(provider["api_key"])) {
                provider.erase("api_key");
                provider["has_key"] = true;
            }
        }
    }
    // Nor does an MCP server's environment, which is where its tokens go:
    // the names are sent, so the screen can say which are set, and the
    // values are not.
    if (document.contains("tools") && document["tools"].contains("mcp") && document["tools"]["mcp"].is_array()) {
        for (json& server : document["tools"]["mcp"]) {
            if (server.is_object() && server.contains("env") && server["env"].is_object()) {
                for (auto it = server["env"].begin(); it != server["env"].end(); ++it) {
                    it.value() = "";
                }
            }
        }
    }
    return good(std::move(document));
}

Reply config_set(const json& params, Host& host) {
    // A patch merged over the current document, then read back through the
    // same parser the config file uses. Not field by field: a list of fields
    // here would be a second place to add every new setting, and the one
    // that got forgotten would be the one that silently stopped saving.
    // Anything the file understands, this understands.
    const json before = document_of(host.config());
    if (!before.is_object()) {
        return bad("the running configuration could not be read back");
    }
    json document = before;
    merge(document, params);

    // The other half of the key not traveling: a list of providers sent
    // back without their keys means "the same keys", not "no keys". Matched
    // by id, since a list is replaced whole.
    if (params.contains("providers") && document["providers"].is_array()
        && before.contains("providers") && before["providers"].is_array()) {
        for (json& provider : document["providers"]) {
            if (!provider.is_object() || provider.contains("api_key")) {
                continue;
            }
            for (const json& old : before["providers"]) {
                if (old.is_object() && old.value("id", "") == provider.value("id", "")
                    && old.contains("api_key")) {
                    provider["api_key"] = old["api_key"];
                }
            }
        }
    }

    // And an MCP server's environment sent back empty means "as it was",
    // by server and name.
    if (params.contains("tools") && params["tools"].contains("mcp") && document["tools"]["mcp"].is_array()
        && before.contains("tools") && before["tools"].contains("mcp") && before["tools"]["mcp"].is_array()) {
        for (json& server : document["tools"]["mcp"]) {
            if (!server.is_object() || !server.contains("env") || !server["env"].is_object()) {
                continue;
            }
            for (const json& old : before["tools"]["mcp"]) {
                if (!old.is_object() || old.value("name", "") != server.value("name", "")
                    || !old.contains("env") || !old["env"].is_object()) {
                    continue;
                }
                for (auto it = server["env"].begin(); it != server["env"].end(); ++it) {
                    if (it.value() == "" && old["env"].contains(it.key())) {
                        it.value() = old["env"][it.key()];
                    }
                }
            }
        }
    }

    std::vector<std::string> warnings;
    Config edited = config_from_json_text(document.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace), warnings);
    if (const std::string refused = host.apply_config(std::move(edited)); !refused.empty()) {
        return bad(refused);
    }
    // Warnings are the parser's complaints about what it was handed -- an
    // out-of-range number clamped, a field of the wrong type ignored.
    // Returned rather than swallowed, because the interface asked for this
    // change and should be able to say it did not entirely happen.
    return good(json{{"warnings", warnings}});
}

/// What a seat can be pointed at on this machine: the models directory.
///
/// One folder, and only that one. A fine-tune made here is written into it
/// (see lab.train), so it appears in this list like anything else put there,
/// and a seat -- the delegator's included -- is filled by choosing a name from
/// one place rather than by browsing the disk for a file.
Reply models(const json&, const Scene& scene) {
    const std::filesystem::path dir = scene.config.resolved_models_dir();
    json files = json::array();
    bool any_mlx = false;
    for (const ModelFile& file : scan_models(dir)) {
        files.push_back(json{{"name", file.name},
                             {"path", file.path.string()},
                             {"bytes", file.bytes},
                             {"format", file.format}});
        any_mlx = any_mlx || file.format == "mlx";
    }
    json out{{"directory", dir.string()},
             {"display", format::short_path(dir)},
             {"models", std::move(files)}};
    // Asked only when there is one to ask about: it starts Python.
    if (any_mlx) {
        out["mlx_unavailable"] = mlx::unavailable();
    }
    return good(std::move(out));
}

/// The graphics cards, and which of the settings about them can work.
///
/// What ggml actually found, not what the machine is believed to have: a
/// card with no runtime that can drive it is not in this list, and that is
/// the answer.
Reply devices(const json&, const Scene&) {
    const std::vector<ComputeDevice> found = gpu_devices();
    json gpus = json::array();
    for (const ComputeDevice& device : found) {
        gpus.push_back(json{{"index", device.index},
                            {"name", device.description},
                            {"backend", device.backend},
                            {"memory_total", device.memory_total},
                            {"memory_free", device.memory_free}});
    }
    // A setting the hardware cannot honor is worse than a missing one: it
    // reads as configured, it saves, and nothing happens. Each of these is
    // empty when the setting works and the reason when it does not.
    const GpuSettingSupport support = gpu_setting_support(found);
    return good(json{{"gpus", std::move(gpus)},
                     {"support", json{{"split",     support.split},
                                      {"gpu_only",  support.gpu_only},
                                      {"vram_only", support.vram_only}}}});
}

/// What this copy is, where it keeps things, and whether there is a newer one.
Reply about(const json&, const Scene& scene) {
    update::State newer;
    update::read_cache(newer);

    // Named, not a temporary in the loop header: `entries` hands back a
    // reference into the store, and a store that had already gone would be a
    // reference into nothing.
    const TrustStore trust(paths::trust_file());
    json trusted = json::array();
    for (const std::filesystem::path& folder : trust.entries()) {
        trusted.push_back(folder.string());
    }

    return good(json{
        {"version", CRUCIBLE_VERSION},
        {"update", json{{"latest", newer.latest},
                        {"available", update::newer_than_this(newer)},
                        {"page", newer.page.empty() ? update::releases_url() : newer.page},
                        {"command", std::string(update::update_command())},
                        {"checked_at", newer.checked_at},
                        {"checks", scene.config.ui.check_updates}}},
        // For reading, so each is written with ~ for the home directory.
        {"files", json{{"config",   format::short_path(paths::config_file())},
                       {"data",     format::short_path(paths::data_dir())},
                       {"models",   format::short_path(scene.config.resolved_models_dir())},
                       {"runtimes", format::short_path(paths::runtimes_dir())},
                       {"projects", format::short_path(paths::projects_dir())},
                       {"python",   format::short_path(lab::python::root())},
                       {"log",      format::short_path(paths::log_file())},
                       {"crashes",  format::short_path(paths::data_dir() / "crash.log")}}},
        {"trusted", std::move(trusted)},
    });
}

/// Ask now, rather than waiting for the daily check.
Reply update_check(const json&, const Scene&) {
    const update::State newer = update::refresh(/*allowed_to_ask=*/true);
    return good(json{{"latest", newer.latest},
                     {"available", update::newer_than_this(newer)},
                     {"page", newer.page.empty() ? update::releases_url() : newer.page},
                     {"command", std::string(update::update_command())},
                     {"checked_at", newer.checked_at}});
}

}  // namespace

// ---------------------------------------------------------------------------
// What the window remembers
// ---------------------------------------------------------------------------
//
// The side menu's width, the box's height, where prompts go: the shape the
// window was left in. Not settings -- nobody looks for them in a config file,
// and they are about this window rather than about how Crucible works -- but
// they have to outlive the window, and the webview's own storage does not.
// So a small file of their own, beside the rest of Crucible's state.

std::filesystem::path window_file() { return paths::data_dir() / "window.json"; }

/// Guards the file: lookups run on whichever worker is free.
std::mutex& window_mutex() {
    static std::mutex mutex;
    return mutex;
}

json read_window_file() {
    std::ifstream in(window_file());
    if (!in) {
        return json::object();
    }
    json doc = json::parse(in, nullptr, /*allow_exceptions=*/false);
    return doc.is_object() ? doc : json::object();
}

Reply prefs(const json&, const Scene&) {
    const std::lock_guard<std::mutex> lock(window_mutex());
    return good(read_window_file());
}

Reply prefs_set(const json& params, const Scene&) {
    const auto key = params.value("key", std::string{});
    if (key.empty() || key.size() > 64) {
        return bad("prefs.set needs a short key");
    }
    if (!params.contains("value")) {
        return bad("prefs.set needs a value");
    }
    const std::lock_guard<std::mutex> lock(window_mutex());
    json doc = read_window_file();
    doc[key] = params["value"];
    // Written whole beside the old one and then moved over it, so a window
    // closed mid-write leaves the last good copy rather than half of one.
    std::error_code ec;
    std::filesystem::create_directories(window_file().parent_path(), ec);
    const std::filesystem::path next = window_file().string() + ".new";
    {
        std::ofstream out(next, std::ios::trunc);
        if (!out) {
            return bad("could not write " + next.string());
        }
        out << doc.dump(2, ' ', false, nlohmann::json::error_handler_t::replace) << '\n';
    }
    std::filesystem::rename(next, window_file(), ec);
    return ec ? bad("could not save " + window_file().string() + ": " + ec.message()) : good();
}

// ---------------------------------------------------------------------------
// Crucible's own source
// ---------------------------------------------------------------------------

/// Where the source is, and how a rebuild of it is going.
Reply self_source(const json&, Host& host) {
    const self::Source source = self::find(host.config().ui.source_dir);
    json out{{"found", source.found}, {"root", source.root.string()}, {"build", source.build.string()},
             {"program", source.program.string()}, {"why", source.why}};
    if (self::Rebuild* rebuild = host.rebuild()) {
        out["rebuilding"] = rebuild->running();
        out["status"]     = rebuild->status();
        out["log"]        = rebuild->log();
    }
    return good(std::move(out));
}

/// Build it again, in the background.
Reply self_rebuild(const json&, Host& host) {
    self::Rebuild* rebuild = host.rebuild();
    if (rebuild == nullptr) {
        return bad("this Crucible cannot rebuild itself");
    }
    std::string error;
    if (!rebuild->start(self::find(host.config().ui.source_dir), error)) {
        return bad(error);
    }
    return good();
}

/// Start what was built, and close this one.
///
/// Only where there is a window to close: from anywhere else -- a test's bare
/// surface, a caller on the pipe -- it would start a second Crucible beside
/// this one, which is how running the tests once opened a window each time.
Reply self_restart(const json&, Host& host) {
    const self::Rebuild* rebuild = host.rebuild();
    if (rebuild == nullptr) {
        return bad("this Crucible cannot restart itself");
    }
    if (rebuild->running()) {
        return bad("the rebuild has not finished");
    }
    std::string error;
    if (!self::restart(self::find(host.config().ui.source_dir), error)) {
        return bad(error);
    }
    host.quit();
    return good();
}

void settings_methods(std::vector<Method>& table) {
    table.push_back({"config",       nullptr, config});
    table.push_back({"config.set",   nullptr, config_set});
    table.push_back({"models",       models, nullptr});
    table.push_back({"devices",      devices, nullptr});
    table.push_back({"about",        about, nullptr});
    table.push_back({"self.source",  nullptr, self_source});
    table.push_back({"self.rebuild", nullptr, self_rebuild});
    table.push_back({"self.restart", nullptr, self_restart});
    table.push_back({"update.check", update_check, nullptr});
    table.push_back({"prefs",        prefs, nullptr});
    table.push_back({"prefs.set",    prefs_set, nullptr});
}

}  // namespace crucible::api
