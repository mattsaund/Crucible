// SPDX-License-Identifier: MIT
//
// The training environment and the run that uses it.
//
// Nothing here starts a Python or touches the network. What is worth testing
// without either is the part that decides things: which stack a machine
// should get, what goes in it, where it lives, and what the run reports
// while it is going. The rest -- does pip resolve, does torch import -- is
// answered by running it, and the program asks that question itself every
// time it reports its status.
#include "test_helpers.hpp"

#include "crucible/lab/pyenv.hpp"
#include "crucible/lab/trainer.hpp"

#include <algorithm>
#include <string>

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
    CHECK(!trainer.start(recipe, {}, {}, {}, error));
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
