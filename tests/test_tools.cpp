// SPDX-License-Identifier: MIT
//
// The tools an expert can reach: the workshop sandbox, web search, and the
// subprocess primitive underneath both.
#include "test_helpers.hpp"

#include <system_error>

#include "crucible/util/platform.hpp"

// ---------------------------------------------------------------------------
// The workshop
// ---------------------------------------------------------------------------

namespace {

tools::WorkshopSettings workshop_at(const std::filesystem::path& root) {
    tools::WorkshopSettings settings;
    settings.enabled = true;
    settings.root    = root;
    return settings;
}

tools::ToolResult do_call(const tools::ToolCall& call, const tools::WorkshopSettings& settings) {
    return tools::run_tool(call, settings, tools::SearchSettings{}, {});
}

}  // namespace

TEST(a_tool_call_is_a_verb_at_the_start_of_a_line) {
    const std::optional<tools::ToolCall> call =
        tools::parse_tool_call("Let me look.\nREAD: src/main.cpp\n", "");
    CHECK(call.has_value());
    if (!call) {
        return;
    }
    CHECK(call->kind == tools::ToolKind::Read);
    CHECK_EQ(call->argument, std::string("src/main.cpp"));
}

TEST(a_verb_in_the_middle_of_a_sentence_is_prose_not_a_call) {
    // An expert explaining a command must not thereby execute it. This is the
    // difference between a coding assistant and a program that runs whatever
    // appears in its own output.
    CHECK(!tools::parse_tool_call("You could just RUN: make clean here.", "").has_value());
    CHECK(!tools::parse_tool_call("The output said ERROR: RUN: failed", "").has_value());
}

TEST(a_write_body_is_taken_from_a_fence_a_sentinel_or_the_rest) {
    const auto body = [](const char* reply) {
        const std::optional<tools::ToolCall> call = tools::parse_tool_call(reply, "");
        return call ? call->content : std::string("<none>");
    };

    // A fence is what a model reaches for by reflex, whatever it was told.
    CHECK_EQ(body("WRITE: a.py\n```python\nprint(1)\nprint(2)\n```\n"),
             std::string("print(1)\nprint(2)"));
    // The explicit sentinel, for a model that followed the letter of it.
    CHECK_EQ(body("WRITE: a.py\n<<<\nprint(1)\n>>>\n"), std::string("print(1)"));
    // And neither: everything up to the next verb.
    CHECK_EQ(body("WRITE: a.py\nprint(1)\nDONE: wrote it\n"), std::string("print(1)"));
}

TEST(a_reply_that_says_something_is_answering_not_calling_a_tool) {
    // Same rule the search tool follows. The reasoning channel is only read
    // when there is nothing for the user, or a programming expert showing you
    // a shell command would run it.
    CHECK(!tools::parse_tool_call("Here is what I would do.", "RUN: rm -rf /").has_value());

    const std::optional<tools::ToolCall> call = tools::parse_tool_call("", "RUN: ls");
    CHECK(call.has_value());
    if (call) {
        CHECK(call->kind == tools::ToolKind::Run);
    }
}

TEST(a_path_cannot_leave_the_project) {
    TempDir dir;
    const auto root = dir.path() / "project";
    std::filesystem::create_directories(root / "src");
    { std::ofstream(root / "src" / "main.cpp") << "int main() {}\n"; }
    { std::ofstream(dir.path() / "secret.txt") << "not yours\n"; }

    CHECK(tools::resolve_in_root(root, "src/main.cpp").has_value());
    CHECK(tools::resolve_in_root(root, "./src/../src/main.cpp").has_value());
    CHECK(tools::resolve_in_root(root, (root / "src" / "main.cpp").string()).has_value());

    // The three ways out, all refused.
    CHECK(!tools::resolve_in_root(root, "../secret.txt").has_value());
    CHECK(!tools::resolve_in_root(root, "src/../../secret.txt").has_value());
    CHECK(!tools::resolve_in_root(root, "/etc/passwd").has_value());
}

TEST(a_symlink_out_of_the_project_is_refused) {
    TempDir dir;
    const auto root = dir.path() / "project";
    std::filesystem::create_directories(root);
    { std::ofstream(dir.path() / "secret.txt") << "not yours\n"; }

    std::error_code ec;
    std::filesystem::create_symlink(dir.path() / "secret.txt", root / "escape.txt", ec);
    if (ec) {
        return;  // a filesystem without symlinks has nothing to test here
    }

    // Resolved before the check, not after. A string comparison would see
    // "project/escape.txt", say it was inside, and then open the file outside.
    CHECK(!tools::resolve_in_root(root, "escape.txt").has_value());
}

TEST(a_sibling_directory_with_a_shared_prefix_is_outside) {
    TempDir dir;
    const auto root = dir.path() / "proj";
    std::filesystem::create_directories(root);
    std::filesystem::create_directories(dir.path() / "project-two");
    { std::ofstream(dir.path() / "project-two" / "x.txt") << "x\n"; }

    // "/tmp/x/proj" is a text prefix of "/tmp/x/project-two" and is not a
    // parent of it. Comparing strings rather than path components is the exact
    // bug this guards.
    CHECK(!tools::resolve_in_root(root, "../project-two/x.txt").has_value());
}

TEST(reading_and_writing_a_file_works_and_says_what_it_did) {
    TempDir dir;
    const auto root = dir.path();
    const tools::WorkshopSettings settings = workshop_at(root);

    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "src/hello.py";
    write.content  = "print('hi')";
    const tools::ToolResult wrote = do_call(write, settings);
    CHECK(wrote.ok);
    CHECK(wrote.summary.find("created") != std::string::npos);
    CHECK_EQ(wrote.changed.size(), std::size_t{1});
    CHECK(std::filesystem::exists(root / "src" / "hello.py"));

    tools::ToolCall read;
    read.kind     = tools::ToolKind::Read;
    read.argument = "src/hello.py";
    const tools::ToolResult got = do_call(read, settings);
    CHECK(got.ok);
    // Numbered, because the next thing an expert wants to say about a file is
    // which line to change.
    CHECK(got.output.find("1\tprint('hi')") != std::string::npos);

    // Writing again reports an update rather than a creation.
    write.content = "print('bye')";
    CHECK(do_call(write, settings).summary.find("updated") != std::string::npos);
}

TEST(a_write_target_that_is_a_sentence_is_refused) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());

    // Every one of these is a real WRITE target a small expert produced during
    // a cook, and every one of them created a file with that name.
    for (const char* junk : {"Run python src/calc.py",
                             "The fix is complete and verified.",
                             "src/calc.py:2: return a + b",
                             "python -c \"import os\" && python src/calc.py"}) {
        tools::ToolCall write;
        write.kind     = tools::ToolKind::Write;
        write.argument = junk;
        write.content  = "x = 1";
        const tools::ToolResult result = do_call(write, settings);
        CHECK(!result.ok);
        // The message has to say what a path looks like, or the model tries the
        // same thing again with different words.
        CHECK(result.output.find("relative to the project root") != std::string::npos);
    }

    // Nothing was created. A project that comes back from a cook with a file
    // called "The fix is complete and verified." in it is worse than one where
    // the write was refused.
    int entries = 0;
    for (const auto& entry : std::filesystem::directory_iterator(dir.path())) {
        (void)entry;
        ++entries;
    }
    CHECK_EQ(entries, 0);
}

TEST(an_ordinary_path_is_still_written) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());

    for (const char* path : {"calc.py", "src/calc.py", "a/b/c/deep-file.txt",
                             "./relative.md", "with_underscores.and.dots"}) {
        tools::ToolCall write;
        write.kind     = tools::ToolKind::Write;
        write.argument = path;
        write.content  = "x = 1";
        CHECK(do_call(write, settings).ok);
    }
}

TEST(a_write_with_no_body_is_refused_rather_than_emptying_the_file) {
    TempDir dir;
    const auto root = dir.path();
    { std::ofstream(root / "keep.txt") << "important\n"; }

    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "keep.txt";
    // A WRITE whose body failed to parse looks exactly like a WRITE of an empty
    // file, and one of those two destroys the file the expert was working on.
    const tools::ToolResult result = do_call(write, workshop_at(root));
    CHECK(!result.ok);

    std::ifstream in(root / "keep.txt");
    std::string   line;
    std::getline(in, line);
    CHECK_EQ(line, std::string("important"));
}

TEST(writing_outside_the_project_is_refused) {
    TempDir dir;
    const auto root = dir.path() / "project";
    std::filesystem::create_directories(root);

    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "../escaped.txt";
    write.content  = "should not exist";
    CHECK(!do_call(write, workshop_at(root)).ok);
    CHECK(!std::filesystem::exists(dir.path() / "escaped.txt"));
}

TEST(a_listing_skips_the_directories_nobody_wants_read) {
    TempDir dir;
    const auto root = dir.path();
    std::filesystem::create_directories(root / ".git" / "objects");
    std::filesystem::create_directories(root / "node_modules");
    std::filesystem::create_directories(root / "src");
    { std::ofstream(root / "README.md") << "hi\n"; }

    tools::ToolCall list;
    list.kind     = tools::ToolKind::List;
    list.argument = ".";
    const tools::ToolResult result = do_call(list, workshop_at(root));
    CHECK(result.ok);
    CHECK(result.output.find("README.md") != std::string::npos);
    CHECK(result.output.find("src/") != std::string::npos);
    // A project's .git is thousands of files, and listing it is the fastest way
    // to fill a context window with nothing.
    CHECK(result.output.find(".git") == std::string::npos);
    CHECK(result.output.find("node_modules") == std::string::npos);
}

TEST(a_quoted_path_is_the_same_path) {
    // Measured, not guessed: a small expert wrote LIST: `calc.py` forty times
    // and was told forty times that the backticked name was not a directory.
    const auto arg = [](const char* reply) {
        const std::optional<tools::ToolCall> call = tools::parse_tool_call(reply, "");
        return call ? call->argument : std::string("<none>");
    };
    CHECK_EQ(arg("READ: `src/main.cpp`"),  std::string("src/main.cpp"));
    CHECK_EQ(arg("READ: \"src/main.cpp\""), std::string("src/main.cpp"));
    CHECK_EQ(arg("READ: 'src/main.cpp'"),  std::string("src/main.cpp"));
    CHECK_EQ(arg("LIST: `.`"),             std::string("."));

    // RUN keeps its argument verbatim, because quoting means something to a
    // shell and stripping it would change the command.
    CHECK_EQ(arg("RUN: echo \"hello world\""), std::string("echo \"hello world\""));
}

TEST(listing_a_file_says_which_command_to_use_instead) {
    TempDir dir;
    { std::ofstream(dir.path() / "calc.py") << "x = 1\n"; }

    tools::ToolCall list;
    list.kind     = tools::ToolKind::List;
    list.argument = "calc.py";
    const tools::ToolResult result =
        tools::run_tool(list, workshop_at(dir.path()), tools::SearchSettings{}, {});

    CHECK(!result.ok);
    // Telling a model only what it did wrong leaves it guessing, which is how
    // one ends up making the same call twenty times.
    CHECK(result.output.find("READ:") != std::string::npos);

    list.argument = "nope.py";
    CHECK(tools::run_tool(list, workshop_at(dir.path()), tools::SearchSettings{}, {})
              .output.find("does not exist") != std::string::npos);
}

TEST(a_read_only_verb_still_works_without_its_colon) {
    // Models drop it. Measured: an expert spent a whole cook writing
    // `READ /path` and `WRITE /path "text"`, and every one was read as prose
    // over one character.
    const auto call = [](const char* reply) { return tools::parse_tool_call(reply, ""); };

    const std::optional<tools::ToolCall> read = call("READ src/main.cpp");
    CHECK(read.has_value());
    if (read) {
        CHECK(read->kind == tools::ToolKind::Read);
        CHECK_EQ(read->argument, std::string("src/main.cpp"));
    }
    const std::optional<tools::ToolCall> list = call("LIST .");
    CHECK(list.has_value());
    if (list) {
        CHECK(list->kind == tools::ToolKind::List);
    }
}

TEST(a_colon_is_still_required_where_being_wrong_costs_something) {
    // `RUN the tests first` is a sentence, and running it because the line
    // opens with a verb would be far worse than not running it. `WRITE /path
    // "fixed the bug"` is worse still: the quoted text is a description of the
    // change, and obeying it would put the description into the file in place
    // of the code.
    CHECK(!tools::parse_tool_call("RUN the tests first", "").has_value());
    CHECK(!tools::parse_tool_call("WRITE calc.py \"Fixed add function\"", "").has_value());
    CHECK(!tools::parse_tool_call("DONE", "").has_value());
}

TEST(a_near_miss_is_recognized_so_it_can_be_corrected) {
    // Not executed -- named, so the cook loop can answer with the syntax
    // instead of a generic "take an action", which is how a whole cook goes by
    // with nothing written.
    CHECK(tools::attempted_tool_call("WRITE calc.py \"Fixed it\"", "")
          == tools::ToolKind::Write);
    CHECK(tools::attempted_tool_call("RUN ./calc.py", "") == tools::ToolKind::Run);
    CHECK(tools::attempted_tool_call("DONE", "") == tools::ToolKind::Done);

    // A correct call is not a near miss, and neither is ordinary prose.
    CHECK(tools::attempted_tool_call("WRITE: calc.py", "") == tools::ToolKind::None);
    CHECK(tools::attempted_tool_call("I will fix the add function.", "")
          == tools::ToolKind::None);
}

TEST(the_instructions_show_the_write_shape_rather_than_only_describing_it) {
    tools::WorkshopSettings settings;
    settings.enabled = true;
    const std::string text =
        tools::workshop_instructions(settings, tools::ToolAudience::Cook);

    // Told only the rules, models write `WRITE path "a description"` and expect
    // it to be applied. A worked example is the cheapest thing that stops that.
    CHECK(text.find("WRITE: src/calc.py") != std::string::npos);
    CHECK(text.find("```") != std::string::npos);
    CHECK(text.find("The colon is required") != std::string::npos);
}

TEST(a_handoff_names_the_next_piece_of_work) {
    const std::optional<tools::ToolCall> call =
        tools::parse_tool_call("HANDOFF: write the API documentation for the parser", "");
    CHECK(call.has_value());
    if (!call) {
        return;
    }
    // It is not executed like a file operation: the cook loop reads it, sends
    // the line back through the delegator, and whoever wins takes the seat.
    CHECK(call->kind == tools::ToolKind::Handoff);
    CHECK_EQ(call->argument, std::string("write the API documentation for the parser"));

    tools::WorkshopSettings settings;
    settings.enabled = true;
    settings.root    = "/tmp";
    CHECK(!tools::run_tool(*call, settings, tools::SearchSettings{}, {}).ok);
}

TEST(the_instructions_offer_a_handoff_to_a_cook_and_not_to_a_chat) {
    tools::WorkshopSettings settings;
    settings.enabled = true;

    // An expert that does not know it can hand over will never do it, and the
    // roster stays a list of seats only one of which ever gets used.
    const std::string cook =
        tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(cook.find("HANDOFF:") != std::string::npos);
    CHECK(cook.find("DONE:") != std::string::npos);

    // A chat turn has no seat to hand over and no loop to say DONE to. Offered
    // them anyway, an expert that has finished the work ends the turn with a
    // status line, and the person who asked gets "DONE: fixed it" instead of an
    // answer.
    const std::string chat =
        tools::workshop_instructions(settings, tools::ToolAudience::Chat);
    CHECK(chat.find("HANDOFF:") == std::string::npos);
    CHECK(chat.find("DONE:") == std::string::npos);

    // The verbs that act on the project are the same either way: what differs
    // is how the work ends, not what may be touched.
    CHECK(chat.find("READ:") != std::string::npos);
    CHECK(chat.find("WRITE:") != std::string::npos);
    CHECK(chat.find("RUN:") != std::string::npos);
}

TEST(a_cook_records_every_expert_that_held_the_seat) {
    Cook cook;
    cook.steps.push_back(step_of(1, "programming", "write", "wrote parse.py", true, 0, {"parse.py"}));
    cook.steps.push_back(step_of(1, "programming", "run",   "$ pytest",       true, 0, {}));
    cook.steps.push_back(step_of(2, "programming", "handoff", "document the parser", true, 0, {}));
    cook.steps.push_back(step_of(2, "language",    "write", "wrote README.md", true, 0, {"README.md"}));
    cook.steps.push_back(step_of(3, "programming", "write", "fixed a typo",    true, 0, {"parse.py"}));

    // In the order they first took the seat, and each once however often they
    // come back.
    const std::vector<ExpertId> used = cook.experts_used();
    CHECK_EQ(used.size(), std::size_t{2});
    CHECK_EQ(used[0], ExpertId("programming"));
    CHECK_EQ(used[1], ExpertId("language"));
}

TEST(a_command_runs_in_the_project_and_reports_its_status) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());

    tools::ToolCall run;
    run.kind     = tools::ToolKind::Run;
#if defined(_WIN32)
    run.argument = "cd && echo hello";   // cmd has no pwd; a bare cd prints the directory
#else
    run.argument = "pwd && echo hello";
#endif
    const tools::ToolResult result = do_call(run, settings);
    CHECK(result.ok);
    CHECK(result.output.find("hello") != std::string::npos);
    CHECK(result.output.find("exit status 0") != std::string::npos);

    run.argument = "exit 3";
    const tools::ToolResult failed = do_call(run, settings);
    CHECK(!failed.ok);
    CHECK(failed.summary.find("exit 3") != std::string::npos);
}

TEST(a_file_named_two_ways_is_recorded_once) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());

    // An expert names the same file relatively once and absolutely the next
    // time, and both are accepted because both are inside the root. Recording
    // what it typed made "3 files changed" a number about the model's phrasing
    // rather than about the project.
    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "src/calc.py";
    write.content  = "x = 1";
    const tools::ToolResult first = do_call(write, settings);
    CHECK(first.ok);

    write.argument = (dir.path() / "src" / "calc.py").string();
    write.content  = "x = 2";
    const tools::ToolResult second = do_call(write, settings);
    CHECK(second.ok);

    CHECK_EQ(first.changed.size(), std::size_t{1});
    CHECK_EQ(second.changed.size(), std::size_t{1});
    if (first.changed.size() != 1 || second.changed.size() != 1) {
        return;  // reported above; indexing an empty list would crash the run
    }
    CHECK_EQ(first.changed[0], std::string("src/calc.py"));
    CHECK_EQ(second.changed[0], first.changed[0]);

    // Which is what makes the journal's file count mean something.
    Cook cook;
    cook.steps.push_back(step_of(1, "programming", "write", "a", true, 0, first.changed));
    cook.steps.push_back(step_of(1, "programming", "write", "b", true, 0, second.changed));
    CHECK_EQ(cook.files_touched().size(), std::size_t{1});
}

TEST(different_work_reaches_a_different_expert) {
    // The whole point of a handoff: the line an expert writes goes back through
    // the delegator, and work of a different kind lands in a different seat.
    // Checked with the keyword router, which needs no model and is therefore
    // the same answer every time.
    CHECK(route_of("write the API documentation and proofread the README prose")
          == "language");
    CHECK(route_of("refactor the parser function and fix the segfault")
          == "programming");
    // Words the Engineering keyword set actually carries -- "preload" and
    // "bolted joint" are in its worked examples, which is what the *model*
    // router reads, not this one.
    CHECK(route_of("what torque should this bearing and weld take") == "engineering");
}

TEST(a_command_starts_in_the_project_but_is_not_confined_to_it) {
    TempDir dir;
    const auto root = dir.path() / "project";
    std::filesystem::create_directories(root);
    { std::ofstream(dir.path() / "outside.txt") << "reachable\n"; }

    tools::ToolCall run;
    run.kind     = tools::ToolKind::Run;
#if defined(_WIN32)
    run.argument = "cd";   // cmd has no pwd; a bare cd prints the directory
#else
    run.argument = "pwd";
#endif
    const tools::ToolResult where =
        tools::run_tool(run, workshop_at(root), tools::SearchSettings{}, {});
    CHECK(where.ok);
    CHECK(where.output.find(root.filename().string()) != std::string::npos);

    // And the part worth stating out loud rather than discovering later: a
    // shell can leave. The file verbs are confined; this is not, which is why
    // it is a switch of its own and why the interface says so. Asserting the
    // real behavior here means a future sandbox has to update this test
    // deliberately rather than quietly appearing to have always worked.
#if defined(_WIN32)
    run.argument = "type ..\\outside.txt";
#else
    run.argument = "cat ../outside.txt";
#endif
    const tools::ToolResult escaped =
        tools::run_tool(run, workshop_at(root), tools::SearchSettings{}, {});
    CHECK(escaped.ok);
    CHECK(escaped.output.find("reachable") != std::string::npos);

    // Whereas the same reach through a file verb is refused.
    tools::ToolCall read;
    read.kind     = tools::ToolKind::Read;
    read.argument = "../outside.txt";
    CHECK(!tools::run_tool(read, workshop_at(root), tools::SearchSettings{}, {}).ok);
}

TEST(a_command_that_never_finishes_is_killed) {
    TempDir dir;
    tools::WorkshopSettings settings = workshop_at(dir.path());
    settings.run_timeout_seconds = 1;

    tools::ToolCall run;
    run.kind     = tools::ToolKind::Run;
#if defined(_WIN32)
    // cmd has no sleep, and timeout refuses to run without a console to read
    // from. ping waits a second between echoes.
    run.argument = "ping -n 30 127.0.0.1 > nul";
#else
    run.argument = "sleep 30";
#endif

    const auto start = std::chrono::steady_clock::now();
    const tools::ToolResult result = do_call(run, settings);
    const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - start);

    // A cook that waits forever on a stuck command has stopped cooking.
    CHECK(!result.ok);
    CHECK(elapsed.count() < 10);
    CHECK(result.summary.find("timed out") != std::string::npos);
}

TEST(running_can_be_switched_off_on_its_own) {
    TempDir dir;
    tools::WorkshopSettings settings = workshop_at(dir.path());
    settings.allow_run = false;

    tools::ToolCall run;
    run.kind     = tools::ToolKind::Run;
    run.argument = "echo nope";
    const tools::ToolResult result = do_call(run, settings);
    CHECK(!result.ok);
    // Reading and writing a project you already trusted is one decision;
    // executing arbitrary commands in it is another.
    CHECK(result.output.find("switched off") != std::string::npos);
}

TEST(nothing_runs_while_the_workshop_is_off) {
    TempDir dir;
    tools::WorkshopSettings settings = workshop_at(dir.path());
    settings.enabled = false;

    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "made.txt";
    write.content  = "x";
    CHECK(!do_call(write, settings).ok);
    CHECK(!std::filesystem::exists(dir.path() / "made.txt"));
}

TEST(long_output_keeps_its_head_and_its_tail) {
    std::string log;
    for (int i = 0; i < 500; ++i) {
        log += "line " + std::to_string(i) + "\n";
    }
    const std::string clamped = tools::clamp_output(log, 400);

    CHECK(clamped.size() < log.size());
    // A compiler puts the first error at the top and the summary at the bottom,
    // and the middle of a long log is the part nobody reads.
    CHECK(clamped.find("line 0") != std::string::npos);
    CHECK(clamped.find("line 499") != std::string::npos);
    CHECK(clamped.find("not shown") != std::string::npos);
    CHECK(clamped.find("line 250") == std::string::npos);
}

TEST(the_instructions_only_offer_what_the_settings_allow) {
    tools::WorkshopSettings settings;
    // Off: say nothing. An untrusted folder produces exactly this.
    CHECK(tools::workshop_instructions(settings, tools::ToolAudience::Cook).empty());
    CHECK(tools::workshop_instructions(settings, tools::ToolAudience::Chat).empty());

    settings.enabled = true;
    const std::string on =
        tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(on.find("READ:") != std::string::npos);
    CHECK(on.find("RUN:") != std::string::npos);

    settings.allow_run = false;
    const std::string no_run =
        tools::workshop_instructions(settings, tools::ToolAudience::Cook);
    CHECK(no_run.find("READ:") != std::string::npos);
    // Offering a verb that will be refused wastes a whole round trip and
    // teaches the model the protocol is unreliable.
    CHECK(no_run.find("RUN:") == std::string::npos);
}

TEST(a_write_records_what_it_changed) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());
    { std::ofstream(dir.path() / "calc.py") << "def add(a, b):\n    return a - b\n"; }

    tools::ToolCall write;
    write.kind     = tools::ToolKind::Write;
    write.argument = "calc.py";
    write.content  = "def add(a, b):\n    return a + b\n";

    const tools::ToolResult result = do_call(write, settings);
    CHECK(result.ok);
    CHECK(result.summary.find("+1 -1") != std::string::npos);
    CHECK(result.detail.find("-    return a - b") != std::string::npos);
    CHECK(result.detail.find("+    return a + b") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Web search
// ---------------------------------------------------------------------------

TEST(a_query_is_encoded_before_it_becomes_a_url) {
    // The query is whatever the model wrote. An unencoded '&' turns one
    // parameter into two, and an unencoded newline splits the request.
    tools::SearchSettings settings;
    settings.provider = "wikipedia";

    const std::string url = tools::request_url("a&b c=d\ne", settings);
    CHECK(url.find("a%26b%20c%3Dd%0Ae") != std::string::npos);
    CHECK(url.find('\n') == std::string::npos);
    // And what is safe is left alone, so a URL stays readable.
    CHECK(tools::request_url("linux-kernel_v6.1~rc", settings).find(
              "linux-kernel_v6.1~rc") != std::string::npos);
}

TEST(each_provider_builds_the_url_it_needs) {
    tools::SearchSettings settings;

    settings.provider = "wikipedia";
    CHECK(tools::request_url("cats", settings).rfind("https://en.wikipedia.org/w/api.php", 0) == 0);

    // searxng is somebody's own instance, so without its address there is
    // nothing to ask -- and returning a half-built URL would send the query to
    // whatever happened to answer.
    settings.provider = "searxng";
    CHECK(tools::request_url("cats", settings).empty());
    settings.endpoint = "http://localhost:8888/";
    const std::string searx = tools::request_url("cats", settings);
    CHECK_EQ(searx, "http://localhost:8888/search?format=json&q=cats");  // no doubled slash

    settings.provider = "brave";
    CHECK(tools::request_url("cats", settings).rfind("https://api.search.brave.com", 0) == 0);

    // An unknown provider asks nobody.
    settings.provider = "altavista";
    CHECK(tools::request_url("cats", settings).empty());

    // And an empty query is not a search.
    settings.provider = "wikipedia";
    CHECK(tools::request_url("", settings).empty());
}

TEST(a_wikipedia_response_becomes_results) {
    // Trimmed from a real response. The snippet arrives with the matched words
    // wrapped in markup, which an expert should not have to read around.
    const std::string body = R"({"query":{"search":[
        {"title":"List of capitals of France","snippet":"The <span class=\"searchmatch\">capital</span>\n  of France has been Paris"},
        {"title":"Paris","snippet":"Paris is the capital of France"}]}})";

    const std::vector<tools::SearchResult> results = tools::parse_results("wikipedia", body, 5);
    CHECK_EQ(results.size(), std::size_t{2});
    CHECK_EQ(results[0].title, "List of capitals of France");
    CHECK_EQ(results[0].snippet, "The capital of France has been Paris");
    CHECK_EQ(results[0].url, "https://en.wikipedia.org/wiki/List%20of%20capitals%20of%20France");
    CHECK_EQ(results[1].title, "Paris");
}

TEST(the_result_limit_is_honored_whatever_the_provider_sent) {
    const std::string body =
        R"({"results":[{"url":"a","title":"A"},{"url":"b","title":"B"},{"url":"c","title":"C"}]})";
    CHECK_EQ(tools::parse_results("searxng", body, 2).size(), std::size_t{2});
    CHECK_EQ(tools::parse_results("searxng", body, 99).size(), std::size_t{3});
}

TEST(a_response_that_is_not_what_was_expected_yields_nothing) {
    // A rate-limit page, an error object, a truncated body. Every one of these
    // has to come back empty rather than throw out of the engine thread.
    CHECK(tools::parse_results("wikipedia", "<html>rate limited</html>", 5).empty());
    CHECK(tools::parse_results("wikipedia", R"({"error":{"code":"badvalue"}})", 5).empty());
    CHECK(tools::parse_results("searxng",   R"({"results":"not an array"})", 5).empty());
    CHECK(tools::parse_results("brave",     R"({"web":{}})", 5).empty());
    CHECK(tools::parse_results("wikipedia", "", 5).empty());
    CHECK(tools::parse_results("nobody",    R"({"query":{"search":[]}})", 5).empty());
}

TEST(an_expert_asks_to_search_on_a_line_of_its_own) {
    CHECK_EQ(tools::search_request("SEARCH: rust 1.90 release date", ""),
             "rust 1.90 release date");
    CHECK_EQ(tools::search_request("Let me look that up.\nSEARCH: tallest building 2026", ""),
             "tallest building 2026");
    // Models dress it up; the query is still the query.
    CHECK_EQ(tools::search_request("**SEARCH:** who won the 2025 world cup**", ""),
             "who won the 2025 world cup");
    CHECK_EQ(tools::search_request("  SEARCH:   spaced out  ", ""), "spaced out");
}

TEST(an_expert_talking_about_searching_is_not_searching) {
    // The failure that matters: an expert explaining the tool, or quoting the
    // instructions back, must not be taken as using it.
    CHECK(tools::search_request("You can write SEARCH: followed by a query.", "").empty());
    CHECK(tools::search_request("The answer is 42.", "").empty());
    CHECK(tools::search_request("", "").empty());
    // A marker with nothing after it is not a query either.
    CHECK(tools::search_request("SEARCH:", "").empty());
}

TEST(a_models_own_tool_call_is_recognized_as_a_search) {
    // What gpt-oss actually does when told it can search: it ignores the
    // convention it was given and writes a call in its own format, on the
    // channel meant for tool calls -- which arrives here as reasoning, with
    // nothing at all on the channel the user reads.
    CHECK_EQ(tools::search_request(
                 "", R"(We need to search.{"query": "JWST launch date", "topn": 5})"),
             "JWST launch date");

    // Nested objects must not end the scan early.
    CHECK_EQ(tools::search_request("", R"({"args": {"depth": 2}, "query": "nested"})"),
             "nested");
}

TEST(a_model_that_answered_is_not_also_asking_to_search) {
    // The reasoning is only read when nothing was said to the user. Otherwise a
    // programming expert showing you a JSON object with a "query" field would
    // send that field to a search engine.
    CHECK(tools::search_request(R"(Here is the payload: {"query": "select 1"})",
                                R"(I should show them {"query": "select 1"})")
              .empty());
    // Whitespace is not an answer, though.
    CHECK_EQ(tools::search_request("  \n ", R"({"query": "still asking"})"), "still asking");
}

TEST(search_results_are_handed_to_the_expert_as_readable_text) {
    const std::vector<tools::SearchResult> results{
        {"Paris", "https://example.org/paris", "The capital of France."}};
    const std::string text = tools::format_for_model("capital of france", results);
    CHECK(text.find("capital of france") != std::string::npos);
    CHECK(text.find("https://example.org/paris") != std::string::npos);
    CHECK(text.find("The capital of France.") != std::string::npos);

    // Nothing found is said plainly rather than as an empty list, so the expert
    // answers from what it knows instead of inventing a citation.
    const std::string nothing = tools::format_for_model("obscure thing", {});
    CHECK(nothing.find("returned nothing") != std::string::npos);
}

TEST(search_does_nothing_at_all_until_it_is_switched_on) {
    // The whole reason the setting exists: no request leaves the machine while
    // it is off, whatever a model asks for.
    tools::SearchSettings settings;
    CHECK(!settings.enabled);
    std::string error;
    CHECK(tools::search("anything", settings, error).empty());
    CHECK(error.find("off") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Subprocess
// ---------------------------------------------------------------------------

TEST(a_child_process_reports_its_output_and_status) {
    util::Subprocess child;
    std::string error;
#if defined(_WIN32)
    CHECK(child.start(util::shell_command("echo one& echo two 1>&2& exit 3"), {}, {}, error));
#else
    CHECK(child.start({"sh", "-c", "echo one; echo two >&2; exit 3"}, {}, {}, error));
#endif
    CHECK(error.empty());

    std::vector<std::string> lines;
    std::string line;
    while (child.read_line(line)) {
        lines.push_back(line);
    }
    // stdout and stderr are merged, so both appear.
    CHECK_EQ(lines.size(), std::size_t{2});
    CHECK_EQ(child.wait(), 3);
    // wait() is idempotent: the builder calls it from more than one place.
    CHECK_EQ(child.wait(), 3);
}

TEST(a_command_that_does_not_exist_fails_rather_than_hanging) {
    util::Subprocess child;
    std::string error;
#if defined(_WIN32)
    // CreateProcess looks the program up itself and fails on the spot, so on
    // Windows the failure arrives at start() rather than as an exit status.
    CHECK(!child.start({"crucible-no-such-program"}, {}, {}, error));
    CHECK(!error.empty());
#else
    CHECK(child.start({"crucible-no-such-program"}, {}, {}, error));

    std::string line;
    while (child.read_line(line)) {
        // drain
    }
    // execvp failed in the child, which exits 127 the way a shell would.
    CHECK_EQ(child.wait(), 127);
#endif
}

TEST(on_path_finds_real_programs_and_not_invented_ones) {
#if defined(_WIN32)
    CHECK(util::on_path("cmd"));
#else
    CHECK(util::on_path("sh"));
#endif
    CHECK(!util::on_path("crucible-definitely-not-a-program"));
    // An empty requirement means "nothing needed", which is how a backend with
    // no SDK says so.
    CHECK(util::on_path(""));
}
