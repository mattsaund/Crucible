// SPDX-License-Identifier: MIT
//
// Benchmarks: starting a run, the results kept on this machine, and the
// public board they can be posted to.
//
// A run is the engine's (engine_bench.cpp) and the orchestrator's (bench.py);
// these are the Bench tab's doors to it. The board is a GitHub repository,
// mattsaund/crucible-bench: a result is posted as an issue there, a GitHub
// Action checks it and adds it to board.json, and every Crucible reads that
// file to put this machine's results beside everybody else's.
//
// The board takes benchmark results and nothing else. Every string it is sent
// is a name of a fixed form, and the model must be a file anybody can fetch
// from Hugging Face, which is looked for here before anything is posted.
#include "methods.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <regex>
#include <set>

#include "crucible/config/paths.hpp"
#include "crucible/lab/recipe.hpp"
#include "crucible/market/hub.hpp"
#include "crucible/tools/git.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::api {
namespace {

constexpr const char* kBoardRepo = "mattsaund/crucible-bench";
constexpr const char* kBoardFile = "https://raw.githubusercontent.com/mattsaund/crucible-bench/main/board.json";
/// The same file through GitHub's API, which answers with what is there now.
/// The raw address is a CDN that keeps a copy five minutes.
constexpr const char* kBoardApi = "https://api.github.com/repos/mattsaund/crucible-bench/contents/board.json";

std::filesystem::path bench_dir() { return paths::data_dir() / "bench"; }

json read_json(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return nullptr;
    }
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return json::parse(text, nullptr, false);
}

/// A result's file, by its id: ids are made by the runner, and a name that
/// could be a path is not one of them.
std::filesystem::path result_file(const std::string& id) {
    const bool plain = !id.empty() && std::all_of(id.begin(), id.end(), [](unsigned char c) {
        return std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.';
    }) && id.find("..") == std::string::npos;
    return plain ? bench_dir() / "results" / (id + ".json") : std::filesystem::path();
}

/// A kept result without the record of every item: what a list needs.
json summary_of(const json& result) {
    json out = result;
    out.erase("items");
    return out;
}

// --- models made here -----------------------------------------------------------
// A fine-tune or a condensed copy from the Create tab is a file nobody else
// has, so it is on nobody's Hugging Face, and the board takes no other kind
// of model. A result of one says what it is, and is never offered to the
// board: the post would only be turned away.

using MadeHere = std::map<std::filesystem::path, std::string>;

/// A path as the file it names, its links followed: the models list and a
/// recipe can spell one file two ways.
std::filesystem::path same_file(const std::string& path) {
    std::error_code             ec;
    const std::filesystem::path file = std::filesystem::weakly_canonical(path, ec);
    return ec ? std::filesystem::path(path) : file;
}

/// Every file the lab has made, and what each is: "fine-tune" or "condensed".
MadeHere made_here() {
    MadeHere made;
    for (const lab::Recipe& recipe : lab::saved_recipes()) {
        if (!recipe.trained_path.empty()) {
            made[same_file(recipe.trained_path)] = recipe.method == lab::Method::Condense ? "condensed" : "fine-tune";
        }
    }
    return made;
}

/// What a result's model is when the lab made it, or empty.
std::string made_of(const json& result, const MadeHere& made) {
    const auto path = result.find("path");
    if (made.empty() || path == result.end() || !path->is_string() || path->get<std::string>().empty()) {
        return {};
    }
    const auto found = made.find(same_file(path->get<std::string>()));
    return found == made.end() ? std::string() : found->second;
}

// --- the board's names --------------------------------------------------------
// The forms the board takes for the strings a result holds, as its accept.py
// checks them. A name of another form is left out of what is posted, so the
// result goes on the board without it rather than being turned away.

/// A GGUF file's name: letters, digits and ._+- only.
bool board_file(const std::string& name) {
    static const std::regex form(R"([A-Za-z0-9][A-Za-z0-9._+-]{0,194}\.gguf)");
    return std::regex_match(name, form);
}

/// An architecture as llama.cpp names it: "llama", "gpt-oss".
bool board_arch(const std::string& name) {
    static const std::regex form(R"([a-z0-9][a-z0-9_.-]{0,31})");
    return std::regex_match(name, form);
}

/// A quantization: "Q4_K_M", "IQ2_XXS", "UD-Q4_K_XL", "MXFP4", "BF16".
bool board_quant(const std::string& name) {
    static const std::regex form(
        R"((?:UD-)?(?:I?Q[1-8](?:_(?:[0-9]|K|M|S|L|P|NL|XS|XL|XXS|XXL)){0,3}|TQ[12]_0|B?F16|F32|MXFP4(?:_MOE)?))");
    return std::regex_match(name, form);
}

bool board_backend(const std::string& name) {
    static const std::set<std::string> known{"CPU", "CUDA", "Metal", "Vulkan", "ROCm", "SYCL"};
    return known.count(name) > 0;
}

/// Whether a model file can be posted: found public on Hugging Face, under a
/// name the board takes.
bool postable(const market::Found& found) { return !found.repo.empty() && board_file(found.file); }

/// What is posted: the result in the board's own format and nothing else. The
/// board refuses a key it does not know, a null where a value goes and a
/// string of any form but the one it expects, so what is not known, or not of
/// that form, is left out rather than sent. The model's own name and the GPUs'
/// names stay here: they are free text, and the board takes none. `found` is
/// where the file is public on Hugging Face, whose name for it is the one
/// posted; without it there is nothing to post. See the board's README.
json posting_of(const json& r, const market::Found& found) {
    const auto copy = [](const json& from, const std::vector<const char*>& keys) {
        json out = json::object();
        for (const char* key : keys) {
            if (from.contains(key) && !from[key].is_null() && !(from[key].is_string() && from[key].get<std::string>().empty())) {
                out[key] = from[key];
            }
        }
        return out;
    };
    const auto name = [](const json& from, const char* key, bool (*form)(const std::string&)) {
        return from.contains(key) && from[key].is_string() && form(from[key].get<std::string>());
    };
    constexpr std::uint64_t kMaxInt = (1ULL << 53) - 1;   // what every reader of board.json holds exactly
    json out = copy(r, {"schema", "suite", "size", "crucible", "llama", "date"});

    const json had = r.value("model", json::object());
    json model = copy(had, {"file", "sha256", "bytes"});
    if (!found.repo.empty()) {
        model["file"] = found.file;
        model["repo"] = found.repo;
    }
    if (name(had, "arch", board_arch)) {
        model["arch"] = had["arch"];
    }
    if (had.contains("params") && had["params"].is_number_unsigned() && had["params"].get<std::uint64_t>() <= kMaxInt) {
        model["params"] = had["params"];
    }
    if (name(had, "quant", board_quant)) {
        model["quant"] = had["quant"];
    }
    out["model"]    = std::move(model);
    out["settings"] = copy(r.value("settings", json::object()), {"context", "max_tokens", "temperature", "effort"});

    // What kind of machine, and how much of it: never what its cards are called.
    const json machine = r.value("hardware", json::object());
    json hardware = json::object();
    if (name(machine, "backend", board_backend)) {
        hardware["backend"] = machine["backend"];
    }
    if (machine.contains("gpu_count") && machine["gpu_count"].is_number_unsigned()) {
        if (machine["gpu_count"].get<std::uint64_t>() <= 64) {
            hardware["gpu_count"] = machine["gpu_count"];
        }
    } else if (machine.contains("gpus") && machine["gpus"].is_array()) {
        hardware["gpu_count"] = machine["gpus"].size();   // a result kept before the count was
    }
    if (machine.contains("memory_gb") && machine["memory_gb"].is_number() && machine["memory_gb"].get<double>() >= 0) {
        hardware["memory_gb"] = machine["memory_gb"];
    }
    if (!hardware.empty()) {
        out["hardware"] = hardware;
    }
    json benchmarks = json::object();
    // Held here: items() of the temporary value() returns would outlive it.
    const json ran = r.value("benchmarks", json::object());
    for (const auto& [id, counts] : ran.items()) {
        if (counts.is_object() && counts.contains("n") && counts.contains("correct")) {
            benchmarks[id] = json{{"n", counts["n"]}, {"correct", counts["correct"]}};
        }
    }
    out["benchmarks"] = std::move(benchmarks);
    const json speed = copy(r.value("speed", json::object()), {"prompt_tps", "gen_tps"});
    if (!speed.empty()) {
        out["speed"] = speed;
    }
    if (r.contains("seconds") && r["seconds"].is_number_integer()) {
        out["seconds"] = r["seconds"];
    }
    return out;
}

/// The repository a model's folder is named for, when the file sits in the
/// models folder as <author>/<repo>/<file>: how the Models tab and LM Studio
/// lay a download out. Empty for a file anywhere else.
std::string repo_of_folder(const std::string& path, const Scene& scene) {
    if (path.empty()) {
        return {};
    }
    std::error_code             ec;
    const std::filesystem::path models = std::filesystem::weakly_canonical(scene.config.resolved_models_dir(), ec);
    const std::filesystem::path file   = std::filesystem::weakly_canonical(path, ec);
    if (models.empty()) {
        return {};
    }
    std::vector<std::string> parts;
    for (const auto& part : file.lexically_relative(models)) {
        parts.push_back(part.string());
    }
    if (parts.size() != 3 || parts[0] == ".." || parts[0] == ".") {
        return {};
    }
    return parts[0] + "/" + parts[1];
}

/// Where a result's model file is public on Hugging Face, looked for by its
/// SHA-256. A file found is kept by its hash, so the dialog opens at once the
/// next time and the post is what the dialog showed; one found nowhere is
/// asked about again, since it may have been uploaded since. False, with
/// `error`, only when Hugging Face could not be asked.
bool where_public(const json& result, const Scene& scene, market::Found& found, std::string& error) {
    static std::mutex mutex;
    found = market::Found{};
    const json          model  = result.value("model", json::object());
    const std::string   sha256 = model.value("sha256", std::string());
    const std::uint64_t bytes  = model.contains("bytes") && model["bytes"].is_number_unsigned()
                               ? model["bytes"].get<std::uint64_t>() : 0;
    if (sha256.size() != 64 || bytes == 0) {
        return true;   // nothing to look for, so nothing to find
    }
    const std::filesystem::path book = bench_dir() / "public.json";
    {
        const std::lock_guard<std::mutex> lock(mutex);
        const json known = read_json(book);
        if (known.is_object() && known.contains(sha256) && known[sha256].is_object()) {
            found.repo = known[sha256].value("repo", std::string());
            found.file = known[sha256].value("file", std::string());
            if (!found.repo.empty() && !found.file.empty()) {
                return true;
            }
            found = market::Found{};
        }
    }
    const std::string hint = repo_of_folder(result.value("path", std::string()), scene);
    if (!market::find_public(hint, model.value("file", std::string()), sha256, bytes, found, error)) {
        return false;
    }
    if (!found.repo.empty()) {
        const std::lock_guard<std::mutex> lock(mutex);
        json known = read_json(book);
        if (!known.is_object()) {
            known = json::object();
        }
        known[sha256] = json{{"repo", found.repo}, {"file", found.file}};
        std::string ignored;
        util::write_file_atomically(book, known.dump(2), ignored);
    }
    return true;
}

/// Whether gh is here and signed in, so a post can go from this machine.
bool gh_signed_in() {
    return tools::git::gh_available() && tools::git::run_gh(paths::data_dir(), {"auth", "status"}, 20).ok;
}

std::string url_escape(std::string_view text) {
    std::string out;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~') {
            out += raw;
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// The methods
// ---------------------------------------------------------------------------

Reply bench_start(const json& params, Host& host) {
    if (host.engine() == nullptr) {
        return bad("the engine is not ready yet");
    }
    json request = json::object();
    json models  = json::array();
    for (const json& one : params.value("models", json::array())) {
        const std::string path = one.value("path", std::string());
        std::error_code ec;
        if (!path.empty() && std::filesystem::is_regular_file(path, ec)) {
            models.push_back(json{{"path", path}, {"name", one.value("name", std::filesystem::path(path).filename().string())}});
        }
    }
    if (models.empty()) {
        return bad("choose a model to test");
    }
    request["models"]     = std::move(models);
    request["benchmarks"] = params.value("benchmarks", json::array());
    request["size"]       = params.value("size", std::string("quick"));
    request["effort"]     = params.value("effort", std::string());
    const std::string refused = host.engine()->start_bench(std::move(request));
    return refused.empty() ? good(json{{"started", true}}) : bad(refused);
}

Reply bench_stop(const json&, Host& host) {
    if (host.engine() != nullptr) {
        host.engine()->stop_bench();
    }
    return good(json{{"stopping", true}});
}

/// Every result kept here, newest first, without their items. One of a model
/// made here says what it is, as `made`.
Reply bench_results(const json&, const Scene&) {
    std::vector<std::pair<std::filesystem::file_time_type, json>> found;
    const MadeHere made = made_here();
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(bench_dir() / "results", ec)) {
        if (entry.path().extension() != ".json") {
            continue;
        }
        json result = read_json(entry.path());
        if (!result.is_object() || result.value("suite", std::string()).empty()) {
            continue;
        }
        json summary = summary_of(result);
        if (const std::string kind = made_of(result, made); !kind.empty()) {
            summary["made"] = kind;
        }
        found.emplace_back(entry.last_write_time(ec), std::move(summary));
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    json out = json::array();
    for (auto& [when, result] : found) {
        out.push_back(std::move(result));
    }
    return good(json{{"results", std::move(out)}});
}

Reply bench_result(const json& params, const Scene&) {
    const std::filesystem::path file = result_file(params.value("id", std::string()));
    json result = file.empty() ? json() : read_json(file);
    if (!result.is_object()) {
        return bad("no such result");
    }
    if (const std::string kind = made_of(result, made_here()); !kind.empty()) {
        result["made"] = kind;
    }
    return good(std::move(result));
}

Reply bench_delete(const json& params, const Scene&) {
    const std::filesystem::path file = result_file(params.value("id", std::string()));
    std::error_code ec;
    if (file.empty() || !std::filesystem::remove(file, ec)) {
        return bad("no such result");
    }
    return good(json{{"deleted", true}});
}

/// The public board, everybody's results. Asked of GitHub at most every ten
/// minutes, and the last copy kept on disk, so the tab opens at once and
/// still shows something offline.
Reply bench_board(const json& params, const Scene&) {
    static std::mutex                            mutex;
    static json                                  cached;
    static std::chrono::steady_clock::time_point fetched{};
    const std::filesystem::path disk = bench_dir() / "board.json";
    const std::lock_guard<std::mutex> lock(mutex);
    const auto now = std::chrono::steady_clock::now();
    const bool fresh = cached.is_object() && now - fetched < std::chrono::minutes(10);
    if (fresh && !params.value("refresh", false)) {
        return good(cached);
    }
    // Refreshed by hand, asked of the API: through the raw address a result
    // posted a minute ago was still not on the board for its poster. The raw
    // address otherwise, which has no rate limit, and if the API says no.
    const bool by_hand = params.value("refresh", false);
    util::http::Response response;
    if (by_hand) {
        util::http::Request asked;
        asked.url             = kBoardApi;
        asked.headers         = {{"Accept", "application/vnd.github.raw+json"}};
        asked.timeout_seconds = 20;
        response = util::http::send(asked);
    }
    if (!by_hand || !response.ok()) {
        util::http::Request request;
        request.url             = kBoardFile;
        request.timeout_seconds = 20;
        response = util::http::send(request);
    }
    json board = response.ok() ? json::parse(response.body, nullptr, false) : json();
    if (board.is_object() && board.contains("entries") && board["entries"].is_array()) {
        std::string ignored;
        util::write_file_atomically(disk, response.body, ignored);
        board["offline"] = false;
        cached  = board;
        fetched = now;
        return good(board);
    }
    // No answer from GitHub: the copy from last time, saying how old it is.
    json kept = read_json(disk);
    if (kept.is_object() && kept.contains("entries")) {
        kept["offline"] = true;
        return good(kept);
    }
    return good(json{{"entries", json::array()}, {"offline", true},
                     {"error", response.ok() ? std::string("the board is not readable") : response.reason()}});
}

/// Post a result to the board: as a GitHub issue, which the board's Action
/// checks and adds. With gh signed in it is posted from here, under the
/// person's own account; without it, GitHub's page for the board's result
/// form opens with the result filled in, for them to post themselves.
Reply bench_post(const json& params, const Scene& scene) {
    const std::filesystem::path file = result_file(params.value("id", std::string()));
    const json result = file.empty() ? json() : read_json(file);
    if (!result.is_object()) {
        return bad("no such result");
    }
    if (const std::string kind = made_of(result, made_here()); !kind.empty()) {
        return bad((kind == "condensed" ? std::string("a condensed copy") : std::string("a fine-tune"))
                   + " made here is not public on Hugging Face, and the board takes no other model");
    }
    market::Found found;
    std::string   error;
    if (!where_public(result, scene, found, error)) {
        return bad(error);
    }
    const json posting = posting_of(result, found);
    if (posting["benchmarks"].empty()) {
        return bad("nothing finished to post: a result needs at least one whole benchmark");
    }
    if (posting["model"].value("sha256", std::string()).size() != 64) {
        return bad("the model file's fingerprint is missing");
    }
    if (!postable(found)) {
        return bad("this model file is not public on Hugging Face, and the board takes no other");
    }
    // The title the board gives an issue it accepts, so accepting it changes nothing.
    const std::string title = "Result: " + found.file + ", " + posting.value("size", std::string("quick"));
    const std::string body  = "<!-- crucible-bench -->\n```json\n" + posting.dump(2) + "\n```\n";

    if (gh_signed_in()) {
        const std::filesystem::path text = bench_dir() / "post.md";
        if (!util::write_file_atomically(text, body, error)) {
            return bad(error);
        }
        const tools::git::Output made = tools::git::run_gh(
            paths::data_dir(), {"issue", "create", "--repo", kBoardRepo, "--title", title, "--body-file", text.string()}, 60);
        std::error_code ec;
        std::filesystem::remove(text, ec);
        if (!made.ok) {
            return bad("GitHub said no: " + (made.error.empty() ? made.text : made.error));
        }
        // gh prints the new issue's address last.
        std::string url = made.text;
        while (!url.empty() && (url.back() == '\n' || url.back() == '\r' || url.back() == ' ')) {
            url.pop_back();
        }
        url = url.substr(url.find_last_of('\n') == std::string::npos ? 0 : url.find_last_of('\n') + 1);
        return good(json{{"posted", true}, {"url", url}});
    }
    // The board's result form, its one field filled from the address. Blank
    // issues are off there, and a prefilled body on its own might never reach
    // a form.
    const std::string url = std::string("https://github.com/") + kBoardRepo + "/issues/new?template=result.yml&title="
                          + url_escape(title) + "&result=" + url_escape(body);
    if (!util::open_in_browser(url, error)) {
        return bad(error);
    }
    return good(json{{"posted", false}, {"opened", true}, {"url", url}});
}

/// Exactly what would be posted, so the dialog that asks can show it, and
/// whether it can be: only a model file public on Hugging Face can. `offline`
/// says Hugging Face could not be asked; `made`, that the model was made
/// here, which nobody is asked about.
Reply bench_posting(const json& params, const Scene& scene) {
    const std::filesystem::path file = result_file(params.value("id", std::string()));
    const json result = file.empty() ? json() : read_json(file);
    if (!result.is_object()) {
        return bad("no such result");
    }
    if (const std::string kind = made_of(result, made_here()); !kind.empty()) {
        return good(json{{"posting", posting_of(result, market::Found{})}, {"repo", kBoardRepo}, {"gh", false},
                         {"public", false}, {"made", kind}});
    }
    market::Found found;
    std::string   error;
    const bool    asked = where_public(result, scene, found, error);
    json out{{"posting", posting_of(result, found)}, {"repo", kBoardRepo}, {"gh", gh_signed_in()},
             {"public", postable(found)}};
    if (!asked) {
        out["offline"] = true;
    }
    return good(std::move(out));
}

}  // namespace

void bench_methods(std::vector<Method>& table) {
    table.push_back({"bench.start",   nullptr, bench_start});
    table.push_back({"bench.stop",    nullptr, bench_stop});
    table.push_back({"bench.results", bench_results, nullptr});
    table.push_back({"bench.result",  bench_result, nullptr});
    table.push_back({"bench.delete",  bench_delete, nullptr});
    table.push_back({"bench.board",   bench_board, nullptr});
    table.push_back({"bench.posting", bench_posting, nullptr});
    table.push_back({"bench.post",    bench_post, nullptr});
}

}  // namespace crucible::api
