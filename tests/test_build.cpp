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

#include "crucible/api/surface.hpp"
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
    original.tools.computer_control = true;
    CHECK(save_config(original, file));
    std::vector<std::string> warnings;
    const Config reloaded = load_config(file, warnings);
    CHECK_EQ(reloaded.build.architect, std::string("claude"));
    CHECK_EQ(reloaded.build.worker_model, std::string("worker.gguf"));
    CHECK(!reloaded.build.auto_commit);
    CHECK(!reloaded.build.confirm_plan);
    CHECK_EQ(reloaded.build.rounds_per_task, 25);
    // The one switch that is not about the folder survives a restart in
    // whichever position it was left: a screen that quietly turned itself
    // back on would be the worse mistake.
    CHECK(reloaded.tools.computer_control);
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
