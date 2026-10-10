// SPDX-License-Identifier: MIT
//
// Running a fine-tune.
//
// The work happens in scripts/trainer/finetune.py, inside the Python
// environment pyenv.hpp puts in place. This starts it, reads what it says,
// and publishes that where the Create tab can draw it.
//
// The script talks in JSON, one object to a line, mixed in with everything
// torch and the CUDA loader write to the same stream. That is deliberate:
// a line that parses as an object with an "event" is progress, and every
// other line is log. Neither side has to be careful about the other, and a
// warning printed by a library in the middle of a training step cannot
// corrupt the progress bar.
//
// A run is a child of this process and ends with it. The same is true of the
// runtime builder next door, and for the same reason -- there is no daemon
// here and adding one to own background work is a larger decision than either
// feature needs. The Create tab says so rather than letting somebody close
// the window on an hour of training.
#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "crucible/lab/recipe.hpp"
#include "crucible/util/subprocess.hpp"

namespace crucible::lab {

/// Where a run has got to. Copied under a lock, a frame at a time.
struct RunProgress {
    enum class Phase {
        Idle,
        Starting,    ///< the interpreter is coming up; torch takes a few seconds
        Preparing,   ///< tokenizer, base model, reading the data
        Training,    ///< the long one, and the only phase with a step count
        Merging,     ///< folding the adapter back into the base
        Exporting,   ///< converting to GGUF and quantizing
        Done,
        Failed,
        Canceled,
    };

    Phase       phase = Phase::Idle;
    std::string recipe_id;
    std::string recipe_name;

    /// What it is doing, in the script's own words.
    std::string step;

    /// Optimizer steps done and expected. Zero until training starts, and the
    /// only honest source of a percentage there is.
    int step_index = 0;
    int step_total = 0;

    /// The last loss, and enough of the curve to draw it.
    ///
    /// Loss is the one number that says whether a run is working rather than
    /// merely progressing: a flat curve after a few hundred steps means the
    /// learning rate is wrong, and that is worth finding out at step 300
    /// rather than at the end.
    float              loss  = 0.0F;
    float              epoch = 0.0F;
    std::vector<float> curve;

    /// What it is training on -- "NVIDIA GeForce RTX 4070" or "cpu".
    std::string device;

    /// How many records the data came to, and how big the adapter is.
    long long records   = 0;
    long long trainable = 0;

    std::string error;
    std::string hint;   ///< what to do about it, when the script knows

    /// What came out, once it has. A GGUF when the export ran, a directory of
    /// safetensors when it could not.
    std::filesystem::path produced;
    bool                  is_gguf = false;

    std::filesystem::path    log_file;
    std::vector<std::string> log_tail;

    /// Anything the script wanted said that is not an error -- "no llama.cpp
    /// converter here, so the result is a model directory".
    std::vector<std::string> notes;

    std::chrono::steady_clock::time_point started{};

    bool finished() const {
        return phase == Phase::Done || phase == Phase::Failed || phase == Phase::Canceled;
    }
    bool running() const { return phase != Phase::Idle && !finished(); }

    /// 0..1 through the training steps, or 0 when there is nothing to go on.
    float percent() const {
        return step_total > 0 ? static_cast<float>(step_index) / static_cast<float>(step_total)
                              : 0.0F;
    }

    /// Roughly how long is left, in seconds, or 0 before there is enough to
    /// say. Measured from this run rather than estimated from the model size:
    /// steps per second on this card with this batch is a thing that can be
    /// counted, and everything else is a guess.
    long long seconds_left() const;

    /// "training  step 120 of 400, loss 1.83" -- one line for the page.
    std::string label() const;
};

/// Runs one fine-tune at a time, on a thread of its own.
class Trainer {
public:
    Trainer() = default;
    ~Trainer();
    Trainer(const Trainer&)            = delete;
    Trainer& operator=(const Trainer&) = delete;

    /// Start `recipe`. `on_change` is called from the worker whenever
    /// progress moved and must be safe off the UI thread. Returns false when
    /// a run is already going, with `error` saying which.
    ///
    /// `convert_script` and `quantize_bin` are llama.cpp's exporter and
    /// quantizer. Both may be empty: the run then stops at a Huggingface
    /// model directory and says so, which is a result, just not a file.
    ///
    /// `export_dir` is where the finished GGUF is put -- the models directory,
    /// so that a fine-tune is picked for a seat like any other model. Empty
    /// leaves it in the run's own folder.
    bool start(const Recipe& recipe,
               const std::filesystem::path& convert_script,
               const std::filesystem::path& quantize_bin,
               const std::filesystem::path& export_dir,
               std::function<void()> on_change,
               std::string& error);

    /// Ask the run to stop. The child is signaled, so a training loop stops
    /// within a step rather than at the end of the epoch.
    void cancel();

    /// Cancel and wait for the worker to be gone. Teardown blocks here.
    void stop();

    /// Forget a finished run, returning to Idle.
    void dismiss();

    RunProgress progress() const;

    /// The slug being trained, or empty. Cheaper than copying the progress
    /// for the list, which asks once a frame per row.
    std::string running_id() const;

private:
    void run(Recipe recipe, std::filesystem::path export_dir, std::filesystem::path convert_script,
             std::filesystem::path quantize_bin);

    /// One line from the child: progress if it parses as an event, log if not.
    void consume(const std::string& line);

    mutable std::mutex       mutex_;
    RunProgress              progress_;
    std::vector<std::string> log_;
    std::thread              worker_;
    std::atomic<bool>        cancel_{false};
    std::function<void()>    on_change_;

    /// Step timing, for seconds_left. Written by the worker only.
    std::chrono::steady_clock::time_point first_step_{};
    int                                   first_step_index_ = 0;

    mutable std::mutex                child_mutex_;
    std::unique_ptr<util::Subprocess> child_;
};

}  // namespace crucible::lab
