// SPDX-License-Identifier: MIT
//
// See mlx_delegator.hpp.
#include "crucible/llm/mlx_delegator.hpp"

#include <chrono>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <system_error>
#include <utility>

#include "crucible/config/paths.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/mlx_server.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::mlx {

namespace embedded {
extern const unsigned char kMlxDelegatorPy[];
extern const unsigned int  kMlxDelegatorPy_size;
}  // namespace embedded

namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

/// How long a model may take to load. A large one is gigabytes read from
/// disk, and a slow disk makes that minutes, as for MLX's server.
constexpr auto kStartLimit = std::chrono::minutes(15);

/// How often a wait looks up to see whether it has been told to stop.
constexpr auto kLook = std::chrono::milliseconds(50);

/// Where the helper's standard error goes, with Python's tracebacks and
/// mlx-lm's warnings in it, beside MLX's server's log.
std::filesystem::path log_file() { return paths::data_dir() / "mlx-delegator.log"; }

/// The helper as compiled in, written where it runs. Written every time it is
/// started, so the script is always the one this build expects, as the
/// orchestrator's package is.
std::filesystem::path write_script(std::string& error) {
    const std::filesystem::path script = lab::pyenv::root() / "mlx_delegator.py";
    std::error_code ec;
    std::filesystem::create_directories(script.parent_path(), ec);
    std::ofstream out(script, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(embedded::kMlxDelegatorPy),
              static_cast<std::streamsize>(embedded::kMlxDelegatorPy_size));
    if (!out) {
        error = "could not write " + script.string();
        return {};
    }
    return script;
}

}  // namespace

std::vector<std::string> delegator_command(const std::filesystem::path& dir, std::string& error) {
    // -E and -s, as the orchestrator is started, so nothing from the
    // environment or the user's own packages changes what runs. -B writes no
    // bytecode beside the script, and -u sends each answer when it is written.
    if (const char* stand_in = std::getenv("CRUCIBLE_MLX_DELEGATOR"); stand_in != nullptr && *stand_in != '\0') {
        return {lab::python::interpreter().string(), "-E", "-s", "-B", "-u", stand_in, dir.string()};
    }
    if (std::string why = unavailable(); !why.empty()) {
        error = std::move(why);
        return {};
    }
    const std::filesystem::path script = write_script(error);
    if (script.empty()) {
        return {};
    }
    return {lab::pyenv::python().string(), "-E", "-s", "-B", "-u", script.string(), dir.string()};
}

Delegator::Delegator() = default;

Delegator::~Delegator() { stop(); }

bool Delegator::serving(const std::filesystem::path& dir) const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_ && hello_ && dir_ == dir;
}

bool Delegator::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_ && hello_.has_value();
}

std::filesystem::path Delegator::served() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_ && hello_ ? dir_ : std::filesystem::path();
}

bool Delegator::takes_effort() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return takes_effort_;
}

std::string Delegator::log_tail() {
    std::ifstream in(log_file());
    std::deque<std::string> lines;
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty()) {
            lines.push_back(line);
        }
        if (lines.size() > 8) {
            lines.pop_front();
        }
    }
    std::string out;
    for (const std::string& one : lines) {
        out += (out.empty() ? "" : "\n") + one;
    }
    return out;
}

void Delegator::stop() {
    std::unique_ptr<util::Subprocess> child;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child = std::move(child_);
    }
    if (child) {
        child->terminate();
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    if (child) {
        child->wait();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    dir_.clear();
    hello_.reset();
    replies_.clear();
    wanted_.clear();
    ended_        = false;
    takes_effort_ = false;
    arrived_.notify_all();
}

void Delegator::read_all(util::Subprocess* child) {
    std::string line;
    while (child->read_line(line)) {
        json said = json::parse(line, nullptr, false);
        if (!said.is_object()) {
            continue;   // not the protocol, but a line it should have written to its log
        }
        // The id is read out first. The assignment below moves the reply
        // before its key is worked out, and the id is inside the reply.
        const auto         found = said.find("id");
        const bool         hello = found == said.end();
        const std::int64_t id    = !hello && found->is_number_integer() ? found->get<std::int64_t>() : 0;
        const std::lock_guard<std::mutex> lock(mutex_);
        if (hello) {
            hello_ = std::move(said);
        } else if (wanted_.count(id) != 0) {
            replies_[id] = std::move(said);
        }
        arrived_.notify_all();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    ended_ = true;
    arrived_.notify_all();
}

bool Delegator::start(const std::filesystem::path& dir, const std::function<bool()>& cancel, std::string& error) {
    if (serving(dir)) {
        return true;
    }
    stop();
    const std::vector<std::string> argv = delegator_command(dir, error);
    if (argv.empty()) {
        return false;
    }
    const std::vector<std::string> env{
        "PYTHONUNBUFFERED=1",
        // A local folder, never a download, as for MLX's server.
        "HF_HUB_OFFLINE=1",
        "HF_HOME=" + (lab::pyenv::root() / "huggingface").string(),
    };
    std::error_code ec;
    std::filesystem::create_directories(log_file().parent_path(), ec);
    std::ofstream(log_file(), std::ios::trunc).close();
    util::Subprocess::Streams streams;
    streams.input  = true;
    streams.errors = log_file();
    auto child = std::make_unique<util::Subprocess>();
    if (!child->start(argv, {}, env, error, streams)) {
        error = "MLX could not be started: " + error;
        return false;
    }
    util::Subprocess* raw = child.get();
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child_ = std::move(child);
        dir_   = dir;
    }
    reader_ = std::thread([this, raw] { read_all(raw); });

    // Ready once it says so, which it does after the model is read. Or it
    // says why it could not read it, and ends.
    const std::string name    = dir.filename().string();
    const auto        started = Clock::now();
    std::unique_lock<std::mutex> lock(mutex_);
    while (!hello_ && !ended_) {
        if (cancel && cancel()) {
            lock.unlock();
            stop();
            error = "stopped";
            return false;
        }
        if (Clock::now() - started > kStartLimit) {
            lock.unlock();
            stop();
            error = "MLX took more than fifteen minutes to load " + name;
            return false;
        }
        arrived_.wait_for(lock, kLook);
    }
    if (hello_ && hello_->value("ready", false)) {
        takes_effort_ = hello_->value("takes_effort", false);
        return true;
    }
    std::string why = hello_ ? hello_->value("error", std::string()) : std::string();
    lock.unlock();
    stop();
    if (why.empty()) {
        why = log_tail();
    }
    error = "MLX could not load " + name + (why.empty() ? std::string() : ": " + why);
    return false;
}

json Delegator::ask(json request, const std::function<bool()>& cancel, std::string& error) {
    util::Subprocess* child = nullptr;
    std::int64_t      id    = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!child_ || ended_ || !hello_) {
            error = "the MLX delegator is not running";
            return nullptr;
        }
        child = child_.get();
        id    = next_id_++;
        wanted_.insert(id);
    }
    request["id"] = id;
    if (!child->write_line(request.dump(-1, ' ', false, json::error_handler_t::replace))) {
        const std::lock_guard<std::mutex> lock(mutex_);
        wanted_.erase(id);
        error = "the MLX delegator stopped";
        return nullptr;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        if (const auto found = replies_.find(id); found != replies_.end()) {
            json reply = std::move(found->second);
            replies_.erase(found);
            wanted_.erase(id);
            if (const auto said = reply.find("error"); said != reply.end()) {
                error = said->is_string() ? said->get<std::string>() : said->dump();
                return nullptr;
            }
            return reply;
        }
        if (ended_) {
            wanted_.erase(id);
            lock.unlock();
            const std::string why = log_tail();
            error = "the MLX delegator stopped" + (why.empty() ? std::string() : ": " + why);
            return nullptr;
        }
        if (cancel && cancel()) {
            // Answered all the same, and dropped then. The helper reads one
            // question at a time, and the next waits behind this one.
            wanted_.erase(id);
            error = "stopped";
            return nullptr;
        }
        arrived_.wait_for(lock, kLook);
    }
}

std::string Delegator::format(const std::vector<ChatMessage>& messages, std::string& error) {
    json list = json::array();
    for (const ChatMessage& message : messages) {
        list.push_back(json{{"role", message.role}, {"content", message.content}});
    }
    const json reply = ask(json{{"method", "format"}, {"messages", std::move(list)}}, {}, error);
    if (reply.is_null()) {
        return {};
    }
    return reply.value("text", std::string());
}

std::vector<float> Delegator::score(const std::string& prompt, const std::vector<std::string>& labels,
                                    const std::string& prefix, int context,
                                    const std::function<bool()>& cancel) {
    std::vector<float> scores(labels.size(), kUnscored);
    if (labels.empty()) {
        return scores;
    }
    std::string error;
    const json reply = ask(json{{"method", "score"}, {"prompt", prompt}, {"labels", labels},
                                {"prefix", prefix}, {"context", context}},
                           cancel, error);
    const auto said = reply.is_object() ? reply.find("scores") : reply.end();
    if (said == reply.end() || !said->is_array() || said->size() != labels.size()) {
        return scores;   // a route on some of its labels is a route on a guess
    }
    for (std::size_t i = 0; i < labels.size(); ++i) {
        const json& one = (*said)[i];
        if (one.is_number()) {
            scores[i] = one.get<float>();
        }
    }
    return scores;
}

std::string Delegator::generate(const std::string& prompt, int max_tokens, float temperature,
                                const std::function<bool()>& cancel, std::string& error) {
    const json reply = ask(json{{"method", "generate"}, {"prompt", prompt}, {"max_tokens", max_tokens},
                                {"temperature", temperature}},
                           cancel, error);
    if (reply.is_null()) {
        return {};
    }
    return reply.value("text", std::string());
}

}  // namespace crucible::mlx
