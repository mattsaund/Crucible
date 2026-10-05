// SPDX-License-Identifier: MIT
//
// Making an expert: the recipes, the run, and where the pieces come from.
#include "methods.hpp"

#include <chrono>

#include "crucible/config/paths.hpp"
#include "crucible/lab/hub.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::api {
namespace {

// ---------------------------------------------------------------------------
// Recipes, both ways
// ---------------------------------------------------------------------------
//
// One shape in and the same shape out, so an interface can read a recipe,
// change a field and send it straight back without knowing which fields it
// did not touch.

json asset_json(const lab::Asset& asset) {
    return json{{"source", std::string(lab::source_id(asset.source))},
                {"id", asset.id}, {"file", asset.file}, {"label", asset.label},
                {"bytes", asset.bytes}, {"path", asset.path}};
}

lab::Asset asset_from_json(const json& node) {
    lab::Asset asset;
    if (!node.is_object()) {
        return asset;
    }
    asset.source = lab::source_from_id(node.value("source", "hub"));
    asset.id     = node.value("id", "");
    asset.file   = node.value("file", "");
    asset.label  = node.value("label", "");
    asset.bytes  = node.value("bytes", std::uint64_t{0});
    asset.path   = node.value("path", "");
    return asset;
}

/// The memory a fine-tune has to fit in: every card this machine can drive,
/// together. Zero when no GPU runtime is installed, which the page says in
/// words rather than as "needs 6 GB of 0".
std::uint64_t training_memory() {
    std::uint64_t total = 0;
    for (const ComputeDevice& gpu : gpu_devices()) {
        total += gpu.memory_total;
    }
    return total;
}

json fit_json(double parameters_b, lab::Method method, std::uint64_t memory) {
    const lab::Fit fit = lab::estimate_fit(parameters_b, method, memory, 0);
    return json{{"possible", fit.possible}, {"needed", fit.needed},
                {"have", fit.have}, {"note", fit.note},
                {"known", parameters_b > 0.0 && memory > 0}};
}

json recipe_json(const lab::Recipe& recipe, std::uint64_t memory) {
    json data = json::array();
    for (const lab::Asset& one : recipe.data)  { data.push_back(asset_json(one)); }
    json tools = json::array();
    for (const lab::Asset& one : recipe.tools) { tools.push_back(asset_json(one)); }

    // Whether the file the recipe says it produced is still where it says.
    // Asked here because the page cannot, and "Test" on a file that has been
    // moved should be a button that explains itself rather than one that
    // fails.
    std::error_code ec;
    const bool there = !recipe.trained_path.empty()
                    && std::filesystem::exists(recipe.trained_path, ec);
    const std::uintmax_t trained_bytes =
        there && std::filesystem::is_regular_file(recipe.trained_path, ec)
            ? std::filesystem::file_size(recipe.trained_path, ec) : 0;

    return json{{"id", recipe.id}, {"name", recipe.name}, {"purpose", recipe.purpose},
                {"base", asset_json(recipe.base)}, {"data", data}, {"tools", tools},
                {"method", std::string(lab::method_id(recipe.method))},
                {"format", std::string(lab::export_id(recipe.format))},
                {"quantization", recipe.quantization},
                {"parameters_b", recipe.parameters_b},
                {"epochs", recipe.epochs}, {"context", recipe.context},
                {"learning_rate", recipe.learning_rate},
                {"trained_path", recipe.trained_path},
                // The same path for reading: with ~ for the home directory.
                {"trained_display", recipe.trained_path.empty()
                                        ? std::string()
                                        : format::short_path(recipe.trained_path)},
                {"trained_there", there},
                {"trained_bytes", trained_bytes},
                {"stage", std::string(lab::stage_id(recipe.stage))},
                {"stage_text", std::string(lab::stage_text(recipe.stage))},
                {"started_at", recipe.started_at}, {"finished_at", recipe.finished_at},
                {"missing", lab::missing(recipe)},
                {"export_bytes", lab::export_bytes(recipe.parameters_b, recipe.quantization)},
                {"fit", fit_json(recipe.parameters_b, recipe.method, memory)}};
}

lab::Recipe recipe_from_json(const json& node) {
    lab::Recipe recipe;
    recipe.name    = node.value("name", "");
    recipe.id      = node.value("id", lab::slug_of(recipe.name));
    if (recipe.id.empty()) {
        recipe.id = lab::slug_of(recipe.name);
    }
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

// ---------------------------------------------------------------------------
// The methods
// ---------------------------------------------------------------------------

Reply lab_recipes(const json&, const Scene&) {
    const std::uint64_t memory = training_memory();
    json out = json::array();
    for (const lab::Recipe& recipe : lab::saved_recipes()) {
        out.push_back(recipe_json(recipe, memory));
    }
    return good(json{{"recipes", std::move(out)}, {"memory", memory}});
}

/// Would this fit, for a recipe that is still being filled in.
///
/// The wizard asks as the choice is made -- QLoRA or LoRA, this base or that
/// -- so the answer is beside the radio button rather than discovered twenty
/// minutes into a run.
Reply lab_fit(const json& params, const Scene&) {
    const std::uint64_t memory = training_memory();
    const double parameters_b  = params.value("parameters_b", 0.0);
    json out = json::object();
    for (const lab::Method method : {lab::Method::Qlora, lab::Method::Lora}) {
        out[std::string(lab::method_id(method))] = fit_json(parameters_b, method, memory);
    }
    json sizes = json::object();
    for (const char* quantization : {"Q4_K_M", "Q5_K_M", "Q6_K", "Q8_0", "F16"}) {
        sizes[quantization] = lab::export_bytes(parameters_b, quantization);
    }
    return good(json{{"methods", std::move(out)}, {"sizes", std::move(sizes)},
                     {"memory", memory}});
}

Reply lab_save(const json& params, Host&) {
    lab::Recipe recipe = recipe_from_json(params);
    std::string error;
    if (!lab::save(recipe, error)) {
        return bad(error);
    }
    return good(recipe_json(recipe, training_memory()));
}

Reply lab_delete(const json& params, Host& host) {
    const lab::Recipe recipe = find_recipe(params.value("id", std::string{}));
    if (recipe.id.empty()) {
        return bad("no recipe with that id");
    }
    if (host.trainer() != nullptr) {
        const lab::RunProgress run = host.trainer()->progress();
        if (run.running() && run.recipe_id == recipe.id) {
            return bad("it is training -- stop the run first");
        }
    }
    std::error_code ec;
    std::filesystem::remove_all(lab::recipe_file(recipe).parent_path(), ec);
    if (ec) {
        return bad("could not remove it: " + ec.message());
    }
    host.say((recipe.name.empty() ? recipe.id : recipe.name) + " was deleted");
    return good();
}

/// A model trained somewhere else, pointed at rather than retrained.
Reply lab_attach(const json& params, Host& host) {
    const auto path = params.value("path", std::string{});
    const std::filesystem::path file = paths::expand_user(path);
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return bad(path + " is not a file on this machine");
    }
    lab::Recipe recipe = find_recipe(params.value("id", std::string{}));
    if (recipe.id.empty()) {
        return bad("no recipe with that id");
    }
    recipe.trained_path = std::filesystem::absolute(file, ec).string();
    recipe.stage        = lab::Stage::Testing;
    std::string error;
    if (!lab::save(recipe, error)) {
        return bad(error);
    }
    host.say(recipe.name + " is ready to test");
    return good(recipe_json(recipe, training_memory()));
}

Reply lab_train(const json& params, Host& host) {
    lab::Trainer* trainer = host.trainer();
    if (trainer == nullptr) {
        return bad("no trainer is available");
    }
    lab::Recipe recipe = find_recipe(params.value("id", std::string{}));
    if (recipe.id.empty()) {
        return bad("no recipe with that id");
    }
    recipe.stage      = lab::Stage::Training;
    recipe.started_at = lab::now_seconds();

    // Woken on every step the run reports, so the page's progress moves and
    // -- more to the point -- so the session notices when it finishes whatever
    // view is on screen.
    Host*       waking = &host;
    std::string error;
    // Into the models directory, so the result is picked for a seat the way
    // every other model is.
    if (!trainer->start(recipe, convert_script(), quantize_bin(),
                        host.config().resolved_models_dir(),
                        [waking] { waking->wake(); }, error)) {
        return bad(error);
    }
    if (!lab::save(recipe, error)) {
        return bad(error);
    }
    host.say(recipe.name + " is training");
    return good(recipe_json(recipe, training_memory()));
}

Reply lab_run(const json& params, Host& host) {
    lab::Trainer* trainer = host.trainer();
    if (trainer == nullptr) {
        return good(json{{"phase", "idle"}});
    }
    if (params.value("cancel", false)) {
        trainer->cancel();
    }
    const lab::RunProgress run = trainer->progress();
    const char* phase = "idle";
    if (run.running()) {
        phase = "running";
    } else if (run.phase == lab::RunProgress::Phase::Done) {
        phase = "done";
    } else if (run.phase == lab::RunProgress::Phase::Failed) {
        phase = "failed";
    } else if (run.phase == lab::RunProgress::Phase::Canceled) {
        phase = "canceled";
    }
    const long long running_for = run.started == std::chrono::steady_clock::time_point{}
        ? 0
        : std::chrono::duration_cast<std::chrono::seconds>(
              std::chrono::steady_clock::now() - run.started).count();
    return good(json{
        {"phase",   phase},
        {"recipe",  run.recipe_id},
        {"name",    run.recipe_name},
        {"label",   run.label()},
        {"percent", run.percent()},
        {"step",    run.step_index},
        {"total",   run.step_total},
        {"loss",    run.loss},
        {"curve",   run.curve},
        {"device",  run.device},
        {"records", run.records},
        {"trainable", run.trainable},
        {"seconds_left", run.seconds_left()},
        {"seconds", running_for},
        {"error",   run.error},
        {"hint",    run.hint},
        {"notes",   run.notes},
        {"log",     run.log_tail},
        {"log_file", run.log_file.string()},
        {"produced", run.produced.string()},
    });
}

// --- trying one before keeping it --------------------------------------------
//
// The seat lasts as long as the window asking the questions. `from` is where
// this conversation starts in the transcript, so the test window can show its
// own exchange and not the project's.

Reply lab_test(const json& params, Host& host) {
    const std::string error = host.test_begin(params.value("id", std::string{}));
    if (!error.empty()) {
        return bad(error);
    }
    return good(json{{"seat", "lab-test"},
                     {"from", host.state() != nullptr ? host.state()->snapshot().turns.size()
                                                      : std::size_t{0}}});
}

Reply lab_test_stop(const json&, Host& host) {
    host.test_end();
    return good(json{{"stopped", true}});
}

Reply lab_keep(const json& params, Host& host) {
    const std::string error = host.keep(params.value("id", std::string{}));
    return error.empty() ? good(json{{"kept", true}}) : bad(error);
}

// ---------------------------------------------------------------------------
// Huggingface
// ---------------------------------------------------------------------------

/// Search the hub, and wait for it.
///
/// A lookup, so the wait is somewhere other than the thread that draws. This
/// used to be two methods -- start a search, then poll for it -- back when
/// every method ran on that thread and a two-second request meant a
/// two-second freeze.
Reply hub_search(const json& params, const Scene&) {
    const auto query = params.value("query", std::string{});
    if (query.empty()) {
        return bad("hub.search needs something to search for");
    }
    const lab::hub::Kind kind = params.value("kind", std::string("model")) == "dataset"
                                    ? lab::hub::Kind::Dataset
                                    : lab::hub::Kind::Model;
    std::vector<lab::hub::Item> found;
    std::string                 error;
    if (!lab::hub::search(kind, query, 25, found, error)) {
        return bad(error.empty() ? "Huggingface could not be asked" : error);
    }
    json items = json::array();
    for (const lab::hub::Item& item : found) {
        items.push_back(json{{"id", item.id}, {"author", item.author},
                             {"summary", item.summary},
                             {"downloads", item.downloads}, {"likes", item.likes},
                             {"parameters_b", item.parameters_b}, {"gated", item.gated}});
    }
    return good(json{{"items", std::move(items)}});
}

}  // namespace

void lab_methods(std::vector<Method>& table) {
    table.push_back({"lab.recipes",   lab_recipes, nullptr});
    table.push_back({"lab.fit",       lab_fit, nullptr});
    table.push_back({"lab.save",      nullptr, lab_save});
    table.push_back({"lab.delete",    nullptr, lab_delete});
    table.push_back({"lab.attach",    nullptr, lab_attach});
    table.push_back({"lab.train",     nullptr, lab_train});
    table.push_back({"lab.run",       nullptr, lab_run});
    table.push_back({"lab.test",      nullptr, lab_test});
    table.push_back({"lab.test.stop", nullptr, lab_test_stop});
    table.push_back({"lab.keep",      nullptr, lab_keep});
    table.push_back({"hub.search",    hub_search, nullptr});
}

}  // namespace crucible::api
