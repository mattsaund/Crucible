// SPDX-License-Identifier: MIT
//
// The training environment and the run that uses it.
//
// Nothing here starts a Python or touches the network. What is worth testing
// without either is the part that decides things: which stack a machine
// should get, what goes in it, where it lives, and what the run reports
// while it is going. The rest -- does pip resolve, does torch import -- is
// answered by running it, and the program asks that question itself every
// time it reports its status. Where a run is followed from start to end, a
// shell script stands in for the Python.
#include "test_helpers.hpp"

#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/trainer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using namespace crucible;
using namespace crucible::lab;

namespace {

bool mentions(const std::vector<pyenv::Step>& steps, std::string_view needle) {
    return std::any_of(steps.begin(), steps.end(), [needle](const pyenv::Step& step) {
        return std::any_of(step.packages.begin(), step.packages.end(),
                           [needle](const std::string& package) {
                               return package.find(needle) != std::string::npos;
                           });
    });
}

Recipe named(const std::string& name) {
    Recipe recipe;
    recipe.name = name;
    recipe.id   = slug_of(name);
    return recipe;
}

#if !defined(_WIN32)
/// A training environment whose Python is a shell script: asked whether it
/// can train, it says yes; run, it prints `lines` and does what they say.
/// The run itself is the real one -- the reader, the redraws, the cleanup --
/// with only the child stood in for.
void fake_environment(const std::string& lines) {
    std::error_code ec;
    std::filesystem::create_directories(pyenv::python().parent_path(), ec);
    { std::ofstream(pyenv::script()) << "# put in place by the run\n"; }
    {
        std::ofstream script(pyenv::python());
        script << "#!/bin/sh\n"
                  "if [ \"$1\" = \"-c\" ]; then\n"
                  "  echo '{\"missing\": [], \"torch\": \"0\", \"usable\": [], \"unusable\": [], \"python\": \"3.14.0\"}'\n"
                  "  exit 0\n"
                  "fi\n"
               << lines;
    }
    using std::filesystem::perms;
    std::filesystem::permissions(pyenv::python(), perms::owner_all | perms::group_read | perms::group_exec,
                                 ec);
}

/// Wait for `condition`, ten seconds at most.
template <typename Condition>
bool soon(Condition condition) {
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > until) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return true;
}
#endif

}  // namespace

TEST(every_flavor_survives_the_round_trip_through_its_name) {
    const pyenv::Flavor all[] = {pyenv::Flavor::Cuda, pyenv::Flavor::Cpu,
                                 pyenv::Flavor::Mlx};
    for (const pyenv::Flavor flavor : all) {
        CHECK(pyenv::flavor_from_id(pyenv::flavor_id(flavor)) == flavor);
        CHECK(!std::string(pyenv::flavor_note(flavor)).empty());
        CHECK(pyenv::download_bytes(flavor) > 0);
        CHECK(pyenv::installed_bytes(flavor) >= pyenv::download_bytes(flavor));
    }
    // A name from a newer Crucible, or a hand-edited manifest, reads as the
    // one that works everywhere rather than as the one that needs a card.
    CHECK(pyenv::flavor_from_id("rocm") == pyenv::Flavor::Cpu);
    CHECK(pyenv::flavor_from_id("") == pyenv::Flavor::Cpu);
}

TEST(torch_comes_from_its_own_index_and_everything_else_from_pypi) {
    // One resolve over two indexes lets pip take transformers from the torch
    // mirror, where the version it finds is whatever happened to be vendored.
    // So torch is a step of its own, and it is the only step with an index.
    for (const pyenv::Flavor flavor : {pyenv::Flavor::Cuda, pyenv::Flavor::Cpu}) {
        const std::vector<pyenv::Step> steps = pyenv::plan(flavor);
        CHECK(!steps.empty());
        CHECK(steps.front().packages.front().rfind("torch", 0) == 0);
        CHECK(steps.front().index_url.find("download.pytorch.org") != std::string::npos);
        for (std::size_t i = 1; i < steps.size(); ++i) {
            CHECK(steps[i].index_url.empty());
        }
    }
}

TEST(the_cuda_stack_carries_a_quantizer_and_the_others_do_not) {
    // QLoRA is bitsandbytes, and bitsandbytes is CUDA. Offering it on a
    // processor would be offering a run that takes a week.
    CHECK(mentions(pyenv::plan(pyenv::Flavor::Cuda), "bitsandbytes"));
    CHECK(!mentions(pyenv::plan(pyenv::Flavor::Cpu), "bitsandbytes"));
    CHECK(!mentions(pyenv::plan(pyenv::Flavor::Mlx), "bitsandbytes"));
}

TEST(apple_silicon_gets_mlx_instead_of_torch_and_not_as_well_as) {
    const std::vector<pyenv::Step> steps = pyenv::plan(pyenv::Flavor::Mlx);
    CHECK(mentions(steps, "mlx-lm"));
    CHECK(!mentions(steps, "torch"));
    // ...and peft is torch's adapter library, so it has no business here.
    CHECK(!mentions(steps, "peft"));
}

TEST(everything_installed_is_something_the_check_then_imports) {
    // The two lists drifting apart is how an environment comes to report
    // itself ready while missing the one package the run needs.
    for (const pyenv::Flavor flavor :
         {pyenv::Flavor::Cuda, pyenv::Flavor::Cpu, pyenv::Flavor::Mlx}) {
        const std::vector<pyenv::Step> steps = pyenv::plan(flavor);
        for (const std::string& name : pyenv::imports(flavor)) {
            // mlx_lm installs as mlx-lm, and gguf brings numpy; the import
            // name and the package name are the same for everything else.
            std::string package = name;
            std::replace(package.begin(), package.end(), '_', '-');
            CHECK(mentions(steps, package));
        }
    }
}

TEST(the_whole_environment_lives_under_one_removable_folder) {
    // The promise the settings page makes when it offers to remove it: this
    // is a folder, and nothing was put anywhere else.
    const std::string root = pyenv::root().string();
    CHECK(pyenv::venv_dir().string().rfind(root, 0) == 0);
    CHECK(pyenv::python().string().rfind(root, 0) == 0);
    CHECK(pyenv::script().string().rfind(root, 0) == 0);
    CHECK(pyenv::manifest_file().string().rfind(root, 0) == 0);
    CHECK(pyenv::log_file().string().rfind(root, 0) == 0);
    CHECK(pyenv::runs_dir().string().rfind(root, 0) == 0);
}

TEST(a_machine_with_no_environment_says_so_rather_than_guessing) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    CHECK(!pyenv::looks_installed());
    const pyenv::Status status = pyenv::status();
    CHECK(!status.present);
    CHECK(!status.ready);
    CHECK(!status.note.empty());
}

TEST(a_run_refuses_to_start_without_an_environment_and_says_why) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    Recipe recipe;
    recipe.name = "Bread Science";
    recipe.id   = "bread-science";

    Trainer     trainer;
    std::string error;
    CHECK(!trainer.start(recipe, {}, {}, {}, {}, error));
    CHECK(!error.empty());
    CHECK(trainer.running_id().empty());
}

TEST(progress_reports_a_fraction_only_once_there_is_one_to_report) {
    RunProgress progress;
    CHECK(progress.percent() == 0.0F);
    CHECK(progress.seconds_left() == 0);
    CHECK(!progress.running());
    CHECK(!progress.finished());

    progress.phase      = RunProgress::Phase::Training;
    progress.step_index = 50;
    progress.step_total = 200;
    CHECK(progress.percent() == 0.25F);
    CHECK(progress.running());

    // Started a moment ago: too early for a rate, and an estimate made from
    // the first step would be made from the one step that includes the
    // kernel compile and the cache warm.
    progress.started = std::chrono::steady_clock::now();
    CHECK(progress.seconds_left() == 0);

    // A minute in at a quarter done is three minutes left.
    progress.started = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    const long long left = progress.seconds_left();
    CHECK(left > 150 && left < 200);
}

TEST(a_finished_run_is_finished_whichever_way_it_ended) {
    for (const RunProgress::Phase phase :
         {RunProgress::Phase::Done, RunProgress::Phase::Failed,
          RunProgress::Phase::Canceled}) {
        RunProgress progress;
        progress.phase = phase;
        CHECK(progress.finished());
        CHECK(!progress.running());
        CHECK(!progress.label().empty());
    }
}

TEST(a_failed_run_says_the_error_rather_than_the_word_failed) {
    RunProgress progress;
    progress.phase = RunProgress::Phase::Failed;
    progress.error = "the base model is a GGUF, which cannot be fine-tuned";
    CHECK(progress.label() == progress.error);
}

TEST(a_condense_moves_its_bar_with_every_tensor) {
    // Every tensor is an event, and every event a redraw. Only a training
    // step used to count as one, so the bar and the time left stood at
    // nothing until the whole model was done.
    RunProgress progress;
    progress.phase = RunProgress::Phase::Preparing;
    CHECK(read_event(R"({"event":"phase","name":"condensing","detail":"Q8_0 to Q4_K_M",)"
                     R"("partial":"/m/x-Q4_K_M.gguf.part"})", progress));
    CHECK(progress.phase == RunProgress::Phase::Condensing);
    CHECK(progress.partial == std::filesystem::path("/m/x-Q4_K_M.gguf.part"));
    for (int done = 1; done <= 4; ++done) {
        CHECK(read_event(R"({"event":"progress","done":)" + std::to_string(done) + R"(,"total":8})", progress));
        CHECK_EQ(progress.step_index, done);
    }
    CHECK(progress.percent() == 0.5F);
    progress.started = std::chrono::steady_clock::now() - std::chrono::seconds(60);
    CHECK(progress.seconds_left() > 0);
}

TEST(nothing_a_child_prints_is_taken_for_the_trainers_own_event) {
    // The fine-tune's quantize step. Crucible's quantizer used to say "done"
    // with the file in the run folder, which finished the fine-tune there a
    // moment before the file was moved into the models folder.
    RunProgress progress;
    CHECK(read_event(R"({"event":"phase","name":"quantizing","detail":"Q4_K_M"})", progress));
    CHECK(progress.phase == RunProgress::Phase::Exporting);
    CHECK(!read_event("quantized 77 of 272 tensors", progress));
    CHECK(!read_event(R"(crucible: {"event":"done","path":"/runs/x.Q4_K_M.gguf"})", progress));
    CHECK(!read_event(R"(["event","done"])", progress));
    CHECK(progress.phase == RunProgress::Phase::Exporting);
    CHECK(progress.produced.empty());
    // A count moves the bar and nothing else: it does not make an export a
    // condense.
    CHECK(read_event(R"({"event":"progress","done":1,"total":2})", progress));
    CHECK(progress.phase == RunProgress::Phase::Exporting);
    // The script's own done is the one that counts.
    CHECK(read_event(R"({"event":"done","path":"/models/x-Q4_K_M.gguf","gguf":true})", progress));
    CHECK(progress.phase == RunProgress::Phase::Done);
    CHECK(progress.produced == std::filesystem::path("/models/x-Q4_K_M.gguf"));
}

TEST(the_held_out_score_arrives_with_the_run_and_stays_with_the_file) {
    RunProgress run;
    CHECK(read_event(R"({"event":"phase","name":"scoring","detail":"the base, on 12 held-out records"})", run));
    CHECK(run.phase == RunProgress::Phase::Scoring);
    CHECK(run.label().find("held-out") != std::string::npos);
    CHECK(read_event(R"({"event":"heldout","of":"base","records":12,"tokens":834,"loss":2.31,"perplexity":10.07})",
                     run));
    CHECK_EQ(run.held_out.records, 12);
    CHECK(run.held_out.base.measured());
    CHECK(!run.held_out.measured());
    CHECK(read_event(R"({"event":"heldout","of":"tuned","records":12,"tokens":834,"loss":1.47,"perplexity":4.35})",
                     run));
    CHECK(run.held_out.measured());

    // Not finished, nothing to keep.
    Recipe recipe = named("Kitchen");
    recipe.stage  = Stage::Training;
    CHECK(!record_result(run, recipe));
    CHECK(recipe.trained_path.empty());
    CHECK(!recipe.held_out.measured());

    CHECK(read_event(R"({"event":"done","path":"/models/kitchen-Q4_K_M.gguf","gguf":true})", run));
    CHECK(record_result(run, recipe));
    CHECK_EQ(recipe.trained_path, std::string("/models/kitchen-Q4_K_M.gguf"));
    CHECK(recipe.stage == Stage::Testing);
    CHECK(recipe.finished_at > 0);
    CHECK_EQ(recipe.held_out.base.loss, 2.31);
    CHECK_EQ(recipe.held_out.tuned.loss, 1.47);

    // Trained again on too little to hold any out: it says so, and the old
    // score goes with the old file.
    RunProgress again;
    CHECK(read_event(R"({"event":"heldout","records":0,"skipped":"too little data"})", again));
    CHECK(read_event(R"({"event":"done","path":"/models/kitchen-2-Q4_K_M.gguf","gguf":true})", again));
    CHECK(record_result(again, recipe));
    CHECK(!recipe.held_out.measured());
    CHECK_EQ(recipe.held_out.skipped, std::string("too little data"));
}

TEST(what_a_closed_window_left_half_written_is_removed_at_the_next_start) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    const std::filesystem::path models = temp.path() / "models";
    std::filesystem::create_directories(models);
    const std::filesystem::path partial  = models / "flint-Q4_K_M.gguf.part";
    const std::filesystem::path download = models / "other-Q8_0.gguf.part";
    const std::filesystem::path model    = models / "flint-Q6_K.gguf";
    for (const std::filesystem::path& file : {partial, download, model}) {
        std::ofstream(file) << "gigabytes";
    }
    // The note a condense keeps beside its recipe while it writes, and one
    // that names something other than a part file, which no note can remove.
    const std::filesystem::path note  = paths::data_dir() / "lab" / "flint-small" / "run" / "unfinished";
    const std::filesystem::path wrong = paths::data_dir() / "lab" / "odd" / "run" / "unfinished";
    for (const auto& [where, what] : {std::pair{note, partial}, std::pair{wrong, model}}) {
        std::filesystem::create_directories(where.parent_path());
        std::ofstream(where) << what.string() << '\n';
    }

    remove_unfinished();
    std::error_code ec;
    CHECK(!std::filesystem::exists(partial, ec));
    CHECK(!std::filesystem::exists(note, ec));
    // A download's part file is carried on from by the next try, and the
    // model is a model.
    CHECK(std::filesystem::exists(download, ec));
    CHECK(std::filesystem::exists(model, ec));
    CHECK(!std::filesystem::exists(wrong, ec));
}

#if !defined(_WIN32)
TEST(a_run_is_read_to_the_end_and_redrawn_at_every_event) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    const std::string made = (temp.path() / "models" / "kitchen-Q4_K_M.gguf").string();
    fake_environment(
        "echo '{\"event\": \"phase\", \"name\": \"scoring\", \"detail\": \"the base\"}'\n"
        "echo '{\"event\": \"heldout\", \"of\": \"base\", \"records\": 3, \"loss\": 2.5, \"perplexity\": 12.18}'\n"
        // A bar drawn on standard error without ending its line, and the
        // step after it on a line of its own, as the script writes them.
        "printf ' 50%%|#####     | 1/2' >&2\n"
        "printf '\\n{\"event\": \"step\", \"step\": 1, \"total\": 2, \"loss\": 2.0}\\n'\n"
        "echo 'some warning from torch'\n"
        "printf '\\n{\"event\": \"step\", \"step\": 2, \"total\": 2, \"loss\": 1.5}\\n'\n"
        "echo '{\"event\": \"heldout\", \"of\": \"tuned\", \"records\": 3, \"loss\": 1.5, \"perplexity\": 4.48}'\n"
        "echo '{\"event\": \"phase\", \"name\": \"quantizing\", \"detail\": \"Q4_K_M\"}'\n"
        "echo '{\"event\": \"progress\", \"done\": 1, \"total\": 2}'\n"
        "echo '{\"event\": \"progress\", \"done\": 2, \"total\": 2}'\n"
        "echo 'quantized 2 of 2 tensors'\n"
        "echo 'crucible: {\"event\": \"done\", \"path\": \"/runs/kitchen.Q4_K_M.gguf\"}'\n"
        "echo '{\"event\": \"done\", \"path\": \"" + made + "\", \"gguf\": true}'\n");

    Trainer          trainer;
    std::atomic<int> redraws{0};
    std::string      error;
    CHECK(trainer.start(named("Kitchen"), {}, {}, temp.path() / "models", [&redraws] { ++redraws; }, error));
    // Nine events and the ending, a tensor's count among them; the log lines
    // are not redraws. The ending is the last thing a run does, so once it
    // has been drawn the run is over.
    CHECK(soon([&redraws] { return redraws.load() >= 10; }));
    CHECK_EQ(redraws.load(), 10);
    const RunProgress run = trainer.progress();
    CHECK(run.phase == RunProgress::Phase::Done);
    CHECK_EQ(run.produced.string(), made);
    CHECK(run.held_out.measured());
    CHECK_EQ(run.step_total, 2);
    CHECK_EQ(run.step_index, 2);
    CHECK(std::find(run.log_tail.begin(), run.log_tail.end(), "some warning from torch") != run.log_tail.end());
    CHECK(std::find(run.log_tail.begin(), run.log_tail.end(), " 50%|#####     | 1/2") != run.log_tail.end());
    // The blank line before each event is nothing to show.
    CHECK(std::find(run.log_tail.begin(), run.log_tail.end(), "") == run.log_tail.end());
    // And the run used the script this build carries, not the one it found.
    std::ifstream script(pyenv::script());
    std::string   first;
    std::getline(script, first);
    CHECK(first.find("put in place by the run") == std::string::npos);
}

TEST(a_stopped_run_takes_its_unfinished_file_with_it) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());
    const std::filesystem::path partial = temp.path() / "models" / "flint-Q4_K_M.gguf.part";
    std::filesystem::create_directories(partial.parent_path());
    { std::ofstream(partial) << "half a model"; }
    fake_environment("echo '{\"event\": \"phase\", \"name\": \"condensing\", \"partial\": \"" + partial.string()
                     + "\"}'\nexec sleep 30\n");

    Trainer     trainer;
    std::string error;
    const Recipe recipe = named("Flint small");
    CHECK(trainer.start(recipe, {}, {}, temp.path() / "models", {}, error));
    // Noted where the next start would look, while it is written.
    const std::filesystem::path note = pyenv::runs_dir() / recipe.id / "unfinished";
    std::error_code ec;
    CHECK(soon([&note] { std::error_code missing; return std::filesystem::exists(note, missing); }));

    trainer.cancel();
    CHECK(soon([&trainer] { return trainer.progress().finished(); }));
    CHECK(trainer.progress().phase == RunProgress::Phase::Canceled);
    CHECK(!std::filesystem::exists(partial, ec));
    CHECK(!std::filesystem::exists(note, ec));
}
#endif
