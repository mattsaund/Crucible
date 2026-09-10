// SPDX-License-Identifier: MIT
//
// Cooking: the journal a cook writes, the diffs it records, and the markdown
// parser both faces render it with.
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Diffs
// ---------------------------------------------------------------------------

TEST(a_diff_reports_only_the_lines_that_moved) {
    const std::string before = "def add(a, b):\n    return a - b\n\ndef main():\n    pass\n";
    const std::string after  = "def add(a, b):\n    return a + b\n\ndef main():\n    pass\n";

    const util::DiffStat stat = util::diff_stat(before, after);
    CHECK_EQ(stat.added, 1);
    CHECK_EQ(stat.removed, 1);
    CHECK_EQ(stat.summary(), std::string("+1 -1"));

    // "updated calc.py" is true and almost useless. This is what makes a
    // journal say whether one character moved or the file was replaced.
    const std::string diff = util::unified_diff(before, after);
    CHECK(diff.find("-    return a - b") != std::string::npos);
    CHECK(diff.find("+    return a + b") != std::string::npos);
    // The untouched lines are context, not changes.
    CHECK(diff.find("-def add") == std::string::npos);
    CHECK(diff.find("+def main") == std::string::npos);
}

TEST(the_changed_range_is_what_the_side_by_side_view_washes) {
    // The desktop shows an edit as two whole files rather than as a diff, and
    // still has to mark the rows that moved -- red on the left, green on the
    // right. That needs the range rather than the rendered hunk.
    const std::string before = "one\ntwo\nthree\nfour\n";
    const std::string after  = "one\nTWO\nTWO AND A HALF\nthree\nfour\n";

    const util::ChangedLines changed = util::changed_lines(before, after);
    CHECK(!changed.identical());
    // Line 0 is shared, so the wash starts at 1 on both sides...
    CHECK_EQ(changed.first, std::size_t{1});
    // ...covers the one line that went...
    CHECK_EQ(changed.before_end, std::size_t{2});
    // ...and the two that arrived. The trailing lines are common and stay
    // unwashed, which is the whole point: on a four-hundred-line file with one
    // function changed, marking everything would be marking nothing.
    CHECK_EQ(changed.after_end, std::size_t{3});
}

TEST(two_identical_files_have_nothing_to_wash) {
    const std::string same = "alpha\nbeta\n";
    const util::ChangedLines changed = util::changed_lines(same, same);
    CHECK(changed.identical());
    CHECK_EQ(changed.first, changed.before_end);
    CHECK_EQ(changed.first, changed.after_end);
}

TEST(a_brand_new_file_is_all_addition) {
    // The case that gets its own panel and an Allow/Deny rather than a
    // comparison: there is no "before" to put beside it.
    const util::ChangedLines changed = util::changed_lines("", "line one\nline two\n");
    CHECK_EQ(changed.first, std::size_t{0});
    CHECK_EQ(changed.before_end, std::size_t{0});
    CHECK(changed.after_end > 0);
}

TEST(an_unchanged_write_produces_no_diff) {
    const std::string same = "one\ntwo\nthree\n";
    CHECK(util::unified_diff(same, same).empty());
    CHECK_EQ(util::diff_stat(same, same).summary(), std::string("unchanged"));
}

TEST(a_diff_handles_an_insertion_at_either_end) {
    // The head and tail trimming is the whole algorithm, and both edges are
    // where it goes wrong if the arithmetic is off by one.
    const util::DiffStat front = util::diff_stat("b\nc\n", "a\nb\nc\n");
    CHECK_EQ(front.added, 1);
    CHECK_EQ(front.removed, 0);

    const util::DiffStat back = util::diff_stat("a\nb\n", "a\nb\nc\n");
    CHECK_EQ(back.added, 1);
    CHECK_EQ(back.removed, 0);

    // And creating a file from nothing is all addition.
    const util::DiffStat fresh = util::diff_stat("", "a\nb\n");
    CHECK_EQ(fresh.added, 2);
    CHECK_EQ(fresh.removed, 0);
}

TEST(a_long_diff_says_it_was_cut_rather_than_stopping) {
    std::string before;
    std::string after;
    for (int i = 0; i < 200; ++i) {
        before += "old " + std::to_string(i) + "\n";
        after  += "new " + std::to_string(i) + "\n";
    }
    const std::string diff = util::unified_diff(before, after, 10);

    // A diff that stops without warning reads as a complete diff of a smaller
    // change, which is a worse lie than no diff at all.
    CHECK(diff.find("not shown") != std::string::npos);
    CHECK(diff.find("+200 -200") != std::string::npos);
}

// ---------------------------------------------------------------------------
// The cook journal
// ---------------------------------------------------------------------------

TEST(a_bare_number_of_minutes_is_what_people_mean_by_a_cook_time) {
    // "give it twenty" means twenty minutes. Nobody sets a cook for twenty
    // seconds, so the bare unit is the one that is actually said out loud.
    CHECK(parse_duration_seconds("30")  == 1800);
    CHECK(parse_duration_seconds("30m") == 1800);
    CHECK(parse_duration_seconds("2h")  == 7200);
    CHECK(parse_duration_seconds("45s") == 45);
    CHECK(parse_duration_seconds("90min") == 5400);
    CHECK(parse_duration_seconds(" 15m") == 900);
}

TEST(a_goal_that_starts_with_a_word_is_not_a_duration) {
    // This is what tells "/cook 30m fix the tests" from "/cook fix the tests".
    CHECK(!parse_duration_seconds("fix").has_value());
    CHECK(!parse_duration_seconds("").has_value());
    // And a number followed by something that is not a unit is part of a
    // sentence: "/cook 3 tests are failing".
    CHECK(!parse_duration_seconds("3tests").has_value());
    CHECK(!parse_duration_seconds("999999999").has_value());
}

TEST(a_cook_reports_the_files_it_touched_in_the_order_it_touched_them) {
    Cook cook;
    cook.steps.push_back(step_of(1, "programming", "write", "created src/a.py", true, 0, {"src/a.py"}));
    cook.steps.push_back(step_of(1, "programming", "run",   "$ pytest",         true, 0, {}));
    cook.steps.push_back(step_of(2, "programming", "write", "created src/b.py", true, 0, {"src/b.py"}));
    cook.steps.push_back(step_of(2, "programming", "write", "updated src/a.py", true, 0, {"src/a.py"}));

    // First-touched order, not sorted: it reads as the story of the cook.
    const std::vector<std::string> files = cook.files_touched();
    CHECK_EQ(files.size(), std::size_t{2});
    CHECK_EQ(files[0], std::string("src/a.py"));
    CHECK_EQ(files[1], std::string("src/b.py"));
}

TEST(a_running_cook_measures_against_now_and_a_finished_one_against_its_end) {
    Cook running;
    running.started_unix = static_cast<std::int64_t>(std::time(nullptr)) - 120;
    // The timer on screen has to move while it is still going.
    CHECK(running.duration().count() >= 119);

    Cook finished = running;
    finished.ended_unix = finished.started_unix + 45;
    CHECK_EQ(finished.duration().count(), 45);
}

TEST(a_cook_round_trips_through_disk) {
    TempDir dir;
    const CookLog log(dir.path());

    Cook cook;
    cook.id             = CookLog::new_id();
    cook.goal           = "make the failing tests pass";
    cook.state          = CookState::Done;
    cook.budget_seconds = 1800;
    cook.started_unix   = 1'700'000'000;
    cook.ended_unix     = 1'700'001'000;
    cook.iterations     = 4;
    cook.outcome        = "fixed the parser and added two tests";
    cook.steps.push_back(step_of(1, "programming", "read",  "read src/parse.py", true, 12, {}));
    cook.steps.push_back(step_of(1, "programming", "write", "updated src/parse.py", true, 8,
                          {"src/parse.py"}));
    cook.steps.push_back(step_of(2, "programming", "run",   "$ pytest -- exit 1", false, 4200, {}));

    std::string error;
    CHECK(log.save(cook, error));
    CHECK(error.empty());

    const std::optional<Cook> back = log.load(cook.id);
    CHECK(back.has_value());
    if (!back) {
        return;
    }
    CHECK_EQ(back->goal, cook.goal);
    CHECK(back->state == CookState::Done);
    CHECK_EQ(back->iterations, 4);
    CHECK_EQ(back->outcome, cook.outcome);
    CHECK_EQ(back->steps.size(), std::size_t{3});
    // Whether a step failed is the thing you most want back: a run that exited
    // non-zero is the whole reason to read a journal.
    CHECK(!back->steps[2].ok);
    CHECK_EQ(back->steps[2].ms, 4200L);
    CHECK_EQ(back->steps[1].changed.size(), std::size_t{1});
    CHECK_EQ(back->duration().count(), 1000);
}

TEST(cooks_are_listed_newest_first_with_what_they_changed) {
    TempDir dir;
    const CookLog log(dir.path());

    for (int i = 1; i <= 3; ++i) {
        Cook cook;
        cook.id           = "2026090" + std::to_string(i) + "-120000";
        cook.goal         = "goal " + std::to_string(i);
        cook.state        = CookState::Done;
        cook.started_unix = 1'700'000'000 + i;
        cook.ended_unix   = cook.started_unix + 600;
        cook.steps.push_back(step_of(1, "programming", "write", "wrote a", true, 0, {"a.txt"}));
        cook.steps.push_back(step_of(1, "programming", "write", "wrote a again", true, 0, {"a.txt"}));
        std::string error;
        CHECK(log.save(cook, error));
    }

    const std::vector<CookSummary> found = log.list();
    CHECK_EQ(found.size(), std::size_t{3});
    CHECK_EQ(found[0].goal, std::string("goal 3"));
    CHECK_EQ(found[2].goal, std::string("goal 1"));
    // Two steps touching one file is one file, which is the number worth
    // reporting: "what did this cook do to my project".
    CHECK_EQ(found[0].files, 1);
    CHECK_EQ(found[0].steps, 2);
    CHECK_EQ(found[0].duration.count(), 600);
}

TEST(an_unreadable_cook_file_does_not_hide_the_others) {
    TempDir dir;
    const CookLog log(dir.path());

    Cook cook;
    cook.id   = "20260901-120000";
    cook.goal = "a real one";
    std::string error;
    CHECK(log.save(cook, error));

    std::filesystem::create_directories(log.dir());
    { std::ofstream(log.dir() / "20260902-120000.json") << "{ this is not json"; }

    const std::vector<CookSummary> found = log.list();
    CHECK_EQ(found.size(), std::size_t{1});
    CHECK_EQ(found[0].goal, std::string("a real one"));
}

TEST(a_cook_can_be_deleted_and_its_state_round_trips_by_name) {
    TempDir dir;
    const CookLog log(dir.path());

    Cook cook;
    cook.id    = CookLog::new_id();
    cook.state = CookState::Stopped;
    std::string error;
    CHECK(log.save(cook, error));
    CHECK(log.load(cook.id).has_value());
    CHECK(log.load(cook.id)->state == CookState::Stopped);

    CHECK(log.remove(cook.id, error));
    CHECK(!log.load(cook.id).has_value());
    CHECK(!log.remove(cook.id, error));
}

TEST(every_cook_state_survives_a_round_trip_through_its_name) {
    for (const CookState state : {CookState::Idle, CookState::Working, CookState::Asking,
                                  CookState::Finishing, CookState::Done, CookState::Stopped,
                                  CookState::Failed}) {
        CHECK(cook_state_from_name(cook_state_name(state)) == state);
    }
    // An unknown name reads as Idle rather than as something that happened.
    CHECK(cook_state_from_name("who knows") == CookState::Idle);
}

TEST(the_headline_says_files_steps_and_how_long) {
    Cook cook;
    cook.started_unix = 1'700'000'000;
    cook.ended_unix   = cook.started_unix + 2460;  // 41 minutes
    cook.steps.push_back(step_of(1, "programming", "write", "a", true, 0, {"x.py"}));
    cook.steps.push_back(step_of(1, "programming", "run",   "b", true, 0, {}));

    const std::string headline = cook.headline();
    CHECK(headline.find("1 file") != std::string::npos);
    CHECK(headline.find("2 steps") != std::string::npos);
    CHECK(headline.find("41 minutes") != std::string::npos);
}

// ---------------------------------------------------------------------------
// Markdown
// ---------------------------------------------------------------------------

namespace {

/// The plain text of a run of spans, with the styling thrown away.
std::string flat_spans(const std::vector<markdown::Span>& spans) {
    std::string text;
    for (const markdown::Span& span : spans) {
        text += span.text;
    }
    return text;
}

std::string flat(const markdown::Block& block) {
    return flat_spans(block.spans);
}

}  // namespace

TEST(headings_lists_and_rules_are_recognized) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "## What a pointer is\n"
        "\n"
        "- a memory address\n"
        "- a reference tool\n"
        "\n"
        "1. first\n"
        "2) second\n"
        "\n"
        "> quoted\n"
        "---\n"
        "ordinary prose\n");

    std::vector<markdown::BlockKind> kinds;
    for (const markdown::Block& block : blocks) {
        if (block.kind != markdown::BlockKind::Blank) {
            kinds.push_back(block.kind);
        }
    }
    const std::vector<markdown::BlockKind> expected{
        markdown::BlockKind::Heading,  markdown::BlockKind::Bullet,
        markdown::BlockKind::Bullet,   markdown::BlockKind::Numbered,
        markdown::BlockKind::Numbered, markdown::BlockKind::Quote,
        markdown::BlockKind::Rule,     markdown::BlockKind::Paragraph};
    CHECK_EQ(kinds.size(), expected.size());
    for (std::size_t i = 0; i < kinds.size() && i < expected.size(); ++i) {
        CHECK(kinds[i] == expected[i]);
    }
    CHECK_EQ(blocks.front().level, 2);
    CHECK_EQ(flat(blocks.front()), "What a pointer is");
}

TEST(a_table_is_recognized_by_the_row_under_its_header) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "| Planet | Radius (km) | Moons |\n"
        "|--------|------------:|:-----:|\n"
        "| Mercury | 2,440 | 0 |\n"
        "| Mars | 3,390 | 2 |\n");

    CHECK_EQ(blocks.size(), std::size_t{4});  // header, rule, two rows
    CHECK(blocks[0].kind == markdown::BlockKind::TableRow);
    CHECK(blocks[1].kind == markdown::BlockKind::TableRule);
    CHECK(blocks[2].kind == markdown::BlockKind::TableRow);

    // Three columns, and the alignment the delimiter row asked for.
    CHECK_EQ(blocks[0].cells.size(), std::size_t{3});
    CHECK_EQ(blocks[1].marker, "lrc");

    // Cell text survives intact -- a thousands separator is not markup.
    CHECK_EQ(flat_spans(blocks[2].cells.at(1)), "2,440");
    CHECK_EQ(flat_spans(blocks[0].cells.at(0)), "Planet");
    CHECK_EQ(flat_spans(blocks[3].cells.at(2)), "2");
}

TEST(a_line_with_a_pipe_in_it_is_not_a_table) {
    // The failure this guards: an answer about shell pipelines is full of
    // pipes, and turning one into a one-row table would be worse than leaving
    // the pipes alone. What makes a table is the delimiter row under it.
    for (const std::string_view prose : {"run `ls | grep foo` to filter",
                                         "the options are a | b | c",
                                         "| not | a | table |"}) {
        for (const markdown::Block& block : markdown::parse(prose)) {
            CHECK(block.kind != markdown::BlockKind::TableRow);
        }
    }
}

TEST(a_table_without_outer_pipes_is_still_a_table) {
    // GitHub-flavoured markdown allows them to be left off, and models do.
    const std::vector<markdown::Block> blocks =
        markdown::parse("a | b\n--- | ---\n1 | 2\n");
    CHECK(blocks.at(0).kind == markdown::BlockKind::TableRow);
    CHECK(blocks.at(1).kind == markdown::BlockKind::TableRule);
    CHECK_EQ(blocks.at(2).cells.size(), std::size_t{2});
    CHECK_EQ(flat_spans(blocks.at(2).cells.at(1)), "2");
}

TEST(a_table_ends_where_its_rows_do) {
    const std::vector<markdown::Block> blocks =
        markdown::parse("| a | b |\n|---|---|\n| 1 | 2 |\nback to prose\n");
    CHECK(blocks.back().kind == markdown::BlockKind::Paragraph);
    CHECK_EQ(flat_spans(blocks.back().spans), "back to prose");
}

TEST(a_fenced_block_is_code_all_the_way_to_its_close) {
    const std::vector<markdown::Block> blocks = markdown::parse(
        "Here:\n```python\n# not a heading\n- not a bullet\n**not bold**\n```\ndone\n");

    int code = 0;
    for (const markdown::Block& block : blocks) {
        if (block.kind == markdown::BlockKind::Code) {
            ++code;
            // Nothing inside a fence is markup, which is the point of a fence.
            CHECK(block.spans.size() == 1);
            CHECK(block.spans.front().code);
            CHECK_EQ(block.marker, "python");
        }
    }
    CHECK_EQ(code, 3);
    CHECK(blocks.front().kind == markdown::BlockKind::Paragraph);
    CHECK(blocks.back().kind == markdown::BlockKind::Paragraph);
}

TEST(code_keeps_its_indentation) {
    // Re-flowed code is code that no longer runs.
    const std::vector<markdown::Block> blocks =
        markdown::parse("```\ndef f():\n    return 1\n```\n");
    CHECK_EQ(blocks.at(1).spans.front().text, "    return 1");
}

TEST(inline_styling_is_split_into_runs) {
    const std::vector<markdown::Span> spans =
        markdown::parse_inline("plain **bold** and `code` and *italic*");
    std::string bold;
    std::string code;
    std::string italic;
    for (const markdown::Span& span : spans) {
        if (span.bold)   { bold   += span.text; }
        if (span.code)   { code   += span.text; }
        if (span.italic) { italic += span.text; }
    }
    CHECK_EQ(bold, "bold");
    CHECK_EQ(code, "code");
    CHECK_EQ(italic, "italic");

    // And the markers themselves are gone.
    std::string all;
    for (const markdown::Span& span : spans) {
        all += span.text;
    }
    CHECK_EQ(all, "plain bold and code and italic");
}

TEST(a_lone_asterisk_is_not_the_start_of_anything) {
    // An expert writing "3 * 4" or a footnote marker must not turn the rest of
    // the line italic and lose the character while doing it.
    for (const std::string_view line : {"3 * 4 = 12", "see note *", "a_b_c and snake_case"}) {
        std::string all;
        for (const markdown::Span& span : markdown::parse_inline(line)) {
            all += span.text;
            CHECK(!span.italic);
        }
        CHECK_EQ(all, std::string(line));
    }
}

TEST(inline_code_is_not_searched_for_markup) {
    const std::vector<markdown::Span> spans = markdown::parse_inline("use `a ** b` for powers");
    for (const markdown::Span& span : spans) {
        CHECK(!span.bold);
    }
    std::string all;
    for (const markdown::Span& span : spans) {
        all += span.text;
    }
    CHECK_EQ(all, "use a ** b for powers");
}

TEST(text_with_no_markdown_in_it_survives_unchanged) {
    // The regression that matters: a model that writes plain prose must come
    // out exactly as it went in.
    const std::string plain = "Water boils at 100 C. That is 212 F, at sea level.";
    const std::vector<markdown::Block> blocks = markdown::parse(plain);
    CHECK_EQ(blocks.size(), std::size_t{1});
    CHECK(blocks.front().kind == markdown::BlockKind::Paragraph);
    CHECK_EQ(flat(blocks.front()), plain);
}
