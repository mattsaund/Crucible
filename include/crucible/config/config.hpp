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

    /// The provider this model is asked through, by `Provider::id`. Empty means
    /// it is a file on this machine, which is what every seat is unless
    /// somebody decided otherwise.
    ///
    /// When it is set, `model` is the provider's own name for the model --
    /// "claude-opus-5-5", "deepseek-chat" -- rather than a file, and everything
    /// below about cards and layers does not apply: nothing is loaded, because
    /// there is nothing here to load.
    std::string provider;

    /// True when this seat is answered somewhere else. See `provider`.
    bool remote() const { return !provider.empty(); }

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

/// Somewhere a model can be asked that is not this machine.
///
/// Crucible is local first and stays that way: nothing here exists until a
/// provider is added by hand, and a prompt only leaves the machine when it is
/// routed to a seat that names one. What this is for is the expert you cannot
/// run -- a frontier model as the specialist of last resort, or as the default
/// expert that catches what the local ones could not place.
///
/// Two wire formats cover everything worth talking to. `anthropic` is Claude's
/// own Messages API. `openai` is the chat-completions shape that OpenAI
/// defined and everybody else adopted: OpenAI, Gemini, DeepSeek, Moonshot's
/// Kimi, Cloudflare Workers AI, OpenRouter, Groq -- and a llama.cpp, Ollama or
/// LM Studio server on another machine on your own network.
struct Provider {
    /// A short name the config refers to it by, unique among providers.
    std::string id;

    /// What it is called on screen. Falls back to `id`.
    std::string name;

    /// "anthropic" or "openai". See above.
    std::string kind = "openai";

    /// Where its API is. Empty means the kind's own default, which only
    /// `anthropic` and plain OpenAI have: every other service is an `openai`
    /// provider with its own address.
    std::string base_url;

    /// The key, as typed -- or "env:NAME" to read it from an environment
    /// variable instead, which keeps it out of the config file altogether.
    /// Left empty, the conventional variable for the service is tried
    /// (ANTHROPIC_API_KEY, OPENAI_API_KEY and so on).
    ///
    /// Written to config.json in plain text when it is typed here. Anyone who
    /// can read the config can read the key, and the settings screen says so.
    std::string api_key;

    /// The models it is known to offer, as last listed. A convenience for the
    /// picker, not a restriction: a seat may name a model this does not list.
    std::vector<std::string> models;

    const std::string& label() const { return name.empty() ? id : name; }

    /// `base_url`, or the kind's default when it is empty. No trailing slash.
    std::string endpoint() const;

    /// The key to send: `api_key` itself, the variable it points at, or the
    /// service's conventional variable. Empty when there is none anywhere --
    /// which is right for a server on your own network that wants no key.
    std::string resolved_key() const;

    /// The environment variable `resolved_key` falls back to, or empty when
    /// this service has no convention. Shown on the settings screen so "leave
    /// it blank" is an instruction somebody can follow.
    std::string conventional_key_variable() const;

    /// True when the address is this machine or a private network: localhost,
    /// a 10.x or 192.168.x address, a `.local` name.
    ///
    /// Two things turn on it. A prompt sent to one of these has not left your
    /// network, so the screen does not say that it has; and plain `http://`
    /// to one of these is ordinary, where to anywhere else it would put the
    /// key on the wire for anyone in between to read.
    bool on_your_network() const;
};

/// The slug for a provider name: lower case, dashes, nothing else.
std::string provider_id_from_name(std::string_view name);

/// Which API shape a provider at `base_url` speaks, from the address alone:
/// "anthropic" for Anthropic's own host, "openai" for everything else -- which
/// is what everything else speaks. An empty address is Anthropic's default
/// only when nothing says otherwise, so it reads as "openai" here; the caller
/// that means Anthropic says so.
std::string provider_kind_for(std::string_view base_url);

/// A service worth having a button for.
///
/// One table, used three ways: the settings screen offers each as a starting
/// point, a provider's address is matched against it to find the environment
/// variable its key conventionally lives in, and the README's list of what
/// can be reached is this list.
///
/// Deliberately no model names. A list of those is out of date the week it
/// is written; the provider is asked instead, and a model can always be
/// named by hand.
struct KnownService {
    std::string_view name;          ///< "DeepSeek"
    std::string_view kind;          ///< "anthropic" or "openai"
    std::string_view base_url;      ///< empty for the kind's default
    std::string_view host_part;     ///< what its address contains: "deepseek.com"
    std::string_view key_variable;  ///< "DEEPSEEK_API_KEY", or empty when it wants none
    std::string_view note;          ///< one line for the settings screen, or empty
};

const std::vector<KnownService>& known_services();

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

    /// duckduckgo | wikipedia | searxng | brave
    std::string search_provider = "duckduckgo";

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

    /// Let experts see and work the screen: take screenshots, move and click
    /// the mouse, type, press keys, scroll. See tools/computer.hpp.
    ///
    /// Off by default, and the one switch here that is worth reading twice.
    /// Trusting a folder lets an expert change what is in it; this lets one
    /// act as you, on everything on the screen, with no folder around it. A
    /// model that mis-reads a screenshot will click on the wrong thing, and
    /// the wrong thing can be anything. Where a platform asks for a
    /// permission first -- macOS asks for Accessibility and Screen Recording
    /// -- the first attempt says so.
    bool computer_control = false;
};

/// How a build is run: who plans it, what a seat made for it runs on, and
/// what is done between its tasks. See scripts/orchestrator's build.py.
struct BuildConfig {
    /// The seat that writes the plan and the write-up. Empty lets the
    /// delegator pick for the directive, which is right until a roster has a
    /// seat that is plainly the architect -- a frontier model, usually.
    ExpertId architect;

    /// The model a seat the build makes for itself runs on -- "CSS layout",
    /// "SQL schema" -- when nobody on the roster fits a task. A file in the
    /// models folder or a provider's model, as a seat's. Empty makes none:
    /// every task then goes to whoever fits best among the seats there are.
    std::string worker_model;
    std::string worker_provider;

    /// Commit after each task that finishes, starting a repository when the
    /// project has none. The record of a build in the project's own history,
    /// a task at a time, which is also what makes a bad task undoable.
    bool auto_commit = true;

    /// Put the plan to the person before the first task starts.
    bool confirm_plan = true;

    /// Rounds a task may take before the build moves on without it.
    int rounds_per_task = 40;
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

/// How the window behaves, as opposed to how the models do.
struct UiConfig {
    /// Ask GitHub, once a day, whether there is a newer Crucible.
    ///
    /// On by default, and it is the one thing in this program that reaches the
    /// network without being asked to. What it sends is a request for a public
    /// version number -- nothing about the machine, the models, the config or
    /// anything typed -- and what it does with the answer is show a word in the
    /// corner. Crucible is installed by compiling, so without this a copy has
    /// no way of learning that the crash it hits every morning was fixed a
    /// month ago. See app/update.hpp.
    bool check_updates = true;
};

/// The whole config file.
struct Config {
    /// Where the GGUFs live. Empty means the built-in default,
    /// ~/.local/share/crucible/models. May be moved anywhere on the system.
    std::string models_dir;

    ModelParams router;    ///< the always-resident delegator
    ModelParams defaults;  ///< inherited by every expert

    /// Who the experts are. Crucible ships none: every seat is one somebody
    /// added, and it is part of the config because it has to survive a
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

    /// Providers that have been added. Empty on every install until somebody
    /// adds one: see Provider.
    std::vector<Provider> providers;

    RoutingConfig routing;
    GpuConfig     gpu;
    ToolsConfig   tools;
    BuildConfig   build;
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

    /// True when this seat has a model behind it: a GGUF, or a provider's.
    bool has_expert(const ExpertId& id) const;

    /// The provider with this id, or null. Null for an empty id, which is the
    /// answer for every local seat.
    const Provider* provider(std::string_view id) const;

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
/// The configuration as JSON text, exactly as save_config would write it.
///
/// For the API, so an interface and the config file cannot disagree about the
/// shape. Returned as a string rather than an nlohmann value to keep that
/// header out of this one, which half the program includes.
std::string config_to_json_text(const Config& config);

/// Read a configuration back out of JSON text, as `config_to_json_text`
/// writes it. Anything the document leaves out keeps its default, so a patch
/// merged over the current document is a complete configuration.
///
/// `warnings` collects the same complaints loading a file would produce. An
/// unparseable document yields the built-in defaults rather than throwing.
Config config_from_json_text(std::string_view text, std::vector<std::string>& warnings);

bool save_config(const Config& config, const std::filesystem::path& file);

/// Convenience overload using `paths::config_file()`.
bool save_config(const Config& config);

}  // namespace crucible
