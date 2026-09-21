// SPDX-License-Identifier: MIT
#include "crucible/lab/recipe.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <sstream>
#include <system_error>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"

namespace crucible::lab {
namespace {

using json = nlohmann::json;

constexpr std::uint64_t kGiB = 1024ULL * 1024 * 1024;

/// Bytes a parameter costs while being fine-tuned, by method.
///
/// The frozen base is nearly all of it: two bytes a parameter loaded in
/// sixteen bits, half a byte quantized to four, with a little on top for the
/// working copies a trainer keeps. The adapter itself, its gradients and its
/// optimizer moments are a rounding error beside that -- rank sixteen on a 7B
/// is tens of megabytes -- and the share on top stands for the activations,
/// which is where the sequence length goes.
constexpr double kLoraBytesPerParam  = 2.4;
constexpr double kQloraBytesPerParam = 0.75;
constexpr double kActivationShare    = 0.30;
constexpr std::uint64_t kQloraFloor  = 2ULL * kGiB;

Asset asset_from_json(const json& node) {
    Asset asset;
    asset.source = source_from_id(node.value("source", "hub"));
    asset.id     = node.value("id", "");
    asset.file   = node.value("file", "");
    asset.label  = node.value("label", "");
    asset.bytes  = node.value("bytes", std::uint64_t{0});
    asset.path   = node.value("path", "");
    return asset;
}

json asset_to_json(const Asset& asset) {
    json node = json::object();
    node["source"] = std::string(source_id(asset.source));
    node["id"]     = asset.id;
    if (!asset.file.empty())  { node["file"]  = asset.file; }
    if (!asset.label.empty()) { node["label"] = asset.label; }
    if (asset.bytes != 0)     { node["bytes"] = asset.bytes; }
    if (!asset.path.empty())  { node["path"]  = asset.path; }
    return node;
}

}  // namespace

std::string_view source_id(Source source) {
    return source == Source::Local ? "local" : "hub";
}

Source source_from_id(std::string_view id) {
    return id == "local" ? Source::Local : Source::Hub;
}

std::string_view method_id(Method method) {
    return method == Method::Lora ? "lora" : "qlora";
}

Method method_from_id(std::string_view id) {
    // Anything else, including a recipe saved before these were the two, reads
    // as QLoRA: the one that fits on the card most people have.
    return id == "lora" ? Method::Lora : Method::Qlora;
}

std::string_view export_id(Export format) {
    return format == Export::Mlx ? "mlx" : "gguf";
}

Export export_from_id(std::string_view id) {
    return id == "mlx" ? Export::Mlx : Export::Gguf;
}

std::string slug_of(std::string_view name) {
    std::string out;
    out.reserve(name.size());
    for (const char raw : name) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0) {
            out += static_cast<char>(std::tolower(c));
        } else if (!out.empty() && out.back() != '-') {
            out += '-';
        }
    }
    while (!out.empty() && out.back() == '-') {
        out.pop_back();
    }
    return out;
}

std::vector<std::string> missing(const Recipe& recipe) {
    std::vector<std::string> gaps;
    if (slug_of(recipe.name).empty()) {
        gaps.emplace_back("a name");
    }
    if (!recipe.base.chosen()) {
        gaps.emplace_back("a base model to start from");
    }
    if (recipe.data.empty()) {
        gaps.emplace_back("training data");
    }
    return gaps;
}

double bits_per_weight(std::string_view quantization) {
    // The measured averages llama-quantize lands at, not the nominal bit width:
    // a "4-bit" K-quant keeps its scales in higher precision, and a model that
    // came out half a gigabyte over what the tab promised would be the tab's
    // fault.
    if (quantization == "Q2_K")   { return 3.35; }
    if (quantization == "Q3_K_M") { return 3.91; }
    if (quantization == "Q4_0")   { return 4.55; }
    if (quantization == "Q4_K_M") { return 4.85; }
    if (quantization == "Q5_K_M") { return 5.69; }
    if (quantization == "Q6_K")   { return 6.56; }
    if (quantization == "Q8_0")   { return 8.50; }
    if (quantization == "F16")    { return 16.0; }
    if (quantization == "F32")    { return 32.0; }
    return 0.0;
}

std::uint64_t export_bytes(double parameters_b, std::string_view quantization) {
    const double bits = bits_per_weight(quantization);
    if (parameters_b <= 0.0 || bits <= 0.0) {
        return 0;
    }
    const double bytes = parameters_b * 1e9 * bits / 8.0;
    return static_cast<std::uint64_t>(bytes);
}

Fit estimate_fit(double parameters_b, Method method, std::uint64_t vram, std::uint64_t ram) {
    Fit fit;
    // The card is where an adapter trains. The machine's own memory is not a
    // second pool to add to it -- fine-tuning on the processor is slow enough
    // to be a different activity -- so it counts only where there is no GPU at
    // all, which is the honest answer to "could this run here".
    fit.have = vram > 0 ? vram : ram;
    if (parameters_b <= 0.0) {
        fit.note = "pick a base model and this will say whether it fits";
        return fit;
    }

    const double per_param = method == Method::Lora ? kLoraBytesPerParam
                                                    : kQloraBytesPerParam;
    double needed = parameters_b * 1e9 * per_param * (1.0 + kActivationShare);
    if (method == Method::Qlora) {
        needed = std::max(needed, static_cast<double>(kQloraFloor));
    }
    fit.needed   = static_cast<std::uint64_t>(needed);
    fit.possible = fit.have >= fit.needed;

    if (fit.possible) {
        fit.note = method == Method::Lora
                       ? "trains an adapter over the base in sixteen bits"
                       : "trains an adapter over a four-bit base";
        return fit;
    }
    fit.note = method == Method::Lora
                   ? "too big in sixteen bits -- QLoRA needs about a third of this"
                   : "too big for this machine even at four bits -- pick a smaller base";
    return fit;
}

std::string serialize(const Recipe& recipe) {
    json out = json::object();
    out["name"]    = recipe.name;
    out["id"]      = recipe.id.empty() ? slug_of(recipe.name) : recipe.id;
    out["purpose"] = recipe.purpose;
    out["base"]    = asset_to_json(recipe.base);

    json data = json::array();
    for (const Asset& asset : recipe.data) {
        data.push_back(asset_to_json(asset));
    }
    out["data"] = std::move(data);

    json tools = json::array();
    for (const Asset& asset : recipe.tools) {
        tools.push_back(asset_to_json(asset));
    }
    out["tools"] = std::move(tools);

    out["method"]        = std::string(method_id(recipe.method));
    out["format"]        = std::string(export_id(recipe.format));
    out["quantization"]  = recipe.quantization;
    out["parameters_b"]  = recipe.parameters_b;
    out["epochs"]        = recipe.epochs;
    out["context"]       = recipe.context;
    out["learning_rate"] = recipe.learning_rate;
    if (!recipe.trained_path.empty()) {
        out["trained_path"] = recipe.trained_path;
    }
    return out.dump(2);
}

bool parse(std::string_view json_text, Recipe& out, std::string& error) {
    const json parsed = json::parse(json_text, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object()) {
        error = "not a recipe";
        return false;
    }
    Recipe recipe;
    recipe.name    = parsed.value("name", "");
    recipe.id      = parsed.value("id", slug_of(recipe.name));
    recipe.purpose = parsed.value("purpose", "");
    if (parsed.contains("base") && parsed["base"].is_object()) {
        recipe.base = asset_from_json(parsed["base"]);
    }
    if (parsed.contains("data") && parsed["data"].is_array()) {
        for (const json& node : parsed["data"]) {
            recipe.data.push_back(asset_from_json(node));
        }
    }
    if (parsed.contains("tools") && parsed["tools"].is_array()) {
        for (const json& node : parsed["tools"]) {
            recipe.tools.push_back(asset_from_json(node));
        }
    }
    recipe.method        = method_from_id(parsed.value("method", "qlora"));
    recipe.format        = export_from_id(parsed.value("format", "gguf"));
    recipe.quantization  = parsed.value("quantization", "Q4_K_M");
    recipe.parameters_b  = parsed.value("parameters_b", 0.0);
    recipe.epochs        = parsed.value("epochs", 2);
    recipe.context       = parsed.value("context", 512);
    recipe.learning_rate = parsed.value("learning_rate", 1e-5F);
    recipe.trained_path  = parsed.value("trained_path", "");
    out = std::move(recipe);
    return true;
}

std::vector<Made> finished_models() {
    std::vector<Made> made;
    for (const Recipe& recipe : saved_recipes()) {
        if (recipe.trained_path.empty()) {
            continue;
        }
        std::error_code ec;
        const std::filesystem::path path(recipe.trained_path);
        if (!std::filesystem::is_regular_file(path, ec)) {
            continue;
        }
        Made one;
        one.name    = recipe.name;
        one.id      = recipe.id;
        one.purpose = recipe.purpose;
        one.path    = path;
        one.bytes   = std::filesystem::file_size(path, ec);
        if (ec) {
            one.bytes = 0;
        }
        made.push_back(std::move(one));
    }
    return made;
}

std::filesystem::path recipe_file(const Recipe& recipe) {
    const std::string id = recipe.id.empty() ? slug_of(recipe.name) : recipe.id;
    return paths::data_dir() / "lab" / id / "recipe.json";
}

bool save(const Recipe& recipe, std::string& error) {
    if (slug_of(recipe.name).empty()) {
        error = "a recipe needs a name before it can be saved";
        return false;
    }
    const std::filesystem::path file = recipe_file(recipe);
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) {
        error = "could not make " + file.parent_path().string() + ": " + ec.message();
        return false;
    }
    std::ofstream out(file, std::ios::trunc);
    if (!out) {
        error = "could not write " + file.string();
        return false;
    }
    out << serialize(recipe) << '\n';
    return true;
}

std::vector<Recipe> saved_recipes() {
    std::vector<Recipe> found;
    std::error_code ec;
    const std::filesystem::path root = paths::data_dir() / "lab";
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::directory_iterator(root, ec)) {
        if (!entry.is_directory()) {
            continue;
        }
        std::ifstream in(entry.path() / "recipe.json");
        if (!in) {
            continue;
        }
        std::ostringstream buffer;
        buffer << in.rdbuf();
        Recipe      recipe;
        std::string error;
        if (parse(buffer.str(), recipe, error)) {
            found.push_back(std::move(recipe));
        }
    }
    std::sort(found.begin(), found.end(), [](const Recipe& a, const Recipe& b) {
        return a.name < b.name;
    });
    return found;
}

}  // namespace crucible::lab
