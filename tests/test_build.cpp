// SPDX-License-Identifier: MIT
//
// A build's journal, and what the tools beyond the folder parse and refuse.
//
// The build loop itself is Python and is tested there, against a scripted
// core. What is here is the core's side of it: the journal a build is kept
// in, with its plan and its tasks; the new verbs the workshop reads; the
// settings that gate them; and the small pure functions under the screen
// and version control tools, which are the parts of those that can be wrong
// without a screen or a repository to try them on.
#include "test_helpers.hpp"

#include <atomic>
#include <fstream>
#include <iterator>
#include <chrono>
#include <thread>

#include "crucible/api/surface.hpp"
#include "crucible/engine/leases.hpp"
#include "crucible/tools/computer.hpp"
#include "crucible/tools/fetch.hpp"
#include "crucible/tools/git.hpp"

namespace {

tools::WorkshopSettings everything_on(const std::filesystem::path& root) {
    tools::WorkshopSettings settings;
    settings.enabled          = true;
    settings.root             = root;
    settings.web              = true;
    settings.computer_control = true;
    return settings;
}

}  // namespace

// ---------------------------------------------------------------------------
// The journal
// ---------------------------------------------------------------------------

TEST(a_build_round_trips_with_its_plan_and_tasks) {
    TempDir dir;
    const CookLog log(dir.path());

    Cook build;
    build.id           = CookLog::new_id();
    build.kind         = "build";
    build.goal         = "a todo CLI";
    build.state        = CookState::Done;
    build.plan.summary = "A Python CLI with tests.";
    build.plan.run     = "python todo.py";
    build.plan.check   = "pytest -q";
    CookTask first;
    first.index   = 0;
    first.title   = "Write todo.py";
    first.detail  = "adds and lists";
    first.needs   = "Python back end";
    first.files   = {"todo.py"};
    first.expert  = "programming";
    first.state   = "done";
    first.outcome = "wrote it";
    CookTask second;
    second.index = 1;
    second.title = "Tests";
    second.after = {0};
    second.state = "skipped";
    build.tasks  = {first, second};
    CookStep step = step_of(1, "programming", "write", "created todo.py", true, 3, {"todo.py"});
    step.task     = 0;
    step.picture  = "/tmp/shot.png";
    build.steps.push_back(step);

    std::string error;
    CHECK(log.save(build, error));
    const std::optional<Cook> back = log.load(build.id);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK(back->is_build());
    CHECK_EQ(back->plan.check, std::string("pytest -q"));
    CHECK_EQ(back->tasks.size(), std::size_t{2});
    CHECK_EQ(back->tasks[0].needs, std::string("Python back end"));
    CHECK_EQ(back->tasks[0].files.size(), std::size_t{1});
    CHECK_EQ(back->tasks[1].after.size(), std::size_t{1});
    CHECK_EQ(back->tasks[1].state, std::string("skipped"));
    CHECK_EQ(back->steps[0].task, 0);
    CHECK_EQ(back->steps[0].picture, std::string("/tmp/shot.png"));

    // The list says what kind each is, so History can tell a build from a cook.
    const std::vector<CookSummary> listed = log.list();
    CHECK_EQ(listed.size(), std::size_t{1});
    CHECK_EQ(listed[0].kind, std::string("build"));
    CHECK_EQ(listed[0].tasks, 2);
}

TEST(a_journal_from_before_builds_reads_as_a_cook) {
    // No kind, no tasks, a step with no task field: what every journal on
    // disk looked like until 0.9.
    const Cook old = cook_from_json(nlohmann::json::parse(
        R"({"id":"x","goal":"g","state":"done","steps":[{"kind":"read","summary":"read a"}]})"), "x");
    CHECK(!old.is_build());
    CHECK(old.tasks.empty());
    CHECK_EQ(old.steps.size(), std::size_t{1});
    CHECK_EQ(old.steps[0].task, -1);
}

TEST(a_seat_a_build_made_says_so_through_the_config_file) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    Config original;
    Expert agent;
    agent.name   = "CSS Layout";
    agent.blurb  = "layout and spacing in stylesheets";
    agent.origin = "build";
    Expert person;
    person.name  = "Physics";
    person.blurb = "mechanics";
    std::string error;
    CHECK(original.roster.add(agent, error));
    CHECK(original.roster.add(person, error));
    CHECK(save_config(original, file));
    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK(reloaded.roster.at(0).made_by_build());
    CHECK(!reloaded.roster.at(1).made_by_build());
    // And the page is told, so it can list the agents under their own heading.
    Snapshot snapshot;
    snapshot.roster = std::make_shared<const Roster>(reloaded.roster);
    const nlohmann::json out = nlohmann::json::parse(api::snapshot_json(snapshot));
    CHECK(out["experts"][0]["made"] == true);
    CHECK(!out["experts"][1].contains("made"));
}

TEST(build_settings_round_trip_through_the_config_file) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    Config original;
    original.build.architect       = "claude";
    original.build.worker_model    = "worker.gguf";
    original.build.auto_commit     = false;
    original.build.confirm_plan    = false;
    original.build.rounds_per_task = 25;
    original.build.agents          = 5;
    original.build.split           = false;
    original.tools.computer_control = true;
    CHECK(save_config(original, file));
    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK_EQ(reloaded.build.architect, std::string("claude"));
    CHECK_EQ(reloaded.build.worker_model, std::string("worker.gguf"));
    CHECK(!reloaded.build.auto_commit);
    CHECK(!reloaded.build.confirm_plan);
    CHECK_EQ(reloaded.build.rounds_per_task, 25);
    CHECK_EQ(reloaded.build.agents, 5);
    CHECK(!reloaded.build.split);
    // The one switch that is not about the folder survives a restart in
    // whichever position it was left: a screen that quietly turned itself
    // back on would be the worse mistake.
    CHECK(reloaded.tools.computer_control);
}

TEST(agents_at_once_outside_one_to_eight_is_refused_for_the_default) {
    TempDir dir;
    const auto file = dir.path() / "config.json";
    std::ofstream(file) << R"({"build": {"agents": 40}})";
    std::vector<std::string> warnings;
    const Config loaded = load_config(file, warnings);
    CHECK_EQ(loaded.build.agents, BuildConfig{}.agents);
    bool said = false;
    for (const std::string& warning : warnings) {
        said = said || warning.find("build.agents") != std::string::npos;
    }
    CHECK(said);
}

// ---------------------------------------------------------------------------
// Leases: a model in use is not freed under its user
// ---------------------------------------------------------------------------

TEST(a_lease_holds_a_model_until_it_is_let_go) {
    Leases leases;
    CHECK(!leases.held("/m/coder.gguf"));
    {
        Leases::Lease first  = leases.take("/m/coder.gguf");
        Leases::Lease second = leases.take("/m/coder.gguf");
        CHECK_EQ(leases.count("/m/coder.gguf"), 2);
        first.release();
        CHECK(leases.held("/m/coder.gguf"));
        // Moved, the hold goes with it and is not counted twice.
        Leases::Lease moved = std::move(second);
        CHECK(!second.held());
        CHECK_EQ(leases.count("/m/coder.gguf"), 1);
    }
    CHECK(!leases.held("/m/coder.gguf"));
}

TEST(a_wait_for_room_ends_when_a_lease_does_or_when_stopped) {
    Leases leases;
    Leases::Lease held = leases.take("/m/big.gguf");

    // A release that happens before the wait starts is not missed: the
    // count read before asking the host is what the wait compares against.
    const std::uint64_t since = leases.releases();
    std::thread letting_go([&held] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        held.release();
    });
    CHECK(leases.wait_for_release({}, since, std::chrono::seconds(5)));
    letting_go.join();

    // Stopped: the cancel switch is looked at while waiting.
    std::atomic<bool> stop{false};
    std::thread stopping([&stop] {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        stop.store(true);
    });
    const auto started = std::chrono::steady_clock::now();
    CHECK(!leases.wait_for_release([&stop] { return stop.load(); }, leases.releases(),
                                   std::chrono::seconds(30)));
    stopping.join();
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(5));
}

// ---------------------------------------------------------------------------
// The verbs beyond the folder
// ---------------------------------------------------------------------------

TEST(a_run_may_name_its_shell_and_a_run_followed_by_prose_is_prose) {
    const std::optional<tools::ToolCall> zsh = tools::parse_tool_call("RUN zsh: ls -la", "");
    CHECK(zsh.has_value());
    if (zsh) {
        CHECK(zsh->kind == tools::ToolKind::Run);
        CHECK_EQ(zsh->shell, std::string("zsh"));
        CHECK_EQ(zsh->argument, std::string("ls -la"));
    }
    const std::optional<tools::ToolCall> ps = tools::parse_tool_call("RUN powershell: Get-ChildItem", "");
    CHECK(ps.has_value() && ps->shell == "powershell");
    // "RUN the tests first" was a sentence before and still is.
    CHECK(!tools::parse_tool_call("RUN the tests first: they matter", "").has_value());

    // One of the verbs handed to RUN is the verb, not a shell command.
    const std::optional<tools::ToolCall> render = tools::parse_tool_call("RUN: RENDER: index.html to out.png", "");
    CHECK(render.has_value() && render->kind == tools::ToolKind::Render);
    CHECK(render.has_value() && render->argument == "index.html to out.png");
    const std::optional<tools::ToolCall> plain = tools::parse_tool_call("RUN: npm test", "");
    CHECK(plain.has_value() && plain->kind == tools::ToolKind::Run);
    CHECK(tools::known_shell("bash"));
    CHECK(!tools::known_shell("python"));
    const std::vector<std::string> cmd  = tools::shell_argv("cmd", "dir");
    const std::vector<std::string> pwsh = tools::shell_argv("pwsh", "ls");
    CHECK_EQ(cmd.size(), std::size_t{3});
    CHECK_EQ(cmd.front(), std::string("cmd"));
    CHECK_EQ(pwsh.size(), std::size_t{5});
    CHECK(tools::shell_argv("python", "x").empty());
}

TEST(python_and_a_fenced_type_take_a_block_and_git_keeps_its_quotes) {
    const std::optional<tools::ToolCall> py =
        tools::parse_tool_call("PYTHON:\n```python\nprint(1 + 1)\n```\n", "");
    CHECK(py.has_value());
    if (py) {
        CHECK(py->kind == tools::ToolKind::Python);
        CHECK_EQ(py->content, std::string("print(1 + 1)"));
    }
    const std::optional<tools::ToolCall> typed = tools::parse_tool_call("TYPE:\n```\nhello\nworld\n```", "");
    CHECK(typed.has_value() && typed->content == "hello\nworld");
    const std::optional<tools::ToolCall> inline_typed = tools::parse_tool_call("TYPE: hello there", "");
    CHECK(inline_typed.has_value() && inline_typed->argument == "hello there" && inline_typed->content.empty());
    const std::optional<tools::ToolCall> git = tools::parse_tool_call("GIT: commit -m \"add the parser\"", "");
    CHECK(git.has_value());
    if (git) {
        CHECK(git->kind == tools::ToolKind::Git);
        // Kept as written: the quotes mean something to the argument split.
        CHECK_EQ(git->argument, std::string("commit -m \"add the parser\""));
    }
    const std::vector<std::string> args = tools::git::split_arguments("commit -m \"add the parser\" --no-verify");
    CHECK_EQ(args.size(), std::size_t{4});
    CHECK_EQ(args[2], std::string("add the parser"));
    CHECK_EQ(tools::git::split_arguments("log 'a b' c\\ d").size(), std::size_t{3});
}

TEST(the_screen_verbs_parse_and_a_word_that_starts_with_a_verb_is_not_one) {
    const std::optional<tools::ToolCall> shot = tools::parse_tool_call("SCREENSHOT:", "");
    CHECK(shot.has_value() && shot->kind == tools::ToolKind::Screenshot);
    const std::optional<tools::ToolCall> look = tools::parse_tool_call("LOOK:", "");
    CHECK(look.has_value() && look->kind == tools::ToolKind::Screenshot);
    const std::optional<tools::ToolCall> click = tools::parse_tool_call("CLICK: 120 340 right", "");
    CHECK(click.has_value() && click->kind == tools::ToolKind::Click && click->argument == "120 340 right");
    CHECK(tools::parse_tool_call("SCROLL: down 5", "")->kind == tools::ToolKind::Scroll);
    CHECK(tools::parse_tool_call("KEY: ctrl+shift+s", "")->kind == tools::ToolKind::Key);
    CHECK(tools::parse_tool_call("LOGS:", "")->kind == tools::ToolKind::Logs);
    // "GHOST:" is not a GH call, and "STOPPING the server" is prose.
    CHECK(!tools::parse_tool_call("GHOST: in the machine", "").has_value());
    CHECK(tools::attempted_tool_call("STOPPING the server now", "") == tools::ToolKind::None);
    CHECK(tools::attempted_tool_call("SCREENSHOT", "") == tools::ToolKind::Screenshot);
}

TEST(the_instructions_offer_each_group_only_when_its_switch_is_on) {
    tools::WorkshopSettings settings;
    settings.enabled = true;
    const std::string plain = tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(plain.find("GIT:") != std::string::npos);
    CHECK(plain.find("PYTHON:") != std::string::npos);
    CHECK(plain.find("FETCH:") == std::string::npos);
    CHECK(plain.find("SCREENSHOT:") == std::string::npos);
    CHECK(plain.find("START:") == std::string::npos);   // nowhere to keep a process

    settings.web = true;
    settings.computer_control = true;
    tools::Processes processes;
    settings.processes = &processes;
    const std::string all = tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(all.find("FETCH:") != std::string::npos);
    CHECK(all.find("SEARCH:") != std::string::npos);
    CHECK(all.find("SCREENSHOT:") != std::string::npos);
    CHECK(all.find("CLICK:") != std::string::npos);
    CHECK(all.find("START:") != std::string::npos);

    settings.allow_run = false;
    const std::string no_run = tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(no_run.find("GIT:") == std::string::npos);
    CHECK(no_run.find("PYTHON:") == std::string::npos);
    CHECK(no_run.find("START:") == std::string::npos);
}

TEST(each_switched_off_verb_refuses_in_words_rather_than_doing_nothing) {
    TempDir dir;
    tools::WorkshopSettings settings;
    settings.enabled = true;
    settings.root    = dir.path();
    const auto refused = [&](tools::ToolKind kind, const char* argument) {
        tools::ToolCall call;
        call.kind     = kind;
        call.argument = argument;
        return tools::run_tool(call, settings, tools::SearchSettings{}, {});
    };
    CHECK(!refused(tools::ToolKind::Fetch, "https://example.com").ok);
    CHECK(refused(tools::ToolKind::Fetch, "https://example.com").summary.find("web") != std::string::npos);
    CHECK(!refused(tools::ToolKind::Screenshot, "").ok);
    CHECK(refused(tools::ToolKind::Screenshot, "").summary.find("switched off") != std::string::npos);
    CHECK(!refused(tools::ToolKind::Start, "sleep 100").ok);   // no process table
    settings.web = true;
    CHECK(refused(tools::ToolKind::Fetch, "file:///etc/passwd").summary.find("http") != std::string::npos);
}

TEST(a_python_snippet_runs_in_the_project_on_the_named_interpreter) {
    const std::string python = [] {
        if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
            return std::string(chosen);
        }
        for (const char* name : {"python3", "python"}) {
            if (util::on_path(name)) {
                return std::string(name);
            }
        }
        return std::string();
    }();
    if (python.empty()) {
        std::printf("      (no Python on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    { std::ofstream(dir.path() / "marker.txt") << "here"; }
    tools::WorkshopSettings settings = everything_on(dir.path());
    settings.python  = python;
    settings.scratch = dir.path() / "scratch";
    tools::ToolCall call;
    call.kind    = tools::ToolKind::Python;
    call.content = "import os\nprint(sorted(os.listdir('.')))";
    const tools::ToolResult result = tools::run_tool(call, settings, tools::SearchSettings{}, {});
    CHECK(result.ok);
    CHECK(result.output.find("marker.txt") != std::string::npos);
    CHECK(result.summary.rfind("python: ", 0) == 0);
}

TEST(a_started_program_keeps_running_and_its_output_can_be_read_and_stopped) {
    const bool have_python = util::on_path("python3") || util::on_path("python");
    if (!have_python) {
        std::printf("      (no Python on PATH; skipped)\n");
        return;
    }
    const std::string python = util::on_path("python3") ? "python3" : "python";
    TempDir dir;
    tools::Processes processes;
    tools::WorkshopSettings settings = everything_on(dir.path());
    settings.processes = &processes;
    tools::ToolCall start;
    start.kind     = tools::ToolKind::Start;
    start.argument = python + " -c \"import sys, time; print('serving'); sys.stdout.flush(); time.sleep(30)\"";
    const tools::ToolResult started = tools::run_tool(start, settings, tools::SearchSettings{}, {});
    CHECK(started.ok);
    CHECK(started.output.find("p1") != std::string::npos);

    tools::ToolCall logs;
    logs.kind     = tools::ToolKind::Logs;
    logs.argument = "p1";
    const tools::ToolResult read = tools::run_tool(logs, settings, tools::SearchSettings{}, {});
    CHECK(read.ok);
    CHECK(read.output.find("serving") != std::string::npos);
    CHECK(read.output.find("is running") != std::string::npos);

    CHECK_EQ(processes.list().size(), std::size_t{1});
    tools::ToolCall stop;
    stop.kind     = tools::ToolKind::Stop;
    stop.argument = "p1";
    CHECK(tools::run_tool(stop, settings, tools::SearchSettings{}, {}).ok);
    CHECK(processes.list().empty());
    CHECK(!tools::run_tool(stop, settings, tools::SearchSettings{}, {}).ok);   // gone
}

TEST(read_hands_a_picture_on_and_a_document_as_its_text) {
    TempDir dir;
    // The smallest PNG there is: a 1x1 pixel.
    const unsigned char png[] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A, 0, 0, 0, 0x0D, 'I', 'H', 'D', 'R',
                                 0, 0, 0, 1, 0, 0, 0, 1, 8, 6, 0, 0, 0, 0x1F, 0x15, 0xC4, 0x89, 0, 0, 0, 0x0A,
                                 'I', 'D', 'A', 'T', 0x78, 0x9C, 0x63, 0, 1, 0, 0, 5, 0, 1, 0x0D, 0x0A, 0x2D,
                                 0xB4, 0, 0, 0, 0, 'I', 'E', 'N', 'D', 0xAE, 0x42, 0x60, 0x82};
    {
        std::ofstream out(dir.path() / "dot.png", std::ios::binary);
        out.write(reinterpret_cast<const char*>(png), sizeof(png));
    }
    { std::ofstream(dir.path() / "page.html") << "<html><body><h1>Hello</h1><p>there</p></body></html>"; }
    tools::WorkshopSettings settings = everything_on(dir.path());

    tools::ToolCall read;
    read.kind     = tools::ToolKind::Read;
    read.argument = "dot.png";
    const tools::ToolResult picture = tools::run_tool(read, settings, tools::SearchSettings{}, {});
    CHECK(picture.ok);
    CHECK_EQ(picture.pictures.size(), std::size_t{1});
    CHECK_EQ(picture.pictures[0].mime, std::string("image/png"));
    CHECK(picture.output.find("[picture: dot.png") != std::string::npos);
    CHECK(!picture.picture_path.empty());

    // A web page is source to the expert that is making it: the tags stay.
    // What an attachment does with the same file -- take the words out -- is
    // right for a prompt and wrong for a WRITE that is about to follow.
    read.argument = "page.html";
    const tools::ToolResult page = tools::run_tool(read, settings, tools::SearchSettings{}, {});
    CHECK(page.ok);
    CHECK(page.output.find("<h1>Hello</h1>") != std::string::npos);
    CHECK(attach::is_markup_source("site/index.htm"));
    CHECK(!attach::is_markup_source("notes.docx"));
}

// ---------------------------------------------------------------------------
// Under the screen and the web
// ---------------------------------------------------------------------------

TEST(key_combinations_are_read_the_way_people_write_them) {
    const tools::computer::detail::Keys combo = tools::computer::detail::parse_keys("Ctrl+Shift+S");
    CHECK(combo.ctrl && combo.shift && !combo.alt && !combo.cmd);
    CHECK_EQ(combo.key, std::string("s"));
    CHECK_EQ(tools::computer::detail::parse_keys("Return").key, std::string("enter"));
    CHECK_EQ(tools::computer::detail::parse_keys("cmd+esc").key, std::string("escape"));
    CHECK(tools::computer::detail::parse_keys("option+f4").alt);
    CHECK_EQ(tools::computer::detail::parse_keys("ctrl++").key, std::string("+"));

    CHECK_EQ(tools::computer::detail::xdotool_key(combo), std::string("ctrl+shift+s"));
    CHECK_EQ(tools::computer::detail::xdotool_key(tools::computer::detail::parse_keys("enter")), std::string("Return"));
    CHECK_EQ(tools::computer::detail::xdotool_key(tools::computer::detail::parse_keys("f5")), std::string("F5"));
    CHECK_EQ(tools::computer::detail::sendkeys(combo), std::string("^+s"));
    CHECK_EQ(tools::computer::detail::sendkeys(tools::computer::detail::parse_keys("alt+f4")), std::string("%{F4}"));
    CHECK_EQ(tools::computer::detail::sendkeys(tools::computer::detail::parse_keys("enter")), std::string("{ENTER}"));
    CHECK_EQ(tools::computer::detail::sendkeys(tools::computer::detail::parse_keys("+")), std::string("{+}"));
    CHECK_EQ(tools::computer::detail::mac_key_code("enter"), 36);
    CHECK_EQ(tools::computer::detail::mac_key_code("c"), 8);
    CHECK_EQ(tools::computer::detail::mac_key_code("euro"), -1);
}

TEST(only_web_addresses_are_fetched_and_a_title_is_one_line) {
    CHECK(tools::detail::fetchable("https://example.com/a"));
    CHECK(tools::detail::fetchable("HTTP://example.com"));
    CHECK(!tools::detail::fetchable("file:///etc/passwd"));
    CHECK(!tools::detail::fetchable("ftp://x"));
    CHECK(!tools::detail::fetchable("example.com"));
    CHECK_EQ(tools::detail::title_of("<html><head><TITLE>\n  Hello &amp;\n  world </TITLE></head>"),
             std::string("Hello & world"));
    CHECK(tools::detail::title_of("<html><body>no title</body>").empty());
}

TEST(duckduckgo_results_are_read_off_its_page) {
    const std::string page =
        "<div class=\"result\"><h2 class=\"result__title\">"
        "<a rel=\"nofollow\" class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fpage&amp;rut=abc\">"
        "Example <b>Page</b></a></h2>"
        "<a class=\"result__snippet\" href=\"//duckduckgo.com/l/?uddg=x\">A snippet about <b>things</b>.</a></div>"
        "<div class=\"result\"><a class=\"result__a\" href=\"https://second.example/\">Second</a>"
        "<div class=\"result__snippet\">Two.</div></div>";
    const std::vector<tools::SearchResult> results = tools::parse_results("duckduckgo", page, 5);
    CHECK_EQ(results.size(), std::size_t{2});
    if (results.size() == 2) {
        CHECK_EQ(results[0].url, std::string("https://example.com/page"));
        CHECK_EQ(results[0].title, std::string("Example Page"));
        CHECK_EQ(results[0].snippet, std::string("A snippet about things."));
        CHECK_EQ(results[1].url, std::string("https://second.example/"));
        CHECK_EQ(results[1].snippet, std::string("Two."));
    }
    CHECK_EQ(tools::parse_results("duckduckgo", page, 1).size(), std::size_t{1});
    tools::SearchSettings settings;
    settings.provider = "duckduckgo";
    CHECK_EQ(tools::request_url("a b", settings), std::string("https://html.duckduckgo.com/html/?q=a%20b"));
}

// ---------------------------------------------------------------------------
// EDIT and FIND: part of a file, and where in the project something is
// ---------------------------------------------------------------------------

TEST(an_edit_is_read_from_its_blocks_with_or_without_a_fence) {
    const std::optional<tools::ToolCall> call = tools::parse_tool_call(
        "I will make it blue.\nEDIT: style.css\n```css\n<<<<<<< SEARCH\n  color: red;\n=======\n"
        "  color: blue;\n>>>>>>> REPLACE\n```\n", "");
    CHECK(call.has_value() && call->kind == tools::ToolKind::Edit);
    CHECK_EQ(call->argument, std::string("style.css"));
    std::string error;
    const std::vector<tools::EditBlock> blocks = tools::parse_edit_blocks(call->content, error);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK_EQ(blocks[0].search, std::string("  color: red;"));
    CHECK_EQ(blocks[0].replace, std::string("  color: blue;"));

    // Two blocks, markers of another length, and an unclosed one refused.
    const std::vector<tools::EditBlock> two = tools::parse_edit_blocks(
        "<<<<< SEARCH\na\n=====\nb\n>>>>> REPLACE\n<<<<<<<<< search\nc\n=========\nd\n>>>>>>>>> replace\n", error);
    CHECK_EQ(two.size(), std::size_t{2});
    CHECK(tools::parse_edit_blocks("<<<<<<< SEARCH\na\n=======\nb\n", error).empty());
    CHECK(error.find("REPLACE") != std::string::npos);
    // Straight from SEARCH to REPLACE is the lines taken out.
    const std::vector<tools::EditBlock> cut =
        tools::parse_edit_blocks("<<<<<<< SEARCH\n32\t<script src=\"chart.js\"></script>\n>>>>>>> REPLACE\n", error);
    CHECK_EQ(cut.size(), std::size_t{1});
    const std::optional<std::string> gone =
        tools::apply_edit_blocks("<p>a</p>\n<script src=\"chart.js\"></script>\n<p>b</p>\n", cut, error);
    CHECK(gone.has_value() && *gone == "<p>a</p>\n<p>b</p>\n");
    CHECK(tools::parse_edit_blocks("just some prose", error).empty());
}

TEST(an_edit_applies_exactly_or_line_by_line_and_refuses_to_guess) {
    const std::string css = "body {\n  margin: 0;\n}\n.button {\n  color: red;\n  margin-left: 8px;\n}\n";
    std::string error;
    // As written.
    std::optional<std::string> out =
        tools::apply_edit_blocks(css, {{"  color: red;", "  color: blue;"}}, error);
    CHECK(out.has_value() && out->find("color: blue;") != std::string::npos);
    CHECK(out->find("color: red") == std::string::npos);

    // Copied without its indentation, as a model reading numbered lines does:
    // found line by line, and the replacement put at the file's indentation.
    out = tools::apply_edit_blocks(css, {{"margin-left: 8px;", "margin-left: 32px;"}}, error);
    CHECK(out.has_value());
    CHECK(out->find("\n  margin-left: 32px;\n") != std::string::npos);

    // Twice in the file is refused, with what to do instead.
    const std::string twice = ".a { color: red; }\n.b { color: red; }\n";
    CHECK(!tools::apply_edit_blocks(twice, {{"color: red;", "color: blue;"}}, error).has_value());
    CHECK(error.find("2 times") != std::string::npos);

    // Not there at all says to read the file again -- or, for a short one,
    // to write it whole.
    CHECK(!tools::apply_edit_blocks(css, {{"color: green;", "color: blue;"}}, error).has_value());
    CHECK(error.find("READ it again") != std::string::npos);
    CHECK(error.find("WRITE the whole file") != std::string::npos);

    // An empty SEARCH makes a file that is not there yet, and only then.
    out = tools::apply_edit_blocks("", {{"", "print('hi')"}}, error);
    CHECK(out.has_value() && *out == "print('hi')\n");
    CHECK(!tools::apply_edit_blocks(css, {{"", "x"}}, error).has_value());
}

TEST(an_edit_forgives_what_a_small_model_copies_wrong) {
    const std::string page = "<body>\n  <h1>Budget</h1>\n\n  <button id=\"add\">Add</button>\n</body>\n";
    std::string error;
    // READ's line numbers, copied with the lines.
    std::optional<std::string> out =
        tools::apply_edit_blocks(page, {{"2\t  <h1>Budget</h1>", "  <h1>My budget</h1>"}}, error);
    CHECK(out.has_value() && out->find("<h1>My budget</h1>") != std::string::npos);
    out = tools::apply_edit_blocks(page, {{"2 | <h1>Budget</h1>\n3 |\n4 | <button id=\"add\">Add</button>",
                                           "<h1>B</h1>\n<button id=\"add\" class=\"wide\">Add</button>"}}, error);
    CHECK(out.has_value() && out->find("class=\"wide\"") != std::string::npos);
    // And the numbers it copied into the lines it wrote come off them too,
    // rather than landing in the file as text.
    out = tools::apply_edit_blocks(page, {{"5\t</body>", "5\t  <p>total</p>\n6\t</body>"}}, error);
    CHECK(out.has_value() && out->find("  <p>total</p>\n</body>") != std::string::npos);
    CHECK(out.has_value() && out->find("6\t") == std::string::npos);
    // A blank line the model left out.
    out = tools::apply_edit_blocks(page, {{"<h1>Budget</h1>\n<button id=\"add\">Add</button>",
                                           "<h1>Budget</h1>\n<button id=\"add\">Plus</button>"}}, error);
    CHECK(out.has_value() && out->find(">Plus<") != std::string::npos);
    CHECK(out.has_value() && out->find("<body>") == 0);
    // A number that is the file's own is left alone: these do not count up.
    const std::string data = "10 apples\n20 pears\n";
    out = tools::apply_edit_blocks(data, {{"20 pears", "30 pears"}}, error);
    CHECK(out.has_value() && *out == "10 apples\n30 pears\n");
    out = tools::apply_edit_blocks(data, {{"  10 apples\n  20 pears", "10 apples\n25 pears"}}, error);
    CHECK(out.has_value() && *out == "10 apples\n25 pears\n");
    // Nothing matching: the nearest lines in the file are said back, numbered,
    // for the next try to copy.
    CHECK(!tools::apply_edit_blocks(page, {{"<h1>Budget</h1>\n<button id=\"plus\">Add</button>", "x"}}, error)
               .has_value());
    CHECK(error.find("nearest lines") != std::string::npos);
    CHECK(error.find("2\t  <h1>Budget</h1>") != std::string::npos);
}

TEST(an_edit_changes_the_file_on_disk_and_says_what_moved) {
    TempDir dir;
    tools::WorkshopSettings settings = everything_on(dir.path());
    std::ofstream(dir.path() / "style.css") << ".button {\n  color: red;\n}\n";
    tools::ToolCall call;
    call.kind     = tools::ToolKind::Edit;
    call.argument = "style.css";
    call.content  = "<<<<<<< SEARCH\n  color: red;\n=======\n  color: blue;\n>>>>>>> REPLACE\n";
    std::string error;
    const std::optional<std::string> preview = tools::edited_contents(call, settings, error);
    CHECK(preview.has_value() && preview->find("blue") != std::string::npos);
    const tools::ToolResult result = tools::run_tool(call, settings, {}, {});
    CHECK(result.ok);
    CHECK(result.summary.rfind("edited style.css", 0) == 0);
    CHECK(result.changed == std::vector<std::string>{"style.css"});
    std::ifstream in(dir.path() / "style.css");
    const std::string now((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    CHECK_EQ(now, std::string(".button {\n  color: blue;\n}\n"));

    // A refused edit keeps what the model wrote, for whoever reads the step.
    call.content = "<<<<<<< SEARCH\n  background: green;\n=======\n  color: blue;\n>>>>>>> REPLACE\n";
    const tools::ToolResult refused = tools::run_tool(call, settings, {}, {});
    CHECK(!refused.ok);
    CHECK(refused.detail.find("background: green;") != std::string::npos);

    // Outside the folder is refused like a WRITE is.
    call.argument = "../elsewhere.css";
    CHECK(!tools::run_tool(call, settings, {}, {}).ok);
}

TEST(find_searches_the_project_and_not_what_it_depends_on) {
    TempDir dir;
    tools::WorkshopSettings settings = everything_on(dir.path());
    std::filesystem::create_directories(dir.path() / "src");
    std::filesystem::create_directories(dir.path() / "node_modules" / "lib");
    std::ofstream(dir.path() / "style.css") << "a { color: red; }\n";
    std::ofstream(dir.path() / "src" / "app.js") << "const red = 'Color: Red';\nconsole.log(red);\n";
    std::ofstream(dir.path() / "node_modules" / "lib" / "x.css") << "b { color: red; }\n";

    tools::ToolCall call;
    call.kind     = tools::ToolKind::Find;
    call.argument = "color: red";
    tools::ToolResult result = tools::run_tool(call, settings, {}, {});
    CHECK(result.ok);
    CHECK(result.output.find("style.css:1:") != std::string::npos);
    CHECK(result.output.find("src/app.js:1:") != std::string::npos);   // whatever its case
    CHECK(result.output.find("node_modules") == std::string::npos);

    call.argument = "color: red in *.css";
    result = tools::run_tool(call, settings, {}, {});
    CHECK(result.output.find("app.js") == std::string::npos);
    CHECK(result.summary.find("1 line in 1 file") != std::string::npos);

    call.argument = "/console\\.log\\(/";
    result = tools::run_tool(call, settings, {}, {});
    CHECK(result.output.find("src/app.js:2:") != std::string::npos);

    call.argument = "nothing like this";
    CHECK(tools::run_tool(call, settings, {}, {}).output.find("Nothing in the project") != std::string::npos);
}

TEST(a_glob_matches_names_anywhere_or_paths_from_the_root) {
    CHECK(tools::glob_matches("*.css", "styles/site.css"));
    CHECK(!tools::glob_matches("*.css", "styles/site.scss"));
    CHECK(tools::glob_matches("src/*.py", "src/app.py"));
    CHECK(!tools::glob_matches("src/*.py", "src/deep/app.py"));
    CHECK(tools::glob_matches("src/**/*.py", "src/deep/app.py"));
    CHECK(tools::glob_matches("src/**/*.py", "src/app.py"));
    CHECK(tools::glob_matches("README.?d", "README.md"));
}

TEST(the_instructions_teach_edit_and_find_and_the_example_parses) {
    TempDir dir;
    const std::string text = tools::workshop_instructions(everything_on(dir.path()), tools::ToolAudience::Cook);
    CHECK(text.find("EDIT: <file>") != std::string::npos);
    CHECK(text.find("FIND: <text>") != std::string::npos);
    // The worked example is itself a call the parser reads.
    const std::size_t at = text.find("EDIT: style.css");
    CHECK(at != std::string::npos);
    const std::optional<tools::ToolCall> example = tools::parse_tool_call(text.substr(at), "");
    CHECK(example.has_value() && example->kind == tools::ToolKind::Edit);
    std::string error;
    CHECK_EQ(tools::parse_edit_blocks(example->content, error).size(), std::size_t{1});
}

TEST(render_draws_a_page_into_a_pdf_and_a_picture) {
    if (tools::browser_here().empty()) {
        std::printf("      (no browser on this machine; skipped)\n");
        return;
    }
    TempDir dir;
    tools::WorkshopSettings settings = everything_on(dir.path());
    std::ofstream(dir.path() / "report.html")
        << "<html><body style='font:32px sans-serif'><h1>Net revenue</h1><p>3,150</p></body></html>";
    tools::ToolCall call;
    call.kind     = tools::ToolKind::Render;
    call.argument = "report.html to out/report.pdf";
    tools::ToolResult result = tools::run_tool(call, settings, {}, {});
    CHECK(result.ok);
    if (!result.ok) {
        std::printf("      %s\n", result.output.c_str());
    }
    std::ifstream pdf(dir.path() / "out" / "report.pdf", std::ios::binary);
    std::string head(5, '\0');
    pdf.read(head.data(), 5);
    CHECK_EQ(head, std::string("%PDF-"));

    call.argument = "report.html -> shot.png at 640x360";
    result = tools::run_tool(call, settings, {}, {});
    CHECK(result.ok);
    CHECK(result.changed == std::vector<std::string>{"shot.png"});
    CHECK(!result.pictures.empty());

    // Where it goes and what it is are checked before the browser starts.
    call.argument = "report.html to ../away.pdf";
    CHECK(!tools::run_tool(call, settings, {}, {}).ok);
    call.argument = "report.html to report.docx";
    CHECK(!tools::run_tool(call, settings, {}, {}).ok);
}

TEST(an_action_after_a_note_is_the_call_and_a_note_alone_is_a_note) {
    const std::optional<tools::ToolCall> both = tools::parse_tool_call(
        "NOTE: starting task 1\nWRITE: index.html\n```\n<p>hi</p>\n```", "");
    CHECK(both.has_value() && both->kind == tools::ToolKind::Write);
    CHECK_EQ(both->argument, std::string("index.html"));
    CHECK_EQ(both->content, std::string("<p>hi</p>"));
    const std::optional<tools::ToolCall> alone = tools::parse_tool_call("NOTE: thinking it over", "");
    CHECK(alone.has_value() && alone->kind == tools::ToolKind::Note);
}
