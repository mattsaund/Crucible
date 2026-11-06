// SPDX-License-Identifier: MIT
//
// Many conversations answered at once by one loaded model.
//
// A card answering one conversation spends nearly all of each token reading
// the weights, for one token's worth of arithmetic. The Bench asks a model
// hundreds of questions, none of which waits on another, so each is given a
// sequence of its own in the context and every pass reads the weights once
// for a token of each.
//
// Each sequence is decoded as if it were alone: its own cache, its own
// sampler, its own end. What can differ is in the last bits of the
// arithmetic: a batch of twenty is added up in another order than a batch
// of one, on a card and on the processor alike, and greedy decoding follows
// a near tie whichever way it falls. So the queue is answered in the order
// it was given and a slot is filled only between passes: the same questions
// asked the same way on the same machine make the same passes, and get the
// same answers.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "crucible/llm/chat_model.hpp"

struct llama_batch;

namespace crucible {

class LoadedModel;
struct ModelShape;

namespace detail {

/// How many sequences of `context` tokens to run side by side for a model
/// of `shape` when the cards have `room` bytes for their caches: as many as
/// fit, at most `most`, and never fewer than one. A model whose header does
/// not describe its attention gets one.
int side_by_side(const ModelShape& shape, std::uint64_t room, int context, int n_batch, int most);

}  // namespace detail

class BatchChat {
public:
    /// One question: what LoadedModel::chat takes, owned, and where the
    /// answer goes. `sink` and `done` are called on the thread that decodes,
    /// `done` once, whether the answer finished, was stopped or failed.
    struct Ask {
        std::vector<ChatMessage>        messages;
        ModelParams                     params;
        ChatSink                        sink;
        std::function<void(ChatResult)> done;
    };

    /// Answers from `model`, `slots` at a time: no more than the context
    /// has sequences for (ModelParams::sequences). The model is this one's
    /// until it is destroyed, and another call to it waits.
    BatchChat(LoadedModel& model, int slots);

    /// Whatever has not been answered is stopped, and told so through its
    /// `done`, before this returns.
    ~BatchChat();

    BatchChat(const BatchChat&)            = delete;
    BatchChat& operator=(const BatchChat&) = delete;

    /// Queue `ask`, to be answered as a slot comes free. From any thread.
    void submit(Ask ask);

    /// Queue `asks` in their order, all of them before the next pass looks:
    /// otherwise the first could take a slot alone, and which pass the rest
    /// joined would be the threads' to say.
    void submit(std::vector<Ask> asks);

    /// Ask and wait for the answer, as LoadedModel::chat does.
    ChatResult chat(const std::vector<ChatMessage>& messages, const ModelParams& params, const ChatSink& sink);

    int slots() const { return slots_; }

private:
    struct Slot;

    /// The decoding, on a thread of its own. Between passes the answers that
    /// were stopped end and the waiting questions take the slots free; then
    /// a pass reads a token for every slot that is answering and as much of
    /// each new prompt as the batch has room for.
    void run();

    /// One pass over the slots: read, then sample each one that has
    /// something to say. False when the decode failed or was stopped.
    bool pass(llama_batch& batch, std::size_t room);

    /// The answer in `slot` is over: its sequence cleared, `done` told.
    void finish(Slot& slot);

    /// Every answer in the pass under way has been stopped, or this is
    /// closing: what aborts a decode partway.
    bool abandoned() const;

    LoadedModel& model_;
    int          slots_ = 1;

    /// The decoding thread's own; read from inside a decode by abandoned(),
    /// while that thread waits on it.
    std::vector<Slot> held_;

    std::mutex              mutex_;
    std::condition_variable wake_;
    std::deque<Ask>         queue_;
    std::atomic<bool>       closing_{false};
    std::thread             thread_;
};

}  // namespace crucible
