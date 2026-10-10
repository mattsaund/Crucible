// SPDX-License-Identifier: MIT
//
// See setup.hpp.
#include "crucible/app/setup.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <utility>

#include "crucible/app/setup_runtimes.hpp"
#include "crucible/kit/kit.hpp"
#include "crucible/lab/python.hpp"
#include "crucible/runtime/registry.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/http.hpp"

namespace crucible {
namespace {

bool runtime_installed(BackendKind kind) {
    for (const RuntimeStatus& status : RuntimeRegistry::scan()) {
        if (status.kind == kind && status.installed && !status.stale && status.missing.empty()) {
            return true;
        }
    }
    return false;
}

void breathe() { std::this_thread::sleep_for(std::chrono::milliseconds(250)); }

}  // namespace

Setup::Setup(RuntimeBuilder& runtimes, lab::pyenv::Installer& trainer,
             std::function<void()> on_change, std::function<void()> on_runtime)
    : runtimes_(runtimes),
      trainer_(trainer),
      on_change_(std::move(on_change)),
      on_runtime_(std::move(on_runtime)) {}

Setup::~Setup() { stop(); }

bool Setup::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return running_;
}

std::vector<Setup::Item> Setup::items() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return items_;
}

void Setup::stop() {
    stop_.store(true);
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Setup::want_model(starter::Model model, std::filesystem::path models_dir) {
    const std::lock_guard<std::mutex> lock(mutex_);
    model_      = std::move(model);
    models_dir_ = std::move(models_dir);
}

std::optional<std::string> Setup::take_model() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(model_ready_, std::nullopt);
}

void Setup::start() {
    if (const char* off = std::getenv("CRUCIBLE_NO_SETUP"); off != nullptr && *off != '\0'
        && std::string(off) != "0") {
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (running_) {
            return;
        }
    }
    if (worker_.joinable()) {
        worker_.join();
    }

    // What is missing, in the order it is needed.
    std::vector<Item>        items;
    std::vector<BackendKind> kinds;
    if (!lab::python::installed()) {
        if (const std::optional<lab::python::Build> build = lab::python::build_here()) {
            items.push_back({"python", "Python " + build->version, Item::State::Waiting,
                             "routing and cooks run on it", -1.0F, build->bytes});
        }
    }
    for (const BackendKind kind : runtimes_wanted_here()) {
        if (!runtime_installed(kind)) {
            const BackendInfo& info = backend_info(kind);
            items.push_back({std::string("runtime-") + std::string(info.id),
                             std::string(info.name) + " runtime", Item::State::Waiting,
                             "what models run on", -1.0F, 0});
            kinds.push_back(kind);
        }
    }
    // The programs an expert reaches for that this machine lacks: see
    // kit/kit.hpp. Before the training environment, which is the long one,
    // so a build has what it needs while torch is still downloading.
    for (const kit::Piece& piece : kit::missing()) {
        items.push_back({"kit-" + piece.id, piece.label, Item::State::Waiting, piece.why, -1.0F, piece.bytes});
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (model_) {
            items.push_back({"model", model_->label, Item::State::Waiting,
                             "a model to build with, on this machine", -1.0F, model_->bytes});
        }
    }
    if (!lab::pyenv::looks_installed()) {
        const lab::pyenv::Flavor flavor = lab::pyenv::flavor_here();
        items.push_back({"trainer",
                         "Training environment (" + std::string(lab::pyenv::flavor_id(flavor)) + ")",
                         Item::State::Waiting,
                         flavor == lab::pyenv::Flavor::Mlx ? "fine-tuning, and MLX models"
                                                           : "fine-tuning in Create",
                         -1.0F, lab::pyenv::download_bytes(flavor)});
    }

    {
        const std::lock_guard<std::mutex> lock(mutex_);
        items_   = std::move(items);
        kinds_   = std::move(kinds);
        running_ = !items_.empty();
        if (!running_) {
            return;
        }
    }
    stop_.store(false);
    worker_ = std::thread([this] { run(); });
    if (on_change_) {
        on_change_();
    }
}

void Setup::update(std::size_t index, Item::State state, std::string detail, float progress) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (index < items_.size()) {
            items_[index].state    = state;
            items_[index].detail   = std::move(detail);
            items_[index].progress = progress;
        }
    }
    if (on_change_) {
        on_change_();
    }
}

void Setup::run() {
    std::vector<Item>        todo;
    std::vector<BackendKind> kinds;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        todo  = items_;
        kinds = kinds_;
    }
    std::size_t next_kind = 0;
    bool        runtime_added = false;

    for (std::size_t i = 0; i < todo.size() && !stop_.load(); ++i) {
        const std::string& id = todo[i].id;

        if (id == "python") {
            update(i, Item::State::Working, "downloading", 0.0F);
            std::string error;
            const bool ok = lab::python::install(
                [&](const lab::python::Progress& progress) {
                    if (progress.phase == "downloading" && progress.total > 0) {
                        update(i, Item::State::Working,
                               "downloading " + format::bytes(progress.done) + " of "
                                   + format::bytes(progress.total),
                               static_cast<float>(progress.done) / static_cast<float>(progress.total));
                    } else {
                        update(i, Item::State::Working, progress.phase, 1.0F);
                    }
                },
                error);
            update(i, ok ? Item::State::Done : Item::State::Failed, ok ? std::string() : error,
                   ok ? 1.0F : -1.0F);
            continue;
        }

        if (id.rfind("runtime-", 0) == 0) {
            const BackendKind kind = kinds[next_kind++];
            // Somebody may have started one from the settings screen; it is
            // the same builder, so its build finishes first.
            while (runtimes_.progress().running() && !stop_.load()) {
                update(i, Item::State::Waiting, "waiting for another runtime to finish", -1.0F);
                breathe();
            }
            if (stop_.load()) {
                break;
            }
            if (runtime_installed(kind)) {
                update(i, Item::State::Done, {}, 1.0F);
                continue;
            }
            if (!runtimes_.start(kind, on_change_)) {
                update(i, Item::State::Failed, "the runtime builder is busy", -1.0F);
                continue;
            }
            for (;;) {
                const BuildProgress progress = runtimes_.progress();
                if (progress.finished()) {
                    const bool ok = progress.phase == BuildProgress::Phase::Done && progress.error.empty();
                    update(i, ok ? Item::State::Done : Item::State::Failed,
                           ok ? std::string() : (progress.error.empty() ? progress.label() : progress.error),
                           ok ? 1.0F : -1.0F);
                    runtime_added = runtime_added || ok;
                    break;
                }
                update(i, Item::State::Working, progress.label(),
                       progress.phase == BuildProgress::Phase::Compiling ? progress.percent : -1.0F);
                if (stop_.load()) {
                    break;
                }
                breathe();
            }
            continue;
        }

        if (id.rfind("kit-", 0) == 0) {
            update(i, Item::State::Working, "downloading", 0.0F);
            std::string error;
            const bool ok = kit::install(
                id.substr(4),
                [&](std::uint64_t done, std::uint64_t total) {
                    if (total > 0) {
                        update(i, Item::State::Working,
                               "downloading " + format::bytes(done) + " of about " + format::bytes(total),
                               std::min(1.0F, static_cast<float>(done) / static_cast<float>(total)));
                    } else {
                        update(i, Item::State::Working, "installing", -1.0F);
                    }
                },
                [this] { return stop_.load(); }, error);
            update(i, ok ? Item::State::Done : Item::State::Failed, ok ? std::string() : error,
                   ok ? 1.0F : -1.0F);
            continue;
        }

        if (id == "model") {
            starter::Model model;
            std::filesystem::path folder;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                model  = *model_;
                folder = models_dir_;
            }
            std::error_code ec;
            std::filesystem::create_directories(folder, ec);
            const std::filesystem::path part = folder / (model.file + ".part");
            const std::filesystem::path done = folder / model.file;
            std::atomic<bool> finished{false};
            bool        fetched = false;
            std::string why;
            std::thread fetch([&] {
                fetched = util::http::download(model.url(), part, 7200, why);
                finished.store(true);
            });
            while (!finished.load()) {
                std::error_code size_ec;
                const std::uintmax_t got = std::filesystem::file_size(part, size_ec);
                const std::uint64_t  have = size_ec ? 0 : static_cast<std::uint64_t>(got);
                update(i, Item::State::Working,
                       "downloading " + format::bytes(have) + " of " + format::bytes(model.bytes),
                       model.bytes > 0 ? std::min(1.0F, static_cast<float>(have) / static_cast<float>(model.bytes))
                                       : -1.0F);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            fetch.join();
            if (fetched) {
                std::filesystem::rename(part, done, ec);
                fetched = !ec;
                if (ec) {
                    why = ec.message();
                }
            } else {
                std::filesystem::remove(part, ec);
            }
            if (fetched) {
                const std::lock_guard<std::mutex> lock(mutex_);
                model_ready_ = model.file;
                model_.reset();
            }
            update(i, fetched ? Item::State::Done : Item::State::Failed,
                   fetched ? std::string() : "the download failed: " + why, fetched ? 1.0F : -1.0F);
            continue;
        }

        if (id == "trainer") {
            if (runtime_added && on_runtime_) {
                // The models move to the new hardware before the long one
                // starts, so a chat can use the card while torch downloads.
                on_runtime_();
                runtime_added = false;
            }
            while (trainer_.progress().running() && !stop_.load()) {
                update(i, Item::State::Working, trainer_.progress().label(), trainer_.progress().percent);
                breathe();
            }
            if (stop_.load()) {
                break;
            }
            if (lab::pyenv::looks_installed()) {
                update(i, Item::State::Done, {}, 1.0F);
                continue;
            }
            if (!trainer_.start(lab::pyenv::flavor_here(), on_change_)) {
                update(i, Item::State::Failed, "the training installer is busy", -1.0F);
                continue;
            }
            for (;;) {
                const lab::pyenv::Progress progress = trainer_.progress();
                if (progress.finished()) {
                    const bool ok = progress.phase == lab::pyenv::Progress::Phase::Done;
                    update(i, ok ? Item::State::Done : Item::State::Failed,
                           ok ? std::string() : progress.error, ok ? 1.0F : -1.0F);
                    break;
                }
                update(i, Item::State::Working, progress.label(), progress.percent);
                if (stop_.load()) {
                    break;
                }
                breathe();
            }
        }
    }

    if (runtime_added && on_runtime_ && !stop_.load()) {
        on_runtime_();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        running_ = false;
    }
    if (on_change_) {
        on_change_();
    }
}

}  // namespace crucible
