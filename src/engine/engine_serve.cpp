// SPDX-License-Identifier: MIT
//
// What the API server answers with. See serve/server.hpp for the server.
//
// The delegator, or one model on this machine, held the way a seat holds one.
// A model from the models folder goes through hold_model like any expert: the
// host plans its memory, its load holds the runtimes still, a lease keeps it
// while a request has it, and between requests it is nobody's, so Eject and a
// seat that needs the room free it like anything else not in use. The
// delegator is the one the chat routes with, answered with under the models'
// lock as delegator.generate is: it is never freed under a request, and a
// route waits its turn behind one rather than the other way round.
//
// The chat and a build go on all the while. A request waits for whoever has
// the model, and says what it waits for; nothing the app is doing waits on a
// request longer than the model takes to answer it.
#include "crucible/engine/engine.hpp"

#include <chrono>
#include <thread>

#include "crucible/llm/model_catalog.hpp"
#include "crucible/llm/response_filter.hpp"

namespace crucible {
namespace {

/// `messages` with the line a reasoning model reads its effort from, which
/// gpt-oss takes from the system message (see Config::reasoning_effort).
std::vector<ChatMessage> with_effort(std::vector<ChatMessage> messages, const std::string& effort) {
    const std::string line = "Reasoning: " + effort;
    for (ChatMessage& message : messages) {
        if (message.role == "system") {
            message.content += "\n" + line;
            return messages;
        }
    }
    messages.insert(messages.begin(), ChatMessage{"system", line});
    return messages;
}

}  // namespace

std::string Engine::served_file(const Config& config) {
    if (config.server.model.empty()) {
        return config.router.model.empty() ? std::string() : config.router.path;
    }
    return resolve_model_ref(config.resolved_models_dir(), config.server.model).string();
}

std::string Engine::served_id(const Config& config) {
    const std::string file = served_file(config);
    return file.empty() ? std::string() : std::filesystem::path(file).filename().string();
}

ModelParams Engine::served_params(const Config& config, ExpertId* seat) {
    const std::string file = served_file(config);
    for (const Expert& expert : config.roster.experts()) {
        const ModelParams& params = config.expert(expert.id);
        if (!params.remote() && !params.path.empty() && params.path == file) {
            if (seat != nullptr) {
                *seat = expert.id;
            }
            return params;
        }
    }
    if (seat != nullptr) {
        *seat = kServerSeat;
    }
    ModelParams params = config.defaults;
    params.model = config.server.model;
    params.path  = file;
    params.provider.clear();
    return params;
}

serve::Status Engine::server_status() const { return server_->status(); }

void Engine::configure_server(const Config& config) {
    serve::Settings settings;
    settings.enabled = config.server.enabled && running_.load(std::memory_order_relaxed);
    settings.port    = config.server.port;
    settings.network = config.server.network;
    settings.key     = config.server.api_key;
    server_->apply(settings);
}

std::string Engine::serve_blocker() const {
    if (benching()) {
        return "the benchmark";
    }
    if (cooking()) {
        return "the build";
    }
    if (is_busy()) {
        return "the chat";
    }
    return "loading";
}

serve::Answer Engine::serve_request(const serve::Ask& ask, const serve::Sink& sink) {
    serve::Answer out;
    const CancelCallback cancel = [this, &sink] {
        return sink.cancel() || !running_.load(std::memory_order_relaxed);
    };
    // Nobody reads this when the client went; Crucible closing is said.
    const auto stopped = [&out] {
        out.status = 503;
        out.error  = "Crucible is closing";
        return out;
    };
    const auto pause = [] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); };

    // The worker makes the host as the engine starts, and a request can come
    // before it has.
    while (!host_ready_.load(std::memory_order_acquire)) {
        if (cancel()) {
            return stopped();
        }
        pause();
    }
    // A benchmark has the cards to itself, and what it times is its model
    // alone: a request waits for it to finish.
    if (benching()) {
        sink.waiting("the benchmark");
        while (benching()) {
            if (cancel()) {
                return stopped();
            }
            pause();
        }
    }

    const Config config = this->config();
    if (served_id(config) != ask.model) {
        out.status = 404;
        out.code   = "model_not_found";
        out.error  = "The model `" + ask.model + "` is no longer served";
        return out;
    }

    // --- the delegator --------------------------------------------------------
    //
    // Under the models' lock throughout, as a route's scoring is: with it on
    // demand it is freed for an expert, and an answer halfway through it would
    // be reading memory that had gone. A route or a load waits for the
    // answer, and the answer for them.
    if (config.server.model.empty()) {
        std::unique_lock<std::mutex> host(host_mutex_, std::try_to_lock);
        if (!host.owns_lock()) {
            sink.waiting(serve_blocker());
            host.lock();
        }
        if (cancel()) {
            return stopped();
        }
        if (host_->router() == nullptr) {
            sink.waiting("loading");
            ensure_router(cancel);
            if (!is_busy() && !cooking()) {
                state_.set_mood(Mood::Idle);   // load_router left it saying it was loading
            }
        }
        LoadedModel* model = host_->router();
        if (model == nullptr) {
            if (cancel()) {
                return stopped();
            }
            out.status = 503;
            out.error  = "The delegator did not load";
            return out;
        }
        // Sampled as Settings, Generation says, not as it routes: greedy, and
        // sixteen tokens at most, is right for a route and wrong for an answer.
        return serve_with(*model, model, config.defaults, ask, sink, cancel);
    }

    // --- a model from the folder -------------------------------------------------
    //
    // Held under the seat that has the same file, when one does: the chat and
    // the server then share one copy, and the chat answering with it while a
    // request comes is a wait for the model, not a swap.
    ExpertId          holder;
    const ModelParams params = served_params(config, &holder);
    const std::string name   = std::filesystem::path(params.path).filename().string();
    std::error_code   missing;
    if (!std::filesystem::exists(params.path, missing)) {
        out.status = 503;
        out.error  = name + " is not there";
        return out;
    }
    // With the delegator on demand it steps aside for a model that is not in
    // memory yet, as it does for any expert; for one that is, both are
    // already in, and freeing it would be a load for the next prompt and a
    // saving for nobody.
    bool resident = false;
    {
        std::unique_lock<std::mutex> host(host_mutex_, std::try_to_lock);
        if (!host.owns_lock()) {
            sink.waiting(serve_blocker());
            host.lock();
        }
        for (const ModelHost::Held& held : host_->held()) {
            resident = resident || held.model->path() == params.path;
        }
        resident = resident || mlx_.serving(params.path);
    }
    if (cancel()) {
        return stopped();
    }
    if (!resident) {
        if (!config.routing.keep_delegator_loaded) {
            if (routes_open_.load() > 0) {
                sink.waiting("the chat");   // a route is using it, and is waited for
            }
            release_router_between_routes();
        }
        sink.waiting("loading");
    }
    long        load_ms = 0;
    std::string error;
    HeldModel   held = hold_model(holder, params, holder == kServerSeat ? name : expert_label(config.roster, holder),
                                  cancel, load_ms, error, [&sink] { sink.waiting("memory"); });
    if (held.model == nullptr) {
        if (cancel() || error == "stopped") {
            return stopped();
        }
        out.status = 503;
        out.error  = name + " did not load" + (error.empty() ? std::string() : ": " + error);
        return out;
    }
    if (!is_busy() && !cooking() && !resident) {
        state_.set_mood(Mood::Idle);   // the load said so on the status line, and is over
    }
    // Somebody else holding it is somebody answering with it: the model takes
    // one caller at a time, and this one is next.
    if (leases_.count(params.path) > 1) {
        sink.waiting(serve_blocker());
    }
    return serve_with(*held.model, dynamic_cast<LoadedModel*>(held.model), params, ask, sink, cancel);
}

serve::Answer Engine::serve_with(ChatModel& model, LoadedModel* local, ModelParams params,
                                 const serve::Ask& ask, const serve::Sink& sink,
                                 const CancelCallback& cancel) {
    serve::Answer out;
    // What the request set, over the model's own.
    if (ask.temperature)    { params.temperature    = *ask.temperature; }
    if (ask.top_p)          { params.top_p          = *ask.top_p; }
    if (ask.top_k)          { params.top_k          = *ask.top_k; }
    if (ask.min_p)          { params.min_p          = *ask.min_p; }
    if (ask.repeat_penalty) { params.repeat_penalty = *ask.repeat_penalty; }
    if (ask.max_tokens)     { params.max_tokens     = *ask.max_tokens; }
    if (ask.seed)           { params.seed           = *ask.seed; }

    const int context   = model.context_size();
    const auto too_long = [&out, context](int asked) {
        out.status = 400;
        out.code   = "context_length_exceeded";
        out.error  = "This model's context is " + std::to_string(context) + " tokens, and the request is "
                   + std::to_string(asked);
        return out;
    };
    GenerationStats stats;

    if (ask.raw) {
        // A text completion is the prompt as it is, with no template: only a
        // model on this machine can be asked one.
        if (local == nullptr) {
            out.status = 400;
            out.error  = "This model takes chat completions only";
            return out;
        }
        const int asked = local->count_tokens(ask.prompt);
        if (asked >= context) {
            return too_long(asked);
        }
        sink.waiting("");
        stats = local->generate(ask.prompt, params, [&sink](std::string_view text) { sink.answer(text); }, cancel);
    } else {
        const std::vector<ChatMessage> messages =
            !ask.effort.empty() && model.takes_effort() ? with_effort(ask.messages, ask.effort) : ask.messages;
        // Measured before it is sent rather than cut to fit: a model that
        // silently loses the start of a conversation, its instructions with
        // it, answers some other question. This waits for whoever is
        // answering with the model, since it reads the model's template.
        const int asked = model.prompt_tokens(messages);
        if (asked >= context) {
            return too_long(asked);
        }
        sink.waiting("");

        // A thinking model's working goes to the reasoning, apart from the
        // answer, as llama-server sends it: in the model's own markers when
        // it is one on this machine, on a channel of its own otherwise.
        ResponseFilter filter;
        const auto     sort = [&sink](const ResponseFilter::Piece& piece) {
            if (!piece.reasoning.empty()) {
                sink.reasoning(piece.reasoning);
            }
            if (!piece.answer.empty()) {
                sink.answer(piece.answer);
            }
        };
        ChatSink chat_sink;
        chat_sink.cancel = cancel;
        if (model.reasons_inline()) {
            chat_sink.on_text = [&](std::string_view chunk) { sort(filter.feed(chunk)); };
        } else {
            chat_sink.on_text = [&sink](std::string_view chunk) { sink.answer(chunk); };
        }
        chat_sink.on_reasoning = [&sink](std::string_view chunk) { sink.reasoning(chunk); };
        const ChatResult result = model.chat(ChatRequest{messages, params, ask.effort}, chat_sink);
        sort(filter.flush());
        if (!result.error.empty()) {
            out.status = 502;
            out.error  = result.error;
        }
        stats = result.stats;
    }

    out.prompt_tokens     = stats.prompt_tokens;
    out.completion_tokens = stats.output_tokens;
    // At max_tokens, or at the end of the context, which ends a reply that
    // has more to say just the same.
    out.hit_limit = stats.hit_limit
                 || (!stats.canceled && stats.prompt_tokens + stats.output_tokens >= context - 1);
    // Cut off by Crucible closing rather than by the client or a stop
    // sequence, whose answers are whole as far as anybody asked.
    if (stats.canceled && !sink.cancel() && !running_.load(std::memory_order_relaxed)) {
        out.status = 503;
        out.error  = "Crucible is closing";
    }
    return out;
}

}  // namespace crucible
