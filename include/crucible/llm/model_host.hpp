// SPDX-License-Identifier: MIT
// The JIT model host: owns every llama.cpp resource and enforces the rule that
// makes Crucible's memory budget work -- the small router stays resident, and at
// most one large expert is loaded at any moment.
#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crucible/config/config.hpp"
#include "crucible/llm/loaded_model.hpp"
#include "crucible/routing/expert.hpp"

struct llama_model;
struct llama_context;
struct llama_vocab;

namespace crucible {

/// Owns the llama.cpp backend and the currently-resident models.
///
/// Exactly one ModelHost should exist per process, and every method must be
/// called from the same thread -- Crucible runs it on the engine worker so the
/// UI thread never blocks behind a model load.
class ModelHost {
public:
    /// `log_path` receives llama.cpp's logging. Redirecting it is not optional:
    /// left on stderr it would draw straight over the TUI.
    explicit ModelHost(std::filesystem::path log_path);
    ~ModelHost();
    ModelHost(const ModelHost&)            = delete;
    ModelHost& operator=(const ModelHost&) = delete;

    /// Load the router and keep it resident for the rest of the session.
    /// Idempotent.
    ///
    /// `cancel` is polled while the weights upload and stops the load where it
    /// stands. It may be empty for a load nothing is allowed to interrupt.
    LoadedModel* acquire_router(const ModelParams& params,
                                const ProgressCallback& progress,
                                const CancelCallback& cancel,
                                std::string& error);

    /// Make `id`'s expert the loaded one, freeing whichever expert was
    /// loaded before. This is the JIT swap, and the cost the design accepts in
    /// exchange for experts far larger than RAM would otherwise allow.
    /// Returns the already-loaded model without doing any work if `id` is
    /// already resident.
    ///
    /// Cancelable. A thirty-gigabyte expert takes the better part of a minute
    /// to come off the disk, and a program that cannot be stopped during that
    /// minute is a program that has frozen, whatever it is doing underneath.
    LoadedModel* acquire_expert(const ExpertId& id,
                                const ModelParams& params,
                                const ProgressCallback& progress,
                                const CancelCallback& cancel,
                                std::string& error);

    /// Free the resident expert, if any. The router is untouched.
    void release_expert();

    /// Free the delegator, if any. The expert is untouched.
    ///
    /// Only for "keep delegator loaded" being off: the caller must have dropped
    /// whatever was holding the LoadedModel first. See Engine::resolve.
    void release_router();

    /// The machine-wide GPU settings, used to re-plan every model's split
    /// against live video memory as it is loaded. See refresh_gpu_split.
    void set_gpu_config(const GpuConfig& gpu) { gpu_ = gpu; }

    /// What Crucible's own models are holding. Almost always just the delegator:
    /// an expert is released before the next one is loaded, so the two are
    /// never resident together.
    std::uint64_t resident_bytes() const;

    std::optional<ExpertId> loaded_expert() const { return loaded_expert_; }
    LoadedModel*           router()        const { return router_.get(); }
    LoadedModel*           expert()        const { return expert_.get(); }

    /// Names of the compute devices llama.cpp found, for the UI and `/devices`.
    static std::vector<std::string> devices();

private:
    /// Which seat a load is for. The two are placed by different rules: an
    /// expert is spread over the cards to fit, the delegator is pinned to one.
    enum class Role { Delegator, Expert };

    std::unique_ptr<LoadedModel> load(const ModelParams& params,
                                      Role role,
                                      const ProgressCallback& progress,
                                      const CancelCallback& cancel,
                                      std::string& error);

    GpuConfig                    gpu_;
    std::unique_ptr<LoadedModel> router_;
    std::unique_ptr<LoadedModel> expert_;
    std::optional<ExpertId>      loaded_expert_;

    /// How the resident expert was loaded, so a second seat naming the same
    /// file the same way can be given it rather than reloading it.
    std::optional<ModelParams>   expert_params_;
};

}  // namespace crucible
