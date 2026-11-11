// SPDX-License-Identifier: MIT
//
// An MLX model as the delegator.
//
// The delegator routes by scoring. Each expert's name is weighed as the text
// that follows the prompt (LoadedModel::score_labels), and whether a message
// is answered or built is the same score with two labels. MLX's server only
// generates, and says nothing of how likely a text it is handed is, so it can
// answer for an MLX expert but cannot route. An MLX delegator is answered by a
// helper of its own instead, scripts/mlx/delegator.py, which loads the model
// with mlx-lm and answers format, score and generate.
//
// It runs the way MLX's server does (mlx_server.hpp), on Crucible's Python,
// which has mlx-lm on a Mac, in a process the core starts when the delegator
// is an MLX folder and stops when it changes or is let go. The two speak one
// JSON object a line over its standard input and output, and it keeps the
// instructions it has read between questions, as the core's own scorer does.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/llm/chat_model.hpp"

namespace crucible::util {
class Subprocess;
}

namespace crucible::mlx {

/// One MLX model as the delegator, in a process of its own.
class Delegator {
public:
    Delegator();
    ~Delegator();
    Delegator(const Delegator&)            = delete;
    Delegator& operator=(const Delegator&) = delete;

    /// Load `dir` and return once it can answer. Whatever was loaded before is
    /// stopped first. `cancel` is asked while it loads, and it returns false
    /// with `error` "stopped" when it said to.
    bool start(const std::filesystem::path& dir, const std::function<bool()>& cancel, std::string& error);

    /// Stop it. Safe to call when nothing is loaded.
    void stop();

    /// True while `dir` is the model loaded and its process is up.
    bool serving(const std::filesystem::path& dir) const;

    /// True while any model is loaded.
    bool running() const;

    /// The model loaded, or empty when none is.
    std::filesystem::path served() const;

    /// Whether its chat template reads a reasoning effort, as gpt-oss's does.
    /// See LoadedModel::takes_effort.
    bool takes_effort() const;

    /// `messages` in the model's own chat template, with the turn it answers
    /// opened at the end, as LoadedModel::format_chat renders them. Empty, with
    /// `error`, when it could not.
    std::string format(const std::vector<ChatMessage>& messages, std::string& error);

    /// What LoadedModel::score_labels says of `labels` after `prompt`, a
    /// natural log-probability each, or kUnscored for one that could not be
    /// read, and for all of them when the helper failed or `cancel` stopped
    /// it. `context` is the delegator's, which a longer prompt is cut to.
    std::vector<float> score(const std::string& prompt, const std::vector<std::string>& labels,
                             const std::string& prefix, int context, const std::function<bool()>& cancel);

    /// What the model writes after `prompt`, at most `max_tokens` of it.
    /// Empty, with `error`, when it could not.
    std::string generate(const std::string& prompt, int max_tokens, float temperature,
                         const std::function<bool()>& cancel, std::string& error);

private:
    /// Send `request` and wait for its answer, asking `cancel` while it
    /// waits. A request given up on is answered all the same, later, and
    /// that answer is dropped. Null, with `error`, when there is none.
    nlohmann::json ask(nlohmann::json request, const std::function<bool()>& cancel, std::string& error);

    /// Read what the helper says, for as long as it runs.
    void read_all(util::Subprocess* child);

    /// The last lines it wrote to its log, for saying why it stopped.
    static std::string log_tail();

    mutable std::mutex                    mutex_;
    std::condition_variable               arrived_;
    std::unique_ptr<util::Subprocess>     child_;
    std::thread                           reader_;
    std::filesystem::path                 dir_;
    std::optional<nlohmann::json>         hello_;
    std::map<std::int64_t, nlohmann::json> replies_;
    std::set<std::int64_t>                wanted_;
    std::int64_t                          next_id_      = 1;
    bool                                  ended_        = false;
    bool                                  takes_effort_ = false;
};

/// How the helper is run for `dir`. That is the script compiled into this
/// build, written beside the training environment and run on its Python, or
/// the script CRUCIBLE_MLX_DELEGATOR names, run on Crucible's own Python
/// (CRUCIBLE_PYTHON when that is set), which is how a test stands one in.
/// Empty, with `error`, when MLX cannot run here.
std::vector<std::string> delegator_command(const std::filesystem::path& dir, std::string& error);

}  // namespace crucible::mlx
