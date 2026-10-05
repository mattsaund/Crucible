// SPDX-License-Identifier: MIT
//
// A session: separating reasoning from the answer, streaming it a byte at a
// time, counting the tokens it cost, and writing the history to disk.
#include "test_helpers.hpp"

// ---------------------------------------------------------------------------
// Reasoning and answer
// ---------------------------------------------------------------------------

namespace {

/// Feed `text` through a filter one `chunk` bytes at a time, so a test can ask
/// what happens when a marker is split across the boundary -- which is the
/// ordinary case, since a marker is several tokens long.
ResponseFilter::Piece filter_in_chunks(std::string_view text, std::size_t chunk) {
    ResponseFilter filter;
    ResponseFilter::Piece total;
    for (std::size_t at = 0; at < text.size(); at += chunk) {
        const ResponseFilter::Piece piece = filter.feed(text.substr(at, chunk));
        total.reasoning += piece.reasoning;
        total.answer    += piece.answer;
    }
    const ResponseFilter::Piece last = filter.flush();
    total.reasoning += last.reasoning;
    total.answer    += last.answer;
    return total;
}

}  // namespace

TEST(a_model_that_marks_nothing_is_left_alone) {
    // The common case, and the one that must not regress: a model with no
    // reasoning convention at all produces exactly what it produced before.
    const std::string plain = "Water boils at 100 C at sea level.";
    for (const std::size_t chunk : {1U, 3U, 7U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(plain, chunk);
        CHECK_EQ(out.answer, plain);
        CHECK(out.reasoning.empty());
    }
}

TEST(harmony_channels_are_split_into_working_and_answer) {
    // What gpt-oss actually emits. llama.cpp classifies these markers as
    // user-defined rather than control, so they arrive as visible text and the
    // whole analysis channel lands in the transcript unless something sorts it.
    const std::string raw =
        "<|channel|>analysis<|message|>We need the boiling point. It is 100 C."
        "<|end|><|start|>assistant<|channel|>final<|message|>"
        "Water boils at 100 °C at sea level.";

    for (const std::size_t chunk : {1U, 2U, 5U, 11U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(raw, chunk);
        CHECK_EQ(out.answer, "Water boils at 100 °C at sea level.");
        CHECK_EQ(out.reasoning, "We need the boiling point. It is 100 C.");
    }
}

TEST(the_role_between_harmony_messages_is_not_part_of_either) {
    // "assistant" appears between <|end|> and <|channel|>. It is a header, and
    // putting it at the front of the answer is exactly the kind of stray text
    // that makes a reply look broken.
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|channel|>analysis<|message|>x<|end|><|start|>assistant"
        "<|channel|>final<|message|>y", 1);
    CHECK_EQ(out.answer, "y");
    CHECK_EQ(out.reasoning, "x");
}

TEST(think_tags_are_split_the_same_way) {
    // DeepSeek-R1, Qwen3 and most of what followed them.
    for (const std::size_t chunk : {1U, 4U, 999U}) {
        const ResponseFilter::Piece out =
            filter_in_chunks("<think>Let me work this out.</think>The answer is 42.", chunk);
        CHECK_EQ(out.answer, "The answer is 42.");
        CHECK_EQ(out.reasoning, "Let me work this out.");
    }
}

TEST(a_less_than_sign_in_an_answer_is_still_a_less_than_sign) {
    // The filter holds text back when it might be the start of a marker, and
    // the risk is that it holds back something that never was one -- or worse,
    // silently eats it.
    const std::string code = "if (a < b) { x<y; } // <not a marker> <|nope|>";
    for (const std::size_t chunk : {1U, 3U, 999U}) {
        CHECK_EQ(filter_in_chunks(code, chunk).answer, code);
    }
}

TEST(a_reply_cut_off_mid_marker_does_not_lose_its_last_characters) {
    // Hitting the token limit part way through "<|chan" leaves bytes held back
    // that nothing else will ever resolve. flush() is what releases them.
    ResponseFilter filter;
    ResponseFilter::Piece out = filter.feed("done <|chan");
    CHECK_EQ(out.answer, "done ");
    out = filter.flush();
    CHECK_EQ(out.answer, "<|chan");
}

TEST(the_filter_knows_when_the_answer_has_started) {
    // What the status line reads to say "thinking" rather than "answering".
    ResponseFilter filter;
    CHECK(filter.answering());  // a model with no markers is answering at once
    filter.feed("<|channel|>analysis<|message|>working");
    CHECK(!filter.answering());
    filter.feed("<|end|><|start|>assistant<|channel|>final<|message|>hello");
    CHECK(filter.answering());
}

// ---------------------------------------------------------------------------
// UTF-8 streaming
// ---------------------------------------------------------------------------

// --- a model's own tool calls ---------------------------------------------
//
// Each of these is a round gpt-oss actually produced in a cook, copied out of
// a trace. Before the filter knew about them every one of them arrived as an
// empty answer, and the journal said "(said nothing)" three hundred times.

TEST(a_protocol_line_written_where_a_channel_name_goes_is_the_answer) {
    // No <|message|> at all: the model put its message after <|channel|>.
    const std::string raw =
        "<|channel|>NOTE: The project has no test file. Need to list.<|end|>"
        "<|start|>assistant<|channel|>LIST: .";
    for (const std::size_t chunk : {1U, 5U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(raw, chunk);
        CHECK(out.answer.find("NOTE: The project has no test file. Need to list.\n") != std::string::npos);
        CHECK(out.answer.find("\nLIST: .") != std::string::npos);
        const std::optional<tools::ToolCall> call = tools::parse_tool_call(out.answer, out.reasoning);
        CHECK(call.has_value());
        if (call) {
            CHECK(call->kind == tools::ToolKind::Note);
        }
    }
}

TEST(a_shell_call_in_the_models_own_format_becomes_run) {
    const std::string raw =
        "<|channel|>analysis<|message|>Let us look around.<|end|>"
        "<|start|>assistant<|channel|>commentary to=container.exec code<|message|>"
        "{\"cmd\":[\"bash\",\"-lc\",\"ls -R\"]}";
    for (const std::size_t chunk : {1U, 7U, 999U}) {
        const ResponseFilter::Piece out = filter_in_chunks(raw, chunk);
        CHECK_EQ(out.answer, std::string("RUN: ls -R\n"));
        CHECK_EQ(out.reasoning, std::string("Let us look around."));
    }
}

TEST(a_call_addressed_to_a_verb_carries_the_verb) {
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|channel|>commentary to=LIST <|constrain|>1<|message|>LIST: .\n", 3);
    CHECK_EQ(out.answer, std::string("LIST: .\n"));
}

TEST(a_recipient_in_the_role_header_counts_too) {
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|start|>assistant to=functions.read<|channel|>commentary json<|message|>"
        "{\"path\": \"orbit.py\"}<|call|>", 4);
    CHECK_EQ(out.answer, std::string("READ: orbit.py\n"));
}

TEST(a_write_in_the_models_own_format_keeps_the_whole_file) {
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|channel|>commentary to=functions.write_file <|constrain|>json<|message|>"
        "{\"path\": \"a.py\", \"content\": \"x = 1\\ny = 2\"}<|call|>", 6);
    const std::optional<tools::ToolCall> call = tools::parse_tool_call(out.answer, out.reasoning);
    CHECK(call.has_value());
    if (call) {
        CHECK(call->kind == tools::ToolKind::Write);
        CHECK_EQ(call->argument, std::string("a.py"));
        CHECK_EQ(call->content, std::string("x = 1\ny = 2"));
    }
}

TEST(a_call_to_a_tool_crucible_does_not_have_stays_reasoning) {
    const ResponseFilter::Piece out = filter_in_chunks(
        "<|channel|>analysis to=python code<|message|>print(1)<|call|>", 2);
    CHECK(out.answer.empty());
    CHECK_EQ(out.reasoning, std::string("print(1)"));
}

TEST(an_ordinary_channel_name_with_no_message_is_not_an_answer) {
    // A real channel cut off before its message is not a message.
    const ResponseFilter::Piece out = filter_in_chunks("<|channel|>analysis", 1);
    CHECK(out.answer.empty());
    // And a final answer is untouched by any of the above.
    const ResponseFilter::Piece plain = filter_in_chunks(
        "<|channel|>final<|message|>The answer is 2x.", 3);
    CHECK_EQ(plain.answer, std::string("The answer is 2x."));
}

TEST(each_call_is_read_the_way_its_tool_reads_arguments) {
    CHECK_EQ(harmony_call_line("container.exec", R"({"cmd":["python3","-m","pytest","-q"]})"),
             std::string("RUN: python3 -m pytest -q"));
    CHECK_EQ(harmony_call_line("functions.run", R"({"command": "make"})"), std::string("RUN: make"));
    CHECK_EQ(harmony_call_line("functions.list_dir", "{}"), std::string("LIST: ."));
    CHECK_EQ(harmony_call_line("functions.search", R"({"query": "orbital speed"})"),
             std::string("SEARCH: orbital speed"));
    CHECK_EQ(harmony_call_line("functions.done", R"({"summary": "added docstrings"})"),
             std::string("DONE: added docstrings"));
    CHECK_EQ(harmony_call_line("READ", "orbit.py"), std::string("READ: orbit.py"));
    CHECK(harmony_call_line("browser.open", R"({"id": 3})").empty());
    // A file that itself holds a fence is written with the other delimiter.
    CHECK(harmony_call_line("write", R"({"path": "R.md", "content": "```\ncode\n```"})")
              .find("<<<") != std::string::npos);
}

TEST(utf8_lengths_are_read_from_the_lead_byte) {
    CHECK_EQ(detail::utf8_length('a'),  1);
    CHECK_EQ(detail::utf8_length(0xC3), 2);
    CHECK_EQ(detail::utf8_length(0xE2), 3);
    CHECK_EQ(detail::utf8_length(0xF0), 4);
    CHECK_EQ(detail::utf8_length(0x80), 1);  // stray continuation: make progress
}

TEST(complete_utf8_passes_straight_through) {
    std::string buffer = "hello";
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("hello"));
    CHECK(buffer.empty());

    buffer = "caf\xC3\xA9";  // café
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("caf\xC3\xA9"));
    CHECK(buffer.empty());
}

TEST(split_codepoints_are_held_back_until_complete) {
    // The exact failure this guards against: a token ending mid-codepoint would
    // otherwise be printed as a replacement character.
    std::string buffer = "abc\xE2\x80";           // '…' missing its last byte
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("abc"));
    CHECK_EQ(buffer, std::string("\xE2\x80"));

    buffer += "\xA6";                              // the byte finally arrives
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string("\xE2\x80\xA6"));
    CHECK(buffer.empty());
}

TEST(a_four_byte_emoji_arriving_one_byte_at_a_time) {
    const std::string emoji = "\xF0\x9F\xA6\x87";  // 🦇, which Crucible has earned
    std::string buffer;
    std::string emitted;
    for (const char byte : emoji) {
        buffer += byte;
        emitted += detail::take_complete_utf8(buffer);
    }
    CHECK_EQ(emitted, emoji);
    CHECK(buffer.empty());
}

TEST(empty_buffer_is_handled) {
    std::string buffer;
    CHECK_EQ(detail::take_complete_utf8(buffer), std::string());
    CHECK(buffer.empty());
}

// ---------------------------------------------------------------------------
// Token accounting
// ---------------------------------------------------------------------------

TEST(usage_accumulates_across_turns) {
    GenerationStats first;
    first.prompt_tokens = 100;
    first.output_tokens = 50;
    first.output_ms     = 1000.0;

    GenerationStats second;
    second.prompt_tokens = 200;
    second.output_tokens = 150;
    second.output_ms     = 3000.0;

    TokenUsage usage;
    usage.add(first);
    usage.add(second);

    CHECK_EQ(usage.input_tokens, std::uint64_t{300});
    CHECK_EQ(usage.output_tokens, std::uint64_t{200});
    CHECK_EQ(usage.turns, std::uint64_t{2});
    CHECK_EQ(usage.total_tokens(), std::uint64_t{500});
    // 200 output tokens in 4 seconds.
    CHECK(std::abs(usage.tokens_per_second() - 50.0) < 0.001);
}

TEST(a_rate_is_only_reported_once_there_is_something_to_divide) {
    TokenUsage empty;
    CHECK(std::abs(empty.tokens_per_second()) < 0.001);

    TokenUsage no_time;
    no_time.output_tokens = 100;
    CHECK(std::abs(no_time.tokens_per_second()) < 0.001);
}

// ---------------------------------------------------------------------------
// Session history
// ---------------------------------------------------------------------------

namespace {

Turn finished_turn(std::string prompt, std::string reply, int output_tokens) {
    Turn turn;
    turn.prompt        = std::move(prompt);
    turn.reply         = std::move(reply);
    turn.output_tokens = output_tokens;
    turn.streaming     = false;
    return turn;
}

}  // namespace

TEST(a_session_round_trips_through_disk) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::vector<Turn> turns{
        finished_turn("what is a tensor?", "A tensor is...", 42),
        finished_turn("and a manifold?", "A manifold is...", 58),
    };
    turns[1].route = RouteDecision{"mathematics", 0.9F, RouteSource::Model, "picked"};

    TokenUsage usage;
    usage.input_tokens  = 30;
    usage.output_tokens = 100;
    usage.turns         = 2;

    std::string error;
    CHECK(store.save(turns, usage, error));
    CHECK(error.empty());

    std::vector<Turn> loaded;
    TokenUsage loaded_usage;
    CHECK(store.load(store.session_id(), loaded, loaded_usage, error));
    CHECK_EQ(loaded.size(), std::size_t{2});
    CHECK_EQ(loaded[0].prompt, std::string("what is a tensor?"));
    CHECK_EQ(loaded[1].reply, std::string("A manifold is..."));
    CHECK_EQ(loaded[1].output_tokens, 58);
    CHECK(loaded[1].route.has_value());
    CHECK_EQ(loaded[1].route->expert, ExpertId("mathematics"));
    // How it was routed has to survive too: labeling a resumed turn
    // "fallback" when the delegator chose it is an untrue claim about history.
    CHECK_EQ(static_cast<int>(loaded[1].route->source), static_cast<int>(RouteSource::Model));
    CHECK(std::abs(loaded[1].route->confidence - 0.9F) < 0.001F);
    CHECK_EQ(loaded_usage.output_tokens, std::uint64_t{100});
}

TEST(every_route_source_survives_a_round_trip_through_its_name) {
    for (const RouteSource source : {RouteSource::Model, RouteSource::Keyword,
                                     RouteSource::Forced, RouteSource::Fallback}) {
        CHECK_EQ(static_cast<int>(route_source_from_name(route_source_name(source))),
                 static_cast<int>(source));
    }
    CHECK_EQ(static_cast<int>(route_source_from_name("something else")),
             static_cast<int>(RouteSource::Fallback));
}

TEST(a_reply_still_streaming_is_not_written) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::vector<Turn> turns{finished_turn("done", "an answer", 10)};
    Turn in_flight;
    in_flight.prompt    = "still going";
    in_flight.reply     = "half an ans";
    in_flight.streaming = true;
    turns.push_back(in_flight);

    std::string error;
    CHECK(store.save(turns, TokenUsage{}, error));

    std::vector<Turn> loaded;
    TokenUsage usage;
    CHECK(store.load(store.session_id(), loaded, usage, error));
    // A half-finished reply is not something to resume into.
    CHECK_EQ(loaded.size(), std::size_t{1});
    CHECK_EQ(loaded[0].prompt, std::string("done"));
}

TEST(sessions_are_listed_newest_first_with_the_first_prompt_as_the_title) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;

    store.adopt("20260101-120000");
    CHECK(store.save({finished_turn("the older question", "...", 5)}, TokenUsage{}, error));

    store.adopt("20260830-090000");
    CHECK(store.save({finished_turn("the newer question", "...", 5)}, TokenUsage{}, error));

    const std::vector<SessionSummary> listed = store.list();
    CHECK_EQ(listed.size(), std::size_t{2});
    CHECK_EQ(listed[0].id, std::string("20260830-090000"));
    CHECK_EQ(listed[0].title, std::string("the newer question"));
    CHECK_EQ(listed[1].title, std::string("the older question"));
    CHECK_EQ(listed[0].turns, 1);
}

TEST(a_multi_line_prompt_still_makes_a_one_line_title) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;
    CHECK(store.save({finished_turn("first line\nsecond line", "...", 1)}, TokenUsage{}, error));

    const std::vector<SessionSummary> listed = store.list();
    CHECK_EQ(listed.size(), std::size_t{1});
    // A newline in the title would break the picker's row layout.
    CHECK(listed[0].title.find('\n') == std::string::npos);
    CHECK(listed[0].title.find("second line") != std::string::npos);
}

TEST(the_project_total_counts_each_token_once_however_often_a_session_is_saved) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;

    TokenUsage usage;
    usage.input_tokens  = 100;
    usage.output_tokens = 200;
    usage.turns         = 1;

    std::vector<Turn> turns{finished_turn("q", "a", 200)};
    CHECK(store.save(turns, usage, error));
    CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{200});

    // Saving the same session again after another turn must add the delta,
    // not the whole total a second time.
    turns.push_back(finished_turn("q2", "a2", 100));
    usage.input_tokens  = 150;
    usage.output_tokens = 300;
    usage.turns         = 2;
    CHECK(store.save(turns, usage, error));

    CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{300});
    CHECK_EQ(store.project_usage().input_tokens, std::uint64_t{150});
    CHECK_EQ(store.project_usage().turns, std::uint64_t{2});
}

TEST(resuming_a_session_does_not_double_count_what_it_already_spent) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    std::string error;
    TokenUsage usage;
    usage.input_tokens  = 100;
    usage.output_tokens = 200;
    usage.turns         = 1;

    std::string id;
    {
        SessionStore store(Project::current());
        CHECK(store.save({finished_turn("q", "a", 200)}, usage, error));
        id = store.session_id();
        CHECK_EQ(store.project_usage().output_tokens, std::uint64_t{200});
    }

    // A second run of Crucible resumes it. Its tokens are already in the total.
    SessionStore resumed(Project::current());
    resumed.adopt(id);
    CHECK(resumed.save({finished_turn("q", "a", 200)}, usage, error));
    CHECK_EQ(resumed.project_usage().output_tokens, std::uint64_t{200});
}

TEST(a_session_can_be_deleted) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    SessionStore store(Project::current());
    std::string error;
    CHECK(store.save({finished_turn("q", "a", 1)}, TokenUsage{}, error));
    CHECK_EQ(store.list().size(), std::size_t{1});

    CHECK(store.remove(store.session_id(), error));
    CHECK_EQ(store.list().size(), std::size_t{0});
    CHECK(!store.remove("20200101-000000", error));
}

TEST(projects_in_different_directories_do_not_share_history) {
    TempDir temp;
    const ScopedDataHome scoped(temp.path());

    // Two directories whose last component is the same -- the case a slug
    // alone would collide on, which is why the key carries a hash.
    TempDir a;
    TempDir b;
    std::filesystem::create_directories(a.path() / "src");
    std::filesystem::create_directories(b.path() / "src");

    const std::filesystem::path original = std::filesystem::current_path();
    std::filesystem::current_path(a.path() / "src");
    const Project first = Project::current();
    std::filesystem::current_path(b.path() / "src");
    const Project second = Project::current();
    std::filesystem::current_path(original);

    CHECK_EQ(first.name, std::string("src"));
    CHECK_EQ(second.name, std::string("src"));
    CHECK(first.dir != second.dir);
}
