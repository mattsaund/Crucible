// SPDX-License-Identifier: MIT
// Crucible's on-disk configuration: which GGUF backs each expert, and how each
// one should be loaded and sampled.
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include "crucible/routing/expert.hpp"

namespace crucible {

/// How a single model is loaded onto the hardware and sampled from.
///
/// Every field has a usable default, and each expert inherits from
/// `Config::defaults` for any field its own entry leaves unset. That way the
/// common case -- nine experts that differ only by file -- stays a nine-line
/// config.
struct ModelParams {
    /// The model as written in the config: normally just a file name inside the
    /// models directory ("physics-q4.gguf"), but an absolute or ~-path is
    /// accepted so a model can live anywhere. Empty means the seat is unfilled.
    std::string model;

    /// `model` resolved to an absolute path. Derived at load time and never
    /// written back to the file.
    std::string path;

    // Loading
    int  n_gpu_layers = -1;        ///< -1 offloads every layer it can fit.
    int  main_gpu     = 0;
    std::string split_mode = "layer";  ///< none | layer | row | tensor
    std::vector<float> tensor_split;   ///< per-GPU share; empty = let llama.cpp decide

    /// Keep the weights out of host memory entirely.
    ///
    /// Derived from GpuConfig, not written to the config file: these are the
    /// llama.cpp-level knobs that the machine-wide GPU settings translate into,
    /// the same way `tensor_split` is derived from the split mode. See
    /// gpu_policy.hpp.
    bool no_host   = false;  ///< no pinned host buffer for weights
    bool direct_io = false;  ///< read straight to the device, not via the page cache
    bool vram_only = false;  ///< refuse the load rather than spill into system RAM
    bool gpu_only  = false;  ///< every layer on the GPU; no partial offload

    // Context
    int  n_ctx     = 8192;
    int  n_batch   = 512;
    int  n_threads = 0;            ///< 0 = hardware_concurrency()
    bool flash_attn = true;

    // Sampling
    float temperature   = 0.7F;
    float top_p         = 0.95F;
    int   top_k         = 40;
    float min_p         = 0.05F;
    float repeat_penalty = 1.05F;
    int   repeat_last_n  = 64;
    int   max_tokens     = 2048;   ///< hard cap per reply; -1 = until EOG
    std::uint32_t seed   = 0xFFFFFFFFU;  ///< 0xFFFFFFFF = random each run

    /// Fill any field this entry left at its default from `base`.
    /// The model is never inherited -- an expert without its own file is unfilled.
    void inherit_from(const ModelParams& base);
};

/// How the delegator's answer is acted on.
struct RoutingConfig {
    /// Below this confidence the delegator is treated as undecided and the
    /// prompt goes to `default_expert` instead -- or is taken at face value
    /// when there is none. 0 disables the check entirely.
    float min_confidence = 0.60F;

    /// The expert that catches what does not fit: a prompt the delegator could
    /// not place confidently, or one routed to a seat with no model.
    ///
    /// An ordinary seat, named by id, and empty by default. Crucible used to
    /// ship a tenth built-in expert called Fallback for this; with a roster the
    /// user owns, a general-purpose expert is something they add and name
    /// themselves, and this says which one it is. Empty means there is none, and
    /// an uncertain route is taken at face value rather than being sent nowhere.
    ExpertId default_expert;

    /// Keep the delegator in memory between prompts.
    ///
    /// Off by default, which is the setting that makes the whole design work on
    /// one machine. Exactly one model is resident at any moment: the delegator
    /// is freed the instant it has routed, the expert is loaded, and when the
    /// expert has answered it is freed too and the delegator comes back ready
    /// for the next prompt. The peak is the larger of the two rather than their
    /// sum, which is what makes room for a delegator big enough to route well
    /// *and* an expert as large as the cards will hold.
    ///
    /// On, it is loaded once and stays -- and its whole footprint is gone from
    /// every expert that follows for the rest of the session. That is a good
    /// trade only when the delegator is small next to the card, which is why it
    /// is a choice and not the default: the honest default is the one that does
    /// not quietly cost an expert the memory it needed.
    ///
    /// The cost of off is a load per model per prompt, so a follow-up question
    /// reloads the expert. A prompt pinned with a slash command skips the
    /// delegator entirely and pays only for the expert.
    bool keep_delegator_loaded = false;
};

/// How the machine's GPUs are used.
///
/// This is a property of the hardware rather than of any one model, so it is
/// configured once and applied to every model that gets loaded. Per-model
/// `tensor_split` values in the config file are only honored while `mode` is
/// "auto" -- otherwise this wins, and the settings screen is the one place the
/// arrangement is decided.
struct GpuConfig {
    /// auto | even | priority | single. See runtime/devices.hpp for what each
    /// one does; "auto" leaves the decision to llama.cpp.
    std::string mode = "auto";

    /// Device indices, best first. Only read in "priority" mode. Indices are
    /// ggml's, which is what `/devices` prints.
    std::vector<int> priority;

    /// The device "single" mode puts everything on, and the one llama.cpp uses
    /// for small tensors in the other modes.
    int main_gpu = 0;

    /// Put every layer on the GPU, whatever "GPU layers" says.
    ///
    /// llama.cpp will happily run part of a model on the processor -- that is
    /// what a GPU-layer count lower than the model's layer count means -- and
    /// the CPU's share is slower than the GPU's by two orders of magnitude, so
    /// a model that is 90% offloaded runs at roughly the speed of one that is
    /// not offloaded at all. Nobody chooses that on purpose; they get there by
    /// leaving a number behind in the config.
    ///
    /// On, this pins the offload to every layer plus the output whenever there
    /// is a GPU to put them on, and stops the weights being staged through
    /// host memory. It does nothing on a machine with no GPU, where the
    /// processor is the only thing there is to compute on.
    bool gpu_only = true;

    /// Refuse to load a model that does not fit in dedicated video memory.
    ///
    /// A graphics driver asked for more memory than the card has does not
    /// usually fail. It spills the excess into system RAM and carries on, and
    /// the model then runs perhaps twenty times slower with nothing on screen
    /// to say why -- which is a far worse outcome than being told it will not
    /// fit. On, Crucible checks the free video memory first and says so, and the
    /// weights are read straight to the card rather than through the operating
    /// system's page cache, so they never occupy RAM on the way past either.
    bool vram_only = false;
};

/// What the experts can reach beyond the machine.
///
/// Everything here is off by default. Crucible is local-first, and a program that
/// quietly started sending what you typed to a search engine would not be.
struct ToolsConfig {
    /// Let experts look things up. See tools/web_search.hpp.
    bool web_search = false;

    /// wikipedia | searxng | brave
    std::string search_provider = "wikipedia";

    /// The address of your own searxng instance, for that provider.
    std::string search_endpoint;

    /// The API key for brave. Written to the config file in plain text, which
    /// is worth knowing before putting one there.
    std::string search_api_key;

    int search_results = 5;   ///< how many to hand the expert
    int search_timeout = 10;  ///< seconds before giving up

    /// How many times one prompt may search before it has to answer. A model
    /// that searches, reads the results and wants to search again is being
    /// useful; one that does it eight times is stuck.
    int search_rounds = 2;

    /// Apply a file edit without asking first.
    ///
    /// Off, every WRITE an expert makes in Chat stops and shows you the file as
    /// it is beside the file as it would be, and nothing is written until you
    /// pick one. On, the edit lands and you read about it afterwards.
    ///
    /// Off by default, because the two are not the same risk. Trusting a folder
    /// says Crucible may work in it; it does not say every change an expert
    /// proposes is one you wanted, and a model that rewrites the wrong file is
    /// not a rare event -- it is a Tuesday. The toggle is in the chat bar rather
    /// than buried here, because whether you are watching is a decision that
    /// changes between one prompt and the next.
    ///
    /// A cook ignores this and always applies. A cook is an hour of work you
    /// started and walked away from; stopping it on the first write to ask a
    /// question nobody is there to answer would mean it never gets past the
    /// first write. Its record is the journal, and every step in it expands to
    /// the diff that step made.
    bool auto_edits = false;

    /// What to do when the conversation outgrows the context. See Overflow.
    ///
    /// Stored as a word rather than the enum so a config file stays readable
    /// and an unknown value degrades to the default instead of to whatever
    /// integer happened to be written.
    std::string overflow = "rolling";

    /// Seconds a single command may take before it is killed. A build is
    /// minutes; a command still going after this is stuck, and a cook waiting
    /// on it has stopped cooking.
    ///
    /// The last of what used to be three switches here. Reading, writing and
    /// running in the project directory were each a setting of their own, on
    /// top of the folder trust Crucible already asks for -- so a person who had
    /// said yes to the trust question still found that nothing could be edited,
    /// with no indication of which of two boxes they had not ticked. Trust is
    /// the decision now, asked once per folder and asked in the terms it is
    /// actually about. See tools/workshop.hpp.
    int workshop_timeout = 120;
};

/// What to do when a conversation no longer fits in the context.
///
/// Every long conversation reaches this eventually: the context is a fixed
/// number of tokens and a transcript is not. The three answers are the three
/// anybody has ever wanted, and which is right depends on what the conversation
/// is -- so it is a setting rather than a decision made once in the engine.
enum class Overflow {
    /// Drop the oldest exchanges until it fits, one at a time.
    ///
    /// The default, and right for a conversation: what was said an hour ago
    /// matters less than what was said a minute ago, and the model keeps its
    /// grip on the thread it is actually on.
    RollingWindow,
    /// Keep the beginning and the end, drop the middle.
    ///
    /// For a conversation that opened with something that has to survive -- a
    /// specification, a file, a set of rules -- and has since wandered. A
    /// rolling window throws exactly that away first.
    TruncateMiddle,
    /// Refuse rather than forget.
    ///
    /// Nothing is dropped and the turn does not run. For work where a silently
    /// shortened context would be worse than no answer: the model cannot tell
    /// you what it stopped being able to see, so this makes the program say it
    /// instead.
    StopAtLimit,
};

std::string_view overflow_id(Overflow policy);
Overflow         overflow_from_id(std::string_view id);

/// Purely cosmetic knobs.
struct UiConfig {
    int  animation_ms   = 90;    ///< frame interval while Crucible is busy
    bool show_experts = true; ///< draw the ring (toggle at runtime with Ctrl-T)
    bool unicode        = true;  ///< false falls back to plain ASCII

    /// Keep a reasoning model's working on screen after it has answered.
    ///
    /// Off, the working is shown while it is happening -- which is the only
    /// sign of life during the seconds before the answer starts -- and then
    /// replaced by the answer. On, it stays, dimmed, above every reply.
    bool show_reasoning = false;
};

/// The whole config file.
struct Config {
    /// Where the GGUFs live. Empty means the built-in default,
    /// ~/.local/share/crucible/models. May be moved anywhere on the system.
    std::string models_dir;

    ModelParams router;    ///< the always-resident delegator
    ModelParams defaults;  ///< inherited by every expert

    /// Who the experts are: the nine that ship plus whatever `/newexpert`
    /// added. Part of the config because a user-made expert has to survive a
    /// restart -- it is the user's list, not the program's.
    ///
    /// Defaulted rather than left empty so that every path which builds a
    /// Config without reading a file -- the tests, the parse-failure fallback,
    /// a first run -- gets a working expert list rather than a program with
    /// nowhere to route to.
    Roster roster = Roster::bare();

    /// Which GGUF backs each seat, keyed by `Expert::id`.
    ///
    /// A map rather than an array parallel to the roster: the two are edited
    /// independently -- ejecting an expert must not renumber the model
    /// assignments of the seats after it -- and a key that no longer names a
    /// seat is simply an entry nothing reads, which is the right outcome for a
    /// config file someone hand-edited.
    std::map<ExpertId, ModelParams> experts;

    RoutingConfig routing;
    GpuConfig     gpu;
    ToolsConfig   tools;
    UiConfig      ui;

    /// How hard a reasoning model should think: low | medium | high.
    ///
    /// Appended to the system prompt as `Reasoning: <level>`, which is where
    /// gpt-oss expects it. llama.cpp does not run the model's own jinja
    /// template -- it recognizes the harmony format and applies a built-in
    /// formatter that emits no system preamble at all -- so the line has to be
    /// written into the system message rather than passed as a template
    /// argument. Measured on gpt-oss-20b: high produces more working than low
    /// on every prompt that produced any.
    ///
    /// A model that does not know the convention reads one more line of system
    /// prompt and carries on.
    std::string reasoning_effort = "medium";

    std::string system_prompt =
        "You are Crucible, a focused local expert. Answer precisely and completely, "
        "showing your reasoning when it helps. Prefer concrete detail over hedging.";

    /// The parameters for one seat, with `defaults` already inherited. Returns
    /// an unfilled entry for a seat with nothing assigned.
    const ModelParams& expert(const ExpertId& id) const;

    /// True when a GGUF is configured for this seat.
    bool has_expert(const ExpertId& id) const;

    /// Seats that currently have a model file configured, in roster order.
    std::vector<ExpertId> configured_experts() const;

    /// True when no expert and no router model is configured at all -- the
    /// state a first run lands in, which the UI explains rather than crashing on.
    bool is_empty() const;

    /// The models directory as an absolute path, applying the default when the
    /// config leaves it blank.
    std::filesystem::path resolved_models_dir() const;

    /// Re-resolve every model reference against the models directory. Call
    /// after changing `models_dir` or any model assignment.
    void resolve_models();
};

/// Load the config, creating a documented default file if none exists.
/// `warnings` collects non-fatal problems (a bad enum, a missing model file)
/// so the UI can surface them instead of failing the whole startup.
Config load_config(const std::filesystem::path& file, std::vector<std::string>& warnings);

/// Convenience overload using `paths::config_file()`.
Config load_config(std::vector<std::string>& warnings);

/// Write a fully-populated config with every field spelled out, so the file
/// doubles as the reference for what is tunable.
void write_default_config(const std::filesystem::path& file);

/// Serialize `config` back to disk. This is what the in-app settings editor
/// calls, so it must round-trip everything the loader understands.
/// Returns false if the file could not be written.
bool save_config(const Config& config, const std::filesystem::path& file);

/// Convenience overload using `paths::config_file()`.
bool save_config(const Config& config);

}  // namespace crucible
