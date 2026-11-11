// SPDX-License-Identifier: MIT
// crucible-smoke -- one prompt through the whole engine, with no window.
//
//   crucible-smoke <delegator.gguf or -> <expert.gguf> "a question"
//   crucible-smoke --build <expert.gguf> "a directive" ["a question"]
//
// Everything a prompt typed into the window goes through, except the window:
// the orchestrator started on Crucible's Python (or CRUCIBLE_PYTHON), the
// route, the delegator and the expert loaded and freed, the answer generated
// in a scratch project folder. It prints the route and the reply, and exits
// non-zero when the turn failed -- so a platform's whole prompt path can be
// run where no window can be opened: a CI runner, or Windows code under Wine.
//
// With --build it runs a build of the directive instead, with up to three
// agents, and -- when a question is given -- asks it in the chat while the
// build is working: the two at once, through the same engine, which is the
// path a person takes who chats while a build runs. It prints the tasks and
// the reply, and exits non-zero when the build failed outright or the chat
// turn did.
//
//   crucible-smoke --bench <model.gguf> [quick|standard|full] [mmlu_pro,arc,...] [seconds]
//
// With --bench it runs the model through the Bench the way the Bench tab
// does, and prints how far it has got every ten seconds and the result at
// the end: how fast a run goes on this machine, timed without a window.
// Given seconds, it presses Stop after that long, and what was answered by
// then is kept as a stopped run's is: a sample of a model too slow to run
// through whole.
//
//   crucible-smoke --runtime cpu
//   crucible-smoke --host <first.gguf> [<second.gguf>]
//   crucible-smoke --app [seconds]
//
// What CI runs on each system, on a runner that is a real machine of it.
// --runtime puts one runtime in place with the installer's own code, which
// downloads the one published for this llama.cpp and platform and compiles
// it only when there is none: the same module an installed Crucible runs.
// --host then loads models on whatever is installed, through the model host
// as a prompt does, swaps them and lets them go: see smoke_host. --app makes
// a page into a program the way Make app does and opens it with
// --self-test, which reloads it in its own window: see smoke_app.

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/app/setup_runtimes.hpp"
#include "crucible/config/config.hpp"
#include "crucible/config/paths.hpp"
#include "crucible/cook/journal.hpp"
#include "crucible/engine/engine.hpp"
#include "crucible/engine/state.hpp"
#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/routing/expert.hpp"
#include "crucible/routing/router.hpp"
#include "crucible/runtime/backend.hpp"
#include "crucible/tools/apps.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/subprocess.hpp"

using namespace crucible;

namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point from) {
    return std::chrono::duration<double, std::milli>(Clock::now() - from).count();
}

/// The last `count` lines of `file`, printed: what llama.cpp said before a
/// step failed, which the model host writes there rather than to a terminal.
void print_tail(const std::filesystem::path& file, std::size_t count) {
    std::ifstream            in(file);
    std::vector<std::string> lines;
    for (std::string line; std::getline(in, line);) {
        lines.push_back(line);
    }
    std::printf("the last of %s:\n", file.string().c_str());
    for (std::size_t i = lines.size() > count ? lines.size() - count : 0; i < lines.size(); ++i) {
        std::printf("  %s\n", lines[i].c_str());
    }
}

/// The runtime `id` put in place as `crucible --install-runtimes` puts it,
/// with the same code, and it alone. See the file comment.
int smoke_runtime(const std::string& id) {
    const std::optional<BackendKind> kind = backend_from_id(id);
    if (!kind) {
        std::printf("no runtime called %s\n", id.c_str());
        return 2;
    }
    const int status = run_runtime_setup(/*quiet=*/false, /*force=*/false, {*kind});
    // 2 is "already in place", which a second run here finds.
    return status == 0 || status == 2 ? 0 : 1;
}

/// What a model says to `prompt`, greedy, so that the same model says the
/// same thing every time.
struct Said {
    std::string     text;
    GenerationStats stats;
};

Said ask(LoadedModel& model, const std::string& prompt, int tokens) {
    ModelParams params;
    params.temperature = 0.0F;
    params.seed        = 1234;
    params.max_tokens  = tokens;
    Said said;
    said.stats = model.generate(prompt, params, [&said](std::string_view chunk) { said.text += chunk; }, {});
    return said;
}

/// The model host alone, as the engine drives it, on whatever runtimes are
/// installed. `first` is loaded and asked to go on with a story, `second` is
/// loaded beside it and asked, and `first` asked for again is held rather
/// than loaded. Both are let go, and `first` loaded again has to tell the
/// same story word for word. Then `second` is the delegator and scores two
/// labels as routing does, and is let go. Each step is said, and one that
/// does not hold fails the run: a platform's whole loading path, on models
/// small enough for any machine. With one file, the second load is the same
/// file with a context of another size, which is still a load of its own.
int smoke_host(const std::string& first_file, const std::string& second_file) {
    int failures = 0;
    const auto held = [&failures](bool ok, const std::string& what) {
        if (!ok) {
            std::printf("FAILED: %s\n", what.c_str());
            ++failures;
        }
        return ok;
    };
    const auto name_of = [](const ModelParams& params) {
        return std::filesystem::path(params.path).filename().string();
    };

    ModelParams first;
    first.model = first.path = std::filesystem::absolute(first_file).string();
    ModelParams second;
    second.model = second.path = std::filesystem::absolute(second_file).string();
    if (second.path == first.path) {
        second.n_ctx = first.n_ctx / 2;
    }
    const std::string story = "Once upon a time";

    auto       at = Clock::now();
    ModelHost  host(paths::log_file());
    std::string devices;
    for (const std::string& device : ModelHost::devices()) {
        devices += (devices.empty() ? "" : ", ") + device;
    }
    std::printf("host up in %.0f ms, on %s\n", ms_since(at), devices.empty() ? "no device" : devices.c_str());
    if (!held(!devices.empty(), "no runtime is loaded: install one with crucible-smoke --runtime cpu")) {
        print_tail(paths::log_file(), 20);
        return 1;
    }

    std::string error;
    at = Clock::now();
    LoadedModel* one = host.acquire_expert("first", first, {}, {}, error);
    if (!held(one != nullptr, name_of(first) + " did not load: " + error)) {
        print_tail(paths::log_file(), 20);
        return 1;
    }
    std::printf("%s loaded in %.0f ms, %s\n", name_of(first).c_str(), ms_since(at),
                format::bytes(one->bytes()).c_str());
    const Said told = ask(*one, story, 24);
    std::printf("  %d tokens at %.0f a second: %s%s\n", told.stats.output_tokens, told.stats.tokens_per_second(),
                story.c_str(), told.text.c_str());
    held(told.stats.output_tokens > 0 && !told.text.empty(), name_of(first) + " said nothing");

    at = Clock::now();
    LoadedModel* two = host.acquire_expert("second", second, {}, {}, error);
    if (!held(two != nullptr && two != one, name_of(second) + " did not load beside it: " + error)) {
        print_tail(paths::log_file(), 20);
        return 1;
    }
    std::printf("%s loaded in %.0f ms, %s, beside it: %zu models held, %s\n", name_of(second).c_str(),
                ms_since(at), format::bytes(two->bytes()).c_str(), host.expert_count(),
                format::bytes(host.resident_bytes()).c_str());
    held(host.expert_count() == 2, "the first model was let go to make room for a second this small");
    const Said other = ask(*two, story, 24);
    std::printf("  %d tokens at %.0f a second: %s%s\n", other.stats.output_tokens,
                other.stats.tokens_per_second(), story.c_str(), other.text.c_str());
    held(other.stats.output_tokens > 0 && !other.text.empty(), name_of(second) + " said nothing");

    at = Clock::now();
    LoadedModel* again = host.acquire_expert("first", first, {}, {}, error);
    std::printf("%s asked for again: %s in %.0f ms\n", name_of(first).c_str(),
                again == one ? "held, not loaded" : "loaded again", ms_since(at));
    held(again == one, "the first model was loaded again while it was still held");

    host.release_experts();
    std::printf("both let go: %zu held, %s\n", host.expert_count(), format::bytes(host.resident_bytes()).c_str());
    held(host.expert_count() == 0 && host.resident_bytes() == 0, "the models were still held after letting go");

    at = Clock::now();
    LoadedModel* back = host.acquire_expert("first", first, {}, {}, error);
    if (!held(back != nullptr, name_of(first) + " did not load again: " + error)) {
        print_tail(paths::log_file(), 20);
        return 1;
    }
    std::printf("%s loaded again in %.0f ms\n", name_of(first).c_str(), ms_since(at));
    const Said retold = ask(*back, story, 24);
    held(retold.text == told.text, "loaded again, it told another story: " + retold.text);
    host.release_experts();

    at = Clock::now();
    LoadedModel* delegator = host.acquire_router(second, {}, {}, error);
    if (!held(delegator != nullptr, name_of(second) + " did not load as the delegator: " + error)) {
        print_tail(paths::log_file(), 20);
        return 1;
    }
    std::printf("%s loaded as the delegator in %.0f ms\n", name_of(second).c_str(), ms_since(at));
    // Labels as routing writes them, a seat's name with no space before it.
    const std::vector<float> scores =
        delegator->score_labels(story + ", there was a little", {"girl", "dragon"}, {});
    held(scores.size() == 2 && scores[0] > kUnscored && scores[1] > kUnscored, "the delegator scored no labels");
    if (scores.size() == 2) {
        std::printf("  scored \"girl\" %.2f and \"dragon\" %.2f\n", static_cast<double>(scores[0]),
                    static_cast<double>(scores[1]));
    }
    host.release_router();
    held(host.router() == nullptr, "the delegator was still held after letting it go");

    if (failures != 0) {
        print_tail(paths::log_file(), 20);
    }
    std::printf("%s\n", failures == 0 ? "every step held" : "some steps did not hold");
    return failures == 0 ? 0 : 1;
}

/// A page made into a program the way the Preview's Make app makes one, and
/// that program opened with --self-test, which loads its page in a window of
/// its own, reloads it as a page does in one click, and exits 0 only when the
/// page was served from the program's own address both times and found what
/// it had kept (tools/app_runner.cpp). What it found is printed, and its exit
/// status is this one's. The program goes where a person's would, unless
/// CRUCIBLE_APPS_DIR names somewhere else.
int smoke_app(int seconds) {
    const std::filesystem::path project = paths::data_dir() / "smoke-app";
    std::error_code ec;
    std::filesystem::remove_all(project, ec);
    std::filesystem::create_directories(project, ec);
    const auto write = [&project](const char* name, const char* text) {
        std::ofstream out(project / name, std::ios::binary | std::ios::trunc);
        out << text;
    };
    // A page of the kind a build makes: its title is the program's name, and
    // its stylesheet and script are files of their own until it is packaged.
    write("index.html",
          "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>Crucible Smoke App</title>"
          "<link rel=\"stylesheet\" href=\"style.css\"></head>\n"
          "<body><h1>Clicks</h1><p id=\"count\">0</p><button id=\"add\">Add</button>"
          "<script src=\"app.js\"></script></body></html>\n");
    write("style.css", "body { font: 16px sans-serif; padding: 2rem; }\n");
    write("app.js",
          "var count = Number(localStorage.getItem('count') || 0);\n"
          "function show() { document.getElementById('count').textContent = count; }\n"
          "document.getElementById('add').addEventListener('click', function () {\n"
          "  count += 1; localStorage.setItem('count', String(count)); show();\n"
          "});\nshow();\n");

    std::string error;
    const std::optional<tools::apps::Made> made = tools::apps::package(project, "index.html", "", error);
    if (!made) {
        std::printf("could not make the program: %s\n", error.c_str());
        return 1;
    }
    std::printf("made %s, %s\n", made->name.c_str(), made->where.c_str());
    std::filesystem::path program = made->launch;
#if defined(__APPLE__)
    // A bundle is opened by the program inside it, which is what is run here
    // so that it can be given the flag and its exit status read.
    if (std::filesystem::is_directory(program, ec)) {
        program = program / "Contents" / "MacOS" / tools::apps::detail::slug(made->name);
    }
#endif
    const std::filesystem::path report = paths::data_dir() / "smoke-app-report.txt";
    std::filesystem::remove(report, ec);
    const auto      at  = Clock::now();
    const util::Ran ran = util::run({program.string(), "--self-test", report.string(), std::to_string(seconds)},
                                    program.parent_path(), {}, seconds + 30);
    std::ifstream in(report);
    const std::string said((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::printf("%s", said.empty() ? "it wrote no report\n" : said.c_str());
    if (!ran.started) {
        std::printf("could not start %s: %s\n", program.string().c_str(), ran.error.c_str());
        return 1;
    }
    const bool passed = ran.status == 0 && !ran.timed_out;
    std::printf("%s exited with %d after %.1f s%s\n", program.filename().string().c_str(), ran.status,
                ms_since(at) / 1000.0, ran.timed_out ? ", stopped for taking too long" : "");
    if (!passed && !ran.output.empty()) {
        std::printf("what it printed:\n%s\n", ran.output.c_str());
    }
    return passed ? 0 : 1;
}

/// A build of `directive` on `expert`, with `question` asked in the chat
/// while it works when there is one. See the file comment.
int smoke_build(const std::string& expert, const std::string& directive, const std::string& question) {
    Config config;
    config.routing.keep_delegator_loaded = false;
    config.build.confirm_plan = false;
    config.build.agents       = 3;
    config.tools.auto_edits   = true;
    std::string error;
    for (const auto& [name, blurb] :
         {std::pair<const char*, const char*>{"Programming", "Code, programs, scripts and tests"},
          {"Writing", "Documentation, READMEs and prose"}}) {
        Expert seat;
        seat.name  = name;
        seat.blurb = blurb;
        if (!config.roster.add(seat, error)) {
            std::printf("could not add %s: %s\n", name, error.c_str());
            return 1;
        }
    }
    for (const Expert& seat : config.roster.experts()) {
        config.experts[seat.id].model = std::filesystem::absolute(expert).string();
    }
    config.resolve_models();

    const std::filesystem::path project = paths::data_dir() / "smoke-build";
    std::filesystem::remove_all(project);
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "smoke-build-history");
    engine.start();
    if (const std::string refused = engine.start_cook(directive, 0, project, {}, std::nullopt, "build");
        !refused.empty()) {
        std::printf("refused: %s\n", refused.c_str());
        return 1;
    }

    // The question, once the build has its plan: asked while its agents work.
    bool asked = question.empty();
    const auto started = std::chrono::steady_clock::now();
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        if (!asked && now.cook && !now.cook->tasks.empty()) {
            std::printf("asking the chat while the build works: %s\n", question.c_str());
            engine.submit(question);
            asked = true;
        }
        const bool chat_done = question.empty()
                            || (!now.turns.empty() && !now.turns.back().streaming && !engine.is_busy());
        if (asked && chat_done && !engine.cooking()) {
            break;
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(30)) {
            std::printf("timed out\n");
            engine.cancel_cook();
            engine.stop();
            return 1;
        }
    }
    const Snapshot after = state.snapshot();
    int failed = 0;
    if (after.cook) {
        std::printf("build: %s -- %s\n", std::string(cook_state_name(after.cook->state)).c_str(),
                    after.cook->outcome.substr(0, 400).c_str());
        for (const CookTask& task : after.cook->tasks) {
            std::printf("  task %d %-10s %s (%s)\n", task.index + 1, task.state.c_str(),
                        task.title.c_str(), task.expert.c_str());
        }
        failed = after.cook->state == CookState::Failed ? 1 : 0;
    }
    if (!question.empty() && !after.turns.empty()) {
        const Turn& turn = after.turns.back();
        std::printf("chat %s: %s\n", turn.failed ? "failed" : "reply", turn.reply.substr(0, 400).c_str());
        failed = failed || turn.failed || turn.reply.empty() ? 1 : 0;
    }
    engine.stop();
    return failed;
}

/// `model` through the Bench at `size`, on the benchmarks named in `only`
/// (comma separated) or every one, stopped after `stop_after` seconds when
/// that is more than none. See the file comment.
int smoke_bench(const std::string& model, const std::string& size, const std::string& only, double stop_after) {
    using nlohmann::json;
    Config config;
    config.routing.keep_delegator_loaded = false;
    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.start();

    json benchmarks = json::array();
    std::stringstream names(only);
    for (std::string name; std::getline(names, name, ',');) {
        if (!name.empty()) {
            benchmarks.push_back(name);
        }
    }
    const std::string path = std::filesystem::absolute(model).string();
    if (const std::string refused = engine.start_bench(json{
            {"models", json::array({json{{"path", path}, {"name", std::filesystem::path(path).filename().string()}}})},
            {"benchmarks", benchmarks}, {"size", size}, {"effort", ""}});
        !refused.empty()) {
        std::printf("refused: %s\n", refused.c_str());
        return 1;
    }

    // Done is what the run says, once the engine has let it go.
    const auto            started = std::chrono::steady_clock::now();
    auto                  said    = started;
    json                  last;
    std::set<std::string> finished;
    bool                  pressed = false;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        if (now.bench) {
            last = *now.bench;
        }
        if (!engine.benching() && last.is_object() && !last.value("running", true)) {
            break;
        }
        const auto at = std::chrono::steady_clock::now();
        if (stop_after > 0 && !pressed && std::chrono::duration<double>(at - started).count() >= stop_after) {
            pressed = true;
            std::printf("%6.1f s  stop\n", std::chrono::duration<double>(at - started).count());
            engine.stop_bench();
        }
        // When each benchmark was done, to the quarter second.
        for (const json& entry : last.is_object() ? last.value("models", json::array()) : json::array()) {
            const json benches = entry.value("benchmarks", json::object());
            for (const auto& [id, progress] : benches.items()) {
                if (progress.value("n", 0) > 0 && progress.value("done", 0) == progress.value("n", 0)
                    && finished.insert(id).second) {
                    std::printf("%6.1f s  %s done\n", std::chrono::duration<double>(at - started).count(), id.c_str());
                    std::fflush(stdout);
                }
            }
        }
        if (at - said >= std::chrono::seconds(10) && last.is_object()) {
            said = at;
            int done = 0;
            int correct = 0;
            std::string where;
            for (const json& entry : last.value("models", json::array())) {
                // Held here: items() of the temporary value() returns would outlive it.
                const json benches = entry.value("benchmarks", json::object());
                for (const auto& [id, progress] : benches.items()) {
                    done += progress.value("done", 0);
                    correct += progress.value("correct", 0);
                }
                where = entry.value("state", std::string()) + " " + entry.value("current", std::string());
            }
            const double seconds = std::chrono::duration<double>(at - started).count();
            std::printf("%6.0f s  %s  %d done, %d right\n", seconds, where.c_str(), done, correct);
            std::fflush(stdout);
        }
    }
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    int failed = last.contains("error") ? 1 : 0;
    if (failed != 0) {
        std::printf("error: %s\n", last["error"].dump().c_str());
    }
    for (const json& entry : last.value("models", json::array())) {
        std::printf("%s: %s, loaded in %ld ms, %s\n", entry.value("name", std::string()).c_str(),
                    entry.value("state", std::string()).c_str(), entry.value("load_ms", 0L),
                    entry.value("saved", std::string()).c_str());
        const json benches = entry.value("benchmarks", json::object());
        for (const auto& [id, progress] : benches.items()) {
            std::printf("  %-11s %3d of %3d, %3d right%s\n", id.c_str(), progress.value("done", 0),
                        progress.value("n", 0), progress.value("correct", 0),
                        progress.contains("error") ? (", " + progress["error"].dump()).c_str() : "");
        }
        if (entry.value("state", std::string()) != "done") {
            failed = 1;
        }
    }
    std::printf("%.0f s in all\n", seconds);
    engine.stop();
    return failed;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc >= 3 && std::string(argv[1]) == "--runtime") {
        return smoke_runtime(argv[2]);
    }
    if (argc >= 3 && std::string(argv[1]) == "--host") {
        return smoke_host(argv[2], argc >= 4 ? argv[3] : argv[2]);
    }
    if (argc >= 2 && std::string(argv[1]) == "--app") {
        return smoke_app(argc >= 3 ? std::atoi(argv[2]) : 60);
    }
    if (argc >= 4 && std::string(argv[1]) == "--build") {
        return smoke_build(argv[2], argv[3], argc >= 5 ? argv[4] : "");
    }
    if (argc >= 3 && std::string(argv[1]) == "--bench") {
        return smoke_bench(argv[2], argc >= 4 ? argv[3] : "quick", argc >= 5 ? argv[4] : "",
                           argc >= 6 ? std::atof(argv[5]) : 0.0);
    }
    if (argc < 4) {
        std::printf("usage: crucible-smoke <delegator.gguf or -> <expert.gguf> \"a question\"\n"
                    "       crucible-smoke --build <expert.gguf> \"a directive\" [\"a question\"]\n"
                    "       crucible-smoke --bench <model.gguf> [quick|standard|full] [mmlu_pro,arc,...] [seconds]\n"
                    "       crucible-smoke --runtime cpu\n"
                    "       crucible-smoke --host <first.gguf> [<second.gguf>]\n"
                    "       crucible-smoke --app [seconds]\n");
        return 2;
    }
    const std::string delegator = argv[1];
    const std::string expert    = argv[2];
    const std::string prompt    = argv[3];

    Config config;
    if (delegator != "-") {
        config.router.model = std::filesystem::absolute(delegator).string();
    }
    config.routing.keep_delegator_loaded = false;
    std::string error;
    for (const auto& [name, blurb] :
         {std::pair<const char*, const char*>{"Mathematics", "Proofs, algebra, calculus and arithmetic"},
          {"Chemistry", "Reactions, bonding, acids and the periodic table"}}) {
        Expert seat;
        seat.name  = name;
        seat.blurb = blurb;
        if (!config.roster.add(seat, error)) {
            std::printf("could not add %s: %s\n", name, error.c_str());
            return 1;
        }
    }
    for (const Expert& seat : config.roster.experts()) {
        config.experts[seat.id].model = std::filesystem::absolute(expert).string();
        // One seed rather than a new one each run, so the same prompt gets
        // the same reply from the same build: what lets two builds' answers
        // be compared word for word.
        config.experts[seat.id].seed = 1234;
    }
    config.resolve_models();

    const std::filesystem::path project = paths::data_dir() / "smoke-project";
    std::filesystem::create_directories(project);

    AppState state;
    state.configure_seats(config);
    Engine engine(config, state, [] {});
    engine.set_project(project, paths::data_dir() / "smoke-history");
    engine.start();
    engine.submit(prompt);

    // The turn is over when it has stopped streaming and the engine is idle.
    const auto started = std::chrono::steady_clock::now();
    std::shared_ptr<const PendingEdit> allowed;
    for (;;) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        const Snapshot now = state.snapshot();
        // A write waits for a yes, and there is nobody at a window to give
        // it: it is given here, as a person would, and said.
        if (now.pending_edit && now.pending_edit != allowed) {
            allowed = now.pending_edit;
            std::printf("edit: %s (allowed)\n", allowed->path.c_str());
            engine.approve_edit(true);
        }
        const bool done = !now.turns.empty() && !now.turns.back().streaming && !engine.is_busy();
        if (done) {
            break;
        }
        if (std::chrono::steady_clock::now() - started > std::chrono::minutes(10)) {
            std::printf("timed out\n");
            engine.stop();
            return 1;
        }
    }
    const Snapshot after = state.snapshot();
    const Turn& turn = after.turns.back();
    if (turn.route) {
        std::printf("route: %s (%s, %.2f) %s\n", turn.route->expert.c_str(),
                    std::string(route_source_name(turn.route->source)).c_str(),
                    static_cast<double>(turn.route->confidence), turn.route->detail.c_str());
    }
    // What the turn did besides talk -- a file read, a command run -- so a
    // turn that used a tool can be told from one that did not.
    for (const TurnAction& action : turn.actions) {
        std::printf("action: %s\n", action.summary.c_str());
    }
    std::printf("%s: %s\n", turn.failed ? "failed" : "reply", turn.reply.c_str());
    engine.stop();
    return turn.failed || turn.reply.empty() ? 1 : 0;
}
