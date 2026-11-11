// SPDX-License-Identifier: MIT
// crucible-loadbench -- how long models take to load, swap and answer.
//
//   crucible-loadbench [--config FILE] load <seat id or "delegator"> [--cold] [--processor] [--tokens N]
//   crucible-loadbench [--config FILE] chat [--cold] [--trace] [--pause S] "a prompt" "@seat a prompt" ...
//
// Both read the config Crucible itself would, or FILE -- a copy of somebody's
// config is theirs exactly -- and write llama.cpp's log, with Crucible's
// timings in it, where Crucible does: under XDG_DATA_HOME when it is set.
//
// `load` brings one model in through ModelHost, as a prompt would, and says
// how long that took, where it went, how soon a short reply began and how
// fast it ran. --cold first drops the file from the operating system's page
// cache, so the time is the disk's as well as the cards'. --processor loads
// it on the processor alone, as the delegator goes when the cards are full.
//
// `chat` sends prompts through the whole engine, the orchestrator and the
// delegator included, one after another, as somebody typing them would: a
// prompt starting "@seat " is sent to that seat, any other is routed. For
// each it says what was routed where, what was loaded, how soon the first
// word came, and how long the engine then took to settle before it could
// take the next one. --trace prints the status line each time it changes,
// with the time since the prompt was sent: where a turn's time went. --pause
// waits that many seconds between prompts, as a person reading the answer
// would; without it the next prompt is sent the moment the engine has
// settled.
//
// The cold runs drop files from the page cache with posix_fadvise, which is
// Linux's; elsewhere --cold does nothing and the runs are as cached as they
// happen to be.

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "crucible/config/config.hpp"
#include "crucible/config/gpu_policy.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/runtime/devices.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

using namespace crucible;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

/// Drop `file` from the page cache, so the next read of it comes off the
/// disk. Linux only: elsewhere a cold load has to be arranged by hand.
void drop_cached(const std::filesystem::path& file) {
#if defined(__linux__)
    const int fd = ::open(file.c_str(), O_RDONLY);
    if (fd >= 0) {
        ::posix_fadvise(fd, 0, 0, POSIX_FADV_DONTNEED);
        ::close(fd);
    }
#else
    (void)file;
#endif
}

/// The model files a config names, and the projectors beside them.
std::vector<std::filesystem::path> config_files(const Config& config) {
    std::vector<std::filesystem::path> files;
    const auto add = [&files](const std::string& path) {
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_regular_file(path, ec)) {
            return;
        }
        files.emplace_back(path);
        for (const auto& entry : std::filesystem::directory_iterator(std::filesystem::path(path).parent_path(), ec)) {
            if (entry.path().filename().string().find("mmproj") != std::string::npos) {
                files.push_back(entry.path());
            }
        }
    };
    add(config.router.path);
    for (const ExpertId& id : config.configured_experts()) {
        add(config.expert(id).path);
    }
    return files;
}

/// Where the config is read from: --config's file, or Crucible's own.
std::filesystem::path g_config_file;

Config read_config() {
    std::vector<std::string> warnings;
    Config config = g_config_file.empty() ? load_config(warnings) : load_config(g_config_file, warnings);
    for (const std::string& warning : warnings) {
        std::printf("config: %s\n", warning.c_str());
    }
    config.resolve_models();
    return config;
}

void print_cards() {
    for (const ComputeDevice& card : gpu_devices()) {
        std::printf("  card %d %s: %s free of %s\n", card.index, card.description.c_str(),
                    format::bytes(card.memory_free).c_str(), format::bytes(card.memory_total).c_str());
    }
}

int bench_load(const std::string& seat, bool cold, bool processor, int tokens) {
    Config config = read_config();
    const bool delegator = seat == "delegator";
    ModelParams params = delegator ? config.router : config.expert(seat);
    params.on_processor = processor;
    if (params.path.empty() || params.remote()) {
        std::printf("%s names no model file here\n", seat.c_str());
        return 2;
    }

    const auto host_start = Clock::now();
    ModelHost host(paths::log_file());
    apply_gpu_policy(config);
    host.set_gpu_config(config.gpu);
    std::printf("host up in %.0f ms\n", ms_since(host_start));
    print_cards();

    if (cold) {
        for (const std::filesystem::path& file : config_files(config)) {
            drop_cached(file);
        }
    }

    std::string error;
    const auto  load_start = Clock::now();
    LoadedModel* model =
        delegator ? host.acquire_router(params, [](float) {}, {}, error)
                  : host.acquire_expert(seat, params, [](float) {}, {}, error);
    const double load_ms = ms_since(load_start);
    if (model == nullptr) {
        std::printf("did not load: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s: %s loaded %s in %.0f ms\n", seat.c_str(), std::filesystem::path(params.path).filename().string().c_str(),
                cold ? "cold" : "warm", load_ms);
    const std::vector<std::uint64_t>& on = model->on_cards();
    for (std::size_t i = 0; i < on.size(); ++i) {
        if (on[i] > 0) {
            std::printf("  on card %zu: %s\n", i, format::bytes(on[i]).c_str());
        }
    }
    print_cards();

    // A short reply, greedy, so two runs are the same work.
    ModelParams asked  = params;
    asked.temperature  = 0.0F;
    asked.seed         = 1234;
    asked.max_tokens   = tokens;
    // Then a long prompt, the size of a delegator's instructions, for how
    // fast one is read.
    std::string story;
    for (int i = 0; i < 40; ++i) {
        story += "The lighthouse keeper wrote down the weather every hour, the wind, the sea and the ships. ";
    }
    for (const std::string& question : {std::string("Count from one to one hundred in words, one per line."),
                                        story + "In one word, what did the keeper write down?"}) {
        const std::string prompt   = model->format_chat({{"user", question}}, true);
        const auto        asked_at = Clock::now();
        double            first_ms = -1.0;
        const GenerationStats stats = model->generate(prompt, asked, [&](std::string_view) {
            if (first_ms < 0.0) {
                first_ms = ms_since(asked_at);
            }
        }, {});
        std::printf("  first token in %.0f ms (prompt of %d tokens in %.0f ms), %d tokens at %.1f a second\n",
                    first_ms, stats.prompt_tokens - stats.prompt_reused, stats.prompt_ms, stats.output_tokens,
                    stats.tokens_per_second());
    }
    return 0;
}

/// Whether the engine has settled after a turn: nothing being answered, and
/// the delegator back, or given up on, as the end of a turn leaves it.
bool settled(const Engine& engine, const Snapshot& now) {
    return !engine.is_busy() && now.mood != Mood::Loading && now.delegator_progress < 0.0F;
}

int bench_chat(const std::vector<std::string>& prompts, bool cold, bool traced, double pause_s) {
    Config config = read_config();
    if (cold) {
        for (const std::filesystem::path& file : config_files(config)) {
            drop_cached(file);
        }
    }
    const std::filesystem::path project = paths::data_dir() / "loadbench-project";
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "loadbench-history");
    engine.start();

    const auto run_start = Clock::now();
    for (const std::string& line : prompts) {
        std::optional<ExpertId> pinned;
        std::string             prompt = line;
        if (line.size() > 1 && line[0] == '@') {
            const std::size_t space = line.find(' ');
            pinned = line.substr(1, space - 1);
            prompt = space == std::string::npos ? std::string() : line.substr(space + 1);
        }
        const std::size_t before = state.snapshot().turns.size();
        const auto        sent   = Clock::now();
        engine.submit(prompt, pinned, {}, true);
        double      first_ms = -1.0;
        std::string said;   // the status line as last printed, for --trace
        const auto  trace = [&](const Snapshot& now) {
            if (traced && now.status != said) {
                said = now.status;
                std::printf("  %+8.0f ms  %s\n", ms_since(sent), said.c_str());
            }
        };
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            const Snapshot now = state.snapshot();
            trace(now);
            if (now.turns.size() <= before) {
                continue;
            }
            const Turn& turn = now.turns.back();
            if (first_ms < 0.0 && !turn.reply.empty()) {
                first_ms = ms_since(sent);
            }
            if (!turn.streaming && !engine.is_busy() && (turn.failed || !turn.reply.empty())) {
                break;
            }
            if (ms_since(sent) > 15 * 60 * 1000.0) {
                std::printf("timed out\n");
                engine.stop();
                return 1;
            }
        }
        const double answered_ms = ms_since(sent);
        // What the end of a turn does next: the delegator brought back, or
        // the expert left. Waited for, so the next prompt is timed alone.
        // A moment first: the engine starts on it just after the turn
        // closes, and until it has there is nothing to see.
        const auto ended = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        while (!settled(engine, state.snapshot()) && ms_since(ended) < 120000.0) {
            trace(state.snapshot());
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const double settle_ms = ms_since(ended);
        const Snapshot after = state.snapshot();
        const Turn&    turn  = after.turns.back();
        std::string loaded;
        for (const LocalModel& model : after.local_models) {
            loaded += (loaded.empty() ? "" : ", ") + model.file;
        }
        // How much it wrote and how much of that was thinking: what a
        // reasoning effort changes.
        std::printf("%-48.48s -> %-14s load %6ld ms  first token %7.0f ms  %5.1f tok/s  answered %7.0f ms  "
                    "settled %6.0f ms  %d tokens, %zu chars thought  [%s]%s\n",
                    line.c_str(), turn.route ? turn.route->expert.c_str() : "?", turn.load_ms, first_ms,
                    turn.tokens_per_second, answered_ms, settle_ms, turn.output_tokens, turn.reasoning.size(),
                    loaded.c_str(), turn.failed ? (" FAILED: " + turn.reply.substr(0, 120)).c_str() : "");
        if (!turn.failed) {
            // The start of what it said, on one line: that it answered, and
            // with something that reads like an answer. Cut where a character
            // ends, so the output stays UTF-8.
            std::string opening = detail::scrub_utf8(std::string_view(turn.reply).substr(0, 200));
            for (char& c : opening) {
                c = c == '\n' || c == '\r' ? ' ' : c;
            }
            std::printf("    said: %s\n", opening.c_str());
        }
        std::fflush(stdout);
        if (pause_s > 0.0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(pause_s * 1000.0)));
        }
    }
    std::printf("all prompts in %.1f s\n", ms_since(run_start) / 1000.0);
    engine.stop();
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() >= 2 && args[0] == "--config") {
        g_config_file = args[1];
        args.erase(args.begin(), args.begin() + 2);
    }
    if (args.size() >= 2 && args[0] == "load") {
        bool cold      = false;
        bool processor = false;
        int  tokens    = 64;
        for (std::size_t i = 2; i < args.size(); ++i) {
            if (args[i] == "--cold") {
                cold = true;
            } else if (args[i] == "--processor") {
                processor = true;
            } else if (args[i] == "--tokens" && i + 1 < args.size()) {
                tokens = std::stoi(args[++i]);
            }
        }
        return bench_load(args[1], cold, processor, tokens);
    }
    if (args.size() >= 2 && args[0] == "chat") {
        bool                     cold   = false;
        bool                     traced = false;
        double                   pause  = 0.0;
        std::vector<std::string> prompts;
        for (std::size_t i = 1; i < args.size(); ++i) {
            if (args[i] == "--cold") {
                cold = true;
            } else if (args[i] == "--trace") {
                traced = true;
            } else if (args[i] == "--pause" && i + 1 < args.size()) {
                pause = std::stod(args[++i]);
            } else {
                prompts.push_back(args[i]);
            }
        }
        return bench_chat(prompts, cold, traced, pause);
    }
    std::printf("usage: crucible-loadbench [--config FILE] load <seat id or delegator> [--cold] [--processor] [--tokens N]\n"
                "       crucible-loadbench [--config FILE] chat [--cold] [--trace] [--pause S] \"a prompt\" \"@seat a prompt\" ...\n");
    return 2;
}
