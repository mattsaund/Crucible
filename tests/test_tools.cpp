// SPDX-License-Identifier: MIT
//
// The tools an expert can reach: the workshop sandbox, web search, and the
// subprocess primitive underneath both.
#include "test_helpers.hpp"

#include <atomic>
#include <chrono>
#include <system_error>

#include "crucible/tools/native_tools.hpp"
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

    // A file with fences of its own: the body ended at the README's ```bash
    // and half the file was written, as if that were all of it.
    CHECK_EQ(body("WRITE: README.md\n```\n# Todo\n```bash\nnpm install\n```\nMore.\n```\n"),
             std::string("# Todo\n```bash\nnpm install\n```\nMore."));
    CHECK_EQ(body("WRITE: README.md\n````markdown\nA\n```\nB\n```\n````\nafter"),
             std::string("A\n```\nB\n```"));
    CHECK_EQ(body("WRITE: a.md\n~~~\n```js\nx\n```\n~~~\n"), std::string("```js\nx\n```"));

    // A block of its own with no language named, a haiku shown in a README:
    // its bare opening fence reads as the body's end. The reply ends on the
    // body's own closing fence, and that is the end, whatever came between.
    CHECK_EQ(body("WRITE: README.md\n```\n# Haiku\n\n## The Haiku\n\n```\nRed leaves fall,\n```\n\n"
                  "## Imagery\nWarm.\n```\n"),
             std::string("# Haiku\n\n## The Haiku\n\n```\nRed leaves fall,\n```\n\n## Imagery\nWarm."));
    CHECK_EQ(body("WRITE: a.md\nText\n"), std::string("Text"));
    CHECK_EQ(body("WRITE: a.md\n```\nText\n```\ncode\n```\n```\n"), std::string("Text\n```\ncode\n```"));
    // The same with the body's own last fence left off, as GPT OSS leaves it:
    // the block's two fences are a pair, and the body runs to the end.
    CHECK_EQ(body("WRITE: README.md\n```\n# Haiku\nThe haiku is:\n\n```\nRed leaves\n```\n\nWarm."),
             std::string("# Haiku\nThe haiku is:\n\n```\nRed leaves\n```\n\nWarm."));
    CHECK_EQ(body("WRITE: a.txt\n```\nabc\ndef"), std::string("abc\ndef"));
    // Words after the body's end, and a block of a named language after
    // them, are not the file's.
    CHECK_EQ(body("WRITE: a.txt\n```\nabc\n```\nThen run it:\n```bash\nnode a.js\n```\n"), std::string("abc"));
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

    // And a link to something that is not there yet: there was nothing for
    // weakly_canonical to resolve, so it passed, and a WRITE through it
    // created its target outside the project.
    std::filesystem::create_symlink(dir.path() / "planted.txt", root / "notes.txt", ec);
    CHECK(!tools::resolve_in_root(root, "notes.txt").has_value());
    // A link that stays inside is still a path like any other.
    std::filesystem::create_symlink(root / "real.txt", root / "alias.txt", ec);
    CHECK(tools::resolve_in_root(root, "alias.txt").has_value());
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

TEST(a_long_file_is_read_at_both_ends_numbered_where_its_lines_are) {
    TempDir dir;
    tools::WorkshopSettings settings = workshop_at(dir.path());
    settings.max_read_bytes = 2000;
    {
        std::ofstream out(dir.path() / "big.txt", std::ios::binary);
        for (int i = 1; i <= 10000; ++i) {
            out << "line " << i << "\r\n";
        }
    }
    tools::ToolCall read;
    read.kind     = tools::ToolKind::Read;
    read.argument = "big.txt";
    const tools::ToolResult whole = do_call(read, settings);
    CHECK(whole.ok);
    CHECK(whole.output.find("1\tline 1\n") != std::string::npos);
    // The end is numbered as the end, not as if it followed the start.
    CHECK(whole.output.find("10000\tline 10000\n") != std::string::npos);
    CHECK(whole.output.find("not shown; READ: big.txt ") != std::string::npos);
    CHECK(whole.output.size() < 2600);

    // And the middle can be asked for.
    read.argument = "big.txt 5000-5002";
    const tools::ToolResult middle = do_call(read, settings);
    CHECK(middle.ok);
    CHECK(middle.output.find("5000\tline 5000\n5001\tline 5001\n5002\tline 5002\n") != std::string::npos);
    CHECK(middle.output.find("4999\t") == std::string::npos);
    CHECK(middle.output.find("5003\t") == std::string::npos);

    // A file that is not text is said to be, not shown.
    { std::ofstream out(dir.path() / "blob.bin", std::ios::binary); out << std::string("\x7f" "ELF\0\0\x01", 7); }
    read.argument = "blob.bin";
    const tools::ToolResult blob = do_call(read, settings);
    CHECK(blob.ok);
    CHECK(blob.output.find("not text") != std::string::npos);
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

TEST(a_sentence_where_the_path_goes_is_shown_the_line_it_should_have_been) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());
    tools::ToolCall write;
    write.kind    = tools::ToolKind::Write;
    write.content = "header { color: blue; }";

    // The account of the change where the path goes, with the file in it: the
    // correction is one line, and its WRITE line names that file.
    write.argument = "Change the header color to blue in style.css";
    const tools::ToolResult named = do_call(write, settings);
    CHECK(!named.ok);
    CHECK(named.output.find("is a sentence, not a path") != std::string::npos);
    CHECK(named.output.find("WRITE: style.css,") != std::string::npos);
    CHECK(named.output.find('\n') == std::string::npos);
    write.argument = "Use e.g. a darker blue in style.css.";
    CHECK(do_call(write, settings).output.find("WRITE: style.css,") != std::string::npos);

    // No file in it, or two of them: none is guessed at.
    for (const char* sentence : {"The fix is complete and verified.", "Move the rules from style.css into theme.css"}) {
        write.argument = sentence;
        CHECK(do_call(write, settings).output.find("WRITE: and the file's path") != std::string::npos);
    }

    // A name with a space in it is not a sentence, and is told what a path is.
    write.argument = "my notes.md";
    CHECK(do_call(write, settings).output.find("is not a file name") != std::string::npos);

    // The same test keeps a sentence from being put to the person as an edit.
    CHECK(!tools::plausible_path("Change the header color to blue in style.css"));
    CHECK(tools::plausible_path("src/app.js"));
    std::string why;
    tools::ToolCall edit;
    edit.kind     = tools::ToolKind::Edit;
    edit.argument = "Make the header blue in style.css";
    CHECK(!tools::edited_contents(edit, settings, why).has_value());
    CHECK(why.find("EDIT: style.css,") != std::string::npos);
}

TEST(an_ordinary_path_is_still_written) {
    TempDir dir;
    const tools::WorkshopSettings settings = workshop_at(dir.path());

    for (const char* path : {"calc.py", "src/calc.py", "a/b/c/deep-file.txt",
                             "./relative.md", "with_underscores.and.dots",
                             "app/[id]/page.tsx", "app/(shop)/[...slug]/page.tsx", "app/routes/$id.tsx"}) {
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
    // Not executed -- named, so the loop can answer with the syntax instead
    // of a generic "take an action", which is how a whole prompt goes by with
    // nothing written.
    CHECK(tools::attempted_tool_call("WRITE calc.py \"Fixed it\"", "")
          == tools::ToolKind::Write);
    CHECK(tools::attempted_tool_call("RUN ./calc.py", "") == tools::ToolKind::Run);
    CHECK(tools::attempted_tool_call("DONE", "") == tools::ToolKind::Done);

    // A correct call is not a near miss, and neither is ordinary prose.
    CHECK(tools::attempted_tool_call("WRITE: calc.py", "") == tools::ToolKind::None);
    CHECK(tools::attempted_tool_call("I will fix the add function.", "")
          == tools::ToolKind::None);
}

TEST(a_write_or_an_edit_after_a_sentence_on_its_line_is_a_near_miss_and_runs_nothing) {
    // LFM2.5 wrote its call at the end of a sentence, with the body after it,
    // and the call itself became the answer. It is not run (a call starts a
    // line), but it is named, so the loop can say where it goes.
    const char* const write = "Here is the page. WRITE: index.html\n```\n<h1>Hi</h1>\n```\n";
    CHECK(!tools::parse_tool_call(write, "").has_value());
    CHECK(tools::call_inside_a_line(write, "") == tools::ToolKind::Write);
    const char* const edit = "I'll make the header blue: EDIT: style.css\n<<<<<<< SEARCH\nh1 { }\n"
                             "=======\nh1 { color: blue; }\n>>>>>>> REPLACE\n";
    CHECK(!tools::parse_tool_call(edit, "").has_value());
    CHECK(tools::call_inside_a_line(edit, "") == tools::ToolKind::Edit);

    // A sentence about one, with nothing to write after it, is prose. So is a
    // RUN in a sentence, whatever follows, and a word that ends in a verb.
    CHECK(tools::call_inside_a_line("Use WRITE: path when you want a file.", "") == tools::ToolKind::None);
    CHECK(tools::call_inside_a_line("You can RUN: npm start\n```\nnpm start\n```", "")
          == tools::ToolKind::None);
    CHECK(tools::call_inside_a_line("Then REWRITE: a.txt\n```\nx\n```", "") == tools::ToolKind::None);
    // A call on a line of its own is a call, not this.
    CHECK(tools::call_inside_a_line("WRITE: a.txt\n```\nx\n```", "") == tools::ToolKind::None);
}

TEST(a_call_in_square_brackets_on_a_line_of_its_own_is_the_call) {
    // "[EDIT: style.css]" above its blocks, as LFM2.5 wrote one: the brackets
    // are set aside, and the edit is the edit.
    const auto edit = tools::parse_tool_call("[EDIT: style.css]\n<<<<<<< SEARCH\nh1 { }\n=======\n"
                                             "h1 { color: blue; }\n>>>>>>> REPLACE\n", "");
    CHECK(edit.has_value());
    if (edit) {
        CHECK(edit->kind == tools::ToolKind::Edit);
        CHECK_EQ(edit->argument, std::string("style.css"));
        CHECK(edit->content.find("h1 { color: blue; }") != std::string::npos);
    }
    const auto write = tools::parse_tool_call("[WRITE: a.txt]\n```\nhello\n```\n", "");
    CHECK(write.has_value());
    if (write) {
        CHECK(write->kind == tools::ToolKind::Write);
        CHECK_EQ(write->argument, std::string("a.txt"));
        CHECK_EQ(write->content, std::string("hello"));
    }
    // A link, a ticked box and a bracket inside are prose.
    CHECK(!tools::parse_tool_call("[READ: the guide](https://example.com/guide)", "").has_value());
    CHECK(!tools::parse_tool_call("[x] READ: done", "").has_value());
    CHECK(!tools::parse_tool_call("[READ: a] and [READ: b]", "").has_value());
}

TEST(a_tool_a_model_calls_in_its_own_format_is_the_call_the_workshop_runs) {
    using tools::ToolKind;
    using tools::call_from_native;
    std::string why;
    const auto check = [&](const char* name, const char* arguments, ToolKind kind, const std::string& argument,
                           const std::string& content = {}) {
        const auto call = call_from_native(name, arguments, why);
        CHECK(call.has_value());
        if (call) {
            CHECK(call->kind == kind);
            CHECK_EQ(call->argument, argument);
            CHECK_EQ(call->content, content);
        }
    };
    check("list_files", "{}", ToolKind::List, ".");
    check("list_files", R"({"path":"src"})", ToolKind::List, "src");
    check("read_file", R"({"path":"app.js"})", ToolKind::Read, "app.js");
    check("read_file", R"({"path":"app.js","start_line":10,"end_line":20})", ToolKind::Read, "app.js 10-20");
    // A number written as a string, and a start with no end.
    check("read_file", R"({"path":"app.js","start_line":"10"})", ToolKind::Read, "app.js 10-409");
    check("find_in_files", R"({"text":"color","files":"*.css"})", ToolKind::Find, "color in *.css");
    check("write_file", R"({"path":"a.txt","content":"hello\nworld\n"})", ToolKind::Write, "a.txt", "hello\nworld\n");
    // An empty file is a file.
    check("write_file", R"({"path":"a.txt","content":""})", ToolKind::Write, "a.txt", "");
    check("edit_file", R"({"path":"a.css","search":"color: red;","replace":"color: blue;"})", ToolKind::Edit, "a.css",
          "<<<<<<< SEARCH\ncolor: red;\n=======\ncolor: blue;\n>>>>>>> REPLACE\n");
    // Deleting text is replacing it with nothing.
    check("edit_file", R"({"path":"a.css","search":"x","replace":""})", ToolKind::Edit, "a.css",
          "<<<<<<< SEARCH\nx\n=======\n\n>>>>>>> REPLACE\n");
    check("render_page", R"({"page":"index.html","output":"p.png","size":"1280x800"})", ToolKind::Render,
          "index.html to p.png at 1280x800");
    check("run_command", R"({"command":"npm test"})", ToolKind::Run, "npm test");
    check("run_python", R"j({"code":"print(1)"})j", ToolKind::Python, "", "print(1)");
    check("git", R"({"arguments":"status"})", ToolKind::Git, "status");
    check("hand_off", R"({"next_step":"write the README"})", ToolKind::Handoff, "write the README");
    check("web_search", R"({"query":"llama"})", ToolKind::Search, "llama");
    check("click", R"({"x":10,"y":"20","button":"right","double":true})", ToolKind::Click, "10 20 right double");
    check("use_tool", R"({"tool":"github/create_issue","arguments":{"title":"x"}})", ToolKind::Mcp,
          "github/create_issue", R"({"title":"x"})");
    // The names other people's tools use for the same things.
    check("read_file", R"({"file_path":"a.txt"})", ToolKind::Read, "a.txt");
    check("edit_file", R"({"file_path":"a","old_string":"x","new_string":"y"})", ToolKind::Edit, "a",
          "<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE\n");

    // What it left out, or made up, is said rather than run.
    const auto refused = [&](const char* name, const char* arguments, const std::string& says) {
        why.clear();
        CHECK(!call_from_native(name, arguments, why).has_value());
        CHECK(why.find(says) != std::string::npos);
    };
    refused("read_file", "{}", "needs a path");
    refused("write_file", R"({"path":"a.txt"})", "contents");
    refused("edit_file", R"({"path":"a.txt","search":"","replace":"x"})", "text to replace");
    refused("edit_file", R"({"path":"a.txt","search":"x"})", "what it becomes");
    refused("run_command", "{}", "a command");
    refused("read_file", "not json", "not a JSON object");
    refused("read_file", "[1]", "not a JSON object");
    refused("delete_everything", "{}", "no tool called delete_everything");
}

TEST(a_path_written_from_the_top_of_the_project_is_in_the_project) {
    TempDir dir;
    const std::filesystem::path root = std::filesystem::weakly_canonical(dir.path()) / "budget";
    std::filesystem::create_directories(root / "src");
    std::ofstream(root / "README.md") << "x";
    const auto at = [&](const char* written) { return tools::resolve_in_root(root, written); };
    // One name, from a folder it has, from its own name, or a file it has with
    // a home folder made up around it.
    CHECK(at("/no-such-page.html") == root / "no-such-page.html");
    CHECK(at("/src/no-such.js") == root / "src" / "no-such.js");
    CHECK(at("/budget/no-such.html") == root / "no-such.html");
    CHECK(at("/home/nobody-here/project/README.md") == root / "README.md");
    // What is really on the disk outside it stays outside.
    CHECK(!at("/etc/hosts").has_value());
#if !defined(_WIN32)
    CHECK(!at("/usr").has_value());   // a folder here, and no such thing on Windows
#endif
    // And a made-up folder for a file it does not have is not made in it.
    CHECK(!at("/home/nobody-here/project/new.txt").has_value());
    // Climbing out is climbing out, however it is written.
    CHECK(!at("/src/../../outside.txt").has_value());
    CHECK(!at("../outside.txt").has_value());
}

TEST(a_call_a_model_spelled_out_in_its_own_format_is_still_the_call) {
    using tools::ToolKind;
    using tools::call_in_text;
    const auto is = [](std::string_view text, ToolKind kind, const std::string& argument, const std::string& content,
                       const std::string& prose) {
        const auto found = call_in_text(text);
        CHECK(found.has_value());
        if (found) {
            CHECK(found->call.kind == kind);
            CHECK_EQ(found->call.argument, argument);
            CHECK_EQ(found->call.content, content);
            CHECK_EQ(found->prose, prose);
        }
    };
    // LFM, after a sentence on the same line, as LFM2.5 2.6B wrote it.
    is("Now the README.<|tool_call_start|>[read_file(path='app.js')]<|tool_call_end|>", ToolKind::Read, "app.js", "",
       "Now the README.");
    is("<|tool_call_start|>[write_file(path='a.html', content='<b class=\"x\">it\\'s</b>\\n')]<|tool_call_end|>\n",
       ToolKind::Write, "a.html", "<b class=\"x\">it's</b>\n", "");
    // Qwen.
    is("Writing it.\n\n<tool_call>\n<function=write_file>\n<parameter=path>\na.txt\n</parameter>\n"
       "<parameter=content>\nline one\nline two\n</parameter>\n</function>\n</tool_call>",
       ToolKind::Write, "a.txt", "line one\nline two", "Writing it.");
    // flint.
    is("<tool_call>edit_file<arg_key>path</arg_key><arg_value>a.css</arg_value><arg_key>search</arg_key>"
       "<arg_value>red</arg_value><arg_key>replace</arg_key><arg_value>blue</arg_value></tool_call>",
       ToolKind::Edit, "a.css", "<<<<<<< SEARCH\nred\n=======\nblue\n>>>>>>> REPLACE\n", "");
    is("<tool_call>list_files</tool_call>", ToolKind::List, ".", "", "");
    // Hermes.
    is(R"(<tool_call>{"name": "run_command", "arguments": {"command": "npm test"}}</tool_call>)", ToolKind::Run,
       "npm test", "", "");
    // gpt-oss.
    is(R"(Next.<|start|>assistant<|channel|>commentary to=functions.read_file <|constrain|>json<|message|>{"path":"a.js"}<|call|>)",
       ToolKind::Read, "a.js", "", "Next.");

    // Not a whole call, not the end of the reply, or not a tool: it is words.
    CHECK(!call_in_text("").has_value());
    CHECK(!call_in_text("Just an answer.").has_value());
    CHECK(!call_in_text("<tool_call>").has_value());
    CHECK(!call_in_text("<tool_call>\n\n\n\n<tool_call>").has_value());
    CHECK(!call_in_text("<tool_call>delete_everything</tool_call>").has_value());
    CHECK(!call_in_text("<tool_call>read_file<arg_key>path</arg_key>").has_value());
    CHECK(!call_in_text("Models write <tool_call>read_file</tool_call> like this, in their own format.").has_value());
    CHECK(!call_in_text("<|tool_call_start|>[write_file(path='a.txt', content='never clo").has_value());
}

TEST(a_model_is_offered_the_tools_the_folder_allows_and_every_one_of_them_can_be_called) {
    TempDir dir;
    tools::WorkshopSettings settings;
    CHECK(tools::native_tools(settings, false).empty());
    CHECK(tools::native_instructions(settings).empty());

    settings.enabled = true;
    settings.root    = dir.path();
    const auto names = [](const nlohmann::ordered_json& list) {
        std::set<std::string> out;
        for (const nlohmann::ordered_json& one : list) {
            CHECK_EQ(one.value("type", std::string()), std::string("function"));
            const nlohmann::ordered_json& function = one["function"];
            CHECK(!function.value("description", std::string()).empty());
            CHECK_EQ(function["parameters"].value("type", std::string()), std::string("object"));
            CHECK(out.insert(function.value("name", std::string())).second);
        }
        return out;
    };
    const std::set<std::string> everything = names(tools::native_tools(settings, false));
    for (const char* const name : {"list_files", "read_file", "find_in_files", "write_file", "edit_file", "render_page",
                                   "run_command", "run_python", "git", "hand_off"}) {
        CHECK(everything.count(name) == 1);
    }
    CHECK(everything.count("web_search") == 0);
    CHECK(everything.count("screenshot") == 0);

    settings.allow_run = false;
    const std::set<std::string> no_commands = names(tools::native_tools(settings, false));
    for (const char* const name : {"run_command", "run_python", "git", "github", "start_process"}) {
        CHECK(no_commands.count(name) == 0);
    }
    CHECK(no_commands.count("write_file") == 1);
    settings.allow_run = true;

    settings.web              = true;
    settings.computer_control = true;
    const nlohmann::ordered_json all = tools::native_tools(settings, true);
    CHECK(names(all).count("fetch_url") == 1);
    CHECK(names(all).count("web_search") == 1);
    CHECK(names(all).count("screenshot") == 1);

    // Each one, called with what it says it requires, is a call the workshop
    // can run: the two lists cannot drift apart.
    for (const nlohmann::ordered_json& one : all) {
        const nlohmann::ordered_json& function = one["function"];
        nlohmann::json                arguments = nlohmann::json::object();
        for (const nlohmann::ordered_json& field : function["parameters"]["required"]) {
            const std::string type =
                function["parameters"]["properties"][field.get<std::string>()].value("type", std::string());
            arguments[field.get<std::string>()] = type == "integer" ? nlohmann::json(1) : nlohmann::json("x");
        }
        std::string why;
        const auto  call = tools::call_from_native(function.value("name", std::string()), arguments.dump(), why);
        CHECK(call.has_value());
        CHECK(why.empty());
    }

    // A file's path comes before its contents: llama.cpp's parsers hold a
    // model to this order, and sorted by name flint could not write a file.
    for (const nlohmann::ordered_json& one : all) {
        if (one["function"].value("name", std::string()) == "write_file") {
            CHECK_EQ(one["function"]["parameters"]["properties"].begin().key(), std::string("path"));
        }
    }

    // Told to do the work with them, and that a reply writes nothing.
    const std::string told = tools::native_instructions(settings);
    CHECK(told.find("showing its contents") != std::string::npos);
    CHECK(told.find("The project folder is empty") != std::string::npos);
    std::ofstream(dir.path() / "a.txt") << "x";
    CHECK(tools::native_instructions(settings).find("The project folder is empty") == std::string::npos);
}

TEST(the_instructions_show_the_write_shape_rather_than_only_describing_it) {
    tools::WorkshopSettings settings;
    settings.enabled = true;
    const std::string text = tools::workshop_instructions(settings);

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
    // It is not executed like a file operation: the loop reads it, sends the
    // line through the delegator, and whoever wins takes the seat.
    CHECK(call->kind == tools::ToolKind::Handoff);
    CHECK_EQ(call->argument, std::string("write the API documentation for the parser"));

    tools::WorkshopSettings settings;
    settings.enabled = true;
    settings.root    = "/tmp";
    CHECK(!tools::run_tool(*call, settings, tools::SearchSettings{}, {}).ok);
}

TEST(the_instructions_offer_a_handoff_and_not_done) {
    tools::WorkshopSettings settings;
    settings.enabled = true;

    // A prompt is worked in one loop, and the next step can go to another
    // expert, so a turn may hand off: an expert that does not know it can
    // hand over will never do it, and the roster stays a list of seats only
    // one of which ever gets used. It is not told DONE: offered it, an expert
    // that has finished the work ends the turn with a status line, and the
    // person who asked gets "DONE: fixed it" instead of an answer.
    const std::string text = tools::workshop_instructions(settings);
    CHECK(text.find("HANDOFF: <the next step>") != std::string::npos);
    CHECK(text.find("DONE:") == std::string::npos);

    // Every verb that acts on the project is there, whoever asks: what a
    // prompt may touch is the folder's to say.
    CHECK(text.find("READ:") != std::string::npos);
    CHECK(text.find("WRITE:") != std::string::npos);
    CHECK(text.find("RUN:") != std::string::npos);
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

    // A prompt that waits forever on a stuck command has stopped working.
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
    CHECK(tools::workshop_instructions(settings).empty());

    settings.enabled = true;
    const std::string on = tools::workshop_instructions(settings);
    CHECK(on.find("READ:") != std::string::npos);
    CHECK(on.find("RUN:") != std::string::npos);

    settings.allow_run = false;
    const std::string no_run = tools::workshop_instructions(settings);
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

#if !defined(_WIN32)
TEST(a_child_that_ignores_being_asked_to_stop_is_made_to) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    // SIGTERM ignored: the deadline asks, and three seconds later insists.
    const util::Ran ran = util::run({"sh", "-c", "trap '' TERM; echo begun; sleep 60"}, {}, {}, 1);
    const double took = std::chrono::duration<double>(Clock::now() - started).count();
    CHECK(ran.started);
    CHECK(ran.timed_out);
    CHECK(ran.output.find("begun") != std::string::npos);
    CHECK(took < 10.0);
}

TEST(a_command_is_over_when_it_is_not_what_it_left_running) {
    using Clock = std::chrono::steady_clock;
    const auto started = Clock::now();
    // The background sleep holds the output open long after the command is
    // done; what it printed is all there was.
    const util::Ran ran = util::run({"sh", "-c", "sleep 30 & echo done"}, {}, {}, 20);
    const double took = std::chrono::duration<double>(Clock::now() - started).count();
    CHECK(!ran.timed_out);
    CHECK_EQ(ran.status, 0);
    CHECK(ran.output.find("done") != std::string::npos);
    CHECK(took < 8.0);
}

TEST(output_without_end_is_kept_at_its_start_and_its_end) {
    const util::Ran ran = util::run({"sh", "-c", "echo first; yes 0123456789 | head -n 200000; echo last"},
                                    {}, {}, 20, {}, 64 * 1024);
    CHECK_EQ(ran.status, 0);
    CHECK(ran.output.size() < 70 * 1024);
    CHECK(ran.output.rfind("first\n", 0) == 0);
    CHECK(ran.output.find("bytes not kept") != std::string::npos);
    CHECK(ran.output.size() >= 5 && ran.output.compare(ran.output.size() - 5, 5, "last\n") == 0);
}

TEST(a_stop_ends_a_run) {
    std::atomic<int> asked{0};
    const util::Ran ran = util::run({"sh", "-c", "sleep 30"}, {}, {}, 60,
                                    [&asked] { return ++asked > 10; });
    CHECK(ran.stopped);
    CHECK(!ran.timed_out);
}
#endif

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

TEST(an_edit_can_say_which_of_two_identical_lines_it_means) {
    const std::string text = "a\nsame\nb\nc\nd\ne\nsame\nf\n";
    std::string error;
    // Not said: refused, with where they are, so nothing has to be read again.
    CHECK(!tools::apply_edit_blocks(text, {tools::EditBlock{"same", "new"}}, error));
    CHECK(error.find("2 times, at lines 2 and 7") != std::string::npos);
    CHECK(error.find("at_line") != std::string::npos);
    // Said, it changes that one and leaves the other.
    auto changed = tools::apply_edit_blocks(text, {tools::EditBlock{"same", "new", 7}}, error);
    CHECK(changed.has_value());
    CHECK_EQ(changed.value_or(""), std::string("a\nsame\nb\nc\nd\ne\nnew\nf\n"));
    // A line or two off, from a read a step ago, is still the nearest one.
    changed = tools::apply_edit_blocks(text, {tools::EditBlock{"same", "new", 3}}, error);
    CHECK_EQ(changed.value_or(""), std::string("a\nnew\nb\nc\nd\ne\nsame\nf\n"));
    // Near neither, or as near one as the other, is not a choice.
    CHECK(!tools::apply_edit_blocks(text, {tools::EditBlock{"same", "new", 40}}, error));
    const std::string close = "same\nx\nsame\n";
    CHECK(!tools::apply_edit_blocks(close, {tools::EditBlock{"same", "new", 2}}, error));
    // By its lines too, with the indentation set aside.
    const std::string indented = "  one\n  two\nmid\n  one\n  two\n";
    changed = tools::apply_edit_blocks(indented, {tools::EditBlock{"one\ntwo", "both", 4}}, error);
    CHECK_EQ(changed.value_or(""), std::string("  one\n  two\nmid\n  both\n"));

    // The native call says it as at_line, and the block keeps it.
    std::string why;
    const auto call = tools::call_from_native(
        "edit_file", R"({"path":"a.txt","search":"same","replace":"new","at_line":7})", why);
    CHECK(call.has_value());
    if (call) {
        const auto blocks = tools::parse_edit_blocks(call->content, error);
        CHECK_EQ(blocks.size(), std::size_t{1});
        if (!blocks.empty()) {
            CHECK_EQ(blocks[0].line, std::size_t{7});
            CHECK_EQ(blocks[0].search, std::string("same"));
        }
    }
    // Text that only looks like a marker with a line stays text.
    CHECK(tools::parse_edit_blocks("<<<<<<< SEARCH line x\na\n=======\nb\n>>>>>>> REPLACE\n", error).empty());
}
