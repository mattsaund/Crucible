// SPDX-License-Identifier: MIT
//
// Making an expert: the recipes, the run, and where the pieces come from.
#include "methods.hpp"

#include <atomic>

#include <chrono>
#include <map>
#include <mutex>
#include <sstream>

#include "crucible/config/paths.hpp"
#include "crucible/lab/condense.hpp"
#include "crucible/lab/hub.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/market/hub.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"
#include "crucible/util/subprocess.hpp"

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

/// The held-out score, both ways: the base's and the fine-tune's loss and
/// perplexity, how many records were held out, or why none were.
json held_out_json(const lab::HeldOut& held) {
    const auto score = [](const lab::Score& one) {
        return json{{"loss", one.loss}, {"perplexity", one.perplexity}};
    };
    return json{{"records", held.records}, {"base", score(held.base)}, {"tuned", score(held.tuned)},
                {"skipped", held.skipped}, {"measured", held.measured()}};
}

lab::HeldOut held_out_from_json(const json& node) {
    lab::HeldOut held;
    if (!node.is_object()) {
        return held;
    }
    const auto score = [](const json& one) {
        lab::Score out;
        if (one.is_object()) {
            out.loss       = one.value("loss", 0.0);
            out.perplexity = one.value("perplexity", 0.0);
        }
        return out;
    };
    held.records = node.value("records", 0);
    held.base    = score(node.value("base", json::object()));
    held.tuned   = score(node.value("tuned", json::object()));
    held.skipped = node.value("skipped", "");
    return held;
}

/// The memory a fine-tune has to fit in: every card this machine can drive,
/// together. Zero when no GPU runtime is installed, which the page says in
/// words rather than as "needs 6 GB of 0".
std::uint64_t training_memory() {
    // Asked once it has an answer: the cards do not change size, and asking
    // CUDA the first time starts it on every card, which takes long enough
    // to freeze the window. The lookups that open the Create page ask
    // before any action here does.
    static std::atomic<std::uint64_t> known{0};
    if (const std::uint64_t had = known.load(); had > 0) {
        return had;
    }
    std::uint64_t total = 0;
    for (const ComputeDevice& gpu : gpu_devices()) {
        total += gpu.memory_total;
    }
    known.store(total);
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

    const bool condense = recipe.method == lab::Method::Condense;
    json out = json{{"id", recipe.id}, {"name", recipe.name}, {"purpose", recipe.purpose},
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
                {"held_out", held_out_json(recipe.held_out)},
                {"stage", std::string(lab::stage_id(recipe.stage))},
                {"stage_text", condense && recipe.stage == lab::Stage::Training
                                   ? std::string("condensing") : std::string(lab::stage_text(recipe.stage))},
                {"started_at", recipe.started_at}, {"finished_at", recipe.finished_at},
                {"missing", lab::missing(recipe)},
                {"export_bytes", lab::export_bytes(recipe.parameters_b, recipe.quantization)},
                {"fit", fit_json(recipe.parameters_b, recipe.method, memory)}};
    if (condense) {
        // What it is condensed from, and whether what it comes out at runs on
        // the cards: the number the whole recipe is for.
        const std::string source = recipe.base.path.empty() ? recipe.base.id : recipe.base.path;
        out["source_quantization"] = lab::quantization_in_name(std::filesystem::path(source).filename().string());
        const std::uint64_t result = trained_bytes > 0 ? trained_bytes
                                                       : lab::export_bytes(recipe.parameters_b, recipe.quantization);
        out["runs"] = result > 0 ? std::string(market::fit_id(market::fit(result, market::Format::Gguf,
                                                                              market::this_machine())))
                                 : std::string();
    }
    return out;
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
    recipe.held_out      = held_out_from_json(node.value("held_out", json::object()));
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

/// A llama-quantize shipped beside Crucible, which none does today. Not one
/// from the PATH: that is some other llama.cpp, and Crucible quantizes with
/// its own when this is empty (see Trainer::run).
std::filesystem::path quantize_bin() {
    std::error_code ec;
    const std::filesystem::path beside =
        util::executable_path().parent_path() / "llama-quantize";
    return std::filesystem::exists(beside, ec) ? beside : std::filesystem::path{};
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
            return bad("it is training: stop the run first");
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
        {"held_out", held_out_json(run.held_out)},
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

/// What a model on this machine would weigh at each quantization the Create
/// tab offers, whether each would then run on the cards, and which to suggest.
///
/// The sizes are llama.cpp's own, from a dry run of every quantization in a
/// child Crucible (see lab/condense.hpp): exact where an average would be a
/// gigabyte out on a mixture of experts. A few seconds for a large model, so
/// the answer is kept for as long as the file is the same file.
Reply lab_condense_sizes(const json& params, const Scene&) {
    const std::filesystem::path file = paths::expand_user(params.value("path", std::string{}));
    std::error_code ec;
    if (!std::filesystem::is_regular_file(file, ec)) {
        return bad(file.string() + " is not a file on this machine");
    }
    const auto stamp = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    const std::string key = file.string() + "|" + std::to_string(stamp);

    static std::mutex                  cache_mutex;
    static std::map<std::string, json> cache;
    json measured;
    {
        const std::lock_guard<std::mutex> lock(cache_mutex);
        if (const auto found = cache.find(key); found != cache.end()) {
            measured = found->second;
        }
    }
    if (measured.is_null()) {
        std::string types;
        for (const lab::Quantization& q : lab::quantizations()) {
            types += (types.empty() ? "" : ",") + std::string(q.name);
        }
        const util::Ran ran = util::run({util::executable_path().string(), "--quantize-sizes",
                                         file.string(), types},
                                        file.parent_path(), {}, 300);
        if (!ran.started) {
            return bad("could not measure it: " + ran.error);
        }
        // The child's answer is the one line that parses as an object with
        // sizes in it; llama.cpp's warnings may be around it.
        std::istringstream lines(ran.output);
        std::string        line;
        while (std::getline(lines, line)) {
            json parsed = json::parse(line, nullptr, false);
            if (parsed.is_object() && parsed.contains("sizes")) {
                measured = std::move(parsed);
            }
        }
        if (!measured.is_object() || !measured.value("ok", false)) {
            return bad(measured.is_object() ? measured.value("error", std::string("could not read it"))
                                            : std::string("could not measure it"));
        }
        const std::lock_guard<std::mutex> lock(cache_mutex);
        cache[key] = measured;
    }

    const market::Machine machine = market::this_machine();
    lab::Original source;
    source.bits         = measured.value("bits", 0.0);
    source.quantization = measured.value("quantization", std::string());
    source.type         = measured.value("type", std::string());
    json options = json::array();
    int  best = -1;
    int  best_rank = 99;
    for (const lab::Quantization& q : lab::quantizations()) {
        const json size = measured["sizes"].value(q.name, json());
        if (!size.is_number_unsigned() && !size.is_number_integer()) {
            continue;
        }
        const std::uint64_t bytes = size.get<std::uint64_t>();
        const market::Fit   fit   = market::fit(bytes, market::Format::Gguf, machine);
        // By the type, and by what the dry run measured: weights that do not
        // divide into the type's blocks get a larger one, so a 2-bit type can
        // come out nearly as big as the original.
        const double elements = measured.value("elements", 0.0);
        const bool   worth    = lab::worth_condensing(source, q.name)
                           && (elements <= 0.0 || lab::shrinks_enough(source.bits, static_cast<double>(bytes) * 8.0 / elements));
        // The one to suggest: the best that runs on the cards alone, or when
        // none does, the best that runs at all. Best first, so the first of a
        // rank is the one.
        const int rank = !worth ? 99 : fit == market::Fit::Gpu ? 0 : fit == market::Fit::Partial ? 1
                       : fit == market::Fit::Cpu ? 2 : 99;
        if (rank < best_rank) {
            best      = static_cast<int>(options.size());
            best_rank = rank;
        }
        options.push_back(json{{"name", q.name}, {"note", q.note}, {"bytes", bytes},
                               {"fit", std::string(market::fit_id(fit))}, {"worth", worth},
                               {"samples", q.samples}});
    }
    if (best >= 0) {
        options[static_cast<std::size_t>(best)]["recommended"] = true;
    }
    const std::uint64_t now_bytes = measured.value("bytes", std::uint64_t{0});
    return good(json{{"path", file.string()}, {"file", file.filename().string()},
                     {"bytes", now_bytes}, {"bits", source.bits},
                     {"quantization", measured.value("quantization", std::string())},
                     {"elements", measured.value("elements", std::uint64_t{0})},
                     {"fit", std::string(market::fit_id(market::fit(now_bytes, market::Format::Gguf, machine)))},
                     {"options", std::move(options)}});
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
    table.push_back({"lab.condense.sizes", lab_condense_sizes, nullptr});
    table.push_back({"hub.search",    hub_search, nullptr});
}

}  // namespace crucible::api
