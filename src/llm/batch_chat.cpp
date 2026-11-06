// SPDX-License-Identifier: MIT
//
// Many conversations answered at once by one loaded model. See batch_chat.hpp.
#include "crucible/llm/batch_chat.hpp"

#include <algorithm>
#include <chrono>
#include <iterator>
#include <utility>

#include <llama.h>

#include "crucible/llm/loaded_model.hpp"
#include "crucible/llm/model_shape.hpp"
#include "crucible/llm/sampling.hpp"
#include "crucible/util/text.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

std::vector<llama_token> tokenize(const llama_vocab* vocab, const std::string& text) {
    // A negative return is llama.cpp saying how much room it needs.
    const int needed = -llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), nullptr, 0, true, true);
    if (needed <= 0) {
        return {};
    }
    std::vector<llama_token> tokens(static_cast<std::size_t>(needed));
    const int written = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.size()), tokens.data(), needed,
                                       true, true);
    tokens.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return tokens;
}

std::string piece_for(const llama_vocab* vocab, llama_token token) {
    std::string piece(64, '\0');
    int written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, false);
    if (written < 0) {
        piece.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, false);
    }
    piece.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return piece;
}

bool stopped(const BatchChat::Ask& ask) { return ask.sink.cancel && ask.sink.cancel(); }

}  // namespace

namespace detail {

int side_by_side(const ModelShape& shape, std::uint64_t room, int context, int n_batch, int most) {
    if (!shape.known || most <= 1 || context <= 0) {
        return 1;
    }
    // Each sequence's own cache: the whole context on the layers that read
    // all of it, the window on the ones that slide. Every question starts
    // from nothing, so no prompt is ever rewound, and a window is all a
    // sliding layer needs.
    const std::uint64_t each = shape.kv_bytes(context, shape.sliding_cells(context, n_batch, true, 1))
                             + shape.state_bytes();
    if (each == 0) {
        return 1;   // an attention the header did not describe: no figure to divide by
    }
    return static_cast<int>(std::clamp<std::uint64_t>(room / each, 1, static_cast<std::uint64_t>(most)));
}

}  // namespace detail

/// A sequence of the context and the question it is answering, if any.
struct BatchChat::Slot {
    llama_seq_id seq  = 0;
    bool         busy = false;
    Ask          ask;
    ChatResult   result;

    std::vector<llama_token> prompt;
    std::size_t              read     = 0;   ///< how much of the prompt the cache holds
    int                      produced = 0;
    int                      limit    = 0;
    llama_token              next     = 0;   ///< sampled, and read in the next pass
    bool                     writing  = false;
    int                      output   = -1;  ///< where its logits are in this pass's batch

    std::string    pending;   ///< the bytes of a character not yet whole
    llama_sampler* chain = nullptr;

    Clock::time_point began;
    Clock::time_point wrote;
};

BatchChat::BatchChat(LoadedModel& model, int slots)
    : model_(model),
      // No more than the context has sequences for, nor than a batch holds
      // a token of each.
      slots_(std::clamp(slots, 1,
                        std::max(1, static_cast<int>(std::min(llama_n_seq_max(model.ctx_), llama_n_batch(model.ctx_)))))) {
    thread_ = std::thread([this] { run(); });
}

BatchChat::~BatchChat() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        closing_.store(true, std::memory_order_relaxed);
    }
    wake_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
}

void BatchChat::submit(Ask ask) {
    std::vector<Ask> one;
    one.push_back(std::move(ask));
    submit(std::move(one));
}

void BatchChat::submit(std::vector<Ask> asks) {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (Ask& ask : asks) {
            queue_.push_back(std::move(ask));
        }
    }
    wake_.notify_all();
}

ChatResult BatchChat::chat(const std::vector<ChatMessage>& messages, const ModelParams& params,
                           const ChatSink& sink) {
    std::mutex              mutex;
    std::condition_variable ended;
    bool                    done = false;
    ChatResult              out;
    submit(Ask{messages, params, sink, [&](ChatResult result) {
                   const std::lock_guard<std::mutex> lock(mutex);
                   out  = std::move(result);
                   done = true;
                   ended.notify_all();
               }});
    std::unique_lock<std::mutex> lock(mutex);
    ended.wait(lock, [&] { return done; });
    return out;
}

bool BatchChat::abandoned() const {
    if (closing_.load(std::memory_order_relaxed)) {
        return true;
    }
    bool any = false;
    for (const Slot& slot : held_) {
        if (slot.busy) {
            if (!stopped(slot.ask)) {
                return false;
            }
            any = true;
        }
    }
    return any;
}

void BatchChat::run() {
    // The model to itself, for as long as this lasts: another caller would
    // decode into the sequences these answers are in.
    const std::lock_guard<std::recursive_mutex> mine(model_.use_);
    llama_context*     ctx    = model_.ctx_;
    const llama_vocab* vocab  = llama_model_get_vocab(model_.model_);
    llama_memory_t     memory = llama_get_memory(ctx);
    llama_memory_clear(memory, true);
    model_.forget();

    const int   n_batch = std::max(1, static_cast<int>(llama_n_batch(ctx)));
    const auto  room    = static_cast<std::size_t>(std::max(1U, llama_n_ctx_seq(ctx)));
    llama_batch batch   = llama_batch_init(n_batch, 0, 1);
    held_.assign(static_cast<std::size_t>(slots_), Slot{});
    for (std::size_t i = 0; i < held_.size(); ++i) {
        held_[i].seq = static_cast<llama_seq_id>(i);
    }

    // A greedy answer picked on the card that holds the output: only the
    // token comes back, where otherwise every slot's logits do on every
    // pass, a quarter of a megabyte each for a vocabulary of 65,536, and a
    // card on a narrow link spent more time sending those than computing
    // them. A slot that samples otherwise has its sampler on the processor.
    std::vector<llama_sampler*> on_card(held_.size(), nullptr);
    const auto pick_on_card = [&](const Slot& slot) {
        const bool greedy = slot.ask.params.temperature <= 0.0F && slot.ask.params.repeat_penalty == 1.0F;
        llama_sampler*& kept = on_card[static_cast<std::size_t>(slot.seq)];
        if (greedy == (kept != nullptr)) {
            return;
        }
        if (greedy) {
            kept = llama_sampler_chain_init(llama_sampler_chain_default_params());
            llama_sampler_chain_add(kept, llama_sampler_init_greedy());
            llama_set_sampler(ctx, slot.seq, kept);
        } else {
            llama_set_sampler(ctx, slot.seq, nullptr);
            llama_sampler_free(kept);
            kept = nullptr;
        }
    };

    // A question takes the first free slot, in the order asked.
    const auto begin = [&](Slot& slot, Ask ask) {
        slot.busy   = true;
        slot.ask    = std::move(ask);
        slot.result = ChatResult{};
        slot.began  = Clock::now();
        slot.prompt = tokenize(vocab, model_.format_chat(slot.ask.messages, /*add_assistant_prefix=*/true));
        if (slot.prompt.size() >= room) {
            // The tail, as LoadedModel::generate keeps it: the end of a
            // conversation matters more than its start.
            slot.prompt.erase(slot.prompt.begin(), slot.prompt.end() - static_cast<long>(
                                  room - std::min<std::size_t>(256, room / 4)));
        }
        slot.read     = 0;
        slot.produced = 0;
        slot.writing  = false;
        slot.limit    = slot.ask.params.max_tokens > 0 ? slot.ask.params.max_tokens : static_cast<int>(room);
        slot.pending.clear();
        slot.chain = llm::build_sampler_chain(vocab, slot.ask.params, {});
        pick_on_card(slot);
        slot.result.stats.prompt_tokens = static_cast<int>(slot.prompt.size());
        if (slot.prompt.empty()) {
            finish(slot);
        }
    };

    for (;;) {
        // The questions stopped since the last pass end where they stand.
        for (Slot& slot : held_) {
            if (slot.busy && stopped(slot.ask)) {
                slot.result.stats.canceled = true;
                finish(slot);
            }
        }

        // Then the waiting ones take the slots that are free, between passes
        // only and in the order they were asked: what makes a run's passes,
        // and so its answers, the same every time.
        std::vector<Ask> taken;
        bool             closing = false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            const auto idle = [&] {
                return std::none_of(held_.begin(), held_.end(), [](const Slot& s) { return s.busy; });
            };
            wake_.wait(lock, [&] { return closing_.load(std::memory_order_relaxed) || !queue_.empty() || !idle(); });
            closing = closing_.load(std::memory_order_relaxed);
            auto free = static_cast<std::size_t>(
                std::count_if(held_.begin(), held_.end(), [](const Slot& s) { return !s.busy; }));
            while (!queue_.empty() && (closing || free > 0)) {
                taken.push_back(std::move(queue_.front()));
                queue_.pop_front();
                free -= free > 0 ? 1 : 0;
            }
        }
        for (Ask& ask : taken) {
            if (closing || stopped(ask)) {
                ChatResult result;
                result.stats.canceled = true;
                if (ask.done) {
                    ask.done(std::move(result));
                }
                continue;
            }
            const auto slot = std::find_if(held_.begin(), held_.end(), [](const Slot& s) { return !s.busy; });
            begin(*slot, std::move(ask));
        }
        if (closing) {
            for (Slot& slot : held_) {
                if (slot.busy) {
                    slot.result.stats.canceled = true;
                    finish(slot);
                }
            }
            break;
        }
        if (std::none_of(held_.begin(), held_.end(), [](const Slot& s) { return s.busy; })) {
            continue;
        }

        // The last answers moved down into the slots the others left. A
        // context whose sequences each have a cache of their own reads a
        // batch in runs of neighboring sequences: four answers left in slots
        // 3, 9, 14 and 20 were four passes of one each, where in slots 0 to
        // 3 they are one pass of four. Moved whole, its cache copied, an
        // answer goes on as it would have. Only when nothing waiting took
        // the slot, which is the end of a run.
        for (;;) {
            const auto hole = std::find_if(held_.begin(), held_.end(), [](const Slot& s) { return !s.busy; });
            const auto last = std::find_if(held_.rbegin(), held_.rend(), [](const Slot& s) { return s.busy; });
            if (hole == held_.end() || last == held_.rend() || hole > std::prev(last.base())) {
                break;
            }
            Slot&              to   = *hole;
            Slot&              from = *std::prev(last.base());
            const llama_seq_id into = to.seq;
            const llama_seq_id away = from.seq;
            llama_memory_seq_cp(memory, away, into, -1, -1);
            llama_memory_seq_rm(memory, away, -1, -1);
            to       = std::move(from);
            to.seq   = into;
            from     = Slot{};
            from.seq = away;
            pick_on_card(to);
        }

        if (!pass(batch, room)) {
            // A pass that failed leaves the cache holding what nobody can
            // say: every answer in it ends, said so unless it was a stop.
            const bool all_stopped = abandoned();
            for (Slot& slot : held_) {
                if (slot.busy) {
                    slot.result.stats.canceled = all_stopped;
                    if (!all_stopped) {
                        slot.result.error = "the model could not read the batch it was given";
                    }
                    finish(slot);
                }
            }
            llama_memory_clear(memory, true);
        }
    }

    llama_batch_free(batch);
    // The context as the chat found it: no sampler of this one's on the
    // card, and nothing in the cache.
    for (std::size_t i = 0; i < on_card.size(); ++i) {
        if (on_card[i] != nullptr) {
            llama_set_sampler(ctx, static_cast<llama_seq_id>(i), nullptr);
            llama_sampler_free(on_card[i]);
        }
    }
    llama_memory_clear(memory, true);
    model_.forget();
    held_.clear();
}

bool BatchChat::pass(llama_batch& batch, std::size_t room) {
    llama_context*     ctx   = model_.ctx_;
    const llama_vocab* vocab = llama_model_get_vocab(model_.model_);
    const int          n_batch = std::max(1, static_cast<int>(llama_n_batch(ctx)));

    // A token for every slot that is answering, and what room is left for
    // prompts, first come first read. In the order of the slots: a context
    // whose sequences each have a cache of their own is read in runs of
    // neighbors, and a batch out of order would be read in more pieces.
    int left = n_batch - static_cast<int>(std::count_if(held_.begin(), held_.end(),
                                                        [](const Slot& s) { return s.busy && s.writing; }));
    batch.n_tokens = 0;
    const auto add = [&batch](llama_token token, std::size_t position, llama_seq_id seq, bool logits) {
        const int at        = batch.n_tokens++;
        batch.token[at]     = token;
        batch.pos[at]       = static_cast<llama_pos>(position);
        batch.n_seq_id[at]  = 1;
        batch.seq_id[at][0] = seq;
        batch.logits[at]    = logits ? 1 : 0;
        return at;
    };
    for (Slot& slot : held_) {
        slot.output = -1;
        if (!slot.busy) {
            continue;
        }
        if (slot.writing) {
            slot.output = add(slot.next, slot.prompt.size() + static_cast<std::size_t>(slot.produced) - 1, slot.seq, true);
            continue;
        }
        const auto take = std::min<std::size_t>(slot.prompt.size() - slot.read, static_cast<std::size_t>(std::max(0, left)));
        for (std::size_t i = 0; i < take; ++i) {
            const std::size_t at = slot.read + i;
            const int index      = add(slot.prompt[at], at, slot.seq, at + 1 == slot.prompt.size());
            if (at + 1 == slot.prompt.size()) {
                slot.output = index;
            }
        }
        slot.read += take;
        left -= static_cast<int>(take);
    }
    if (batch.n_tokens == 0) {
        return true;
    }

    // A Stop heard inside a pass as well as between them: on the processor
    // one pass of a large model is seconds.
    llama_set_abort_callback(
        ctx, [](void* data) { return static_cast<const BatchChat*>(data)->abandoned(); }, this);
    const int32_t decoded = llama_decode(ctx, batch);
    llama_set_abort_callback(ctx, nullptr, nullptr);
    if (decoded != 0) {
        return false;
    }

    for (Slot& slot : held_) {
        if (!slot.busy || slot.output < 0) {
            continue;
        }
        if (!slot.writing) {
            slot.writing                 = true;
            slot.result.stats.prompt_ms = ms_since(slot.began);
            slot.wrote                   = Clock::now();
        }
        const llama_token token = llama_sampler_sample(slot.chain, ctx, slot.output);
        if (llama_vocab_is_eog(vocab, token)) {
            finish(slot);
            continue;
        }
        slot.pending += piece_for(vocab, token);
        if (std::string ready = detail::take_complete_utf8(slot.pending); !ready.empty() && slot.ask.sink.on_text) {
            slot.ask.sink.on_text(ready);
        }
        ++slot.produced;
        ++slot.result.stats.output_tokens;
        if (slot.produced >= slot.limit) {
            slot.result.stats.hit_limit = true;
            finish(slot);
            continue;
        }
        // No room left in its cache to read it: where a reply alone would
        // have ended too, its next decode refused.
        if (slot.prompt.size() + static_cast<std::size_t>(slot.produced) > room) {
            finish(slot);
            continue;
        }
        slot.next = token;
    }
    return true;
}

void BatchChat::finish(Slot& slot) {
    // Whatever is left, even half a character: nothing more is coming.
    if (!slot.pending.empty() && slot.ask.sink.on_text) {
        slot.ask.sink.on_text(slot.pending);
    }
    if (slot.writing) {
        slot.result.stats.output_ms = ms_since(slot.wrote);
    } else {
        slot.result.stats.prompt_ms = ms_since(slot.began);
    }
    // A whole sequence always goes, recurrent state and all.
    llama_memory_seq_rm(llama_get_memory(model_.ctx_), slot.seq, -1, -1);
    if (slot.chain != nullptr) {
        llama_sampler_free(slot.chain);
    }
    const auto done   = std::move(slot.ask.done);
    ChatResult result = std::move(slot.result);
    const llama_seq_id seq = slot.seq;
    slot      = Slot{};
    slot.seq  = seq;
    if (done) {
        done(std::move(result));
    }
}

}  // namespace crucible
