// SPDX-License-Identifier: MIT
//
// An MLX model as the delegator, from the core's side.
//
// The helper the core starts is scripts/mlx/delegator.py, which needs MLX and
// a model and is tested with them in scripts/mlx/tests. Here a stand-in that
// speaks the same lines answers instead (tests/scripted_delegator.py), so
// what the core does with the answers is tested on every system. That is the
// scores it hands the orchestrator, a question given up on, and a helper that
// could not load or that went away. It runs on a Python from CRUCIBLE_PYTHON
// or PATH, and these are skipped without one.
#include "test_helpers.hpp"

#include <chrono>
#include <fstream>

#include <nlohmann/json.hpp>

#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/mlx_delegator.hpp"
#include "crucible/llm/mlx_server.hpp"
#include "crucible/util/subprocess.hpp"

using namespace crucible;
using json = nlohmann::json;

namespace {

std::string stand_in_python() {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return chosen;
    }
    for (const char* name : {"python3", "python"}) {
        if (util::on_path(name)) {
            return name;
        }
    }
    return {};
}

/// An environment variable set for as long as this lives.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const std::string& value) : name_(name) {
        if (const char* now = std::getenv(name); now != nullptr) {
            previous_ = now;
            had_      = true;
        }
        set_env(name, value);
    }
    ~ScopedEnv() {
        if (had_) {
            set_env(name_, previous_);
        } else {
            unset_env(name_);
        }
    }
    ScopedEnv(const ScopedEnv&)            = delete;
    ScopedEnv& operator=(const ScopedEnv&) = delete;

private:
    const char* name_;
    std::string previous_;
    bool        had_ = false;
};

/// The stand-in as the helper, on a Python, with Crucible's data in `dir`.
struct StandIn {
    explicit StandIn(const TempDir& dir)
        : python(stand_in_python()),
          data(dir.path() / "data"),
          chosen("CRUCIBLE_PYTHON", python),
          helper("CRUCIBLE_MLX_DELEGATOR",
                 (std::filesystem::path(CRUCIBLE_SOURCE_DIR) / "tests" / "scripted_delegator.py").string()) {}

    bool ok() const { return !python.empty(); }

    std::string    python;
    ScopedDataHome data;
    ScopedEnv      chosen;
    ScopedEnv      helper;
};

/// A folder an MLX model would be, holding what the stand-in is to do.
std::filesystem::path fake_model(const std::filesystem::path& dir, const json& script) {
    const std::filesystem::path folder = dir / "Tiny-MLX";
    std::filesystem::create_directories(folder);
    std::ofstream(folder / "config.json") << "{}";
    std::ofstream(folder / "model.safetensors") << "weights";
    std::ofstream(folder / "scripted.json") << script.dump();
    return folder;
}

/// What the stand-in was asked, in order.
std::vector<json> asked(const std::filesystem::path& folder) {
    std::vector<json> out;
    std::ifstream in(folder / "requests.jsonl");
    std::string line;
    while (std::getline(in, line)) {
        out.push_back(json::parse(line));
    }
    return out;
}

void skipped() { std::printf("      (no Python to run the stand-in on; skipped)\n"); }

}  // namespace

TEST(an_mlx_delegator_formats_scores_and_writes_through_its_helper) {
    TempDir dir;
    StandIn stand_in(dir);
    if (!stand_in.ok()) {
        skipped();
        return;
    }
    const std::filesystem::path model =
        fake_model(dir.path(), {{"scores", {{"Writing", -0.25}, {"Answer", -0.5}}}, {"otherwise", -4.0},
                                {"takes_effort", true}, {"written", "Rain on the roof"}});
    CHECK(mlx::is_model_dir(model));

    mlx::Delegator delegator;
    std::string    error;
    CHECK(delegator.start(model, {}, error));
    CHECK_EQ(error, std::string());
    CHECK(delegator.running());
    CHECK(delegator.serving(model));
    CHECK_EQ(delegator.served(), model);
    CHECK(delegator.takes_effort());
    // Asked again for the same model, nothing is started again.
    CHECK(delegator.start(model, {}, error));

    const std::string text = delegator.format({{"system", "Route it."}, {"user", "hi"}}, error);
    CHECK_EQ(text, std::string("<system>Route it.<user>hi<assistant>"));
    CHECK_EQ(error, std::string());

    // As score_labels says them, a log-probability each, and kUnscored for a
    // label that could not be read.
    const std::vector<float> scores = delegator.score("the prompt", {"Writing", "Programming", ""}, "the", 4096, {});
    CHECK_EQ(scores.size(), std::size_t{3});
    if (scores.size() == 3) {
        CHECK(scores[0] > -0.26F && scores[0] < -0.24F);
        CHECK(scores[1] > -4.01F && scores[1] < -3.99F);
        CHECK(scores[2] <= kUnscored);
    }
    CHECK(delegator.score("the prompt", {}, "", 4096, {}).empty());

    CHECK_EQ(delegator.generate("<user>name this", 16, 0.2F, {}, error), std::string("Rain on the roof"));

    // What it was sent, with the prompt, the labels, the prefix kept between
    // questions, and the delegator's context, which a longer prompt is cut to.
    const std::vector<json> requests = asked(model);
    CHECK_EQ(requests.size(), std::size_t{3});
    if (requests.size() == 3) {
        CHECK_EQ(requests[1]["method"].get<std::string>(), std::string("score"));
        CHECK_EQ(requests[1]["prefix"].get<std::string>(), std::string("the"));
        CHECK_EQ(requests[1]["context"].get<int>(), 4096);
        CHECK_EQ(requests[2]["max_tokens"].get<int>(), 16);
    }

    delegator.stop();
    CHECK(!delegator.running());
    CHECK(delegator.served().empty());
    CHECK(delegator.format({{"user", "hi"}}, error).empty());
    CHECK(!error.empty());
}

TEST(a_score_given_up_on_comes_back_at_once_and_the_next_is_answered) {
    TempDir dir;
    StandIn stand_in(dir);
    if (!stand_in.ok()) {
        skipped();
        return;
    }
    // The first score takes two seconds, and Stop is pressed a moment in.
    const std::filesystem::path model = fake_model(dir.path(), {{"scores", {{"Math", -1.0}}}, {"pause", 2.0}});
    mlx::Delegator delegator;
    std::string    error;
    CHECK(delegator.start(model, {}, error));

    const auto started = std::chrono::steady_clock::now();
    const auto stopped = [&started] {
        return std::chrono::steady_clock::now() - started > std::chrono::milliseconds(150);
    };
    const std::vector<float> given_up = delegator.score("prompt", {"Math", "Writing"}, "", 4096, stopped);
    const auto waited = std::chrono::steady_clock::now() - started;
    CHECK(waited < std::chrono::milliseconds(1500));
    CHECK_EQ(given_up.size(), std::size_t{2});
    CHECK(given_up.size() == 2 && given_up[0] <= kUnscored && given_up[1] <= kUnscored);

    // The helper finishes the first and answers the next, whose answer is not
    // the first one's arriving late.
    const std::vector<float> next = delegator.score("prompt", {"Writing", "Math"}, "", 4096, {});
    CHECK(next.size() == 2 && next[0] > -5.01F && next[0] < -4.99F && next[1] > -1.01F && next[1] < -0.99F);
    CHECK(delegator.running());
}

TEST(a_helper_that_cannot_load_its_model_says_why) {
    TempDir dir;
    StandIn stand_in(dir);
    if (!stand_in.ok()) {
        skipped();
        return;
    }
    const std::filesystem::path model = fake_model(dir.path(), {{"fail", "no weights in it"}});
    mlx::Delegator delegator;
    std::string    error;
    CHECK(!delegator.start(model, {}, error));
    CHECK(error.find("Tiny-MLX") != std::string::npos);
    CHECK(error.find("no weights in it") != std::string::npos);
    CHECK(!delegator.running());
}

TEST(a_helper_that_ends_mid_question_is_noticed_rather_than_waited_on) {
    TempDir dir;
    StandIn stand_in(dir);
    if (!stand_in.ok()) {
        skipped();
        return;
    }
    const std::filesystem::path model = fake_model(dir.path(), {{"die_on", "score"}});
    mlx::Delegator delegator;
    std::string    error;
    CHECK(delegator.start(model, {}, error));
    const std::vector<float> scores = delegator.score("prompt", {"Math"}, "", 4096, {});
    CHECK(scores.size() == 1 && scores[0] <= kUnscored);
    CHECK(!delegator.running());
    CHECK(delegator.generate("prompt", 8, 0.2F, {}, error).empty());
    CHECK(!error.empty());
}

TEST(without_mlx_the_helper_is_not_started_and_says_why) {
    // No stand-in is named and no training environment is in this data
    // folder, so the real helper has nothing to run on, here or on a machine
    // without MLX.
    TempDir              dir;
    const ScopedDataHome data(dir.path());
    const ScopedEnv      none("CRUCIBLE_MLX_DELEGATOR", "");
    std::string          error;
    CHECK(mlx::delegator_command(dir.path() / "Tiny-MLX", error).empty());
    CHECK(!error.empty());
}
