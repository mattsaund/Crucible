// SPDX-License-Identifier: MIT
#include "crucible/lab/trainer.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "crucible/lab/pyenv.hpp"

namespace crucible::lab {
namespace {

using json = nlohmann::json;

constexpr std::size_t kMaxLogLines = 4000;
constexpr std::size_t kTailLines   = 24;

/// How much of the curve to keep.
///
/// Enough to see the shape and no more: a run of ten thousand steps logs ten
/// thousand losses, the page draws them into four hundred pixels, and the
/// difference between keeping all of them and keeping the last six hundred
/// is invisible.
constexpr std::size_t kCurvePoints = 600;

/// The phase a script event moves us to. The script names what it is doing in
/// its own words; this is the mapping to the four that have a meaning here.
RunProgress::Phase phase_for(const std::string& name) {
    if (name == "tokenizer" || name == "base" || name == "data") {
        return RunProgress::Phase::Preparing;
    }
    if (name == "training")   { return RunProgress::Phase::Training; }
    if (name == "merging")    { return RunProgress::Phase::Merging; }
    if (name == "converting" || name == "quantizing" || name == "exporting") {
        return RunProgress::Phase::Exporting;
    }
    return RunProgress::Phase::Preparing;
}

}  // namespace

long long RunProgress::seconds_left() const {
    if (phase != Phase::Training || step_total <= 0 || step_index <= 0) {
        return 0;
    }
    const auto now  = std::chrono::steady_clock::now();
    const auto gone = std::chrono::duration_cast<std::chrono::seconds>(now - started).count();
    if (gone < 20 || step_index >= step_total) {
        // Under twenty seconds the rate is dominated by the first step, which
        // includes the compile and the cache warm and is nothing like the
        // rest. Saying nothing beats saying four hours.
        return 0;
    }
    const double rate = static_cast<double>(step_index) / static_cast<double>(gone);
    if (rate <= 0.0) {
        return 0;
    }
    return static_cast<long long>(static_cast<double>(step_total - step_index) / rate);
}

std::string RunProgress::label() const {
    switch (phase) {
        case Phase::Idle:     return "not started";
        case Phase::Starting: return "starting Python";
        case Phase::Preparing:
            return step.empty() ? "preparing" : step;
        case Phase::Training: {
            std::string line = "training";
            if (step_total > 0) {
                line += "  step " + std::to_string(step_index) + " of "
                      + std::to_string(step_total);
            }
            if (loss > 0.0F) {
                char number[16];
                std::snprintf(number, sizeof(number), "%.3f", static_cast<double>(loss));
                line += std::string(", loss ") + number;
            }
            return line;
        }
        case Phase::Merging:   return "folding the adapter in";
        case Phase::Exporting: return step.empty() ? "exporting" : step;
        case Phase::Done:      return "finished";
        case Phase::Failed:    return error.empty() ? "failed" : error;
        case Phase::Canceled:  return "stopped";
    }
    return {};
}

Trainer::~Trainer() { stop(); }

RunProgress Trainer::progress() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return progress_;
}

std::string Trainer::running_id() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return progress_.running() ? progress_.recipe_id : std::string{};
}

bool Trainer::start(const Recipe& recipe,
                    const std::filesystem::path& convert_script,
                    const std::filesystem::path& quantize_bin,
                    const std::filesystem::path& export_dir,
                    std::function<void()> on_change,
                    std::string& error) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (progress_.running()) {
            error = progress_.recipe_name + " is already training";
            return false;
        }
    }
    const pyenv::Status env = pyenv::status();
    if (!env.ready) {
        error = env.note.empty() ? "the training environment is not installed"
                                 : "the training environment is not usable: " + env.note;
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    const std::filesystem::path run_dir = pyenv::runs_dir() / recipe.id;
    std::error_code             ec;
    std::filesystem::create_directories(run_dir, ec);
    if (ec) {
        error = "could not make " + run_dir.string() + ": " + ec.message();
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_             = RunProgress{};
        progress_.phase       = RunProgress::Phase::Starting;
        progress_.recipe_id   = recipe.id;
        progress_.recipe_name = recipe.name;
        progress_.log_file    = run_dir / "train.log";
        progress_.started     = std::chrono::steady_clock::now();
        log_.clear();
    }
    cancel_.store(false);
    on_change_ = std::move(on_change);
    worker_    = std::thread([this, recipe, export_dir, convert_script, quantize_bin]() {
        run(recipe, export_dir, convert_script, quantize_bin);
    });
    return true;
}

void Trainer::cancel() {
    cancel_.store(true);
    const std::lock_guard<std::mutex> lock(child_mutex_);
    if (child_) {
        child_->terminate();
    }
}

void Trainer::stop() {
    cancel();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Trainer::dismiss() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (progress_.running()) {
            return;
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    progress_ = RunProgress{};
}

/// One line from the child.
///
/// Anything that parses as an object with an "event" is progress; everything
/// else is log. That rule is what lets torch, the CUDA loader and the
/// tokenizer all write to the same stream without any of them having to know
/// this is listening.
void Trainer::consume(const std::string& line) {
    const json parsed = json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("event")) {
        const std::lock_guard<std::mutex> lock(mutex_);
        log_.push_back(line);
        if (log_.size() > kMaxLogLines) {
            log_.erase(log_.begin(), log_.begin() + static_cast<long>(kMaxLogLines / 4));
        }
        return;
    }

    const std::string event = parsed.value("event", "");
    const std::lock_guard<std::mutex> lock(mutex_);

    if (event == "phase") {
        const std::string name = parsed.value("name", "");
        progress_.phase = phase_for(name);
        const std::string detail = parsed.value("detail", "");
        progress_.step = detail.empty() ? name : name + ": " + detail;
    } else if (event == "device") {
        progress_.device = parsed.value("name", "");
    } else if (event == "data_ready") {
        progress_.records = parsed.value("records", 0LL);
    } else if (event == "adapter") {
        progress_.trainable = parsed.value("trainable", 0LL);
    } else if (event == "step") {
        progress_.phase      = RunProgress::Phase::Training;
        progress_.step_index = parsed.value("step", 0);
        progress_.step_total = parsed.value("total", 0);
        progress_.loss       = parsed.value("loss", 0.0F);
        progress_.epoch      = parsed.value("epoch", 0.0F);
        progress_.curve.push_back(progress_.loss);
        if (progress_.curve.size() > kCurvePoints) {
            // Halve rather than drop the front, so the shape of the whole run
            // survives instead of becoming a window onto its end.
            std::vector<float> thinned;
            thinned.reserve(progress_.curve.size() / 2 + 1);
            for (std::size_t i = 0; i < progress_.curve.size(); i += 2) {
                thinned.push_back(progress_.curve[i]);
            }
            progress_.curve = std::move(thinned);
        }
    } else if (event == "note") {
        progress_.notes.push_back(parsed.value("text", ""));
    } else if (event == "done") {
        progress_.phase    = RunProgress::Phase::Done;
        progress_.produced = std::filesystem::path(parsed.value("path", ""));
        progress_.is_gguf  = parsed.value("gguf", false);
    } else if (event == "failed") {
        progress_.phase = RunProgress::Phase::Failed;
        progress_.error = parsed.value("message", "the run failed");
        progress_.hint  = parsed.value("hint", "");
    } else if (event == "canceled") {
        progress_.phase = RunProgress::Phase::Canceled;
    }
}

void Trainer::run(Recipe recipe, std::filesystem::path export_dir,
                  std::filesystem::path convert_script,
                  std::filesystem::path quantize_bin) {
    const std::filesystem::path run_dir = pyenv::runs_dir() / recipe.id;

    // The recipe as the script will read it, beside the run rather than in
    // the lab folder: what a run was given must not change under it because
    // somebody edited the recipe while it trained.
    const std::filesystem::path recipe_file = run_dir / "recipe.json";
    {
        std::ofstream out(recipe_file, std::ios::trunc);
        out << serialize(recipe) << '\n';
    }

    std::vector<std::string> argv{
        pyenv::python().string(), pyenv::script().string(),
        "--recipe", recipe_file.string(),
        "--out",    run_dir.string(),
    };
    if (!convert_script.empty()) {
        argv.emplace_back("--convert-script");
        argv.push_back(convert_script.string());
    }
    if (!quantize_bin.empty()) {
        argv.emplace_back("--quantize-bin");
        argv.push_back(quantize_bin.string());
    }
    if (!export_dir.empty()) {
        argv.emplace_back("--export-dir");
        argv.push_back(export_dir.string());
    }

    auto        child = std::make_unique<util::Subprocess>();
    std::string error;
    // HOME is left alone but the caches are not: a fine-tune downloads a base
    // model, and putting that in Crucible's own folder means uninstalling
    // Crucible takes it with it rather than leaving several gigabytes in a
    // dot-directory nobody remembers agreeing to.
    const std::vector<std::string> env{
        "PYTHONUNBUFFERED=1",
        "HF_HOME=" + (pyenv::root() / "huggingface").string(),
        "TOKENIZERS_PARALLELISM=false",
    };
    if (!child->start(argv, run_dir, env, error)) {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = RunProgress::Phase::Failed;
        progress_.error = "could not start the trainer: " + error;
        if (on_change_) {
            on_change_();
        }
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        child_ = std::move(child);
    }

    std::ofstream log(run_dir / "train.log", std::ios::trunc);
    std::string   line;
    int           last_drawn = -1;
    for (;;) {
        util::Subprocess* running = nullptr;
        {
            const std::lock_guard<std::mutex> lock(child_mutex_);
            running = child_.get();
        }
        if (running == nullptr || !running->read_line(line)) {
            break;
        }
        log << line << '\n';
        log.flush();
        consume(line);

        // Redraw on each step and each phase, not on each line. A training
        // run prints thousands of lines a minute and the page has one bar.
        int now = -1;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            now = progress_.phase == RunProgress::Phase::Training
                      ? progress_.step_index
                      : -static_cast<int>(progress_.phase);
        }
        if (now != last_drawn) {
            last_drawn = now;
            if (on_change_) {
                on_change_();
            }
        }
    }

    int status = -1;
    {
        const std::lock_guard<std::mutex> lock(child_mutex_);
        if (child_) {
            status = child_->wait();
            child_.reset();
        }
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.log_tail.assign(
            log_.size() > kTailLines ? log_.end() - static_cast<long>(kTailLines) : log_.begin(),
            log_.end());

        if (cancel_.load()) {
            progress_.phase = RunProgress::Phase::Canceled;
        } else if (progress_.phase != RunProgress::Phase::Done
                   && progress_.phase != RunProgress::Phase::Failed) {
            // It stopped without saying why, which is what a killed process
            // and an interpreter that died on import both look like.
            progress_.phase = RunProgress::Phase::Failed;
            progress_.error = status == 0
                                  ? "the trainer stopped without producing anything"
                                  : "the trainer exited " + std::to_string(status);
        }
    }
    if (on_change_) {
        on_change_();
    }
}

}  // namespace crucible::lab
