// SPDX-License-Identifier: MIT
//
// Benchmarks: models run through the suite, one item at a time.
//
// The suite itself -- which items, how each is asked, how an answer is
// checked -- is Python, in the orchestrator (bench.py and bench_score.py),
// for the reason the build loop is: it is orchestration, and checking an
// answer means regular expressions, LaTeX and running the code a model wrote,
// each of which Python does in a line. What it asks of the core is what only
// the core has: a model loaded on this machine's cards, a reply from it, the
// datasets (fetched here, with the system's own certificates, which the
// bundled Python does not have on every platform), and somewhere to publish
// how far it has got and to keep what it found.
#include "crucible/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include <gguf.h>
#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/lab/condense.hpp"
#include "crucible/llm/response_filter.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/sha256.hpp"
#include "crucible/util/text.hpp"

namespace crucible {
namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

std::filesystem::path bench_dir() { return paths::data_dir() / "bench"; }

/// Text cut to at most `limit` bytes, never through a character: the board
/// counts what it is sent, and half a character is not valid JSON text.
std::string cut(std::string text, std::size_t limit) {
    if (text.size() > limit) {
        while (limit > 0 && (static_cast<unsigned char>(text[limit]) & 0xC0) == 0x80) {
            --limit;
        }
        text.resize(limit);
    }
    return text;
}

std::string read_text(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

/// Written whole and then renamed, so a reader never sees half a file.
bool write_text(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const std::filesystem::path part = file.string() + ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out << text;
        if (!out) {
            return false;
        }
    }
    std::filesystem::rename(part, file, ec);
    return !ec;
}

std::string url_escape(std::string_view text) {
    std::string out;
    for (const char raw : text) {
        const auto c = static_cast<unsigned char>(raw);
        if (std::isalnum(c) != 0 || c == '-' || c == '_' || c == '.' || c == '~' || c == '/') {
            out += raw;
        } else {
            char hex[4];
            std::snprintf(hex, sizeof(hex), "%%%02X", c);
            out += hex;
        }
    }
    return out;
}

/// A folder name for a dataset's cached pages: "openai/gsm8k" to "openai__gsm8k".
std::string cache_name(std::string text) {
    for (char& c : text) {
        if (std::isalnum(static_cast<unsigned char>(c)) == 0 && c != '-' && c != '.') {
            c = '_';
        }
    }
    return text;
}

/// Wait `delay`, a tenth of a second at a time, unless `stopped` says to give
/// up first. Whether it waited it out.
bool pause(std::chrono::milliseconds delay, const CancelCallback& stopped) {
    const auto until = Clock::now() + delay;
    while (!stopped()) {
        const auto now = Clock::now();
        if (now >= until) {
            return true;
        }
        std::this_thread::sleep_for(std::min<Clock::duration>(std::chrono::milliseconds(100), until - now));
    }
    return false;
}

/// A page of a dataset's rows, from the cache or from Hugging Face's datasets
/// server. Kept for good once fetched: the suite pins its items, and a quick
/// run of a second model should not wait on the network for the first's.
///
/// Stopped at once by `stopped`, the request and the waits between tries
/// alike: closing the window waits for whatever this thread is doing.
json dataset_rows(const std::string& dataset, const std::string& config, const std::string& split,
                  int offset, int length, const CancelCallback& stopped, std::string& error) {
    const std::filesystem::path cached = bench_dir() / "rows" / cache_name(dataset + "__" + config + "__" + split)
                                       / (std::to_string(offset) + "-" + std::to_string(length) + ".json");
    std::error_code ec;
    if (std::filesystem::is_regular_file(cached, ec)) {
        json page = json::parse(read_text(cached), nullptr, false);
        if (page.is_object() && page.contains("rows")) {
            return page;
        }
    }
    util::http::Request request;
    request.url = "https://datasets-server.huggingface.co/rows?dataset=" + url_escape(dataset)
                + "&config=" + url_escape(config) + "&split=" + url_escape(split)
                + "&offset=" + std::to_string(offset) + "&length=" + std::to_string(length);
    request.timeout_seconds = 60;
    // The server says "too many requests" to a burst, and a run asks for a
    // few dozen pages; a short wait and another try is what it wants.
    constexpr int kTries = 4;
    for (int attempt = 0; attempt < kTries; ++attempt) {
        const util::http::Response response = util::http::send(request, stopped);
        if (response.ok()) {
            json page = json::parse(response.body, nullptr, false);
            if (page.is_object() && page.contains("rows")) {
                write_text(cached, response.body);
                return page;
            }
            error = "the datasets server sent something that is not rows";
            return {};
        }
        if (stopped()) {
            error = "stopped";
            return {};
        }
        error = dataset + ": " + response.reason();
        if (response.status != 429 && response.status < 500 && response.status != 0) {
            break;
        }
        // No wait after the last try: there is nothing left to wait for.
        if (attempt + 1 < kTries && !pause(std::chrono::seconds(2 << attempt), stopped)) {
            error = "stopped";
            return {};
        }
    }
    return {};
}

/// The SHA-256 of a model file: what says, on the board, exactly which file
/// was run. A minute for a 33 GB model, so kept by path, size and time, and
/// given up on at a Stop. Empty then, and not kept: half a file's digest is
/// nobody's.
std::string model_sha256(const std::filesystem::path& file, const CancelCallback& stopped) {
    std::error_code ec;
    const std::uintmax_t bytes = std::filesystem::file_size(file, ec);
    const auto stamp = std::filesystem::last_write_time(file, ec).time_since_epoch().count();
    const std::filesystem::path book = bench_dir() / "hashes.json";
    json known = json::parse(read_text(book), nullptr, false);
    if (!known.is_object()) {
        known = json::object();
    }
    const std::string key = file.string();
    if (known.contains(key) && known[key].is_object() && known[key].value("bytes", std::uintmax_t{0}) == bytes
        && known[key].value("stamp", std::int64_t{0}) == static_cast<std::int64_t>(stamp)) {
        return known[key].value("sha256", std::string());
    }
    const std::string hash = util::sha256_file(file, stopped);
    if (!hash.empty()) {
        known[key] = json{{"bytes", bytes}, {"stamp", static_cast<std::int64_t>(stamp)}, {"sha256", hash}};
        write_text(book, known.dump(2));
    }
    return hash;
}

/// What a GGUF says it is: its name, its architecture, how many weights,
/// what quantization. The file name's quantization first, as people read it.
json model_facts(const std::filesystem::path& file) {
    json out{{"file", cut(detail::scrub_utf8(file.filename().string()), 200)}};
    const lab::Original now = lab::read_source(file);
    if (now.ok) {
        out["bytes"]  = now.bytes;
        out["params"] = now.elements;
        if (!now.quantization.empty()) {
            out["quant"] = now.quantization;
        }
    }
    gguf_init_params params{};
    params.no_alloc = true;
    params.ctx      = nullptr;
    if (gguf_context* gguf = gguf_init_from_file(file.string().c_str(), params); gguf != nullptr) {
        for (const auto& [key, field] : {std::pair<const char*, const char*>{"general.name", "name"},
                                         {"general.architecture", "arch"}}) {
            const std::int64_t id = gguf_find_key(gguf, key);
            if (id >= 0 && gguf_get_kv_type(gguf, id) == GGUF_TYPE_STRING) {
                out[field] = cut(detail::scrub_utf8(gguf_get_val_str(gguf, id)), 100);
            }
        }
        gguf_free(gguf);
    }
    return out;
}

}  // namespace

std::string Engine::start_bench(json request) {
    if (cooking() || is_busy()) {
        return "wait for the build or the reply to finish: a benchmark wants the cards to itself";
    }
    if (benching_.exchange(true)) {
        return "a benchmark is already running";
    }
    if (bench_thread_.joinable()) {
        bench_thread_.join();
    }
    bench_stop_.store(false, std::memory_order_relaxed);
    bench_thread_ = std::thread([this, request = std::move(request)]() mutable {
        while (!host_ready_.load(std::memory_order_acquire) && running_.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        try {
            do_bench(std::move(request));
        } catch (const std::exception& e) {
            state_.set_mood(Mood::Error, std::string("benchmark failed: ") + e.what());
        } catch (...) {
            state_.set_mood(Mood::Error, "benchmark failed");
        }
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench_seat_.reset();
        }
        benching_.store(false, std::memory_order_relaxed);
        if (wake_) {
            wake_();
        }
        if (!busy_.load(std::memory_order_relaxed) && !cooking()) {
            tidy_models();
        }
    });
    return {};
}

void Engine::stop_bench() {
    bench_stop_.store(true, std::memory_order_relaxed);
    // A run waiting for memory another seat holds is woken to see the stop.
    leases_.wake_all();
}

void Engine::do_bench(json request) {
    const CancelCallback stopped = [this] {
        return bench_stop_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
    };
    state_.set_mood(Mood::Loading, "starting the benchmarks");
    if (wake_) {
        wake_();
    }
    std::string error;
    if (wait_for_python(error, stopped)) {
        try {
            request["context"] = "bench";
            orchestra_.call("bench.run", request);
        } catch (const std::exception& e) {
            error = e.what();
        }
    }
    // However it ended, what is on screen says it has.
    std::shared_ptr<const json> last = state_.snapshot().bench;
    json ended = last ? *last : json::object();
    ended["running"] = false;
    if (!ended.contains("ended")) {
        ended["ended"] = static_cast<std::int64_t>(std::time(nullptr));
    }
    const bool halted = bench_stop_.load(std::memory_order_relaxed);
    if (halted) {
        ended["stopped"] = true;
    }
    if (!error.empty() && error != "stopped" && !halted) {
        ended["error"] = error;
        state_.set_mood(Mood::Error, "benchmarks failed: " + error);
    } else {
        state_.set_mood(Mood::Idle, halted ? "benchmarks stopped" : "benchmarks finished");
    }
    state_.set_bench(std::make_shared<const json>(std::move(ended)));
}

json Engine::serve_bench(const std::string& method, const json& params) {
    const CancelCallback stopped = [this] {
        return bench_stop_.load(std::memory_order_relaxed) || !running_.load(std::memory_order_relaxed);
    };

    if (method == "bench.stopped") {
        return json{{"stopped", stopped()}};
    }

    // --- the model under test --------------------------------------------
    if (method == "bench.load") {
        const std::filesystem::path path = params.value("path", std::string());
        const std::string name = params.value("name", path.filename().string());
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench_seat_.reset();   // the one before goes before the next comes
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return json{{"ok", false}, {"error", path.string() + " is not there"}};
        }
        // The defaults every seat starts from, so a model is measured the
        // way it would answer here, and the context the suite asks for.
        const Config config = this->config();
        ModelParams model = config.defaults;
        model.model = path.string();
        model.path  = path.string();
        model.n_ctx = std::max(model.n_ctx, params.value("n_ctx", 8192));
        model.provider.clear();
        // The cards to itself: the delegator steps aside the way it does for
        // an expert. See seat.take.
        if (!config.routing.keep_delegator_loaded) {
            release_router_between_routes();
        }
        auto seat    = std::make_shared<AgentSeat>();
        seat->id     = "bench";
        seat->name   = name;
        seat->params = model;
        long        load_ms = 0;
        std::string error;
        state_.set_mood(Mood::Loading, "loading " + name + " for the benchmarks");
        if (wake_) {
            wake_();
        }
        seat->held = hold_model("bench", model, name, stopped, load_ms, error);
        if (seat->held.model == nullptr) {
            return json{{"ok", false}, {"error", error.empty() ? name + " could not be loaded" : error}};
        }
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench_seat_ = std::move(seat);
        }
        state_.set_mood(Mood::Thinking, "benchmarking " + name);
        if (wake_) {
            wake_();
        }
        return json{{"ok", true}, {"load_ms", load_ms}, {"n_ctx", model.n_ctx}};
    }
    if (method == "bench.release") {
        const std::lock_guard<std::mutex> lock(bench_mutex_);
        bench_seat_.reset();
        return json::object();
    }
    if (method == "bench.chat") {
        std::shared_ptr<AgentSeat> seat;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            seat = bench_seat_;
        }
        if (!seat || seat->held.model == nullptr) {
            return json{{"error", "no model is loaded for the benchmarks"}};
        }
        std::vector<ChatMessage> messages;
        for (const json& one : params.value("messages", json::array())) {
            messages.push_back(ChatMessage{one.value("role", "user"), one.value("content", "")});
        }
        // Greedy and repeatable: the same item asked twice gets the same
        // answer, so two runs of one model agree and a difference between two
        // models is the models'.
        ModelParams asked    = seat->params;
        asked.temperature    = 0.0F;
        asked.top_k          = 1;
        asked.top_p          = 1.0F;
        asked.min_p          = 0.0F;
        asked.repeat_penalty = 1.0F;
        asked.seed           = 1234;
        asked.max_tokens     = std::clamp(params.value("max_tokens", 2048), 16, 32768);
        const std::string effort = params.value("effort", std::string());

        std::string answer;
        std::string reasoning;
        ResponseFilter filter;
        ChatSink sink;
        sink.cancel = stopped;
        if (seat->held.model->reasons_inline()) {
            sink.on_text = [&](std::string_view chunk) {
                const ResponseFilter::Piece piece = filter.feed(chunk);
                answer += piece.answer;
                reasoning += piece.reasoning;
            };
        } else {
            sink.on_text = [&](std::string_view chunk) { answer += chunk; };
        }
        sink.on_reasoning = [&](std::string_view chunk) { reasoning += chunk; };
        const auto start = Clock::now();
        const ChatResult result = seat->held.model->chat(ChatRequest{messages, asked, effort}, sink);
        const ResponseFilter::Piece tail = filter.flush();
        answer += tail.answer;
        reasoning += tail.reasoning;
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
        return json{{"answer", answer}, {"reasoning", reasoning}, {"error", result.error},
                    {"stopped", stopped()}, {"ms", ms},
                    {"prompt_tokens", result.stats.prompt_tokens},
                    {"prompt_reused", result.stats.prompt_reused},
                    {"output_tokens", result.stats.output_tokens},
                    {"prompt_ms", result.stats.prompt_ms}, {"output_ms", result.stats.output_ms},
                    {"hit_limit", result.stats.hit_limit}};
    }

    // --- what the run needs to know --------------------------------------
    if (method == "bench.rows") {
        std::string error;
        json page = dataset_rows(params.value("dataset", std::string()), params.value("config", std::string()),
                                 params.value("split", std::string()), params.value("offset", 0),
                                 std::clamp(params.value("length", 100), 1, 100), stopped, error);
        if (page.is_null()) {
            return json{{"error", error.empty() ? "could not fetch the rows" : error}};
        }
        return page;
    }
    if (method == "bench.hash") {
        return json{{"sha256", model_sha256(params.value("path", std::string()), stopped)}};
    }
    if (method == "bench.model") {
        return model_facts(params.value("path", std::string()));
    }
    if (method == "bench.machine") {
        // The cards' names are for this machine's own screen; the board is
        // told only how many there are and what kind.
        const std::vector<ComputeDevice> devices = gpu_devices();
        json gpus = json::array();
        std::uint64_t memory = 0;
        std::string   backend;
        for (const ComputeDevice& gpu : devices) {
            if (gpus.size() < 8) {
                gpus.push_back(cut(gpu.description.empty() ? gpu.name : gpu.description, 80));
            }
            memory += gpu.memory_total;
            if (backend.empty()) {
                backend = gpu.backend;
            }
        }
        // ggml registers Metal as "MTL"; the board names it for what it is.
        // CUDA, ROCm, SYCL and Vulkan register under their own names.
        backend = backend == "MTL" ? std::string("Metal") : cut(backend, 20);
        return json{{"crucible", CRUCIBLE_VERSION}, {"llama", CRUCIBLE_LLAMA_TAG},
                    {"backend", backend.empty() ? std::string("CPU") : backend}, {"gpus", gpus},
                    {"gpu_count", devices.size()}, {"memory_gb", static_cast<double>(memory) / 1e9}};
    }

    // --- what it found ---------------------------------------------------
    if (method == "bench.publish") {
        state_.set_bench(std::make_shared<const json>(params.value("run", json::object())));
        if (wake_) {
            wake_();
        }
        return json::object();
    }
    if (method == "bench.save") {
        const json result = params.value("result", json::object());
        std::string id = result.value("id", std::string());
        if (id.empty()) {
            id = std::to_string(std::time(nullptr));
        }
        const std::filesystem::path file = bench_dir() / "results" / (cache_name(id) + ".json");
        if (!write_text(file, result.dump(2, ' ', false, json::error_handler_t::replace))) {
            return json{{"error", "could not save " + file.string()}};
        }
        return json{{"path", file.string()}};
    }

    throw std::runtime_error("the core has no method " + method);
}

}  // namespace crucible
