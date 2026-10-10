// SPDX-License-Identifier: MIT
// The JIT model host: owns every llama.cpp resource and enforces the rule that
// makes Crucible's memory budget work -- the small router stays resident, and an
// expert is freed before the next one is loaded. Unless somebody is using it:
// see engine/leases.hpp, and `keep` below.
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
/// Exactly one ModelHost should exist per process, and its methods are called
/// one at a time -- the engine holds a lock around them -- so the UI thread
/// never blocks behind a model load and no two loads race. A model it hands
/// out is answered from by whoever holds it, outside that lock; LoadedModel
/// takes its own turns.
class ModelHost {
public:
    /// Whether the model at a path must stay: somebody holds a lease on it.
    /// Empty means nothing must, which is the single-user case.
    using Keep = std::function<bool(const std::string& path)>;

    /// `log_path` receives llama.cpp's logging. Redirecting it is not optional:
    /// left on stderr it would be lost: a windowed program has no terminal.
    explicit ModelHost(std::filesystem::path log_path);
    ~ModelHost();
    ModelHost(const ModelHost&)            = delete;
    ModelHost& operator=(const ModelHost&) = delete;

    /// Load the router and keep it resident for the rest of the session.
    /// Idempotent.
    ///
    /// `cancel` is polled while the weights upload and stops the load where it
    /// stands. It may be empty for a load nothing is allowed to interrupt.
    ///
    /// Experts are freed to make room for it when it would not fit beside
    /// them -- the ones `keep` does not hold. When the ones it does hold leave
    /// no room, `crowded` is set and nothing is loaded: routing goes on
    /// keywords until they are let go.
    LoadedModel* acquire_router(const ModelParams& params,
                                const ProgressCallback& progress,
                                const CancelCallback& cancel,
                                std::string& error,
                                const Keep& keep = {},
                                bool* crowded = nullptr);

    /// Make `id`'s expert a loaded one. This is the JIT swap, and the cost
    /// the design accepts in exchange for experts far larger than RAM would
    /// otherwise allow: every resident expert `keep` does not hold is freed
    /// first, so with nothing held this is exactly one expert in, one out.
    /// Returns the already-loaded model without doing any work if it is
    /// already resident, under this seat or another loaded the same way.
    ///
    /// The experts `keep` holds stay, and the new one is loaded beside them
    /// when it fits. When it does not, `crowded` is set and nothing is
    /// loaded -- the caller waits for a lease to end and asks again, rather
    /// than this freeing a model an agent is generating with.
    ///
    /// Cancelable. A thirty-gigabyte expert takes the better part of a minute
    /// to come off the disk, and a program that cannot be stopped during that
    /// minute is a program that has frozen, whatever it is doing underneath.
    LoadedModel* acquire_expert(const ExpertId& id,
                                const ModelParams& params,
                                const ProgressCallback& progress,
                                const CancelCallback& cancel,
                                std::string& error,
                                const Keep& keep = {},
                                bool* crowded = nullptr);

    /// Free every resident expert `keep` does not hold. The router is
    /// untouched.
    void release_experts(const Keep& keep = {});

    /// Would `params` load beside what is resident now? Asked of the cards
    /// where they say how much is free, and of the machine's memory where
    /// they share it -- a Mac's GPU memory is its RAM, and an MLX model in a
    /// process of its own is invisible to the cards' figure.
    bool fits_beside(const ModelParams& params) const;

    /// About what `params` would hold once loaded: read from a GGUF's own
    /// header where it can be, and from the size of the weights otherwise --
    /// an MLX model's folder, or a file whose header would not read.
    static std::uint64_t estimate_bytes(const ModelParams& params);

    /// Free the delegator, if any. The expert is untouched.
    ///
    /// Only for "keep delegator loaded" being off: the caller must have dropped
    /// whatever was holding the LoadedModel first. See Engine::resolve.
    void release_router();

    /// The machine-wide GPU settings, used to re-plan every model's split
    /// against live video memory as it is loaded. See refresh_gpu_split.
    void set_gpu_config(const GpuConfig& gpu) { gpu_ = gpu; }

    /// What Crucible's own models are holding: the delegator and whichever
    /// experts are resident -- one, unless several are in use at once.
    std::uint64_t resident_bytes() const;

    /// The expert most recently asked for, and its model. Null when none is
    /// resident.
    std::optional<ExpertId> loaded_expert() const;
    LoadedModel*            expert() const;
    LoadedModel*            router() const { return router_.get(); }

    /// How many experts are resident.
    std::size_t expert_count() const { return experts_.size(); }

    /// One resident expert: the seat that last asked for it, and its model.
    struct Held {
        ExpertId           id;
        const LoadedModel* model = nullptr;
    };

    /// Every resident expert, the least recently asked for first. For the
    /// list of what is in memory; the pointers are good until the next load
    /// or release.
    std::vector<Held> held() const;

    /// Names of the compute devices llama.cpp found, for the UI and `/devices`.
    static std::vector<std::string> devices();

private:
    /// Which seat a load is for. The two are placed by different rules: an
    /// expert is spread over the cards to fit, the delegator is pinned to one.
    enum class Role { Delegator, Expert };

    /// Where the delegator goes: a card of its own when one has room for it,
    /// and divided like an expert when none does. See delegator_cards.
    ModelParams placed_delegator(const ModelParams& requested) const;

    std::unique_ptr<LoadedModel> load(const ModelParams& params,
                                      Role role,
                                      const ProgressCallback& progress,
                                      const CancelCallback& cancel,
                                      std::string& error);

    /// One expert in memory: the seat that last asked for it, how it was
    /// loaded -- so a second seat naming the same file the same way is given
    /// it rather than a reload -- and the model.
    struct Resident {
        ExpertId                     id;
        ModelParams                  params;
        std::unique_ptr<LoadedModel> model;
    };

    /// Free the least recently used expert `keep` does not hold. False when
    /// there is none to free.
    bool free_one(const Keep& keep);

    GpuConfig                    gpu_;
    std::unique_ptr<LoadedModel> router_;
    /// Least recently used first: the one at the back was asked for last.
    std::vector<Resident>        experts_;
};

}  // namespace crucible
