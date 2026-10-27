// SPDX-License-Identifier: MIT
// The JIT model host: owns every llama.cpp resource and enforces the rule that
// makes Crucible's memory budget work -- an expert is freed before the next one
// is loaded, unless somebody is using it (see engine/leases.hpp, and `keep`
// below). The router is held until the engine lets it go: kept loaded, or, on
// demand, freed while an expert works and brought back after.
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

    /// Load the router and hold it until release_router(). Idempotent.
    ///
    /// `cancel` is polled while the weights upload and stops the load where it
    /// stands. It may be empty for a load nothing is allowed to interrupt.
    ///
    /// Experts are freed to make room for it when it would not fit beside
    /// them -- the ones `keep` does not hold. When the ones it does hold leave
    /// no room, `crowded` is set and nothing is loaded: routing goes on
    /// keywords until they are let go.
    ///
    /// `beside_mlx` says an MLX model is in use, as acquire_expert's does:
    /// the delegator has to fit beside it as an expert would, and `crowded`
    /// is set the same way when it does not.
    LoadedModel* acquire_router(const ModelParams& params,
                                const ProgressCallback& progress,
                                const CancelCallback& cancel,
                                std::string& error,
                                const Keep& keep = {},
                                bool* crowded = nullptr,
                                bool beside_mlx = false);

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
    /// `beside_mlx` says an MLX model is in use as well, which this host does
    /// not hold: the new one has to fit beside it too, with or without an
    /// expert held, and `crowded` is set the same way when it does not.
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
                                bool* crowded = nullptr,
                                bool beside_mlx = false);

    /// Free every resident expert `keep` does not hold. The router is
    /// untouched.
    void release_experts(const Keep& keep = {});

    /// Would `params` load beside what is resident now? Asked of the cards
    /// where they say how much is free, and of the machine's memory where
    /// they share it -- a Mac's GPU memory is its RAM, and an MLX model in a
    /// process of its own is invisible to the cards' figure.
    ///
    /// `beside_mlx` says one is in use. None of it is this host's, so a host
    /// with nothing of its own resident would otherwise answer yes without
    /// looking.
    bool fits_beside(const ModelParams& params, bool beside_mlx = false) const;

    /// About what `params` would hold once loaded: read from a GGUF's own
    /// header where it can be, and from the size of the weights otherwise --
    /// an MLX model's folder, or a file whose header would not read.
    static std::uint64_t estimate_bytes(const ModelParams& params);

    /// Whether `params` would load on the cards this machine shows now, were
    /// Crucible's own models let go for it, as they are for a model that
    /// needs the room -- all but the delegator when `delegator_stays`.
    /// Planned and measured the way a load is: the header, then a dry run
    /// before saying no.
    ///
    /// Not whether it fits beside what is loaded this minute: a model in use
    /// is waited for. Whether it can be loaded here at all, which a model
    /// chosen for a machine of three cards cannot on one. True for whatever
    /// is not the cards' question: a provider's model, an MLX folder, a model
    /// set to run partly on the processor, a machine whose cards say nothing.
    bool could_hold(const ModelParams& params, bool delegator_stays) const;

    /// Free the delegator, if any. The experts are untouched.
    ///
    /// Between routes with "keep delegator loaded" off, and for Eject, a
    /// changed runtime and changed loading settings: the caller must have
    /// dropped whatever was holding the LoadedModel first. See
    /// Engine::release_router.
    void release_router();

    /// The machine-wide GPU settings, used to re-plan every model's split
    /// against live video memory as it is loaded. See refresh_gpu_split.
    void set_gpu_config(const GpuConfig& gpu) { gpu_ = gpu; }

    /// What Crucible's own models are holding: the delegator and whichever
    /// experts are resident -- one, unless several are in use at once.
    std::uint64_t resident_bytes() const;

    /// The expert most recently asked for, or nothing when none is resident.
    std::optional<ExpertId> loaded_expert() const;

    /// The delegator, or null when it is not loaded.
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

    /// `requested`, split over the cards as `role` is: the delegator by
    /// placed_delegator, an expert by refresh_gpu_split.
    ModelParams placed(const ModelParams& requested, Role role) const;

    /// fits_beside, for a model placed as `role` is.
    bool fits_beside(const ModelParams& requested, Role role, bool beside_mlx) const;

    /// `asked` with its context grown towards ModelParams::n_ctx_most, as far
    /// as the cards hold it now: the free memory once the experts nobody is
    /// using have gone, less the delegator's room when it is away and had
    /// room beside this model. Each context tried is judged as a load is.
    /// `asked` as it is when it asks for no more, or none fits.
    ModelParams with_room(const ModelParams& asked) const;

    /// The file and the context a model is loaded with, as far as what its
    /// cache holds goes: two loads with the same key hold the same thing for
    /// the same prompt. See kept_routing_.
    static std::string context_key(const ModelParams& params);

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

    /// The delegator as it was last asked for, loaded or not: what an
    /// expert's grown context leaves room for. See with_room.
    ModelParams                  router_asked_;
    /// Least recently used first: the one at the back was asked for last.
    std::vector<Resident>        experts_;

    /// What routing kept evaluated in the delegator's cache, held while the
    /// delegator is freed between routes -- which it is after every prompt
    /// unless "keep delegator loaded" is on. Without it, loading it again
    /// would mean reading every order's prompt again too. Handed back only to
    /// a delegator whose context_key matches `kept_key_`; see
    /// LoadedModel::score_labels.
    std::vector<LoadedModel::KeptPrompt> kept_routing_;
    std::string                          kept_key_;
    std::string                          router_key_;   ///< the loaded delegator's
};

/// The context a load that asked for `n_ctx`, and may have up to `most`,
/// gets: the first of `most`, three quarters of it, a half, three eighths
/// and so on down that `fits`, in whole kilotokens above `n_ctx`, and never
/// past `trained`, the context the model was trained to (zero when it does
/// not say). `n_ctx` when none of them fits.
int roomiest_context(int n_ctx, int most, std::uint32_t trained, const std::function<bool(int)>& fits);

}  // namespace crucible
