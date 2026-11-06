// SPDX-License-Identifier: MIT
// crucible-soak -- the engine run for a long while, with no window, the way a
// person uses it: many turns, the seats swapped, a seat picked in the box,
// Stop pressed mid-reply, pictures attached, a build now and then, Eject, the
// orchestrator killed in the middle of a turn, and the API server asked all
// the while. Each turn is a line of what happened and what the
// process held after it, so a leak shows as a column that climbs and a hang
// as a turn that never ends.
//
//   crucible-soak --model a.gguf [--second b.gguf] [--delegator d.gguf]
//                 [--minutes 30] [--picture p.png] [--gpu-layers 0] [--threads 0]
//
// The second seat is the first model with a context of another size when no
// second file is given, which is still a swap: a load with other settings is
// a load. Exits non-zero when a turn hangs or the engine stops answering.
// Linux only: what it measures, and the orchestrator it kills, are read from
// /proc.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <signal.h>
#include <sys/types.h>
#include <unistd.h>

#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/serve/server.hpp"
#include "crucible/tools/attachments.hpp"
#include "crucible/util/http.hpp"
#include "crucible/util/subprocess.hpp"

using namespace crucible;
using Clock = std::chrono::steady_clock;

namespace {

/// A figure from /proc/self/status, in its own unit (kB for memory).
long status_field(const std::string& name) {
    std::ifstream in("/proc/self/status");
    std::string line;
    while (std::getline(in, line)) {
        if (line.rfind(name + ":", 0) == 0) {
            return std::stol(line.substr(name.size() + 1));
        }
    }
    return -1;
}

long open_files() {
    long count = 0;
    std::error_code ec;
    for (std::filesystem::directory_iterator it("/proc/self/fd", ec), end; !ec && it != end; it.increment(ec)) {
        ++count;
    }
    return count;
}

/// What this process holds on the cards, in MiB, as nvidia-smi says: -1
/// where there is no nvidia-smi to ask.
long card_mib() {
    if (!util::on_path("nvidia-smi")) {
        return -1;
    }
    const util::Ran ran = util::run({"nvidia-smi", "--query-compute-apps=pid,used_memory",
                                     "--format=csv,noheader,nounits"}, {}, {}, 10);
    long total = 0;
    const std::string me = std::to_string(::getpid());
    std::size_t at = 0;
    while (at < ran.output.size()) {
        const std::size_t end = ran.output.find('\n', at);
        const std::string line = ran.output.substr(at, end == std::string::npos ? std::string::npos : end - at);
        if (const std::size_t comma = line.find(','); comma != std::string::npos && line.substr(0, comma) == me) {
            total += std::stol(line.substr(comma + 1));
        }
        at = end == std::string::npos ? ran.output.size() : end + 1;
    }
    return total;
}

/// The orchestrator, by the pid /proc gives this process's children: 0 when
/// it is not running.
long orchestrator_pid() {
    std::error_code ec;
    for (const auto& task : std::filesystem::directory_iterator("/proc/self/task", ec)) {
        std::ifstream children(task.path() / "children");
        long pid = 0;
        while (children >> pid) {
            std::ifstream in("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
            const std::string command{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
            if (command.find("crucible_orchestrator") != std::string::npos) {
                return pid;
            }
        }
    }
    return 0;
}

const char* const kQuestions[] = {
    "What is 17 times 23? Show the steps.",
    "Write two sentences about why the sky is blue.",
    "List three prime numbers larger than 100.",
    "Explain what a hash table is in one short paragraph.",
    "Write a haiku about a kettle.",
    "What is the derivative of x cubed plus 2x?",
    "Name the planets in order from the Sun.",
    "Give a one line Python function that reverses a string.",
};

/// The first `most` bytes of `text`, ending on a whole character, so a reply
/// cut short does not leave the log half a UTF-8 sequence that tools then
/// take the whole file for binary over.
std::string start_of(const std::string& text, std::size_t most) {
    std::size_t end = std::min(most, text.size());
    while (end > 0 && end < text.size() && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end);
}

/// How a turn ended, as the transcript has it.
std::string outcome_of(const Turn& turn) {
    if (!turn.build.empty()) {
        return "build";
    }
    return turn.failed ? "failed" : turn.canceled ? "canceled" : "finished";
}

}  // namespace

int main(int argc, char** argv) {
    std::map<std::string, std::string> flags;
    for (int i = 1; i + 1 < argc; i += 2) {
        flags[argv[i]] = argv[i + 1];
    }
    if (flags.count("--model") == 0) {
        std::printf("usage: crucible-soak --model a.gguf [--second b.gguf] [--delegator d.gguf]\n"
                    "                     [--minutes 30] [--picture p.png] [--gpu-layers 0] [--threads 0]\n");
        return 2;
    }
    const std::string first   = std::filesystem::absolute(flags["--model"]).string();
    const std::string second  = flags.count("--second") ? std::filesystem::absolute(flags["--second"]).string() : first;
    const double      minutes = flags.count("--minutes") ? std::stod(flags["--minutes"]) : 30.0;
    const std::string picture = flags.count("--picture") ? std::filesystem::absolute(flags["--picture"]).string() : "";
    const int         layers  = flags.count("--gpu-layers") ? std::stoi(flags["--gpu-layers"]) : 0;
    const int         threads = flags.count("--threads") ? std::stoi(flags["--threads"]) : 0;

    Config config;
    if (flags.count("--delegator")) {
        config.router.model        = std::filesystem::absolute(flags["--delegator"]).string();
        config.router.n_gpu_layers = layers;
        config.router.n_threads    = threads;
        config.router.n_ctx        = 2048;
    }
    config.routing.keep_delegator_loaded = false;   // freed and loaded again around every route
    config.tools.auto_edits   = true;
    config.build.confirm_plan = false;
    config.build.agents       = 2;
    config.build.rounds_per_task = 6;
    std::string error;
    for (const auto& [name, blurb] :
         {std::pair<const char*, const char*>{"Mathematics", "Arithmetic, algebra, calculus and proofs"},
          {"Writing", "Prose, poems, explanations and code"}}) {
        Expert seat;
        seat.name  = name;
        seat.blurb = blurb;
        if (!config.roster.add(seat, error)) {
            std::printf("could not add %s: %s\n", name, error.c_str());
            return 1;
        }
    }
    std::vector<ExpertId> seats;
    for (const Expert& seat : config.roster.experts()) {
        ModelParams& params = config.experts[seat.id];
        params.model        = seats.empty() ? first : second;
        params.n_gpu_layers = layers;
        params.n_threads    = threads;
        params.n_ctx        = seats.empty() ? 4096 : 3072;   // never the same load twice
        params.max_tokens   = 160;
        seats.push_back(seat.id);
    }
    // The API server, serving the first seat's model on a free port: asked
    // from a thread of its own all the while, so a request and a turn want
    // the same model at once.
    config.server.enabled = true;
    config.server.port    = 0;
    config.server.model   = first;
    config.resolve_models();

    const std::filesystem::path project = paths::data_dir() / "soak-project";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "soak-history");
    engine.start();

    const auto started  = Clock::now();
    const auto deadline = started + std::chrono::duration<double, std::ratio<60>>(minutes);

    std::atomic<bool>          asking{true};
    std::map<std::string, int> served;   // by how each request came back
    std::mutex                 served_mutex;
    std::thread                client([&] {
        while (asking.load()) {
            const serve::Status status = engine.server_status();
            if (status.listening && !status.model.empty()) {
                util::http::Request request;
                request.method = "POST";
                request.url    = "http://127.0.0.1:" + std::to_string(status.port) + "/v1/chat/completions";
                request.headers.push_back({"Content-Type", "application/json"});
                request.body = nlohmann::json{{"model", status.model}, {"max_tokens", 24},
                                              {"messages", {{{"role", "user"}, {"content", "Say hi in three words."}}}}}
                                   .dump();
                request.timeout_seconds = 600;
                const util::http::Response response = util::http::send(request, [&] { return !asking.load(); });
                const std::lock_guard<std::mutex> lock(served_mutex);
                ++served[response.error.empty() ? std::to_string(response.status) : "error"];
            }
            for (int i = 0; i < 30 && asking.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    });
    std::map<std::string, int> tally;
    int  hangs = 0;
    long heap_first = -1;
    long heap_most  = 0;
    // Resident memory moves by a model's size as one is loaded and let go;
    // what a leak would grow is the anonymous part, the heap, which the
    // weights are not: they are a mapping of their file.
    std::printf("turn,seconds,action,outcome,turn_ms,reply_chars,rss_mib,heap_mib,card_mib,threads,files\n");
    for (int turn = 0; Clock::now() < deadline && hangs == 0; ++turn) {
        const int step = turn % 12;
        std::string action;
        std::vector<attach::Attachment> attachments;
        std::optional<ExpertId> pinned = seats[static_cast<std::size_t>(turn) % 2];
        bool stop_mid = false;
        bool kill_mid = false;
        switch (step) {
            case 2: pinned.reset(); action = "routed"; break;
            case 3: stop_mid = true; action = "stop"; break;
            case 5: kill_mid = true; action = "kill-orchestrator"; break;
            // A seat picked in the box loads on a thread of its own, and the
            // prompt sent to it the same moment waits on that load.
            case 6: engine.choose(pinned, /*load=*/true); action = "chosen"; break;
            case 7: pinned.reset(); action = "routed-new-chat"; break;
            case 8: engine.choose(std::nullopt, /*load=*/false); action = "unchosen"; break;
            case 9: engine.release_all(); action = "eject"; break;
            default: action = "pinned"; break;
        }
        if ((step == 4 || step == 10) && !picture.empty()) {
            attach::Attachment one;
            one.path  = picture;
            one.name  = std::filesystem::path(picture).filename().string();
            one.label = "PNG";
            one.kind  = attach::Kind::Image;
            one.image = attach::picture(picture, 8U << 20);
            attachments.push_back(std::move(one));
            action = "picture";
        }

        // A build every so often, with the chat asked beside it.
        const bool build = turn % 24 == 23;
        if (build) {
            action = "build";
            const std::string refused = engine.start_cook(
                "Make a single index.html page that shows the current time, updated every second.", 0,
                project, {}, std::nullopt, "build");
            if (!refused.empty()) {
                std::printf("# build refused: %s\n", refused.c_str());
            }
        }

        const std::size_t before = state.snapshot().turns.size();
        const auto        asked  = Clock::now();
        engine.submit(kQuestions[static_cast<std::size_t>(turn) % std::size(kQuestions)], pinned,
                      std::move(attachments));
        bool acted = false;
        for (;;) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const Snapshot now = state.snapshot();
            const bool begun = now.turns.size() > before;
            if (begun && !acted && !now.turns.back().reply.empty()) {
                if (stop_mid) {
                    engine.cancel();
                    acted = true;
                } else if (kill_mid) {
                    if (const long pid = orchestrator_pid(); pid > 0) {
                        ::kill(static_cast<pid_t>(pid), SIGKILL);
                    }
                    acted = true;
                }
            }
            const bool cooking = build && engine.cooking();
            if (begun && !now.turns.back().streaming && !engine.is_busy() && !cooking) {
                break;
            }
            // A build is stopped after a few minutes; a turn that has not
            // ended in ten is a hang.
            if (cooking && Clock::now() - asked > std::chrono::minutes(4)) {
                engine.cancel_cook();
            }
            if (Clock::now() - asked > std::chrono::minutes(10)) {
                std::printf("# HANG at turn %d (%s): busy %d, cooking %d, mood status '%s'\n", turn,
                            action.c_str(), engine.is_busy() ? 1 : 0, engine.cooking() ? 1 : 0,
                            now.status.c_str());
                ++hangs;
                break;
            }
        }
        const Snapshot after = state.snapshot();
        const Turn&    last  = after.turns.back();
        const std::string outcome = outcome_of(last);
        ++tally[action + ":" + outcome];
        if (last.failed) {
            std::printf("# turn %d failed: %s\n", turn, start_of(last.reply, 200).c_str());
        }
        // The turn of a build is the question asked beside it; the build
        // says how it went in its journal.
        if (build && after.cook) {
            const std::string how(cook_state_name(after.cook->state));
            ++tally["build-journal:" + how];
            std::printf("# build %s: %s\n", how.c_str(), start_of(after.cook->outcome, 200).c_str());
        }
        const long heap = status_field("RssAnon") / 1024;
        heap_first = heap_first < 0 ? heap : heap_first;
        heap_most  = std::max(heap_most, heap);
        std::printf("%d,%.0f,%s,%s,%lld,%zu,%ld,%ld,%ld,%ld,%ld\n", turn,
                    std::chrono::duration<double>(Clock::now() - started).count(), action.c_str(),
                    outcome.c_str(),
                    static_cast<long long>(std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - asked).count()),
                    last.reply.size(), status_field("VmRSS") / 1024, heap, card_mib(), status_field("Threads"),
                    open_files());
        std::fflush(stdout);

        // A new conversation now and then, as the window starts one.
        if (step == 7) {
            engine.reset_history();
            state.clear_turns();
        }
    }
    asking = false;
    client.join();
    engine.stop();
    std::printf("# summary:");
    for (const auto& [what, count] : tally) {
        std::printf(" %s=%d", what.c_str(), count);
    }
    std::printf("\n# the server answered:");
    for (const auto& [how, count] : served) {
        std::printf(" %s=%d", how.c_str(), count);
    }
    std::printf("\n# heap first %ld MiB, most %ld MiB, at the end %ld MiB; hangs %d\n", heap_first, heap_most,
                status_field("RssAnon") / 1024, hangs);
    return hangs == 0 ? 0 : 1;
}
