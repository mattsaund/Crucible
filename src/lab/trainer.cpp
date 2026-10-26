// SPDX-License-Identifier: MIT
#include "crucible/lab/trainer.hpp"

#include <algorithm>
#include <fstream>
#include <system_error>
#include <utility>

#include <nlohmann/json.hpp>

#include "crucible/config/paths.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/util/platform.hpp"

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
/// its own words; this is the mapping to the few that have a meaning here.
RunProgress::Phase phase_for(const std::string& name) {
    if (name == "tokenizer" || name == "base" || name == "data") {
        return RunProgress::Phase::Preparing;
    }
    if (name == "training")   { return RunProgress::Phase::Training; }
    if (name == "scoring")    { return RunProgress::Phase::Scoring; }
    if (name == "merging")    { return RunProgress::Phase::Merging; }
    if (name == "converting" || name == "quantizing" || name == "exporting") {
        return RunProgress::Phase::Exporting;
    }
    if (name == "condensing" || name == "sampling") { return RunProgress::Phase::Condensing; }
    return RunProgress::Phase::Preparing;
}

/// Where a run keeps its log and its copy of the recipe. A fine-tune's goes
/// with the training environment; a condense needs none, so it does not make
/// one appear.
std::filesystem::path run_dir_for(const Recipe& recipe) {
    if (recipe.method == Method::Condense) {
        return recipe_file(recipe).parent_path() / "run";
    }
    return pyenv::runs_dir() / recipe.id;
}

/// Beside a run's recipe while it writes a file it has not finished: the
/// file's path, so that what a closed window left half written can be found
/// at the next start. See remove_unfinished.
constexpr const char* kUnfinished = "unfinished";

/// Remove a run's unfinished file, and the note of it. Only a ".part": the
/// path came from the child, and whatever else it might name is not a thing
/// to delete on its say.
void remove_partial(const std::filesystem::path& partial, const std::filesystem::path& note) {
    std::error_code ec;
    if (!partial.empty() && partial.extension() == ".part"
        && std::filesystem::is_regular_file(partial, ec)) {
        std::filesystem::remove(partial, ec);
    }
    std::filesystem::remove(note, ec);
}

}  // namespace

bool read_event(const std::string& line, RunProgress& progress) {
    const json parsed = json::parse(line, nullptr, false);
    if (parsed.is_discarded() || !parsed.is_object() || !parsed.contains("event")) {
        return false;
    }

    const std::string event = parsed.value("event", "");
    if (event == "phase") {
        const std::string name = parsed.value("name", "");
        progress.phase = phase_for(name);
        const std::string detail = parsed.value("detail", "");
        progress.step = detail.empty() ? name : name + ": " + detail;
        if (const std::string partial = parsed.value("partial", ""); !partial.empty()) {
            progress.partial = partial;
        }
    } else if (event == "device") {
        progress.device = parsed.value("name", "");
    } else if (event == "data_ready") {
        progress.records = parsed.value("records", 0LL);
    } else if (event == "adapter") {
        progress.trainable = parsed.value("trainable", 0LL);
    } else if (event == "step") {
        progress.phase      = RunProgress::Phase::Training;
        progress.step_index = parsed.value("step", 0);
        progress.step_total = parsed.value("total", 0);
        progress.loss       = parsed.value("loss", 0.0F);
        progress.epoch      = parsed.value("epoch", 0.0F);
        progress.curve.push_back(progress.loss);
        if (progress.curve.size() > kCurvePoints) {
            // Halve rather than drop the front, so the shape of the whole run
            // survives instead of becoming a window onto its end.
            std::vector<float> thinned;
            thinned.reserve(progress.curve.size() / 2 + 1);
            for (std::size_t i = 0; i < progress.curve.size(); i += 2) {
                thinned.push_back(progress.curve[i]);
            }
            progress.curve = std::move(thinned);
        }
    } else if (event == "progress") {
        // A condense, tensor by tensor: a bar without a loss to draw. The
        // phase is whatever the run last said it was, so a count from
        // anywhere else moves the bar and nothing more.
        progress.step_index = parsed.value("done", 0);
        progress.step_total = parsed.value("total", 0);
    } else if (event == "heldout") {
        // The answers held out of training, scored: the base's before, the
        // fine-tune's after, or why there are none.
        progress.held_out.records = parsed.value("records", progress.held_out.records);
        progress.held_out.skipped = parsed.value("skipped", "");
        Score score;
        score.loss       = parsed.value("loss", 0.0);
        score.perplexity = parsed.value("perplexity", 0.0);
        const std::string of = parsed.value("of", "");
        if (of == "base") {
            progress.held_out.base = score;
        } else if (of == "tuned") {
            progress.held_out.tuned = score;
        }
    } else if (event == "note") {
        progress.notes.push_back(parsed.value("text", ""));
    } else if (event == "done") {
        progress.phase    = RunProgress::Phase::Done;
        progress.produced = std::filesystem::path(parsed.value("path", ""));
        progress.is_gguf  = parsed.value("gguf", false);
    } else if (event == "failed") {
        progress.phase = RunProgress::Phase::Failed;
        progress.error = parsed.value("message", "the run failed");
        progress.hint  = parsed.value("hint", "");
    } else if (event == "canceled") {
        progress.phase = RunProgress::Phase::Canceled;
    }
    return true;
}

bool record_result(const RunProgress& run, Recipe& recipe) {
    if (run.phase != RunProgress::Phase::Done || run.produced.empty()) {
        return false;
    }
    recipe.trained_path = run.produced.string();
    // From this run, whatever the last one said: the score belongs to the
    // file, and a run that held nothing out leaves nothing to compare.
    recipe.held_out    = run.held_out;
    recipe.stage       = Stage::Testing;
    recipe.finished_at = now_seconds();
    return true;
}

void remove_unfinished() {
    // Where a condense keeps its run and where a fine-tune does: run_dir_for.
    std::vector<std::filesystem::path> notes;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(paths::data_dir() / "lab", ec)) {
        notes.push_back(entry.path() / "run" / kUnfinished);
    }
    for (const auto& entry : std::filesystem::directory_iterator(pyenv::runs_dir(), ec)) {
        notes.push_back(entry.path() / kUnfinished);
    }
    for (const std::filesystem::path& note : notes) {
        std::ifstream in(note);
        if (!in) {
            continue;
        }
        std::string partial;
        std::getline(in, partial);
        in.close();
        remove_partial(std::filesystem::path(partial), note);
    }
}

long long RunProgress::seconds_left() const {
    if ((phase != Phase::Training && phase != Phase::Condensing) || step_total <= 0 || step_index <= 0) {
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
        case Phase::Scoring:   return step.empty() ? "scoring" : step;
        case Phase::Merging:   return "folding the adapter in";
        case Phase::Exporting: return step.empty() ? "exporting" : step;
        case Phase::Condensing: return step.empty() ? "condensing" : step;
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
    // Only whether it is there: whether it can train is asked on the run's
    // thread (run), since asking starts Python and imports torch -- a second
    // and more that the window would otherwise be frozen for. Condensing is
    // Crucible itself and needs none of it.
    if (recipe.method != Method::Condense && !pyenv::looks_installed()) {
        error = "the training environment is not installed";
        return false;
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    const std::filesystem::path run_dir = run_dir_for(recipe);
    std::error_code             ec;
    std::filesystem::create_directories(run_dir, ec);
    if (ec) {
        error = "could not make " + run_dir.string() + ": " + ec.message();
        return false;
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_             = RunProgress{};
        progress_.phase       = recipe.method == Method::Condense ? RunProgress::Phase::Preparing
                                                                  : RunProgress::Phase::Starting;
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
/// this is listening. See read_event.
bool Trainer::consume(const std::string& line) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (read_event(line, progress_)) {
        return true;
    }
    // The script starts every event on a line of its own, which leaves a
    // blank one wherever nothing was there before it: nothing to show.
    if (line.find_first_not_of(" \t\r") == std::string::npos) {
        return false;
    }
    log_.push_back(line);
    if (log_.size() > kMaxLogLines) {
        log_.erase(log_.begin(), log_.begin() + static_cast<long>(kMaxLogLines / 4));
    }
    return false;
}

void Trainer::run(Recipe recipe, std::filesystem::path export_dir,
                  std::filesystem::path convert_script,
                  std::filesystem::path quantize_bin) {
    const std::filesystem::path run_dir = run_dir_for(recipe);
    const bool condensing = recipe.method == Method::Condense;

    if (const pyenv::Status env = condensing ? pyenv::Status{} : pyenv::status(); !condensing && !env.ready) {
        const std::lock_guard<std::mutex> lock(mutex_);
        progress_.phase = RunProgress::Phase::Failed;
        progress_.error = env.note.empty() ? "the training environment is not installed"
                                           : "the training environment is not usable: " + env.note;
        if (on_change_) {
            on_change_();
        }
        return;
    }

    // The recipe as the script will read it, beside the run rather than in
    // the lab folder: what a run was given must not change under it because
    // somebody edited the recipe while it trained.
    const std::filesystem::path recipe_file = run_dir / "recipe.json";
    {
        std::ofstream out(recipe_file, std::ios::trunc);
        out << serialize(recipe) << '\n';
    }

    // The script this build carries, put in place for every run. The
    // installer wrote the one of whichever Crucible built the environment,
    // and an update brings a new script without building it again: a run
    // after one would otherwise be the old trainer, without whatever the
    // update changed in it. A build with no script compiled in runs the one
    // that is there.
    if (std::string why; !condensing && !pyenv::write_script(why)) {
        const std::lock_guard<std::mutex> lock(mutex_);
        log_.push_back("the trainer script was not refreshed: " + why);
    }

    // This program, for condensing and for a fine-tune's own quantize step:
    // the llama.cpp it was built with, rather than whatever llama-quantize
    // might be on the PATH, and nothing to install.
    const std::filesystem::path self = util::executable_path();
    std::vector<std::string> argv;
    if (condensing) {
        argv = {self.string(), "--condense",
                "--recipe", recipe_file.string(),
                "--out", run_dir.string(),
                "--export-dir", export_dir.string()};
    } else {
        argv = {pyenv::python().string(), pyenv::script().string(),
                "--recipe", recipe_file.string(),
                "--out",    run_dir.string()};
        if (!convert_script.empty()) {
            argv.emplace_back("--convert-script");
            argv.push_back(convert_script.string());
        }
        if (!quantize_bin.empty()) {
            argv.emplace_back("--quantize-bin");
            argv.push_back(quantize_bin.string());
        } else if (!self.empty()) {
            argv.emplace_back("--quantize-cmd");
            argv.push_back(json::array({self.string(), "--quantize"}).dump());
        }
        if (!export_dir.empty()) {
            argv.emplace_back("--export-dir");
            argv.push_back(export_dir.string());
        }
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

    std::ofstream               log(run_dir / "train.log", std::ios::trunc);
    std::string                 line;
    std::filesystem::path       noted;
    const std::filesystem::path note = run_dir / kUnfinished;
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
        const bool event = consume(line);

        // A file the run has started writing, noted where the next start
        // looks: a window closed now leaves it half written, and nothing
        // else would know whose it was.
        std::filesystem::path partial;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            partial = progress_.partial;
        }
        if (partial != noted) {
            noted = partial;
            std::ofstream(note, std::ios::trunc) << partial.string() << '\n';
        }

        // Redraw on each event, not on each line. A training run prints
        // thousands of lines a minute and the page has one bar; the events
        // are the steps, the tensors of a condense and the phases between.
        if (event && on_change_) {
            on_change_();
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

    // Done renamed the unfinished file into place. Any other ending leaves
    // gigabytes that look nearly like a model, beside the one it was made
    // from: gone before the run says it has ended, so nothing that reads it
    // as over finds the file still there.
    std::filesystem::path partial;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (progress_.phase != RunProgress::Phase::Done) {
            partial = progress_.partial;
        }
    }
    remove_partial(partial, note);

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
            const std::string who = condensing ? "condensing" : "the trainer";
            progress_.error = status == 0
                                  ? who + " stopped without producing anything"
                                  : who + " exited " + std::to_string(status);
        }
    }
    if (on_change_) {
        on_change_();
    }
}

}  // namespace crucible::lab
