// SPDX-License-Identifier: MIT
//
// The interface's text-to-markup functions.
//
// Markdown, syntax coloring and line diffs moved out of C++ and into
// ui/render.js when the window became a webview. They are the same functions
// doing the same job -- util/markdown.cpp, util/syntax.cpp and
// util/code_lines.cpp are gone -- so they are tested the same way, and the
// tests below are the old ones carried across.
//
// They run in JavaScriptCore, which is the engine WebKitGTK gives the webview
// on this platform, so this is the interpreter that will actually run them.
// The bytes are the embedded ones, not a copy: if the embed is stale, these
// fail.
#include <JavaScriptCore/JavaScript.h>

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.hpp"

namespace crucible::gui::web {
extern const unsigned char kRenderJs[];
extern const unsigned int  kRenderJs_size;
extern const unsigned char kIndexHtml[];
extern const unsigned int  kIndexHtml_size;
}  // namespace crucible::gui::web

namespace {

/// A JSStringRef that releases itself, because every call below makes one.
class Str {
public:
    explicit Str(const std::string& text) : ref_(JSStringCreateWithUTF8CString(text.c_str())) {}
    explicit Str(JSStringRef ref) : ref_(ref) {}
    ~Str() { if (ref_) JSStringRelease(ref_); }
    Str(const Str&)            = delete;
    Str& operator=(const Str&) = delete;
    operator JSStringRef() const { return ref_; }   // NOLINT(google-explicit-constructor)

    std::string utf8() const {
        const std::size_t  bytes = JSStringGetMaximumUTF8CStringSize(ref_);
        std::vector<char>  buffer(bytes);
        JSStringGetUTF8CString(ref_, buffer.data(), bytes);
        return std::string(buffer.data());
    }

private:
    JSStringRef ref_;
};

/// One context with render.js already evaluated in it.
///
/// Built once: the tables in that file are the same every time and parsing
/// them per assertion would be the slowest part of this suite.
class Engine {
public:
    Engine() : ctx_(JSGlobalContextCreate(nullptr)) {
        const std::string source(reinterpret_cast<const char*>(crucible::gui::web::kRenderJs),
                                 crucible::gui::web::kRenderJs_size);
        const Str   script(source);
        JSValueRef  thrown = nullptr;
        JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
        if (thrown != nullptr) {
            loaded_error_ = to_text(thrown);
        }
    }
    ~Engine() { JSGlobalContextRelease(ctx_); }
    Engine(const Engine&)            = delete;
    Engine& operator=(const Engine&) = delete;

    const std::string& load_error() const { return loaded_error_; }

    /// Evaluate an expression and return it as a string. A thrown exception
    /// comes back as "threw: ..." so a broken call shows up as a failed
    /// comparison rather than as a crash.
    std::string eval(const std::string& expression) {
        const Str  script(expression);
        JSValueRef thrown = nullptr;
        JSValueRef value  = JSEvaluateScript(ctx_, script, nullptr, nullptr, 0, &thrown);
        if (thrown != nullptr) {
            return "threw: " + to_text(thrown);
        }
        return to_text(value);
    }

private:
    std::string to_text(JSValueRef value) const {
        JSValueRef thrown = nullptr;
        JSStringRef text  = JSValueToStringCopy(ctx_, value, &thrown);
        if (text == nullptr) {
            return "<unconvertible>";
        }
        const Str held(text);
        return held.utf8();
    }

    JSGlobalContextRef ctx_;
    std::string        loaded_error_;
};

Engine& js() {
    static Engine engine;
    return engine;
}

/// The page's own script, between its last <script> and </script>.
///
/// index.html carries two: the error handlers near the top and the interface
/// below them. The second is the one worth parsing.
std::string page_script() {
    const std::string page(reinterpret_cast<const char*>(crucible::gui::web::kIndexHtml),
                           crucible::gui::web::kIndexHtml_size);
    const std::size_t open = page.rfind("<script>");
    if (open == std::string::npos) {
        return {};
    }
    const std::size_t start = open + std::strlen("<script>");
    const std::size_t close = page.find("</script>", start);
    return page.substr(start, close == std::string::npos ? std::string::npos : close - start);
}

/// Does `expression` evaluate to something containing `needle`?
bool has(const std::string& expression, const std::string& needle) {
    const std::string got = js().eval(expression);
    if (got.find(needle) == std::string::npos) {
        std::printf("      expression: %s\n      wanted:     %s\n      got:        %s\n",
                    expression.c_str(), needle.c_str(), got.c_str());
        return false;
    }
    return true;
}

/// A JS string literal, single-quoted, with the characters that would end it
/// escaped. The tests below are full of backslashes and quotes.
std::string lit(const std::string& text) {
    std::string out = "'";
    for (const char ch : text) {
        switch (ch) {
            case '\'': out += "\\'";  break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            default:   out += ch;     break;
        }
    }
    return out + "'";
}

std::string highlight(const std::string& code, const std::string& lang) {
    return "highlight(" + lit(code) + ", " + lit(lang) + ")";
}

}  // namespace

TEST(render_js_loads_in_javascriptcore) {
    CHECK(js().load_error().empty());
    if (!js().load_error().empty()) {
        std::printf("      %s\n", js().load_error().c_str());
    }
    CHECK_EQ(js().eval("typeof highlight"), "function");
    CHECK_EQ(js().eval("typeof markdown"), "function");
    CHECK_EQ(js().eval("typeof diffLines"), "function");
}

TEST(the_pages_own_script_parses) {
    // Built, not run: it reaches for `document` on the first line and there
    // is none here. Parsing is the half that can be checked, and it is the
    // half that fails silently -- a syntax error in index.html reaches the
    // user as a window with nothing in it and nothing in the log.
    const std::string script = page_script();
    CHECK(script.size() > 1000);
    const std::string expression =
        "(function(){ try { new Function(" + lit(script) + "); return 'parsed'; }"
        "catch (e) { return String(e); } })()";
    CHECK_EQ(js().eval(expression), "parsed");
}

// --- escaping ---------------------------------------------------------

TEST(escape_closes_the_tags_that_would_otherwise_open) {
    CHECK_EQ(js().eval("escape('<b>&</b>')"), "&lt;b&gt;&amp;&lt;/b&gt;");
    CHECK_EQ(js().eval("escape(null)"), "");
    CHECK_EQ(js().eval("escape(undefined)"), "");
    CHECK_EQ(js().eval("escape(7)"), "7");
}

// --- markdown ---------------------------------------------------------
    //
    // The block set util/markdown.hpp had, asked of the renderer that replaced
    // it.

TEST(headings_lists_and_rules_are_recognized) {
    const std::string source =
        "## What a pointer is\n\n"
        "- a memory address\n- a reference tool\n\n"
        "1. first\n2) second\n\n"
        "> quoted\n---\nordinary prose\n";
    CHECK(has("markdown(" + lit(source) + ")", "<h2>What a pointer is</h2>"));
    CHECK(has("markdown(" + lit(source) + ")", "<ul><li>a memory address</li>"
                                               "<li>a reference tool</li></ul>"));
    // Both ways a model numbers a list, and they are one list either way.
    CHECK(has("markdown(" + lit(source) + ")", "<ol><li>first</li><li>second</li></ol>"));
    CHECK(has("markdown(" + lit(source) + ")", "<blockquote>"));
    CHECK(has("markdown(" + lit(source) + ")", "<hr>"));
    CHECK(has("markdown(" + lit(source) + ")", "<p>ordinary prose</p>"));
}

TEST(a_table_is_recognized_by_the_row_under_its_header) {
    const std::string source =
        "| Planet | Radius (km) | Moons |\n"
        "|--------|------------:|:-----:|\n"
        "| Mercury | 2,440 | 0 |\n"
        "| Mars | 3,390 | 2 |\n";
    const std::string call = "markdown(" + lit(source) + ")";
    CHECK(has(call, "<table>"));
    CHECK(has(call, "<th>Planet</th>"));
    // The alignment the delimiter row asked for: right for the radius,
    // centered for the moons, and nothing said about the name.
    CHECK(has(call, "<th style=\"text-align:right\">Radius (km)</th>"));
    CHECK(has(call, "<th style=\"text-align:center\">Moons</th>"));
    // A thousands separator is not markup.
    CHECK(has(call, "<td style=\"text-align:right\">2,440</td>"));
    CHECK(has(call, "<td>Mars</td>"));
    CHECK_EQ(js().eval(call + ".match(/<tr>/g).length"), "3");
}

TEST(a_line_with_a_pipe_in_it_is_not_a_table) {
    // The failure this guards: an answer about shell pipelines is full of
    // pipes, and turning one into a one-row table would be worse than leaving
    // the pipes alone. What makes a table is the delimiter row under it.
    for (const char* prose : {"run `ls | grep foo` to filter",
                              "the options are a | b | c",
                              "| not | a | table |"}) {
        CHECK_EQ(js().eval("markdown(" + lit(prose) + ").includes('<table>')"), "false");
    }
}

TEST(a_table_without_outer_pipes_is_still_a_table) {
    // GitHub-flavored markdown allows them to be left off, and models do.
    const std::string call = "markdown('a | b\\n--- | ---\\n1 | 2\\n')";
    CHECK(has(call, "<table>"));
    CHECK(has(call, "<th>a</th>"));
    CHECK(has(call, "<td>2</td>"));
}

TEST(a_table_ends_where_its_rows_do) {
    CHECK(has("markdown('| a | b |\\n|---|---|\\n| 1 | 2 |\\nback to prose\\n')",
              "</table><p>back to prose</p>"));
}

TEST(a_code_block_says_what_it_is_and_numbers_its_lines) {
    // The header and the gutter are what make a block readable: a model that
    // says "line 12" is talking about something that has to be findable.
    const std::string call = "markdown('```python\\na = 1\\nb = 2\\n```')";
    CHECK(has(call, "<span class=\"lang\">python</span>"));
    CHECK(has(call, "2 lines"));
    CHECK(has(call, "code-gutter"));
    // The gutter counts the lines of code, not the fence.
    CHECK_EQ(js().eval(call + ".match(/code-gutter[^>]*>([^<]*)</)[1]"), "1\n2");
    CHECK(has(call, "data-copy"));
}

TEST(one_line_of_code_is_one_line) {
    CHECK(has("markdown('```\\nonly\\n```')", "1 line<"));
}

TEST(a_fenced_block_is_code_all_the_way_to_its_close) {
    const std::string source =
        "Here:\n```python\n# not a heading\n- not a bullet\n**not bold**\n```\ndone\n";
    const std::string call = "markdown(" + lit(source) + ")";
    // Nothing inside a fence is markup, which is the point of a fence.
    CHECK_EQ(js().eval(call + ".includes('<h1>')"), "false");
    CHECK_EQ(js().eval(call + ".includes('<li>')"), "false");
    CHECK_EQ(js().eval(call + ".includes('<strong>')"), "false");
    CHECK(has(call, "<p>Here:</p>"));
    CHECK(has(call, "<p>done</p>"));
    CHECK(has(call, "python"));
}

TEST(code_keeps_its_indentation) {
    // Re-flowed code is code that no longer runs.
    CHECK(has("markdown('```\\ndef f():\\n    return 1\\n```\\n')", "\n    return 1"));
}

TEST(inline_styling_is_split_into_runs) {
    const std::string call = "markdown('plain **bold** and `code` and *italic*')";
    CHECK(has(call, "<strong>bold</strong>"));
    CHECK(has(call, "<code>code</code>"));
    CHECK(has(call, "<em>italic</em>"));
    // And the markers themselves are gone.
    CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), "plain bold and code and italic");
}

TEST(a_lone_asterisk_is_not_the_start_of_anything) {
    // An expert writing "3 * 4" or a footnote marker must not turn the rest of
    // the line italic and lose the character while doing it.
    for (const char* line : {"3 * 4 = 12", "see note *", "a_b_c and snake_case"}) {
        const std::string call = "markdown(" + lit(line) + ")";
        CHECK_EQ(js().eval(call + ".includes('<em>')"), "false");
        CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), line);
    }
}

TEST(inline_code_is_not_searched_for_markup) {
    const std::string call = "markdown('use `a ** b` for powers')";
    CHECK_EQ(js().eval(call + ".includes('<strong>')"), "false");
    CHECK_EQ(js().eval(call + ".replace(/<[^>]*>/g,'')"), "use a ** b for powers");
}

TEST(text_with_no_markdown_in_it_survives_unchanged) {
    // The regression that matters: a model that writes plain prose must come
    // out exactly as it went in.
    const std::string plain = "Water boils at 100 C. That is 212 F, at sea level.";
    CHECK_EQ(js().eval("markdown(" + lit(plain) + ")"), "<p>" + plain + "</p>");
}

TEST(markdown_never_lets_a_tag_through_from_the_model) {
    // A reply is text that chose some emphasis, not a document that brings
    // its own HTML.
    CHECK(has("markdown('a <script>x</script> b')", "&lt;script&gt;"));
    CHECK_EQ(js().eval("markdown('<img src=x onerror=y>').includes('<img')"), "false");
    CHECK_EQ(js().eval("markdown('| <b>x</b> |\\n|---|\\n| y |').includes('<b>')"), "false");
}

TEST(a_fence_still_open_renders_as_code) {
    // A reply mid-stream has an opening fence and no closing one, and it
    // should read as code for the whole time it is arriving.
    CHECK(has("markdown('```c\\nint x;')", "class=\"code\""));
    // The text is there, in pieces: the lexer has colored `int` by now.
    CHECK_EQ(js().eval("markdown('```c\\nint x;').includes('int')"), "true");
}

// --- syntax coloring --------------------------------------------------
    //
    // Carried over from tests/test_syntax.cpp.

TEST(c_and_c_keywords_types_numbers) {
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-key\">const</span>"));
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-typ\">int</span>"));
    CHECK(has(highlight("const int x = 42;", "cpp"), "<span class=\"tok-num\">42</span>"));
    CHECK(has(highlight("#include <string>", "c"),   "<span class=\"tok-key\">#include</span>"));
}

TEST(comments_to_the_end_of_the_line_and_block_comments_past_it) {
    CHECK(has(highlight("f(); // note", "cpp"), "<span class=\"tok-com\">// note</span>"));
    CHECK(has(highlight("/* two\nlines */ x", "cpp"),
              "<span class=\"tok-com\">/* two\nlines */</span>"));
    CHECK(has(highlight("x = 1  # note", "py"), "<span class=\"tok-com\"># note</span>"));
    CHECK(has(highlight("-- note", "sql"), "<span class=\"tok-com\">-- note</span>"));
}

TEST(strings_including_the_escaped_quote_that_does_not_end_one) {
    CHECK(has(highlight("s = \"hi\";", "cpp"), "<span class=\"tok-str\">&quot;hi&quot;</span>"));
    CHECK(has(highlight("s = \"a\\\"b\";", "cpp"), "a\\&quot;b"));
    CHECK(has(highlight("`a${b}c`", "ts"), "<span class=\"tok-str\">`a${b}c`</span>"));
}

TEST(python_s_triple_quotes_outlive_their_line) {
    CHECK(has(highlight("\"\"\"doc\nstring\"\"\"", "python"),
              "<span class=\"tok-str\">&quot;&quot;&quot;doc\nstring&quot;&quot;&quot;</span>"));
    CHECK(has(highlight("def f(x):", "py"), "<span class=\"tok-key\">def</span>"));
}

TEST(shell_variables) {
    CHECK(has(highlight("echo $HOME", "bash"), "<span class=\"tok-var\">$HOME</span>"));
    CHECK(has(highlight("echo ${A:-b} x", "sh"), "<span class=\"tok-var\">${A:-b}</span>"));
}

TEST(sql_and_cmake_are_case_folded) {
    CHECK(has(highlight("select a from t", "sql"), "<span class=\"tok-key\">select</span>"));
    CHECK(has(highlight("SELECT a FROM t", "sql"), "<span class=\"tok-key\">SELECT</span>"));
    CHECK(has(highlight("set(X 1)", "cmake"),      "<span class=\"tok-key\">set</span>"));
}

TEST(a_diff_is_colored_by_its_first_column) {
    CHECK(has(highlight("+added\n-gone\n ctx", "diff"), "<span class=\"tok-add\">+added</span>"));
    CHECK(has(highlight("+added\n-gone\n ctx", "diff"), "<span class=\"tok-del\">-gone</span>"));
    CHECK(has(highlight("@@ -1 +1 @@", "patch"),        "<span class=\"tok-com\">@@ -1 +1 @@</span>"));
}

TEST(a_language_it_does_not_know_is_still_escaped) {
    CHECK_EQ(js().eval(highlight("<b>&</b>", "brainfuck")), "&lt;b&gt;&amp;&lt;/b&gt;");
    CHECK_EQ(js().eval(highlight("a < b", "")), "a &lt; b");
}

TEST(the_aliases_a_model_actually_writes) {
    for (const char* lang : {"c++", "cxx", "hpp", "cc"}) {
        CHECK(has(highlight("const x", lang), "tok-key"));
    }
    for (const char* lang : {"py", "python3"}) {
        CHECK(has(highlight("def f", lang), "tok-key"));
    }
    CHECK(has(highlight("fn main", "rs"), "tok-key"));
    CHECK(has(highlight("func main", "golang"), "tok-key"));
}

TEST(coloring_never_loses_or_invents_text) {
    // The one property that matters: strip the spans and the code must be
    // exactly what went in. A lexer that drops a character silently
    // corrupts what the model wrote.
    const std::vector<std::pair<std::string, std::string>> samples = {
        {"cpp",  "auto f = [&](int n) { /* x */ return \"a\" + n; }; // end"},
        {"py",   "def g():\n    \"\"\"d\"\"\"\n    return {1: 'x'}  # t"},
        {"sh",   "for f in *.txt; do echo \"${f}\" $HOME; done # loop"},
        {"rs",   "fn main() { let s: &str = \"hi\"; }"},
        {"sql",  "SELECT a FROM t WHERE b = 'q' -- note"},
        {"json", "{\"k\": [1, 2.5, null]}"},
        {"",     "plain text & nothing"},
        {"cpp",  "\"unterminated"},
        {"cpp",  "/* unterminated"},
        {"py",   "'''unterminated"},
        {"cpp",  ""},
    };
    for (const auto& [lang, text] : samples) {
        const std::string expression =
            "(function(){const h=" + highlight(text, lang) + ";"
            "return h.replace(/<[^>]*>/g,'')"
            ".replace(/&lt;/g,String.fromCharCode(60))"
            ".replace(/&gt;/g,String.fromCharCode(62))"
            ".replace(/&quot;/g,String.fromCharCode(34))"
            ".replace(/&#39;/g,String.fromCharCode(39))"
            // &amp; last, or text that was already an entity unescapes twice.
            ".replace(/&amp;/g,String.fromCharCode(38));})()";
        CHECK_EQ(js().eval(expression), text);
    }
}

// --- a file an expert wants to write ----------------------------------
//
// The one screen where the two answers are not interchangeable, so what it
// shows has to be right: which file, whether it exists, and what would be in
// it afterwards.

TEST(a_new_file_is_shown_whole_and_marked_as_an_addition) {
    const std::string call =
        "pendingEdit({path:'src/hello.py', before:'', after:'print(1)\\nprint(2)'})";
    CHECK(has(call, "New file"));
    CHECK(has(call, "src/hello.py"));
    // The language comes from the extension, since nothing else says.
    CHECK(has(call, "<span class=\"lang\">python</span>"));
    CHECK(has(call, "code-add"));
    CHECK(has(call, "2 lines"));
    CHECK(has(call, "id=\"edit-yes\""));
    CHECK(has(call, "id=\"edit-no\""));
}

TEST(a_change_to_a_file_is_shown_as_the_lines_that_move) {
    const std::string call =
        "pendingEdit({path:'a.py', before:'one\\ntwo', after:'one\\nthree'})";
    CHECK(has(call, "Edit"));
    CHECK_EQ(js().eval(call + ".includes('New file')"), "false");
    // The rest of the file is not what is being decided, so it is not shown
    // whole: one line leaves and one arrives.
    CHECK(has(call, "dl-del"));
    CHECK(has(call, "dl-add"));
    CHECK(has(call, "+1"));
}

TEST(an_edit_that_would_write_nothing_says_so) {
    // An empty code block reporting "1 line" reads as the interface being
    // broken rather than the request being odd.
    CHECK(has("pendingEdit({path:'a.py', before:'', after:''})", "an empty file"));
    CHECK(has("pendingEdit({path:'a.py', before:'x', after:'  '})",
              "this would empty the file"));
    CHECK_EQ(js().eval("pendingEdit({path:'a.py', before:'', after:''}).includes('1 line')"),
             "false");
}

TEST(a_path_from_a_model_is_escaped_like_everything_else) {
    CHECK(has("pendingEdit({path:'<b>x</b>', before:'', after:'y'})", "&lt;b&gt;"));
}

// --- diffs ------------------------------------------------------------
//
// Carried over from tests/test_code_lines.cpp.

TEST(an_unchanged_file_is_all_context) {
    CHECK_EQ(js().eval("diffLines('a\\nb', 'a\\nb').every(l => l.kind === 'same')"), "true");
    CHECK_EQ(js().eval("diffLines('a\\nb', 'a\\nb').length"), "2");
}

TEST(an_added_line_is_marked_and_only_it) {
    CHECK_EQ(js().eval("diffLines('a\\nc', 'a\\nb\\nc')"
                    ".filter(l => l.kind === 'add').map(l => l.text).join()"), "b");
    CHECK_EQ(js().eval("diffLines('a\\nc', 'a\\nb\\nc')"
                    ".filter(l => l.kind === 'del').length"), "0");
}

TEST(a_removed_line_is_marked_and_only_it) {
    CHECK_EQ(js().eval("diffLines('a\\nb\\nc', 'a\\nc')"
                    ".filter(l => l.kind === 'del').map(l => l.text).join()"), "b");
}

TEST(a_changed_line_is_a_removal_and_an_addition) {
    CHECK_EQ(js().eval("diffLines('a\\nold\\nc', 'a\\nnew\\nc')"
                       ".filter(l => l.kind !== 'same')"
                       ".map(l => l.kind + ':' + l.text).join()"),
             "del:old,add:new");
}

TEST(a_new_file_is_all_additions) {
    CHECK_EQ(js().eval("diffLines('', 'a\\nb').filter(l => l.kind === 'add').length"), "2");
}

TEST(the_diff_keeps_every_line_of_both_texts) {
    // Reading the kept and removed lines back gives the first file; the
    // kept and added lines give the second. Anything else means the view
    // is showing a file that does not exist.
    const char* expression =
        "(function(){const d=diffLines('a\\nb\\nc\\nd', 'a\\nx\\nc\\ny\\nz');"
        "const before=d.filter(l=>l.kind!=='add').map(l=>l.text).join('\\n');"
        "const after=d.filter(l=>l.kind!=='del').map(l=>l.text).join('\\n');"
        "return (before==='a\\nb\\nc\\nd') + ',' + (after==='a\\nx\\nc\\ny\\nz');})()";
    CHECK_EQ(js().eval(expression), "true,true");
}

TEST(a_file_too_large_to_diff_says_so_instead_of_hanging) {
    CHECK_EQ(js().eval("diffLines(Array(3000).fill('x').join('\\n'), "
                    "Array(3000).fill('y').join('\\n'))[0].kind"), "note");
}
