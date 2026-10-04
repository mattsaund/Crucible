// SPDX-License-Identifier: MIT
#include "crucible/api/surface.hpp"

#include "crucible/config/paths.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/llm/model_catalog.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/session/store.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

#include <algorithm>
#include <map>
#include <thread>

#include <nlohmann/json.hpp>

namespace crucible::api {
namespace {

using json = nlohmann::json;

// --- the vocabulary -------------------------------------------------------
//
// Enums cross the boundary as words, not numbers. A number is a thing the two
// sides have to agree about forever and cannot be read in a log; a word
// survives a reordering of the enum and says what it means when a request is
// printed out.

std::string_view mood_word(Mood mood) {
    switch (mood) {
        case Mood::Routing:  return "routing";
        case Mood::Loading:  return "loading";
        case Mood::Thinking: return "thinking";
        case Mood::Talking:  return "talking";
        case Mood::Error:    return "error";
        case Mood::Idle:     break;
    }
    return "idle";
}

std::string_view seat_word(SeatPhase phase) {
    switch (phase) {
        case SeatPhase::Missing:      return "missing";
        case SeatPhase::Dormant:      return "dormant";
        case SeatPhase::Loading:      return "loading";
        case SeatPhase::Active:       return "active";
        case SeatPhase::Unconfigured: break;
    }
    return "unconfigured";
}

json usage_json(const TokenUsage& usage) {
    return json{
        {"input_tokens",  usage.input_tokens},
        {"output_tokens", usage.output_tokens},
        {"turns",         usage.turns},
        {"output_ms",     usage.output_ms},
    };
}

json turn_json(const Turn& turn) {
    json out{
        {"prompt",            turn.prompt},
        {"reply",             turn.reply},
        {"streaming",         turn.streaming},
        {"canceled",          turn.canceled},
        {"failed",            turn.failed},
        {"tokens_per_second", turn.tokens_per_second},
    };
    // Reasoning is sent but kept apart from the reply, for the same reason the
    // window draws it apart: it is not the answer, and an interface that
    // concatenated them would make a reasoning model look broken.
    if (!turn.reasoning.empty()) {
        out["reasoning"] = turn.reasoning;
    }
    if (turn.route) {
        out["route"] = json{
            {"expert",     turn.route->expert},
            {"confidence", turn.route->confidence},
        };
    }
    if (!turn.actions.empty()) {
        json actions = json::array();
        for (const TurnAction& action : turn.actions) {
            json one{{"summary", action.summary}};
            if (!action.body.empty())     { one["body"]     = action.body; }
            if (!action.language.empty()) { one["language"] = action.language; }
            actions.push_back(std::move(one));
        }
        out["actions"] = std::move(actions);
    }
    return out;
}

/// The whole drawable state.
///
/// Sent entire rather than as a diff. A snapshot is a few kilobytes, it is
/// produced once per wake rather than per token, and a diff protocol would
/// buy some bandwidth across an in-process call in exchange for two sides
/// that can disagree about what they are looking at.
json snapshot_to_json(const Snapshot& snapshot) {
    json out{
        {"mood",             mood_word(snapshot.mood)},
        {"status",           snapshot.status},
        {"busy",             snapshot.busy},
        {"delegator_ready",  snapshot.delegator_ready},
        {"context_used",     snapshot.context_used},
        {"context_size",     snapshot.context_size},
        {"tokens_per_second", snapshot.live_tokens_per_second},
        {"session_usage",    usage_json(snapshot.session_usage)},
        {"project_usage",    usage_json(snapshot.project_usage)},
        {"notices",          snapshot.notices},
    };

    if (snapshot.resident) { out["resident"] = *snapshot.resident; }
    if (snapshot.linked)   { out["linked"]   = *snapshot.linked; }

    // The roster and the seat states are parallel arrays in the engine and one
    // array of objects here. Two arrays that have to be zipped by index is a
    // thing every interface would have to get right separately.
    json experts = json::array();
    if (snapshot.roster) {
        const std::vector<Expert>& list = snapshot.roster->experts();
        for (std::size_t i = 0; i < list.size(); ++i) {
            json one{
                {"id",    list[i].id},
                {"name",  list[i].name},
                {"tag",   list[i].tag},
                {"blurb", list[i].blurb},
                {"phase", i < snapshot.seats.size() ? seat_word(snapshot.seats[i].phase)
                                                    : seat_word(SeatPhase::Unconfigured)},
                {"progress", i < snapshot.seats.size() ? snapshot.seats[i].progress : 0.0F},
            };
            experts.push_back(std::move(one));
        }
    }
    out["experts"] = std::move(experts);

    json turns = json::array();
    for (const Turn& turn : snapshot.turns) {
        turns.push_back(turn_json(turn));
    }
    out["turns"] = std::move(turns);

    if (snapshot.cook) {
        const Cook& cook = *snapshot.cook;
        json steps = json::array();
        // The tail rather than all of them. An hour's cook is thousands of
        // steps, the view shows the end of the list, and sending the whole
        // journal on every wake would be most of the bytes for none of the
        // information. The full record is on disk and history reads it.
        const std::size_t keep = 200;
        const std::size_t from = cook.steps.size() > keep ? cook.steps.size() - keep : 0;
        for (std::size_t i = from; i < cook.steps.size(); ++i) {
            const CookStep& step = cook.steps[i];
            json one{{"iteration", step.iteration},
                     {"expert",    step.expert},
                     {"kind",      step.kind},
                     {"summary",   step.summary}};
            if (!step.detail.empty())  { one["detail"]  = step.detail; }
            if (!step.changed.empty()) { one["changed"] = step.changed; }
            steps.push_back(std::move(one));
        }
        out["cook"] = json{
            {"running",  true},
            {"id",       cook.id},
            {"goal",     cook.goal},
            {"state",    std::string(cook_state_name(cook.state))},
            {"question", cook.question},
            {"outcome",  cook.outcome},
            {"total",    cook.steps.size()},
            {"shown_from", from},
            {"steps",    std::move(steps)},
        };
    }
    if (snapshot.pending_edit) {
        // Both sides of the edit, because the interface draws them side by
        // side and asking for them in a second request would mean the file
        // could change between the two.
        out["pending_edit"] = json{
            {"path",   snapshot.pending_edit->path},
            {"before", snapshot.pending_edit->before},
            {"after",  snapshot.pending_edit->after},
        };
    }
    return out;
}

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

// --- recipes, both ways ---------------------------------------------------
//
// One shape in and the same shape out, so an interface can read a recipe,
// change a field and send it straight back without knowing which fields it
// did not touch.

json asset_json(const lab::Asset& asset) {
    return json{{"source", std::string(lab::source_id(asset.source))},
                {"id", asset.id}, {"label", asset.label}, {"path", asset.path}};
}

lab::Asset asset_from_json(const json& node) {
    lab::Asset asset;
    asset.source = lab::source_from_id(node.value("source", "hub"));
    asset.id     = node.value("id", "");
    asset.label  = node.value("label", "");
    asset.path   = node.value("path", "");
    return asset;
}

json recipe_json(const lab::Recipe& recipe) {
    json data = json::array();
    for (const lab::Asset& one : recipe.data)  { data.push_back(asset_json(one)); }
    json tools = json::array();
    for (const lab::Asset& one : recipe.tools) { tools.push_back(asset_json(one)); }

    return json{{"id", recipe.id}, {"name", recipe.name}, {"purpose", recipe.purpose},
                {"base", asset_json(recipe.base)}, {"data", data}, {"tools", tools},
                {"method", std::string(lab::method_id(recipe.method))},
                {"format", std::string(lab::export_id(recipe.format))},
                {"quantization", recipe.quantization},
                {"parameters_b", recipe.parameters_b},
                {"epochs", recipe.epochs}, {"context", recipe.context},
                {"learning_rate", recipe.learning_rate},
                {"trained_path", recipe.trained_path},
                {"stage", std::string(lab::stage_id(recipe.stage))},
                {"stage_text", std::string(lab::stage_text(recipe.stage))},
                {"started_at", recipe.started_at}, {"finished_at", recipe.finished_at},
                {"missing", lab::missing(recipe)},
                {"export_bytes", lab::export_bytes(recipe.parameters_b, recipe.quantization)}};
}

lab::Recipe recipe_from_json(const json& node) {
    lab::Recipe recipe;
    recipe.name    = node.value("name", "");
    recipe.id      = node.value("id", lab::slug_of(recipe.name));
    recipe.purpose = node.value("purpose", "");
    if (node.contains("base")) { recipe.base = asset_from_json(node["base"]); }
    for (const json& one : node.value("data",  json::array()))  { recipe.data.push_back(asset_from_json(one)); }
    for (const json& one : node.value("tools", json::array())) { recipe.tools.push_back(asset_from_json(one)); }
    recipe.method        = lab::method_from_id(node.value("method", "qlora"));
    recipe.format        = lab::export_from_id(node.value("format", "gguf"));
    recipe.quantization  = node.value("quantization", "Q4_K_M");
    recipe.parameters_b  = node.value("parameters_b", 0.0);
    recipe.epochs        = node.value("epochs", 2);
    recipe.context       = node.value("context", 512);
    recipe.learning_rate = node.value("learning_rate", 1e-5F);
    recipe.trained_path  = node.value("trained_path", "");
    recipe.stage         = lab::stage_from_id(node.value("stage", "draft"));
    recipe.started_at    = node.value("started_at", std::int64_t{0});
    recipe.finished_at   = node.value("finished_at", std::int64_t{0});
    return recipe;
}

/// The saved recipe with this id, or one with an empty id.
lab::Recipe find_recipe(const std::string& id) {
    for (const lab::Recipe& recipe : lab::saved_recipes()) {
        if (recipe.id == id && !id.empty()) {
            return recipe;
        }
    }
    return lab::Recipe{};
}

/// llama.cpp's exporter and quantizer, when this machine has them. Both are
/// optional: a run without them stops at a Huggingface model directory and
/// says so, which is a result rather than a failure.
std::filesystem::path convert_script() {
    const std::filesystem::path script = paths::runtime_src_dir() / "convert_hf_to_gguf.py";
    std::error_code ec;
    return std::filesystem::exists(script, ec) ? script : std::filesystem::path{};
}

std::filesystem::path quantize_bin() {
    std::error_code ec;
    const std::filesystem::path beside =
        util::executable_path().parent_path() / "llama-quantize";
    if (std::filesystem::exists(beside, ec)) {
        return beside;
    }
    return util::on_path("llama-quantize") ? std::filesystem::path("llama-quantize")
                                           : std::filesystem::path{};
}

json ok(const json& id, json result) {
    return json{{"id", id}, {"ok", true}, {"result", std::move(result)}};
}

json fail(const json& id, std::string message) {
    return json{{"id", id}, {"ok", false}, {"error", std::move(message)}};
}

/// A long install's progress, in the shape the page draws.
///
/// The two installers are different jobs with the same story: a phase, how
/// far along, what it is doing right now, and -- when it failed -- enough of
/// the log to act on without opening the file.
template <typename Progress>
json progress_json(const Progress& progress, std::string_view phase) {
    json log = json::array();
    for (const std::string& line : progress.log_tail) {
        log.push_back(line);
    }
    return json{
        {"phase",    std::string(phase)},
        {"running",  progress.running()},
        {"finished", progress.finished()},
        {"percent",  progress.percent},
        {"step",     progress.step},
        {"label",    progress.label()},
        {"error",    progress.error},
        {"log",      log},
        {"log_file", progress.log_file.string()},
    };
}

std::string_view phase_name(BuildProgress::Phase phase) {
    switch (phase) {
        case BuildProgress::Phase::Idle:           return "idle";
        case BuildProgress::Phase::FetchingSource: return "fetching";
        case BuildProgress::Phase::Configuring:    return "configuring";
        case BuildProgress::Phase::Compiling:      return "compiling";
        case BuildProgress::Phase::Installing:     return "installing";
        case BuildProgress::Phase::Done:           return "done";
        case BuildProgress::Phase::Failed:         return "failed";
        case BuildProgress::Phase::Canceled:       return "canceled";
    }
    return "idle";
}

std::string_view phase_name(lab::pyenv::Progress::Phase phase) {
    switch (phase) {
        case lab::pyenv::Progress::Phase::Idle:          return "idle";
        case lab::pyenv::Progress::Phase::FindingPython: return "finding python";
        case lab::pyenv::Progress::Phase::CreatingVenv:  return "creating venv";
        case lab::pyenv::Progress::Phase::Installing:    return "installing";
        case lab::pyenv::Progress::Phase::Verifying:     return "verifying";
        case lab::pyenv::Progress::Phase::Done:          return "done";
        case lab::pyenv::Progress::Phase::Failed:        return "failed";
        case lab::pyenv::Progress::Phase::Canceled:      return "canceled";
    }
    return "idle";
}

json build_progress_json(const BuildProgress& progress) {
    json out = progress_json(progress, phase_name(progress.phase));
    out["backend"] = std::string(backend_info(progress.kind).id);
    return out;
}

json pyenv_progress_json(const lab::pyenv::Progress& progress) {
    json out = progress_json(progress, phase_name(progress.phase));
    out["flavor"] = std::string(lab::pyenv::flavor_id(progress.flavor));
    return out;
}

}  // namespace

std::string snapshot_json(const Snapshot& snapshot) {
    return snapshot_to_json(snapshot).dump();
}

std::vector<std::string> Surface::methods() {
    return {
        "ping", "methods", "snapshot",
        "submit", "cancel", "release",
        "cook.start", "cook.stop", "cook.answer",
        "edit.approve",
        "config", "config.set",
        "models", "runtimes", "devices",
        "runtime.build", "runtime.cancel", "runtime.dismiss", "runtime.progress",
        "project", "project.open", "projects", "browse", "trust.answer",
        "history", "history.open",
        "lab.recipes", "lab.save", "lab.delete", "lab.train", "lab.attach", "lab.run",
        "lab.test", "lab.test.stop", "lab.keep",
        "trainer", "trainer.flavors", "trainer.install", "trainer.cancel", "trainer.dismiss",
        "hub.search", "hub.results",
    };
}

std::string Surface::handle(std::string_view request) {
    const json parsed = json::parse(request, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        return fail(nullptr, "not a request object").dump();
    }

    const json id     = parsed.contains("id") ? parsed["id"] : json(nullptr);
    const auto method = parsed.value("method", std::string{});
    const json params = parsed.contains("params") && parsed["params"].is_object()
                            ? parsed["params"]
                            : json::object();

    if (method.empty()) {
        return fail(id, "no method named").dump();
    }
    // These two answer before the engine is looked at, because they are what
    // a caller asks to find out whether anything is listening at all. A door
    // that cannot say "open" until the house is finished is not much of a
    // door.
    if (method == "ping") {
        return ok(id, json{{"version", CRUCIBLE_VERSION}}).dump();
    }
    if (method == "methods") {
        return ok(id, methods()).dump();
    }

    // Is this a method at all? Asked before the engine is, because whether a
    // name exists is a fact about this surface and not about what is running
    // behind it -- and a caller with a typo told "the engine is not running"
    // goes and debugs the wrong thing entirely.
    const std::vector<std::string> known = methods();
    if (std::find(known.begin(), known.end(), method) == known.end()) {
        return fail(id, "no method called '" + method + "'").dump();
    }

    // --- compiling one ------------------------------------------------------
    //
    // The long job the settings screen starts and watches. Start, cancel,
    // and forget-the-result are three methods rather than one with a verb,
    // because they are three different things to be allowed to do.

    if (method == "runtime.build") {
        // The argument before the capability, for the same reason the method
        // name is checked before the engine: a caller who misspelled a
        // backend and is told this build cannot compile runtimes goes and
        // debugs their build options instead of their typo.
        const std::optional<BackendKind> kind =
            backend_from_id(params.value("backend", std::string{}));
        if (!kind) {
            return fail(id, "no such backend").dump();
        }
        if (deps_.runtime_builder == nullptr) {
            return fail(id, "this build cannot compile runtimes").dump();
        }
        std::function<void()> wake = deps_.wake;
        if (!deps_.runtime_builder->start(*kind, [wake] { if (wake) { wake(); } })) {
            return fail(id, "a runtime is already being built").dump();
        }
        return ok(id, json{{"started", true}}).dump();
    }

    if (method == "runtime.cancel") {
        if (deps_.runtime_builder == nullptr) {
            return fail(id, "this build cannot compile runtimes").dump();
        }
        deps_.runtime_builder->cancel();
        return ok(id, json{{"canceled", true}}).dump();
    }

    if (method == "runtime.dismiss") {
        if (deps_.runtime_builder == nullptr) {
            return fail(id, "this build cannot compile runtimes").dump();
        }
        deps_.runtime_builder->dismiss();
        return ok(id, json{{"dismissed", true}}).dump();
    }

    if (method == "runtime.progress") {
        if (deps_.runtime_builder == nullptr) {
            return ok(id, json{{"running", false}, {"phase", "idle"}}).dump();
        }
        return ok(id, build_progress_json(deps_.runtime_builder->progress())).dump();
    }

    // Building it: the same three verbs, for the same reasons. Which flavor
    // is the machine's to suggest and the caller's to confirm -- a CUDA stack
    // on a machine with no NVIDIA driver imports and then fails at the first
    // tensor, which is a confusing place to find out.
    if (method == "trainer.install") {
        if (deps_.pyenv_installer == nullptr) {
            return fail(id, "this build cannot install the trainer").dump();
        }
        const std::string wanted = params.value("flavor", std::string{});
        const lab::pyenv::Flavor flavor = wanted.empty() ? lab::pyenv::flavor_here()
                                                         : lab::pyenv::flavor_from_id(wanted);
        std::function<void()> wake = deps_.wake;
        if (!deps_.pyenv_installer->start(flavor, [wake] { if (wake) { wake(); } })) {
            return fail(id, "the trainer is already being installed").dump();
        }
        return ok(id, json{{"started", true},
                           {"flavor", std::string(lab::pyenv::flavor_id(flavor))}}).dump();
    }

    if (method == "trainer.cancel") {
        if (deps_.pyenv_installer == nullptr) {
            return fail(id, "this build cannot install the trainer").dump();
        }
        deps_.pyenv_installer->cancel();
        return ok(id, json{{"canceled", true}}).dump();
    }

    if (method == "trainer.dismiss") {
        if (deps_.pyenv_installer == nullptr) {
            return fail(id, "this build cannot install the trainer").dump();
        }
        deps_.pyenv_installer->dismiss();
        return ok(id, json{{"dismissed", true}}).dump();
    }

    // What installing would cost, which somebody on a metered connection is
    // entitled to know before it starts rather than after.
    if (method == "trainer.flavors") {
        json out = json::array();
        for (const lab::pyenv::Flavor flavor :
             {lab::pyenv::Flavor::Cuda, lab::pyenv::Flavor::Cpu, lab::pyenv::Flavor::Mlx}) {
            out.push_back(json{
                {"id",        std::string(lab::pyenv::flavor_id(flavor))},
                {"note",      std::string(lab::pyenv::flavor_note(flavor))},
                {"download",  lab::pyenv::download_bytes(flavor)},
                {"installed", lab::pyenv::installed_bytes(flavor)},
                {"suggested", flavor == lab::pyenv::flavor_here()},
            });
        }
        return ok(id, out).dump();
    }

    // Everything past here moves the engine, so a null one is a sentence
    // rather than a crash. The two installs above it do not: compiling a
    // runtime or fetching the trainer's Python has nothing to do with
    // whether a model is loaded, and the moment you most need to do either
    // is the one where nothing is running yet.
    if (deps_.engine == nullptr || deps_.state == nullptr) {
        return fail(id, "the engine is not running").dump();
    }

    if (method == "snapshot") {
        return ok(id, snapshot_to_json(deps_.state->snapshot())).dump();
    }

    if (method == "submit") {
        const auto prompt = params.value("prompt", std::string{});
        if (prompt.empty()) {
            return fail(id, "submit needs a prompt").dump();
        }
        // An expert named here skips routing, which is what `/physics ...`
        // does in the window. An empty one is not an error: it is the normal
        // case, and it means "let the delegator decide".
        const auto expert = params.value("expert", std::string{});
        deps_.engine->submit(prompt, expert.empty() ? std::nullopt
                                                    : std::optional<ExpertId>(expert));
        return ok(id, json::object()).dump();
    }
    if (method == "cancel") {
        deps_.engine->cancel();
        return ok(id, json::object()).dump();
    }
    if (method == "release") {
        if (params.value("all", false)) {
            deps_.engine->release_all();
        } else {
            deps_.engine->release_expert();
        }
        return ok(id, json::object()).dump();
    }

    if (method == "cook.start") {
        const auto goal = params.value("goal", std::string{});
        if (goal.empty()) {
            return fail(id, "cook.start needs a goal").dump();
        }
        // A cook reads and writes files, so it needs somewhere it is allowed
        // to. Refused here rather than in each interface, because "which
        // folder has been trusted" is the session's answer and not theirs.
        const std::filesystem::path root =
            deps_.project_root ? deps_.project_root() : std::filesystem::path{};
        if (root.empty()) {
            return fail(id, "no project is open, and a cook works on a project").dump();
        }
        deps_.engine->start_cook(goal, params.value("seconds", 0), root);
        return ok(id, json::object()).dump();
    }
    if (method == "cook.stop") {
        // Not a cancel: the cook stops taking new work and makes a finishing
        // pass. The distinction is the whole reason stop_cook exists.
        deps_.engine->stop_cook();
        return ok(id, json::object()).dump();
    }
    if (method == "cook.answer") {
        deps_.engine->answer_cook(params.value("answer", std::string{}));
        return ok(id, json::object()).dump();
    }

    // --- what the settings screen needs ------------------------------------

    if (method == "config") {
        if (!deps_.config) {
            return fail(id, "no configuration is loaded").dump();
        }
        // Parsed and re-embedded rather than passed through as a string: an
        // interface asking for the configuration wants an object, and the one
        // serializer in config_io.cpp is the only thing that should know its
        // shape.
        const json document = json::parse(config_to_json_text(deps_.config()),
                                          nullptr, false);
        return ok(id, document.is_object() ? document : json::object()).dump();
    }

    if (method == "config.set") {
        if (!deps_.config || !deps_.apply_config) {
            return fail(id, "the configuration cannot be changed from here").dump();
        }
        // A patch merged over the current document, then read back through
        // the same parser the config file uses. Not field by field: a list of
        // fields here would be a second place to add every new setting, and
        // the one that got forgotten would be the one that silently stopped
        // saving. Anything the file understands, this understands.
        json document = json::parse(config_to_json_text(deps_.config()), nullptr, false);
        if (!document.is_object()) {
            return fail(id, "the running configuration could not be read back").dump();
        }
        merge(document, params);

        std::vector<std::string> warnings;
        Config edited = config_from_json_text(document.dump(), warnings);
        const std::string error = deps_.apply_config(std::move(edited));
        if (!error.empty()) {
            return fail(id, error).dump();
        }
        // Warnings are the parser's complaints about what it was handed --
        // an out-of-range number clamped, a field of the wrong type ignored.
        // Returned rather than swallowed, because the interface asked for
        // this change and should be able to say it did not entirely happen.
        return ok(id, json{{"warnings", warnings}}).dump();
    }

    if (method == "models") {
        // What a seat can be pointed at: the models directory, and what the
        // lab has finished. Both, because a fine-tune made here is a model
        // and having to go and find its file would make the Create tab a
        // detour.
        json out = json::array();
        if (deps_.config) {
            for (const ModelFile& file : scan_models(deps_.config().resolved_models_dir())) {
                out.push_back(json{{"name", file.name},
                                   {"path", file.path.string()},
                                   {"bytes", file.bytes},
                                   {"made_here", false}});
            }
        }
        for (const lab::Made& made : lab::finished_models()) {
            out.push_back(json{{"name", made.name},
                               {"path", made.path.string()},
                               {"bytes", made.bytes},
                               {"made_here", true}});
        }
        return ok(id, out).dump();
    }

    if (method == "devices") {
        // What ggml actually found, not what the machine is believed to have.
        json out = json::array();
        for (const ComputeDevice& device : gpu_devices()) {
            out.push_back(json{{"index", device.index},
                               {"name", device.description},
                               {"backend", device.backend},
                               {"memory_total", device.memory_total},
                               {"memory_free", device.memory_free}});
        }
        return ok(id, out).dump();
    }

    if (method == "runtimes") {
        json out = json::array();
        for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
            out.push_back(json{
                {"id",        std::string(backend_info(status.kind).id)},
                {"installed", status.installed},
                {"active",    status.active},
                {"devices",   status.device_count},
                {"bytes",     status.bytes},
                {"stale",     status.stale},
                {"source",    status.source},
            });
        }
        return ok(id, out).dump();
    }

    // --- which folder we are working in ------------------------------------

    if (method == "project") {
        const std::filesystem::path root =
            deps_.project_root ? deps_.project_root() : std::filesystem::path{};
        const std::filesystem::path asking =
            deps_.pending_trust ? deps_.pending_trust() : std::filesystem::path{};
        json out{{"open", !root.empty()},
                 {"root", root.string()},
                 {"name", root.empty() ? "" : root.filename().string()}};
        // Reported with the project rather than as its own method, because
        // "which folder are we in" and "which folder is waiting to be allowed"
        // are one question asked at one moment.
        if (!asking.empty()) {
            out["pending_trust"] = asking.string();
        }
        return ok(id, out).dump();
    }
    if (method == "trust.answer") {
        if (!deps_.answer_trust) {
            return fail(id, "there is nothing to answer").dump();
        }
        deps_.answer_trust(params.value("trusted", false));
        return ok(id, json::object()).dump();
    }
    if (method == "projects") {
        json out = json::array();
        for (const Project& project : recent_projects()) {
            out.push_back(json{{"root", project.root.string()}, {"name", project.name}});
        }
        return ok(id, out).dump();
    }
    if (method == "project.open") {
        if (!deps_.open_project) {
            return fail(id, "no project can be opened from here").dump();
        }
        const auto path = params.value("path", std::string{});
        if (path.empty()) {
            return fail(id, "project.open needs a path").dump();
        }
        const std::string error = deps_.open_project(std::filesystem::path(path));
        return error.empty() ? ok(id, json::object()).dump() : fail(id, error).dump();
    }

    if (method == "browse") {
        // The folder picker's one question. Hidden directories are left out,
        // which is what every file dialog does and what makes the list
        // readable in a home directory.
        const auto where = params.value("path", std::string{});
        std::filesystem::path at = where.empty() ? paths::expand_user("~")
                                                 : paths::expand_user(where);
        std::error_code ec;
        if (!std::filesystem::is_directory(at, ec)) {
            return fail(id, at.string() + " is not a directory").dump();
        }
        json entries = json::array();
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::directory_iterator(
                 at, std::filesystem::directory_options::skip_permission_denied, ec)) {
            std::error_code each;
            const std::string name = entry.path().filename().string();
            if (entry.is_directory(each) && !name.empty() && name.front() != '.') {
                entries.push_back(name);
            }
        }
        std::sort(entries.begin(), entries.end(),
                  [](const json& a, const json& b) {
                      return a.get<std::string>() < b.get<std::string>();
                  });
        return ok(id, json{{"path",   at.string()},
                           {"parent", at.has_parent_path() ? at.parent_path().string() : ""},
                           {"entries", entries}}).dump();
    }

    // --- what has been done before ----------------------------------------

    if (method == "history") {
        const std::filesystem::path root =
            deps_.project_root ? deps_.project_root() : std::filesystem::path{};
        if (root.empty()) {
            return fail(id, "no project is open, and history is per project").dump();
        }
        const Project project = Project::at(root);

        json sessions = json::array();
        for (const SessionSummary& one : SessionStore(project).list()) {
            sessions.push_back(json{{"id", one.id}, {"title", one.title},
                                    {"when", one.when()}, {"turns", one.turns}});
        }
        json cooks = json::array();
        for (const CookSummary& one : CookLog(project.dir).list()) {
            cooks.push_back(json{{"id", one.id}, {"goal", one.goal},
                                 {"state", std::string(cook_state_name(one.state))},
                                 {"seconds", static_cast<long long>(one.duration.count())}});
        }
        return ok(id, json{{"sessions", sessions}, {"cooks", cooks}}).dump();
    }

    if (method == "history.open") {
        const auto kind = params.value("kind", std::string("session"));
        const auto what = params.value("id", std::string{});
        if (what.empty()) {
            return fail(id, "history.open needs an id").dump();
        }
        if (kind == "cook") {
            const std::filesystem::path root =
                deps_.project_root ? deps_.project_root() : std::filesystem::path{};
            if (root.empty()) {
                return fail(id, "no project is open").dump();
            }
            const std::optional<Cook> cook = CookLog(Project::at(root).dir).load(what);
            if (!cook) {
                return fail(id, "no cook called " + what).dump();
            }
            json steps = json::array();
            for (const CookStep& step : cook->steps) {
                json one{{"iteration", step.iteration}, {"expert", step.expert},
                         {"kind", step.kind}, {"summary", step.summary}};
                if (!step.detail.empty())  { one["detail"]  = step.detail; }
                if (!step.changed.empty()) { one["changed"] = step.changed; }
                steps.push_back(std::move(one));
            }
            return ok(id, json{{"goal", cook->goal}, {"outcome", cook->outcome},
                               {"state", std::string(cook_state_name(cook->state))},
                               {"steps", std::move(steps)}}).dump();
        }
        if (!deps_.open_session) {
            return fail(id, "conversations cannot be reopened from here").dump();
        }
        const std::string error = deps_.open_session(what);
        return error.empty() ? ok(id, json::object()).dump() : fail(id, error).dump();
    }

    // --- the lab ------------------------------------------------------------

    if (method == "lab.recipes") {
        json out = json::array();
        for (const lab::Recipe& recipe : lab::saved_recipes()) {
            out.push_back(recipe_json(recipe));
        }
        return ok(id, out).dump();
    }
    // --- trying one before keeping it --------------------------------------
    //
    // The seat lasts as long as the window asking the questions. `from` is
    // where this conversation starts in the transcript, so the test window
    // can show its own exchange and not the project's.
    if (method == "lab.test") {
        if (!deps_.test_begin) {
            return fail(id, "this build cannot seat a fine-tune").dump();
        }
        const std::string error = deps_.test_begin(params.value("id", std::string{}));
        if (!error.empty()) {
            return fail(id, error).dump();
        }
        return ok(id, json{{"seat", "lab-test"},
                           {"from", deps_.state != nullptr
                                        ? deps_.state->snapshot().turns.size()
                                        : std::size_t{0}}}).dump();
    }

    if (method == "lab.test.stop") {
        if (deps_.test_end) {
            deps_.test_end();
        }
        return ok(id, json{{"stopped", true}}).dump();
    }

    if (method == "lab.keep") {
        if (!deps_.keep) {
            return fail(id, "this build cannot keep a fine-tune").dump();
        }
        const std::string error = deps_.keep(params.value("id", std::string{}));
        if (!error.empty()) {
            return fail(id, error).dump();
        }
        return ok(id, json{{"kept", true}}).dump();
    }

    if (method == "lab.save") {
        lab::Recipe recipe = recipe_from_json(params);
        std::string error;
        if (!lab::save(recipe, error)) {
            return fail(id, error).dump();
        }
        return ok(id, recipe_json(recipe)).dump();
    }
    if (method == "lab.delete") {
        lab::Recipe recipe;
        recipe.id = params.value("id", std::string{});
        if (recipe.id.empty()) {
            return fail(id, "lab.delete needs an id").dump();
        }
        std::error_code ec;
        std::filesystem::remove_all(lab::recipe_file(recipe).parent_path(), ec);
        if (ec) {
            return fail(id, "could not remove it: " + ec.message()).dump();
        }
        return ok(id, json::object()).dump();
    }
    if (method == "lab.attach") {
        // A model trained somewhere else, pointed at rather than retrained.
        const auto path = params.value("path", std::string{});
        std::error_code ec;
        if (!std::filesystem::is_regular_file(std::filesystem::path(path), ec)) {
            return fail(id, path + " is not a file on this machine").dump();
        }
        lab::Recipe recipe = find_recipe(params.value("id", std::string{}));
        if (recipe.id.empty()) {
            return fail(id, "no recipe with that id").dump();
        }
        recipe.trained_path = std::filesystem::absolute(path, ec).string();
        recipe.stage        = lab::Stage::Testing;
        std::string error;
        if (!lab::save(recipe, error)) {
            return fail(id, error).dump();
        }
        return ok(id, recipe_json(recipe)).dump();
    }
    if (method == "lab.train") {
        if (deps_.trainer == nullptr) {
            return fail(id, "no trainer is available").dump();
        }
        lab::Recipe recipe = find_recipe(params.value("id", std::string{}));
        if (recipe.id.empty()) {
            return fail(id, "no recipe with that id").dump();
        }
        recipe.stage      = lab::Stage::Training;
        recipe.started_at = lab::now_seconds();

        std::string error;
        if (!deps_.trainer->start(recipe, convert_script(), quantize_bin(), {}, error)) {
            return fail(id, error).dump();
        }
        if (!lab::save(recipe, error)) {
            return fail(id, error).dump();
        }
        return ok(id, recipe_json(recipe)).dump();
    }
    if (method == "lab.run") {
        if (deps_.trainer == nullptr) {
            return ok(id, json{{"phase", "idle"}}).dump();
        }
        if (params.value("cancel", false)) {
            deps_.trainer->cancel();
        }
        const lab::RunProgress run = deps_.trainer->progress();
        return ok(id, json{
            {"phase",   run.running() ? "running" : (run.finished() ? "finished" : "idle")},
            {"recipe",  run.recipe_id},
            {"label",   run.label()},
            {"percent", run.percent()},
            {"step",    run.step_index},
            {"total",   run.step_total},
            {"loss",    run.loss},
            {"curve",   run.curve},
            {"device",  run.device},
            {"seconds_left", run.seconds_left()},
            {"error",   run.error},
            {"notes",   run.notes},
            {"produced", run.produced.string()},
        }).dump();
    }

    if (method == "trainer") {
        const lab::pyenv::Status status = lab::pyenv::status();
        return ok(id, json{{"ready", status.ready}, {"present", status.present},
                           {"flavor", std::string(lab::pyenv::flavor_id(status.flavor))},
                           {"python", status.python_version},
                           {"torch",  status.torch_version},
                           {"bytes",  status.bytes},
                           {"note",   status.note},
                           {"usable_gpus",   status.usable_gpus},
                           {"unusable_gpus", status.unusable_gpus},
                           {"install", deps_.pyenv_installer != nullptr
                                           ? pyenv_progress_json(deps_.pyenv_installer->progress())
                                           : json{{"running", false}, {"phase", "idle"}}}}).dump();
    }


    // --- Huggingface --------------------------------------------------------

    if (method == "hub.search") {
        // Started here and collected by hub.results. The page calls this on
        // the thread that draws it, and the hub takes seconds -- doing it
        // inline would freeze the window for the length of the request.
        const auto query = params.value("query", std::string{});
        if (query.empty()) {
            return fail(id, "hub.search needs something to search for").dump();
        }
        const lab::hub::Kind kind = params.value("kind", std::string("model")) == "dataset"
                                        ? lab::hub::Kind::Dataset
                                        : lab::hub::Kind::Model;
        auto search = std::make_shared<Search>();
        search_ = search;
        std::thread([search, kind, query] {
            std::vector<lab::hub::Item> items;
            std::string                 error;
            lab::hub::search(kind, query, 25, items, error);
            const std::lock_guard<std::mutex> lock(search->mutex);
            search->items = std::move(items);
            search->error = std::move(error);
            search->done  = true;
        }).detach();
        return ok(id, json::object()).dump();
    }
    if (method == "hub.results") {
        if (!search_) {
            return ok(id, json{{"searching", false}, {"items", json::array()}}).dump();
        }
        const std::lock_guard<std::mutex> lock(search_->mutex);
        if (!search_->done) {
            return ok(id, json{{"searching", true}, {"items", json::array()}}).dump();
        }
        json items = json::array();
        for (const lab::hub::Item& item : search_->items) {
            items.push_back(json{{"id", item.id}, {"downloads", item.downloads},
                                 {"parameters_b", item.parameters_b}, {"gated", item.gated}});
        }
        return ok(id, json{{"searching", false}, {"error", search_->error},
                           {"items", std::move(items)}}).dump();
    }

    if (method == "edit.approve") {
        deps_.engine->approve_edit(params.value("approved", false));
        return ok(id, json::object()).dump();
    }

    // Listed by methods() and not handled above. Not a caller's mistake --
    // ours -- and worth saying differently so it is not mistaken for a typo.
    return fail(id, "'" + method + "' is listed but not implemented").dump();
}

}  // namespace crucible::api
