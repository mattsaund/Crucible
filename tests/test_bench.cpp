// SPDX-License-Identifier: MIT
//
// The Bench's side of the core: how many questions a model is asked at once,
// and the batch that answers them.
#include "test_helpers.hpp"

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <llama.h>

#include "crucible/llm/batch_chat.hpp"
#include "crucible/llm/model_host.hpp"
#include "crucible/llm/model_shape.hpp"

namespace {

/// LFM2.5-1.2B's shape, near enough: 6 of 16 layers attend, 8 heads of 64
/// for keys and values in 16 bits, and a small recurrent state for the rest.
ModelShape small_model() {
    ModelShape shape;
    shape.known        = true;
    shape.weights      = 1250ULL << 20;
    shape.kv_per_token = 6 * 2 * 8 * 64 * 2;
    for (int i = 0; i < 16; ++i) {
        shape.units.push_back(ModelUnit{70ULL << 20, 0, i % 3 == 0 ? 0ULL : 16384ULL, false});
    }
    return shape;
}

}  // namespace

TEST(as_many_questions_at_once_as_the_cards_hold_a_cache_for) {
    const ModelShape shape = small_model();
    const std::uint64_t each = shape.kv_bytes(20480) + shape.state_bytes();
    CHECK_EQ(detail::side_by_side(shape, each * 5 + each / 2, 20480, 512, 32), 5);
    // Never more than the most, and never none.
    CHECK_EQ(detail::side_by_side(shape, each * 100, 20480, 512, 32), 32);
    CHECK_EQ(detail::side_by_side(shape, 0, 20480, 512, 32), 1);
    CHECK_EQ(detail::side_by_side(shape, each * 100, 20480, 512, 1), 1);
    // A smaller context, more of them.
    CHECK(detail::side_by_side(shape, each * 5, 8192, 512, 32) > 5);
}

TEST(a_sliding_window_is_charged_its_window_for_each_question) {
    // gpt-oss's attention: half its layers look 128 back. A question's cache
    // is the whole context on the others and a window on those.
    ModelShape oss;
    oss.known                = true;
    oss.window               = 128;
    oss.kv_per_token         = 24 * 2 * 8 * 64 * 2;
    oss.sliding_kv_per_token = 12 * 2 * 8 * 64 * 2;
    const std::uint64_t each = oss.kv_bytes(20480, oss.sliding_cells(20480, 512, true, 1));
    CHECK(each < oss.kv_bytes(20480) * 6 / 10);
    CHECK_EQ(detail::side_by_side(oss, each * 3, 20480, 512, 32), 3);
}

TEST(a_shape_the_header_did_not_give_is_asked_one_at_a_time) {
    ModelShape unknown;
    CHECK_EQ(detail::side_by_side(unknown, 64ULL << 30, 20480, 512, 32), 1);
    ModelShape no_cache = small_model();
    no_cache.kv_per_token = 0;
    no_cache.units.clear();
    CHECK_EQ(detail::side_by_side(no_cache, 64ULL << 30, 20480, 512, 32), 1);
}

TEST(a_batch_answers_every_question_and_says_when_it_ends) {
    // Only where a model is named: CRUCIBLE_TEST_MODEL, a small GGUF, with a
    // runtime installed that loads it.
    const char* named = std::getenv("CRUCIBLE_TEST_MODEL");
    if (named == nullptr || *named == '\0') {
        return;
    }
    TempDir dir;
    ModelHost host(dir.path() / "llama.log");
    ModelParams params;
    params.model        = named;
    params.path         = named;
    params.n_ctx        = 4 * 2048;
    params.sequences    = 4;
    params.own_caches   = true;
    params.window_cache = true;
    params.temperature  = 0.0F;
    params.max_tokens   = 24;
    std::string  error;
    LoadedModel* model = host.acquire_expert("bench", params, {}, {}, error);
    CHECK(model != nullptr);
    if (model == nullptr) {
        std::printf("      %s\n", error.c_str());
        return;
    }

    // Asked alone first, one question at a time: what each answer should be.
    const std::vector<std::string> questions{"Name a color.", "What is 2 + 3?", "Say hello.",
                                             "Name a planet.", "What is the capital of France?",
                                             "Count to three."};
    std::vector<std::string> alone;
    for (const std::string& question : questions) {
        std::string answer;
        ChatSink    sink;
        sink.on_text = [&answer](std::string_view text) { answer += text; };
        const std::vector<ChatMessage> messages{{"user", question}};
        model->chat(ChatRequest{messages, params, ""}, sink);
        alone.push_back(answer);
    }

    // A batch of one decodes as the model alone does: the same answer.
    {
        BatchChat   one(*model, 1);
        std::string answer;
        ChatSink    sink;
        sink.on_text = [&answer](std::string_view text) { answer += text; };
        CHECK(one.chat({{"user", questions[0]}}, params, sink).error.empty());
        CHECK_EQ(answer, alone[0]);
    }

    std::vector<std::string> together(questions.size());
    std::vector<int>         ended(questions.size(), 0);
    std::vector<ChatResult>  results(questions.size());
    std::mutex               mutex;
    {
        BatchChat batch(*model, 4);
        CHECK_EQ(batch.slots(), 4);
        for (std::size_t i = 0; i < questions.size(); ++i) {
            BatchChat::Ask ask;
            ask.messages     = {{"user", questions[i]}};
            ask.params       = params;
            ask.sink.on_text = [&together, i](std::string_view text) { together[i] += text; };
            ask.done         = [&, i](ChatResult result) {
                const std::lock_guard<std::mutex> lock(mutex);
                ++ended[i];
                results[i] = std::move(result);
            };
            batch.submit(std::move(ask));
        }
        // One more, stopped before it starts: it ends unanswered.
        BatchChat::Ask stopped;
        stopped.messages    = {{"user", "Never mind."}};
        stopped.params      = params;
        stopped.sink.cancel = [] { return true; };
        bool told           = false;
        stopped.done        = [&told](ChatResult result) { told = result.stats.canceled; };
        batch.submit(std::move(stopped));
        // And the convenience, which waits.
        const ChatResult waited = batch.chat({{"user", "Say hello."}}, params, {});
        CHECK(waited.error.empty());
        CHECK(told);
    }
    for (std::size_t i = 0; i < questions.size(); ++i) {
        CHECK_EQ(ended[i], 1);
        CHECK(results[i].error.empty());
        CHECK(!together[i].empty());
        CHECK(results[i].stats.output_tokens > 0);
        CHECK(results[i].stats.output_tokens <= 24);
        // Side by side the last bits of the arithmetic can differ, and a
        // greedy answer with them: said, not failed.
        if (together[i] != alone[i]) {
            std::printf("      differs from alone: \"%s\" against \"%s\"\n", together[i].c_str(), alone[i].c_str());
        }
    }

    // Queued together, the same questions make the same passes, and so get
    // the same answers, however the threads fall.
    const auto queued_together = [&] {
        std::vector<std::string>    said(questions.size());
        std::mutex                  guard;
        std::condition_variable     all_done;
        std::size_t                 done = 0;
        BatchChat                   batch(*model, 4);
        std::vector<BatchChat::Ask> asks;
        for (std::size_t i = 0; i < questions.size(); ++i) {
            BatchChat::Ask ask;
            ask.messages     = {{"user", questions[i]}};
            ask.params       = params;
            ask.sink.on_text = [&said, i](std::string_view text) { said[i] += text; };
            ask.done         = [&](ChatResult) {
                const std::lock_guard<std::mutex> lock(guard);
                ++done;
                all_done.notify_all();
            };
            asks.push_back(std::move(ask));
        }
        batch.submit(std::move(asks));
        std::unique_lock<std::mutex> lock(guard);
        all_done.wait(lock, [&] { return done == questions.size(); });
        return said;
    };
    const std::vector<std::string> first = queued_together();
    CHECK(first == queued_together());

    // The model is the chat's again, its cache as if from cold.
    std::string again;
    ChatSink    sink;
    sink.on_text = [&again](std::string_view text) { again += text; };
    const std::vector<ChatMessage> messages{{"user", questions[0]}};
    model->chat(ChatRequest{messages, params, ""}, sink);
    CHECK_EQ(again, alone[0]);
    llama_log_set(nullptr, nullptr);
}

TEST(an_answer_moved_down_a_slot_goes_on_as_it_would_have) {
    const char* named = std::getenv("CRUCIBLE_TEST_MODEL");
    if (named == nullptr || *named == '\0') {
        return;
    }
    TempDir dir;
    ModelHost host(dir.path() / "llama.log");
    ModelParams params;
    params.model       = named;
    params.path        = named;
    params.n_ctx       = 2 * 2048;
    params.sequences   = 2;
    params.own_caches  = true;
    params.temperature = 0.0F;
    params.max_tokens  = 48;
    std::string  error;
    LoadedModel* model = host.acquire_expert("bench", params, {}, {}, error);
    CHECK(model != nullptr);
    if (model == nullptr) {
        return;
    }
    // The long answer asked second, in the second slot, and moved down when
    // the short one ends; then asked first, where it stays. The passes are
    // the same shape either way, so the answer must be too.
    const auto long_answer = [&](bool second) {
        std::vector<std::string>    said(2);
        BatchChat                   batch(*model, 2);
        std::vector<BatchChat::Ask> asks(2);
        std::mutex                  guard;
        std::condition_variable     all_done;
        int                         done = 0;
        const std::vector<std::string> questions{"Reply with the one word yes.",
                                                 "Write a long story about a lighthouse keeper."};
        for (std::size_t i = 0; i < 2; ++i) {
            const std::size_t at = second ? i : 1 - i;
            asks[at].messages     = {{"user", questions[i]}};
            asks[at].params       = params;
            asks[at].sink.on_text = [&said, i](std::string_view text) { said[i] += text; };
            asks[at].done         = [&](ChatResult) {
                const std::lock_guard<std::mutex> lock(guard);
                ++done;
                all_done.notify_all();
            };
        }
        batch.submit(std::move(asks));
        std::unique_lock<std::mutex> lock(guard);
        all_done.wait(lock, [&] { return done == 2; });
        return said;
    };
    const std::vector<std::string> moved = long_answer(true);
    const std::vector<std::string> stayed = long_answer(false);
    CHECK(moved[0].size() < moved[1].size());
    CHECK_EQ(moved[1], stayed[1]);
    CHECK_EQ(moved[0], stayed[0]);
    llama_log_set(nullptr, nullptr);
}

TEST(a_batch_going_away_tells_every_question_it_ended) {
    const char* named = std::getenv("CRUCIBLE_TEST_MODEL");
    if (named == nullptr || *named == '\0') {
        return;
    }
    TempDir dir;
    ModelHost host(dir.path() / "llama.log");
    ModelParams params;
    params.model       = named;
    params.path        = named;
    params.n_ctx       = 2 * 2048;
    params.sequences   = 2;
    params.own_caches  = true;
    params.temperature = 0.0F;
    params.max_tokens  = 2000;
    std::string  error;
    LoadedModel* model = host.acquire_expert("bench", params, {}, {}, error);
    CHECK(model != nullptr);
    if (model == nullptr) {
        return;
    }
    std::atomic<int> ended{0};
    std::atomic<int> stopped{0};
    {
        BatchChat batch(*model, 2);
        for (int i = 0; i < 6; ++i) {
            BatchChat::Ask ask;
            ask.messages = {{"user", "Write a long story about a lighthouse keeper."}};
            ask.params   = params;
            ask.done     = [&](ChatResult result) {
                ++ended;
                stopped += result.stats.canceled ? 1 : 0;
            };
            batch.submit(std::move(ask));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }
    CHECK_EQ(ended.load(), 6);
    CHECK(stopped.load() >= 4);
    llama_log_set(nullptr, nullptr);
}
