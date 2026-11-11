// SPDX-License-Identifier: MIT
//
// What the API server answers with. See serve/server.hpp for the server.
//
// The delegator, one model on this machine, or a provider's model, held the
// way a seat holds one. A model from the models folder goes through
// hold_model like any expert: the host plans its memory, its load holds the
// runtimes still, a lease keeps it while a request has it, and between
// requests it is nobody's, so Eject and a seat that needs the room free it
// like anything else not in use. The delegator is the one the chat routes
// with, answered with under the models' lock as delegator.generate is: it is
// never freed under a request, and a route waits its turn behind one rather
// than the other way round. It answers in a context of its own (see
// ModelHost::router_for_serving), so routing keeps the prompts it has read.
//
// A model on this machine answers in OpenAI's own shape -- tools, calls and
// their results, a schema for the answer, pictures -- through its own chat
// template (see LoadedModel::open_chat). A provider's model and an MLX one
// are asked as the chat asks them: text and pictures.
//
// The chat and a build go on all the while. A request waits for whoever has
// the model, and says what it waits for; nothing the app is doing waits on a
// request longer than the model takes to answer it.
#include "crucible/engine/engine.hpp"

#include <algorithm>
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

/// Whether the conversation has a call in it, or a call's result: a turn
/// only a chat template can write.
bool has_tool_turns(const nlohmann::json& chat) {
    if (!chat.is_array()) {
        return false;
    }
    for (const nlohmann::json& message : chat) {
        if (message.value("role", std::string()) == "tool" || message.contains("tool_calls")) {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string Engine::served_file(const Config& config) {
    if (!config.server.provider.empty()) {
        return {};   // a provider's model: nothing here to load
    }
    if (config.server.model.empty()) {
        return config.router.model.empty() ? std::string() : config.router.path;
    }
    return resolve_model_ref(config.resolved_models_dir(), config.server.model).string();
}

std::string Engine::served_id(const Config& config) {
    if (!config.server.provider.empty()) {
        return config.server.model;
    }
    const std::string file = served_file(config);
    return file.empty() ? std::string() : std::filesystem::path(file).filename().string();
}

std::string Engine::embedder_file(const Config& config) {
    return config.server.embeddings.empty()
             ? std::string()
             : resolve_model_ref(config.resolved_models_dir(), config.server.embeddings).string();
}

std::string Engine::embedder_id(const Config& config) {
    const std::string file = embedder_file(config);
    return file.empty() ? served_id(config) : std::filesystem::path(file).filename().string();
}

ModelParams Engine::file_params(const Config& config, const std::string& file, ExpertId* seat) {
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
    params.model = std::filesystem::path(file).filename().string();
    params.path  = file;
    params.provider.clear();
    return params;
}

ModelParams Engine::served_params(const Config& config, ExpertId* seat) {
    if (!config.server.provider.empty()) {
        if (seat != nullptr) {
            *seat = kServerSeat;
        }
        ModelParams params = config.defaults;
        params.model    = config.server.model;
        params.provider = config.server.provider;
        params.path.clear();
        return params;
    }
    ExpertId    found;
    ModelParams params = file_params(config, served_file(config), &found);
    if (found == kServerSeat) {
        params.model = config.server.model;
    }
    if (seat != nullptr) {
        *seat = found;
    }
    return params;
}

serve::Status Engine::server_status() const { return server_->status(); }

void Engine::configure_server(const Config& config) {
    serve::Settings settings;
    settings.enabled = config.server.enabled && running_.load(std::memory_order_relaxed);
    settings.port    = config.server.port;
    // A provider's model is the person's credit spent with every request:
    // other computers are let at it only with a key, and without one the
    // server listens on this machine alone. Settings says so where the
    // model is chosen.
    settings.network = config.server.network && (config.server.provider.empty() || !config.server.api_key.empty());
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
    if (ask.embed) {
        return serve_embeddings(ask, sink, config, cancel);
    }
    if (served_id(config) != ask.model) {
        out.status = 404;
        out.code   = "model_not_found";
        out.error  = "The model `" + ask.model + "` is no longer served";
        return out;
    }

    // --- a provider's model ------------------------------------------------------
    if (!config.server.provider.empty()) {
        const ModelParams params = served_params(config);
        std::string       error;
        ChatModel*        remote = hub_.model(params, error);
        if (remote == nullptr) {
            out.status = 503;
            out.error  = error.empty() ? "The provider " + params.provider + " is not set up" : error;
            return out;
        }
        return serve_with(*remote, nullptr, params, ask, sink, cancel);
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
        // In a context of its own, as long as a model from the folder is
        // given where the cards hold it: routing's is a few thousand tokens,
        // and a client's conversation would push out the prompts it keeps.
        LoadedModel* model = host_->router_for_serving(config.defaults.n_ctx);
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
    HeldModel         held   = hold_served(params, holder, sink, cancel, out);
    if (held.model == nullptr) {
        return out;
    }
    return serve_with(*held.model, dynamic_cast<LoadedModel*>(held.model), params, ask, sink, cancel);
}

Engine::HeldModel Engine::hold_served(const ModelParams& params, const ExpertId& holder, const serve::Sink& sink,
                                      const CancelCallback& cancel, serve::Answer& out) {
    const std::string name = std::filesystem::path(params.path).filename().string();
    std::error_code   missing;
    if (!std::filesystem::exists(params.path, missing)) {
        out.status = 503;
        out.error  = name + " is not there";
        return {};
    }
    // With the delegator on demand it steps aside for a model that is not in
    // memory yet and needs its room, as it does for any expert; for one that
    // is, both are already in, and freeing it would be a load for the next
    // prompt and a saving for nobody.
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
        out.status = 503;
        out.error  = "Crucible is closing";
        return {};
    }
    if (!resident) {
        // Said only when the delegator is to go and a route is using it: then
        // the route is waited for.
        make_room_for(params, [&sink] { sink.waiting("the chat"); });
        sink.waiting("loading");
    }
    long        load_ms = 0;
    std::string error;
    HeldModel   held = hold_model(holder, params, holder == kServerSeat ? name : expert_label(config().roster, holder),
                                  cancel, load_ms, error, [&sink] { sink.waiting("memory"); });
    if (held.model == nullptr) {
        out.status = 503;
        out.error  = cancel() || error == "stopped" ? std::string("Crucible is closing")
                                                    : name + " did not load" + (error.empty() ? std::string() : ": " + error);
        return held;
    }
    if (!is_busy() && !cooking() && !resident) {
        state_.set_mood(Mood::Idle);   // the load said so on the status line, and is over
    }
    // Somebody else holding it is somebody answering with it: the model takes
    // one caller at a time, and this one is next.
    if (leases_.count(params.path) > 1) {
        sink.waiting(serve_blocker());
    }
    return held;
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
    const bool pictures = std::any_of(ask.messages.begin(), ask.messages.end(),
                                      [](const ChatMessage& message) { return !message.images.empty(); });
    // What only a chat template can write: tools the model may call, the
    // calls and results of a conversation, an answer held to a schema.
    const bool offered  = !ask.tools.is_null() && ask.tool_choice != "none";
    const bool templated = offered || has_tool_turns(ask.chat) || !ask.schema.empty();
    GenerationStats stats;
    bool            answered = false;   ///< by the model's own template

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
        stats    = local->generate(ask.prompt, params, [&sink](std::string_view text) { sink.answer(text); }, cancel);
        answered = true;
    } else if (local != nullptr) {
        // OpenAI's shape, through the model's own template: the tools and the
        // calls in it written as the model was trained to read them, and the
        // answer read back into its reasoning, its content and its calls.
        OpenChat chat;
        chat.messages = ask.chat;
        for (const ChatMessage& message : ask.messages) {
            chat.images.insert(chat.images.end(), message.images.begin(), message.images.end());
        }
        chat.tools               = ask.tools;
        chat.tool_choice         = ask.tool_choice;
        chat.parallel_tool_calls = ask.parallel_tool_calls;
        chat.schema              = ask.schema;
        chat.effort              = ask.effort;
        chat.params              = params;
        OpenChatSink open;
        open.cancel   = cancel;
        open.on_start = [&sink] { sink.waiting(""); };
        open.on_delta = [&sink](const OpenChatDelta& delta) {
            if (!delta.reasoning.empty()) {
                sink.reasoning(delta.reasoning);
            }
            if (!delta.content.empty()) {
                sink.answer(delta.content);
            }
            if (delta.call >= 0 && sink.call) {
                sink.call(serve::CallPiece{delta.call, delta.tool.id, delta.tool.name, delta.tool.arguments});
            }
        };
        const OpenChatResult result = local->open_chat(chat, open);
        if (result.too_long > 0) {
            return too_long(result.too_long);
        }
        // A template that cannot render a conversation of plain turns -- a
        // Jinja this one cannot read, or one refusing a system message --
        // still leaves the model the chat's own way of being asked, which
        // is how every request was put to it before there were tools.
        answered = !(result.untemplated && !templated);
        if (answered && !result.error.empty()) {
            out.status = result.refused ? 400 : 500;
            out.error  = result.error;
        }
        stats = result.stats;
    }
    if (!answered) {
        // A provider's model, an MLX one, or one whose template would not
        // render the conversation, asked as the chat asks it: what only a
        // chat template can write is refused rather than dropped, for an
        // answer to a question it never saw is no answer.
        if (templated) {
            out.status = 400;
            out.error  = std::string(offered || has_tool_turns(ask.chat) ? "Tools need" : "A JSON response_format needs")
                       + " a GGUF model on this machine, and this server's is "
                       + (params.remote() ? "a provider's" : "an MLX model");
            return out;
        }
        if (pictures && !model.sees_images()) {
            out.status = 400;
            out.error  = "This model reads no pictures";
            return out;
        }
        const std::vector<ChatMessage> messages =
            !ask.effort.empty() && ask.effort != "none" && model.takes_effort() ? with_effort(ask.messages, ask.effort)
                                                                              : ask.messages;
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
        const std::string_view effort = ask.effort == "none" ? std::string_view() : std::string_view(ask.effort);
        const ChatResult result = model.chat(ChatRequest{messages, params, effort}, chat_sink);
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

serve::Answer Engine::serve_embeddings(const serve::Ask& ask, const serve::Sink& sink, const Config& config,
                                       const CancelCallback& cancel) {
    serve::Answer out;
    const auto    embed_with = [&](LoadedModel& model) {
        sink.waiting("");
        Embedded made = model.embed(ask.texts, cancel);
        if (made.too_long > 0) {
            out.status = 400;
            out.code   = "context_length_exceeded";
            out.error  = made.error;
        } else if (made.error == "stopped") {
            out.status = 503;
            out.error  = "Crucible is closing";
        } else if (!made.error.empty()) {
            out.status = made.error == "An input is empty" ? 400 : 500;
            out.error  = made.error;
        }
        out.vectors       = std::move(made.vectors);
        out.prompt_tokens = made.tokens;
        return out;
    };

    // The model chosen for embeddings, when one is and it is the one asked.
    const std::string apart = embedder_file(config);
    if (!apart.empty() && ask.model == std::filesystem::path(apart).filename().string()) {
        ExpertId          holder;
        const ModelParams params = file_params(config, apart, &holder);
        HeldModel         held   = hold_served(params, holder, sink, cancel, out);
        if (held.model == nullptr) {
            return out;
        }
        auto* local = dynamic_cast<LoadedModel*>(held.model);
        if (local == nullptr) {
            out.status = 400;
            out.error  = "An MLX model makes no embeddings here: choose a GGUF for them";
            return out;
        }
        return embed_with(*local);
    }
    if (ask.model != served_id(config)) {
        out.status = 404;
        out.code   = "model_not_found";
        out.error  = "The model `" + ask.model + "` is no longer served";
        return out;
    }
    if (!config.server.provider.empty()) {
        out.status = 400;
        out.error  = "A provider's model makes no embeddings here: choose a model for them in Settings, Server";
        return out;
    }
    if (config.server.model.empty()) {
        // The delegator's, under the models' lock as its answers are.
        std::unique_lock<std::mutex> host(host_mutex_, std::try_to_lock);
        if (!host.owns_lock()) {
            sink.waiting(serve_blocker());
            host.lock();
        }
        if (host_->router() == nullptr) {
            sink.waiting("loading");
            ensure_router(cancel);
            if (!is_busy() && !cooking()) {
                state_.set_mood(Mood::Idle);
            }
        }
        LoadedModel* model = host_->router();
        if (model == nullptr) {
            out.status = 503;
            out.error  = cancel() ? "Crucible is closing" : "The delegator did not load";
            return out;
        }
        return embed_with(*model);
    }
    ExpertId          holder;
    const ModelParams params = served_params(config, &holder);
    HeldModel         held   = hold_served(params, holder, sink, cancel, out);
    if (held.model == nullptr) {
        return out;
    }
    auto* local = dynamic_cast<LoadedModel*>(held.model);
    if (local == nullptr) {
        out.status = 400;
        out.error  = "An MLX model makes no embeddings here: choose a GGUF for them in Settings, Server";
        return out;
    }
    return embed_with(*local);
}

}  // namespace crucible
