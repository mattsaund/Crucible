// SPDX-License-Identifier: MIT
//
// The two things Crucible installs for itself: a runtime to drive the
// hardware, and the Python environment the lab trains in.
//
// Both are long jobs with the same three verbs -- start, cancel, and forget
// the result -- and they are three methods rather than one with a verb
// argument because they are three different things to be allowed to do.
//
// Nothing here needs an engine. Compiling a runtime or fetching the trainer's
// Python has nothing to do with whether a model is loaded, and the moment you
// most need to do either is the one where nothing can run yet.
#include "methods.hpp"

#include <mutex>

#include "crucible/config/paths.hpp"
#include "crucible/lab/pyenv.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/platform.hpp"

namespace crucible::api {
namespace {

// ---------------------------------------------------------------------------
// Progress, in the shape the page draws
// ---------------------------------------------------------------------------

/// The two installers are different jobs with the same story: a phase, how
/// far along, what it is doing right now, and -- when it failed -- enough of
/// the log to act on without opening the file.
template <typename Progress>
json progress_json(const Progress& progress, std::string_view phase) {
    return json{
        {"phase",    std::string(phase)},
        {"running",  progress.running()},
        {"finished", progress.finished()},
        {"percent",  progress.percent},
        {"step",     progress.step},
        {"label",    progress.label()},
        {"error",    progress.error},
        {"log",      progress.log_tail},
        {"log_file", progress.log_file.string()},
    };
}

const json kIdle = json{{"running", false}, {"finished", false}, {"phase", "idle"}};

std::string_view phase_name(BuildProgress::Phase phase) {
    switch (phase) {
        case BuildProgress::Phase::Idle:           return "idle";
        case BuildProgress::Phase::FetchingSource: return "fetching";
        case BuildProgress::Phase::Configuring:    return "configuring";
        case BuildProgress::Phase::Compiling:      return "compiling";
        case BuildProgress::Phase::Installing:     return "installing";
        case BuildProgress::Phase::Done:           return "done";
        case BuildProgress::Phase::Failed:         return "failed";
        case BuildProgress::Phase::Canceled:       return "canceled";
    }
    return "idle";
}

std::string_view phase_name(lab::pyenv::Progress::Phase phase) {
    switch (phase) {
        case lab::pyenv::Progress::Phase::Idle:          return "idle";
        case lab::pyenv::Progress::Phase::FindingPython: return "finding python";
        case lab::pyenv::Progress::Phase::CreatingVenv:  return "creating venv";
        case lab::pyenv::Progress::Phase::Installing:    return "installing";
        case lab::pyenv::Progress::Phase::Verifying:     return "verifying";
        case lab::pyenv::Progress::Phase::Done:          return "done";
        case lab::pyenv::Progress::Phase::Failed:        return "failed";
        case lab::pyenv::Progress::Phase::Canceled:      return "canceled";
    }
    return "idle";
}

// ---------------------------------------------------------------------------
// Runtimes
// ---------------------------------------------------------------------------

/// Every backend this build knows about, installed or not.
Reply runtimes(const json&, const Scene&) {
    json listed = json::array();
    std::uintmax_t total = 0;
    for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
        const BackendInfo& info = backend_info(status.kind);
        json modules = json::array();
        for (const std::filesystem::path& file : status.files) {
            modules.push_back(json{{"name", file.filename().string()},
                                   {"preferred", file == status.preferred}});
        }
        total += status.bytes;
        listed.push_back(json{
            {"id",        std::string(info.id)},
            {"name",      std::string(info.name)},
            {"blurb",     std::string(info.blurb)},
            {"installed", status.installed},
            {"active",    status.active},
            {"devices",   status.device_count},
            {"bytes",     status.bytes},
            {"stale",     status.stale},
            {"source",    status.source},
            {"llama_tag", status.llama_tag},
            {"built_at",  status.built_at},
            {"needs_tag", std::string(RuntimeStatus::required_llama_tag())},
            {"needs_tool", std::string(info.required_tool)},
            // Whether it can be compiled here, and what is in the way when it
            // cannot: "the build failed" is a worse sentence than "you need
            // nvcc".
            {"buildable", status.buildable},
            {"blocker",   status.blocker},
            {"missing",   status.missing},
            {"modules",   std::move(modules)},
        });
    }
    return good(json{{"runtimes",  std::move(listed)},
                     // For reading, not for opening: written with ~ for the
                     // home directory, as the top bar writes the project.
                     {"directory", format::short_path(paths::runtimes_dir())},
                     {"bytes",     total},
                     // False for a build with its backend compiled in, where
                     // there is nothing to install and the page says so.
                     {"loadable",  RuntimeRegistry::loadable_backends_supported()}});
}

Reply runtime_build(const json& params, Host& host) {
    // The argument before the capability, for the same reason a method's
    // name is checked before the engine: a caller who misspelled a backend
    // and is told this build cannot compile runtimes goes and debugs their
    // build options instead of their typo.
    const std::optional<BackendKind> kind =
        backend_from_id(params.value("backend", std::string{}));
    if (!kind) {
        return bad("no such backend");
    }
    RuntimeBuilder* builder = host.runtime_builder();
    if (builder == nullptr) {
        return bad("this build cannot compile runtimes");
    }
    Host* waking = &host;
    if (!builder->start(*kind, [waking] { waking->wake(); })) {
        return bad("a runtime is already being built");
    }
    return good(json{{"started", true}});
}

/// Deleting one. The modules are files in a directory Crucible owns, and a
/// runtime that is wrong -- built against another llama.cpp, or for a card
/// that has since gone -- is otherwise only removable by hand.
Reply runtime_remove(const json& params, Host& host) {
    const std::optional<BackendKind> kind =
        backend_from_id(params.value("backend", std::string{}));
    if (!kind) {
        return bad("no such backend");
    }
    std::string error;
    if (!RuntimeRegistry::remove(*kind, error)) {
        return bad(error.empty() ? "could not remove it" : error);
    }
    host.say(std::string(backend_info(*kind).name)
             + " was removed -- it stays loaded until Crucible is restarted");
    return good(json{{"removed", true}});
}

Reply runtime_cancel(const json&, Host& host) {
    if (host.runtime_builder() == nullptr) {
        return bad("this build cannot compile runtimes");
    }
    host.runtime_builder()->cancel();
    return good(json{{"canceled", true}});
}

Reply runtime_dismiss(const json&, Host& host) {
    if (host.runtime_builder() == nullptr) {
        return bad("this build cannot compile runtimes");
    }
    host.runtime_builder()->dismiss();
    return good(json{{"dismissed", true}});
}

Reply runtime_progress(const json&, Host& host) {
    if (host.runtime_builder() == nullptr) {
        return good(kIdle);
    }
    const BuildProgress progress = host.runtime_builder()->progress();
    json out = progress_json(progress, phase_name(progress.phase));
    out["backend"] = std::string(backend_info(progress.kind).id);
    return good(std::move(out));
}

// ---------------------------------------------------------------------------
// The trainer
// ---------------------------------------------------------------------------

/// What is installed, remembered.
///
/// Finding out means starting Python and importing torch, which is most of a
/// second on a good day -- and the page asks every time the Training tab or
/// the Create view opens. The answer only changes when the environment does,
/// and the environment only changes by being installed or removed, both of
/// which touch the manifest. So the manifest's timestamp is the key.
lab::pyenv::Status trainer_status() {
    static std::mutex                      mutex;
    static bool                            known = false;
    static std::filesystem::file_time_type stamp{};
    static bool                            was_present = false;
    static lab::pyenv::Status              cached;

    std::error_code ec;
    const bool present = std::filesystem::exists(lab::pyenv::python(), ec);
    const std::filesystem::file_time_type now =
        std::filesystem::last_write_time(lab::pyenv::manifest_file(), ec);

    const std::lock_guard<std::mutex> lock(mutex);
    if (!known || present != was_present || (!ec && now != stamp)) {
        cached      = lab::pyenv::status();
        known       = true;
        was_present = present;
        stamp       = ec ? std::filesystem::file_time_type{} : now;
    }
    return cached;
}

Reply trainer(const json&, const Scene&) {
    const lab::pyenv::Status status = trainer_status();
    return good(json{{"ready", status.ready}, {"present", status.present},
                     {"flavor", std::string(lab::pyenv::flavor_id(status.flavor))},
                     {"python", status.python_version},
                     {"torch",  status.torch_version},
                     {"installed_at", status.installed_at},
                     {"bytes",  status.bytes},
                     {"note",   status.note},
                     {"directory", format::short_path(lab::pyenv::root())},
                     {"usable_gpus",   status.usable_gpus},
                     {"unusable_gpus", status.unusable_gpus}});
}

/// What installing would cost, which somebody on a metered connection is
/// entitled to know before it starts rather than after.
Reply trainer_flavors(const json&, const Scene&) {
    json out = json::array();
    for (const lab::pyenv::Flavor flavor :
         {lab::pyenv::Flavor::Cuda, lab::pyenv::Flavor::Cpu, lab::pyenv::Flavor::Mlx}) {
        json packages = json::array();
        for (const lab::pyenv::Step& step : lab::pyenv::plan(flavor)) {
            packages.push_back(step.label);
        }
        out.push_back(json{
            {"id",        std::string(lab::pyenv::flavor_id(flavor))},
            {"note",      std::string(lab::pyenv::flavor_note(flavor))},
            {"download",  lab::pyenv::download_bytes(flavor)},
            {"installed", lab::pyenv::installed_bytes(flavor)},
            {"suggested", flavor == lab::pyenv::flavor_here()},
            {"steps",     std::move(packages)},
        });
    }
    return good(std::move(out));
}

/// Which flavor is the machine's to suggest and the caller's to confirm -- a
/// CUDA stack on a machine with no NVIDIA driver imports and then fails at
/// the first tensor, which is a confusing place to find out.
Reply trainer_install(const json& params, Host& host) {
    lab::pyenv::Installer* installer = host.trainer_installer();
    if (installer == nullptr) {
        return bad("this build cannot install the trainer");
    }
    const std::string wanted = params.value("flavor", std::string{});
    const lab::pyenv::Flavor flavor = wanted.empty() ? lab::pyenv::flavor_here()
                                                     : lab::pyenv::flavor_from_id(wanted);
    Host* waking = &host;
    if (!installer->start(flavor, [waking] { waking->wake(); })) {
        return bad("the trainer is already being installed");
    }
    return good(json{{"started", true},
                     {"flavor", std::string(lab::pyenv::flavor_id(flavor))}});
}

Reply trainer_cancel(const json&, Host& host) {
    if (host.trainer_installer() == nullptr) {
        return bad("this build cannot install the trainer");
    }
    host.trainer_installer()->cancel();
    return good(json{{"canceled", true}});
}

Reply trainer_dismiss(const json&, Host& host) {
    if (host.trainer_installer() == nullptr) {
        return bad("this build cannot install the trainer");
    }
    host.trainer_installer()->dismiss();
    return good(json{{"dismissed", true}});
}

Reply trainer_progress(const json&, Host& host) {
    if (host.trainer_installer() == nullptr) {
        return good(kIdle);
    }
    const lab::pyenv::Progress progress = host.trainer_installer()->progress();
    json out = progress_json(progress, phase_name(progress.phase));
    out["flavor"] = std::string(lab::pyenv::flavor_id(progress.flavor));
    return good(std::move(out));
}

/// Take the environment away. Gigabytes come back, and nothing that was
/// trained with it is touched: a finished model is a file somewhere else.
Reply trainer_remove(const json&, Host& host) {
    if (host.trainer() != nullptr && host.trainer()->progress().running()) {
        return bad("a training run is using it -- stop that first");
    }
    if (host.trainer_installer() != nullptr
        && host.trainer_installer()->progress().running()) {
        return bad("it is being installed -- cancel that first");
    }
    std::string error;
    if (!lab::pyenv::remove(error)) {
        return bad(error.empty() ? "could not remove it" : error);
    }
    host.say("the training environment was removed");
    return good(json{{"removed", true}});
}

}  // namespace

void install_methods(std::vector<Method>& table) {
    table.push_back({"runtimes",         runtimes, nullptr});
    table.push_back({"runtime.build",    nullptr, runtime_build});
    table.push_back({"runtime.cancel",   nullptr, runtime_cancel});
    table.push_back({"runtime.dismiss",  nullptr, runtime_dismiss});
    table.push_back({"runtime.progress", nullptr, runtime_progress});
    table.push_back({"runtime.remove",   nullptr, runtime_remove});
    table.push_back({"trainer",          trainer, nullptr});
    table.push_back({"trainer.flavors",  trainer_flavors, nullptr});
    table.push_back({"trainer.install",  nullptr, trainer_install});
    table.push_back({"trainer.cancel",   nullptr, trainer_cancel});
    table.push_back({"trainer.dismiss",  nullptr, trainer_dismiss});
    table.push_back({"trainer.progress", nullptr, trainer_progress});
    table.push_back({"trainer.remove",   nullptr, trainer_remove});
}

}  // namespace crucible::api
