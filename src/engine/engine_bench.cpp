// SPDX-License-Identifier: MIT
//
// Benchmarks: models run through the suite, many questions at a time.
//
// The suite itself -- which items, how each is asked, how an answer is
// checked -- is Python, in the orchestrator (bench.py and bench_score.py),
// for the reason the build loop is: it is orchestration, and checking an
// answer means regular expressions, LaTeX and running the code a model wrote,
// each of which Python does in a line. What it asks of the core is what only
// the core has: a model loaded on this machine's cards, its answers, the
// datasets (fetched here, with the system's own certificates, which the
// bundled Python does not have on every platform), and somewhere to publish
// how far it has got and to keep what it found.
//
// The answers come from a BatchChat: the questions the orchestrator queues
// are decoded side by side, as many as the cards hold a cache for, and each
// answer waits to be taken as it is finished.
#include "crucible/engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <deque>
#include <fstream>
#include <iterator>
#include <sstream>
#include <thread>

#include <gguf.h>
#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/lab/condense.hpp"
#include "crucible/llm/batch_chat.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/llm/model_shape.hpp"
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

/// The most questions asked of a model at once. Past a few dozen a card is
/// busy with the arithmetic rather than waiting on reads, and more at once
/// only makes each answer slower.
constexpr int kMostAtOnce = 32;

/// The same for a model on the processor, which runs out of arithmetic
/// sooner.
constexpr int kMostOnProcessor = 8;

/// `messages` with the line a reasoning model reads its effort from, which
/// gpt-oss takes from the system message, as the server and the chat give
/// it (see Config::reasoning_effort). Without it a run at "high" was a run
/// at the model's own default that said "high".
std::vector<ChatMessage> with_effort(std::vector<ChatMessage> messages, const std::string& effort) {
    const std::string line = "Reasoning: " + effort;
    for (ChatMessage& message : messages) {
        if (message.role == "system") {
            message.content += "\n" + line;
            return messages;
        }
    }
    messages.insert(messages.begin(), ChatMessage{"system", line});
    return messages;
}

/// One answer as it arrives: sorted into what it says and the thinking it
/// did, and watched for going in circles.
struct Reply {
    std::string    key;
    std::string    answer;
    std::string    reasoning;
    ResponseFilter filter;
    bool           inline_reasoning = true;

    // Greedy decoding has no way out of a loop: a thinking model that starts
    // saying the same lines over says them until the budget is spent.
    // gpt-oss-20b did on 73 of 450 Quick items, three minutes each, for
    // answers that were wrong either way. Stopped once the text ends in a
    // loop, which changes no score and gives the minutes back.
    bool        looped  = false;
    std::size_t watched = 0;

    void take(std::string_view chunk) {
        if (inline_reasoning) {
            const ResponseFilter::Piece piece = filter.feed(chunk);
            answer += piece.answer;
            reasoning += piece.reasoning;
        } else {
            answer += chunk;
        }
        if (answer.size() + reasoning.size() >= watched + 512) {
            watched = answer.size() + reasoning.size();
            looped  = detail::ends_in_loop(reasoning) || detail::ends_in_loop(answer);
        }
    }

    /// The answer as the orchestrator reads it, from how it ended: what it
    /// says, without the thinking, which nothing checks. Its time is from
    /// when it took a slot: what it waited in the queue was other answers'.
    json done(const ChatResult& result, bool halted) {
        answer += filter.flush().answer;
        const auto ms = static_cast<long long>(result.stats.prompt_ms + result.stats.output_ms);
        return json{{"key", key}, {"answer", answer}, {"error", result.error},
                    // Not answered: a Stop, or the run letting the model go.
                    {"stopped", halted || (result.stats.canceled && !looped)}, {"ms", ms},
                    {"prompt_tokens", result.stats.prompt_tokens},
                    {"output_tokens", result.stats.output_tokens},
                    {"prompt_ms", result.stats.prompt_ms}, {"output_ms", result.stats.output_ms},
                    {"hit_limit", result.stats.hit_limit}, {"looped", looped}};
    }
};

}  // namespace

/// The model under test: the seat that holds it, and the answers it is
/// giving, each kept until the orchestrator takes it. The questions in
/// flight are the batch's, and it goes first: declared last.
struct Engine::BenchModel {
    std::shared_ptr<AgentSeat> seat;
    LoadedModel*               model = nullptr;

    std::mutex              mutex;
    std::condition_variable answered;
    std::deque<json>        answers;

    std::unique_ptr<BatchChat> batch;
};

int Engine::bench_at_once(const ModelParams& model, int context) {
    const ModelShape shape = read_model_shape(model.path);
    if (!shape.known) {
        return 1;
    }
    const bool delegator_stays = config().routing.keep_delegator_loaded;
    // What the cards have free, and what Crucible's own models hold there,
    // which they let go for this one; the delegator keeps its share when it
    // stays loaded.
    std::uint64_t free  = 0;
    std::uint64_t cards = 0;
    for (const ComputeDevice& gpu : gpu_devices()) {
        free += gpu.memory_free;
        ++cards;
    }
    if (cards == 0) {
        // The processor: the caches are the system's memory, which the
        // whole machine shares, so half of it at most.
        std::uint64_t memory = 0;
        for (const ComputeDevice& device : compute_devices()) {
            memory = device.is_gpu ? memory : std::max<std::uint64_t>(memory, device.memory_free);
        }
        const std::uint64_t room = memory / 2 > shape.weights ? memory / 2 - shape.weights : 0;
        return detail::side_by_side(shape, room, context, model.n_batch, kMostOnProcessor);
    }
    {
        const std::lock_guard<std::mutex> host(host_mutex_);
        if (host_ != nullptr) {
            free += host_->resident_bytes();
            if (delegator_stays && host_->router() != nullptr) {
                const std::uint64_t kept = host_->router()->bytes();
                free = free > kept ? free - kept : 0;
            }
        }
    }
    // Less the weights, the buffers every card keeps (the host's own margin
    // for a desktop that moves and the backend's pools), and a twentieth
    // more for those pools growing with the batch. A tenth left gpt-oss-20b
    // at 13.8 GB one at a time on a 16 GB card with room for more.
    const std::uint64_t weights = shape.weights - shape.host_weights;
    const std::uint64_t buffers = shape.compute_bytes(model.n_batch) + cards * (384ULL << 20) + free / 20;
    const std::uint64_t room    = free > weights + buffers ? free - weights - buffers : 0;
    return detail::side_by_side(shape, room, context, model.n_batch, kMostAtOnce);
}

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
        std::shared_ptr<BenchModel> held;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            held = std::move(bench_model_);
        }
        held.reset();   // outside the lock: its batch tells every answer it has ended
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
        std::shared_ptr<BenchModel> before;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            before = std::move(bench_model_);   // the one before goes before the next comes
        }
        before.reset();
        std::error_code ec;
        if (!std::filesystem::is_regular_file(path, ec)) {
            return json{{"ok", false}, {"error", path.string() + " is not there"}};
        }
        // The defaults every seat starts from, so a model is measured the
        // way it would answer here, and the context the suite asks for.
        const Config config  = this->config();
        const int    context = std::max(16, params.value("n_ctx", 8192));
        ModelParams  model   = config.defaults;
        model.model = path.string();
        model.path  = path.string();
        model.n_ctx = std::max(model.n_ctx, context);
        model.provider.clear();
        // The cards to itself: the delegator steps aside the way it does for
        // an expert. See seat.take.
        if (!config.routing.keep_delegator_loaded) {
            release_router_between_routes();
        }
        // As many questions at once as the cards hold a cache for, each with
        // the whole context: a cache of its own for each, and a window on
        // the sliding layers, since no question is ever rewound. Asked of
        // the host the way a load is, and fewer until it says they fit. All
        // on the cards or not at all: caches that pushed layers onto the
        // processor would cost more than they gave, and one at a time is
        // what such a model gets.
        ModelParams many    = model;
        many.own_caches     = true;
        many.window_cache   = true;
        many.gpu_only       = model.n_gpu_layers < 0 && !gpu_devices().empty();
        int at_once         = bench_at_once(model, context);
        while (at_once > 1) {
            many.n_ctx     = at_once * context;
            many.sequences = at_once;
            const std::lock_guard<std::mutex> host(host_mutex_);
            if (host_ == nullptr || host_->could_hold(many, config.routing.keep_delegator_loaded)) {
                break;
            }
            at_once = at_once * 3 / 4;
        }
        auto seat  = std::make_shared<AgentSeat>();
        seat->id   = "bench";
        seat->name = name;
        long        load_ms = 0;
        std::string error;
        state_.set_mood(Mood::Loading, "loading " + name + " for the benchmarks");
        if (wake_) {
            wake_();
        }
        if (at_once > 1) {
            seat->params = many;
            seat->held   = hold_model("bench", many, name, stopped, load_ms, error);
            if (seat->held.model == nullptr && !stopped()) {
                at_once = 1;   // one at a time, as it would load for a chat
                error.clear();
            }
        }
        if (at_once <= 1) {
            at_once      = 1;
            seat->params = model;
            seat->held   = hold_model("bench", model, name, stopped, load_ms, error);
        }
        auto* loaded = dynamic_cast<LoadedModel*>(seat->held.model);
        if (loaded == nullptr) {
            return json{{"ok", false}, {"error", error.empty() ? name + " could not be loaded" : error}};
        }
        auto bench   = std::make_shared<BenchModel>();
        bench->seat  = std::move(seat);
        bench->model = loaded;
        bench->batch = std::make_unique<BatchChat>(*loaded, at_once);
        at_once      = bench->batch->slots();
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench_model_ = std::move(bench);
        }
        state_.set_mood(Mood::Thinking, "benchmarking " + name);
        if (wake_) {
            wake_();
        }
        return json{{"ok", true}, {"load_ms", load_ms}, {"n_ctx", context}, {"at_once", at_once}};
    }
    if (method == "bench.release") {
        std::shared_ptr<BenchModel> held;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            held = std::move(bench_model_);
        }
        return json::object();   // and `held` goes here, with whatever it was still answering
    }

    // --- its answers -----------------------------------------------------
    // Questions are queued with bench.ask and answers taken with
    // bench.answers, in the order they finish. Queued rather than asked one
    // at a time on threads of the orchestrator's: the batch takes them in
    // the order given, which is what makes two runs make the same passes.
    if (method == "bench.ask") {
        std::shared_ptr<BenchModel> bench;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench = bench_model_;
        }
        if (!bench) {
            return json{{"error", "no model is loaded for the benchmarks"}};
        }
        // Greedy and repeatable: the same item asked twice gets the same
        // answer, so two runs of one model agree and a difference between two
        // models is the models'.
        ModelParams asked    = bench->seat->params;
        asked.temperature    = 0.0F;
        asked.top_k          = 1;
        asked.top_p          = 1.0F;
        asked.min_p          = 0.0F;
        asked.repeat_penalty = 1.0F;
        asked.seed           = 1234;
        asked.max_tokens     = std::clamp(params.value("max_tokens", 2048), 16, 32768);
        const std::string effort = params.value("effort", std::string());
        const bool        steer  = !effort.empty() && bench->model->takes_effort();
        const bool        sorted = bench->model->reasons_inline();
        std::vector<BatchChat::Ask> asks;
        for (const json& item : params.value("items", json::array())) {
            std::vector<ChatMessage> messages;
            for (const json& one : item.value("messages", json::array())) {
                messages.push_back(ChatMessage{one.value("role", "user"), one.value("content", "")});
            }
            auto reply              = std::make_shared<Reply>();
            reply->key              = item.value("key", std::string());
            reply->inline_reasoning = sorted;
            BatchChat::Ask ask;
            ask.messages       = steer ? with_effort(std::move(messages), effort) : std::move(messages);
            ask.params         = asked;
            ask.sink.on_text   = [reply](std::string_view chunk) { reply->take(chunk); };
            ask.sink.cancel    = [reply, stopped] { return reply->looped || stopped(); };
            // The bench model outlives its batch, which tells every question
            // it has ended before it goes.
            ask.done = [reply, stopped, model = bench.get()](ChatResult result) {
                json answer = reply->done(result, stopped());
                {
                    const std::lock_guard<std::mutex> lock(model->mutex);
                    model->answers.push_back(std::move(answer));
                }
                model->answered.notify_all();
            };
            asks.push_back(std::move(ask));
        }
        const auto queued = asks.size();
        bench->batch->submit(std::move(asks));
        return json{{"queued", queued}};
    }
    if (method == "bench.answers") {
        std::shared_ptr<BenchModel> bench;
        {
            const std::lock_guard<std::mutex> lock(bench_mutex_);
            bench = bench_model_;
        }
        if (!bench) {
            return json{{"error", "no model is loaded for the benchmarks"}};
        }
        // Whatever has finished, waiting up to `wait` seconds for the first.
        const auto wait = std::chrono::milliseconds(
            static_cast<long long>(std::clamp(params.value("wait", 1.0), 0.0, 10.0) * 1000.0));
        json       out  = json::array();
        {
            std::unique_lock<std::mutex> lock(bench->mutex);
            bench->answered.wait_for(lock, wait, [&] { return !bench->answers.empty() || stopped(); });
            while (!bench->answers.empty()) {
                out.push_back(std::move(bench->answers.front()));
                bench->answers.pop_front();
            }
        }
        return json{{"answers", std::move(out)}, {"stopped", stopped()}};
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
