// SPDX-License-Identifier: MIT
//
// The API server's chat and embeddings, for a model on this machine. See
// open_chat.hpp for what a conversation in OpenAI's shape is, and why it goes
// through the model's own template.
//
// The chat is llama-server's: the template renders the conversation and its
// tools, the parser that goes with it reads the answer back as it comes --
// reasoning, content, calls -- and the plan the template makes holds the
// answer to a grammar where it must be held. All three are llama.cpp's
// `common`, compiled in (see cmake/CrucibleDependencies.cmake). What is
// Crucible's is the rest: the sampling (sample_shaped), the context the
// prompt is read into, which is the one the chat keeps its cache in, and the
// pictures, which go through the projector as chat_with_pictures sends them.
#include "crucible/llm/loaded_model.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>

#include <common/chat.h>
#include <llama.h>

#if defined(CRUCIBLE_HAS_MTMD)
#include <mtmd.h>
#endif

#include "crucible/llm/response_filter.hpp"
#include "crucible/llm/sampling.hpp"
#include "crucible/util/format.hpp"
#include "crucible/util/text.hpp"

namespace crucible {
namespace {

using Clock = std::chrono::steady_clock;

double ms_since(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}

/// What a picture left out of the prompt is called in its place, as
/// chat_with_pictures calls one.
constexpr const char* kLeftOut = "[a picture from earlier, left out]";

/// How many texts embed() reads in one decode, each a sequence of its own.
constexpr int kEmbedAtOnce = 32;

/// A token's text. A special token is its text when `special`, and nothing
/// otherwise, as the chat's own sampling writes one.
std::string piece_of(const llama_vocab* vocab, llama_token token, bool special) {
    std::string piece(16, '\0');
    int written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, special);
    if (written < 0) {
        piece.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, special);
    }
    piece.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return piece;
}

/// `text` with every character a regular expression reads as more than
/// itself escaped: a trigger word, matched as the words it is.
std::string regex_escaped(const std::string& text) {
    std::string out;
    for (const char c : text) {
        if (c != '\0' && std::strchr(".^$|()*+?[]{}\\", c) != nullptr) {
            out += '\\';
        }
        out += c;
    }
    return out;
}

/// Whether `text` ends on a whole character, not partway through one.
bool whole_characters(const std::string& text) {
    std::string rest = text;
    detail::take_complete_utf8(rest);
    return rest.empty();
}

/// Whether `tag` ends somewhere in the last `added` bytes of `text`.
bool arrived(const std::string& text, std::size_t added, const std::string& tag) {
    if (tag.empty() || text.size() < tag.size()) {
        return false;
    }
    const std::size_t from = text.size() > added + tag.size() ? text.size() - added - tag.size() : 0;
    return text.find(tag, from) != std::string::npos;
}

/// A tool call's id as OpenAI writes them: "call_" and 24 letters.
std::string new_call_id() {
    static constexpr char kLetters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    thread_local std::mt19937_64 dice{std::random_device{}()};
    std::string id = "call_";
    for (int i = 0; i < 24; ++i) {
        id += kLetters[dice() % (sizeof(kLetters) - 1)];
    }
    return id;
}

/// What has been handed on of an answer, given the answer as the parser
/// reads it each time more of it comes: llama-server's deltas, worked out
/// the way it works them out.
class Handing {
public:
    explicit Handing(const OpenChatSink& sink) : sink_(sink) { shown_.role = "assistant"; }

    /// The answer so far is `now`: what is new of it goes to the sink.
    /// `last` once nothing more is coming.
    void update(common_chat_msg now, bool last) {
        now.set_tool_call_ids(ids_, new_call_id);
        std::vector<common_chat_msg_diff> diffs;
        try {
            diffs = common_chat_msg_diff::compute_diffs(shown_, now);
        } catch (const std::exception&) {
            return;   // a reading that takes back what was handed on: the next one will not
        }
        shown_ = std::move(now);
        for (common_chat_msg_diff& diff : diffs) {
            // A call's name goes out once, whole: held until its arguments
            // start or the answer ends, since a name read partway is a
            // prefix of what it will be, and a client puts the pieces of a
            // name together as it does the arguments.
            for (std::size_t i = 0; i < shown_.tool_calls.size(); ++i) {
                if (said_.count(i) == 0 && !shown_.tool_calls[i].name.empty()
                    && (diff.tool_call_index != i || !diff.tool_call_delta.arguments.empty())) {
                    begin(i, {});
                }
            }
            if (diff.tool_call_index == std::string::npos) {
                OpenChatDelta delta;
                delta.reasoning = std::move(diff.reasoning_content_delta);
                delta.content   = std::move(diff.content_delta);
                if (!delta.reasoning.empty() || !delta.content.empty()) {
                    give(delta);
                }
                continue;
            }
            const std::size_t i = diff.tool_call_index;
            if (said_.count(i) != 0) {
                if (!diff.tool_call_delta.arguments.empty()) {
                    OpenChatDelta delta;
                    delta.call           = static_cast<int>(i);
                    delta.tool.arguments = std::move(diff.tool_call_delta.arguments);
                    give(delta);
                }
            } else if (!diff.tool_call_delta.arguments.empty() || last) {
                begin(i, std::move(diff.tool_call_delta.arguments));
            }
        }
        if (last) {
            for (std::size_t i = 0; i < shown_.tool_calls.size(); ++i) {
                if (said_.count(i) == 0 && !shown_.tool_calls[i].name.empty()) {
                    begin(i, {});
                }
            }
        }
    }

    const common_chat_msg& shown() const { return shown_; }

private:
    void begin(std::size_t i, std::string arguments) {
        said_.insert(i);
        OpenChatDelta delta;
        delta.call           = static_cast<int>(i);
        delta.tool.id        = shown_.tool_calls[i].id;
        delta.tool.name      = shown_.tool_calls[i].name;
        delta.tool.arguments = std::move(arguments);
        give(delta);
    }

    void give(const OpenChatDelta& delta) const {
        if (sink_.on_delta) {
            sink_.on_delta(delta);
        }
    }

    const OpenChatSink&      sink_;
    common_chat_msg          shown_;
    std::vector<std::string> ids_;     ///< each call's id, by its place in the answer
    std::set<std::size_t>    said_;    ///< the calls whose id and name have gone out
};

}  // namespace

const common_chat_templates* LoadedModel::chat_templates(std::string& error) {
    if (!templates_tried_) {
        templates_tried_ = true;
        try {
            common_chat_templates_ptr made = common_chat_templates_init(model_, "");
            templates_ = std::shared_ptr<common_chat_templates>(made.release(), common_chat_templates_free);
        } catch (const std::exception& e) {
            templates_error_ = e.what();
        }
    }
    error = templates_error_;
    return templates_.get();
}

OpenChatResult LoadedModel::open_chat(const OpenChat& request, const OpenChatSink& sink) {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    OpenChatResult result;
    const auto refused = [&result](std::string why) {
        result.error   = std::move(why);
        result.refused = true;
        return result;
    };
    if (!request.images.empty() && !sees_images()) {
        return refused("This model reads no pictures: no projector (an mmproj file) was found beside it");
    }
    std::string why;
    const common_chat_templates* templates = chat_templates(why);
    if (templates == nullptr) {
        return refused("This model's chat template could not be read: " + why);
    }
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // What the request asks, read as llama-server reads it.
    common_chat_templates_inputs inputs;
    try {
        // Tools the model may not call are not shown it: a small model shown
        // a tool calls it, whatever it was told, and the call would come back
        // as words.
        if (!request.tools.is_null() && request.tool_choice != "none") {
            inputs.tools = common_chat_tools_parse_oaicompat(common_json::parse(request.tools.dump()));
        }
        inputs.tool_choice = common_chat_tool_choice_parse_oaicompat(request.tool_choice);
        const std::map<std::string, bool> caps = common_chat_templates_get_caps(templates);
        const auto parallel = caps.find("supports_parallel_tool_calls");
        inputs.parallel_tool_calls =
            request.parallel_tool_calls.value_or(parallel != caps.end() && parallel->second);
    } catch (const std::exception& e) {
        return refused(e.what());
    }
    inputs.json_schema      = request.schema;
    inputs.reasoning_format = COMMON_REASONING_FORMAT_AUTO;
    if (request.effort == "none") {
        inputs.enable_thinking = false;
    } else if (!request.effort.empty()) {
        // The template's own variable, which gpt-oss's writes into its
        // system message.
        inputs.chat_template_kwargs["reasoning_effort"] = nlohmann::json(request.effort).dump();
    }

    // The pictures' bytes, and which message each came with: a picture's
    // place is its part of the message, in the order the parts come.
    std::vector<std::string> blobs;
    std::vector<std::size_t> owners;
    for (const ChatImage& image : request.images) {
        std::string bytes;
        if (!format::from_base64(image.data, bytes) || bytes.empty()) {
            return refused("A picture is not base64");
        }
        blobs.push_back(std::move(bytes));
    }
    for (std::size_t m = 0; m < request.messages.size(); ++m) {
        const auto content = request.messages[m].find("content");
        if (content == request.messages[m].end() || !content->is_array()) {
            continue;
        }
        for (const nlohmann::json& part : *content) {
            if (part.is_object() && part.value("type", std::string()) == "media_marker") {
                owners.push_back(m);
            }
        }
    }
    if (owners.size() != blobs.size()) {
        return refused("The pictures and their places in the messages do not agree");
    }
#if defined(CRUCIBLE_HAS_MTMD)
    const std::string marker = blobs.empty() ? std::string() : std::string(mtmd_default_marker());
#else
    const std::string marker;
#endif

    // The conversation, rendered: a marker where each picture from `first`
    // on goes, and a word in place of each one before it. Rendered again for
    // each `first` read_pictures tries, and the session of the last one kept
    // to read the answer with -- the parser and the grammar are the same for
    // all of them.
    std::optional<common_chat_session> session;
    std::string                        failed;
    const auto render = [&](std::size_t first) -> std::string {
        nlohmann::json messages = request.messages;
        std::size_t    picture  = 0;
        for (nlohmann::json& message : messages) {
            const auto content = message.find("content");
            if (content == message.end() || !content->is_array()) {
                continue;
            }
            for (nlohmann::json& part : *content) {
                if (!part.is_object() || part.value("type", std::string()) != "media_marker") {
                    continue;
                }
                part = picture >= first ? nlohmann::json{{"type", "media_marker"}, {"text", marker}}
                                        : nlohmann::json{{"type", "text"}, {"text", kLeftOut}};
                ++picture;
            }
        }
        try {
            inputs.messages = common_chat_msgs_parse_oaicompat(
                common_json::parse(messages.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace)));
            session.emplace(templates, vocab, inputs);
            return session->prompt().str();
        } catch (const std::exception& e) {
            failed = e.what();
            session.reset();
            return {};
        }
    };

    const detail::Aborting aborting(ctx_, sink.cancel);
    const int              context = context_size();
    if (blobs.empty()) {
        const std::string prompt = render(0);
        if (!session) {
            return refused(failed);
        }
        // Measured before it is read rather than cut to fit: a model that
        // loses the start of a conversation, its instructions and its tools
        // with it, answers some other question.
        const int asked = count_tokens(prompt);
        if (asked >= context) {
            result.too_long = asked;
            result.error    = "This model's context is " + std::to_string(context) + " tokens, and the request is "
                            + std::to_string(asked);
            return result;
        }
        if (sink.on_start) {
            sink.on_start();
        }
        if (!read_prompt(prompt, request.params, sink.cancel, result.stats)) {
            if (!result.stats.canceled) {
                result.error = "the model could not read the prompt";
            }
            return result;
        }
    } else {
        // Rendered once before the pictures are, so that a conversation the
        // template will not take is refused as one.
        if (render(0).empty() && !session) {
            return refused(failed);
        }
        if (sink.on_start) {
            sink.on_start();
        }
        const std::string error = read_pictures(blobs, owners, render, request.params, sink.cancel, result.stats);
        if (!session) {
            return refused(failed.empty() ? error : failed);
        }
        if (!error.empty()) {
            return refused(error);
        }
        if (result.stats.canceled) {
            return result;
        }
    }

    // How the answer is held and read, from the template's plan.
    Shaping shaping;
    {
        common_params_sampling sampling;
        session->apply_sampling(sampling);
        shaping.grammar = common_grammar_value(sampling.grammar);
        shaping.lazy    = sampling.grammar_lazy;
        for (const common_grammar_trigger& trigger : sampling.grammar_triggers) {
            switch (trigger.type) {
            case COMMON_GRAMMAR_TRIGGER_TYPE_WORD:
                shaping.patterns.push_back(regex_escaped(trigger.value));
                break;
            case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN:
                shaping.patterns.push_back(trigger.value);
                break;
            case COMMON_GRAMMAR_TRIGGER_TYPE_PATTERN_FULL:
                shaping.patterns.push_back((trigger.value.empty() || trigger.value.front() != '^' ? "^" : "")
                                           + trigger.value
                                           + (trigger.value.empty() || trigger.value.back() != '$' ? "$" : ""));
                break;
            case COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN:
                shaping.triggers.push_back(trigger.token);
                break;
            }
        }
        shaping.shown.assign(sampling.preserved_tokens.begin(), sampling.preserved_tokens.end());
    }
    const std::string& opening = session->generation_prompt();
    if (!shaping.grammar.empty() && !shaping.lazy) {
        // A grammar from the first token describes the reply from its
        // opening, which the template has already written: the grammar reads
        // that first, as llama-server's sampler has it read it.
        const std::vector<llama_token> written = common_tokenize(vocab, opening, false, true);
        for (std::size_t i = 0; i < written.size(); ++i) {
            const std::string piece = piece_of(vocab, written[i], true);
            if (i == 0 && !piece.empty() && !opening.empty() && std::isspace(static_cast<unsigned char>(piece[0]))
                && !std::isspace(static_cast<unsigned char>(opening[0]))) {
                continue;   // a space some tokenizers put before a special token
            }
            shaping.prefill.push_back(written[i]);
        }
    }
    shaping.think_open  = session->thinking_start_tag();
    shaping.think_close = session->thinking_end_tags();
    if (!shaping.think_open.empty()) {
        // The opening may leave the model inside its thinking already.
        const std::size_t open = opening.rfind(shaping.think_open);
        if (open != std::string::npos) {
            shaping.thinking = std::none_of(shaping.think_close.begin(), shaping.think_close.end(),
                                            [&](const std::string& close) {
                                                return !close.empty()
                                                    && opening.find(close, open + shaping.think_open.size())
                                                           != std::string::npos;
                                            });
        }
    }

    // The answer, read as it comes. A piece is read once it ends on a whole
    // character; the template's own stops end the answer before them.
    Handing                        handing(sink);
    const std::vector<std::string> stops = session->additional_stops();
    common_chat_input              pending;
    std::string                    said;   ///< all of the answer, as the model wrote it
    shaping.on_piece = [&](std::string_view piece, llama_token token) {
        pending.append(std::string(piece), token);
        if (!whole_characters(pending.text)) {
            return true;
        }
        bool              more   = true;
        const std::size_t before = said.size();
        said += pending.text;
        std::size_t first = std::string::npos;
        for (const std::string& stop : stops) {
            if (!stop.empty()) {
                first = std::min(first, said.find(stop, before > stop.size() ? before - stop.size() : 0));
            }
        }
        if (first != std::string::npos) {
            more = false;
            pending.truncate(first > before ? first - before : 0);
            said.resize(std::max(first, before));
        }
        try {
            handing.update(session->feed(pending), false);
        } catch (const std::exception&) {
            // Not readable yet: read again with what comes next.
        }
        pending = common_chat_input();
        return more;
    };
    const std::string ended = sample_shaped(request.params, shaping, sink.cancel, result.stats);
    said += pending.text;
    try {
        handing.update(session->finish(pending), true);
    } catch (const std::exception&) {
        // The end of it is in no shape the parser knows -- cut off by
        // max_tokens partway through a call, say. What was handed on stands,
        // and an answer none of which could be read is sorted the way the
        // chat sorts one, its thinking from its words, rather than lost.
        const common_chat_msg& shown = handing.shown();
        if (shown.content.empty() && shown.reasoning_content.empty() && shown.tool_calls.empty()) {
            ResponseFilter          filter;
            ResponseFilter::Piece   sorted = filter.feed(said);
            const ResponseFilter::Piece rest   = filter.flush();
            common_chat_msg         whole;
            whole.role              = "assistant";
            whole.reasoning_content = sorted.reasoning + rest.reasoning;
            whole.content           = sorted.answer + rest.answer;
            handing.update(whole, true);
        } else {
            handing.update(shown, true);
        }
    }
    if (!blobs.empty()) {
        // What the context now holds starts with pictures, which no later
        // prompt of text can share: the next one starts over too.
        forget();
    }
    for (const common_chat_tool_call& call : handing.shown().tool_calls) {
        result.calls.push_back(ToolCall{call.id, call.name, call.arguments});
    }
    if (!ended.empty() && !result.stats.canceled) {
        result.error = ended;
    }
    return result;
}

std::string LoadedModel::sample_shaped(const ModelParams& params, const Shaping& shaping, const CancelCallback& cancel,
                                       GenerationStats& stats) {
    const llama_vocab* vocab        = llama_model_get_vocab(model_);
    const int          context_size = static_cast<int>(llama_n_ctx(ctx_));
    const int          n_vocab      = llama_vocab_n_tokens(vocab);

    struct Freed {
        llama_sampler* sampler;
        ~Freed() { llama_sampler_free(sampler); }
    };
    const Freed chain{llm::build_sampler_chain(vocab, params, {})};
    const Freed grammar{llm::build_grammar_sampler(vocab, shaping.grammar, shaping.lazy, shaping.patterns,
                                                   shaping.triggers)};
    if (!shaping.grammar.empty() && grammar.sampler == nullptr) {
        return "the grammar for this answer would not parse";
    }
    if (grammar.sampler != nullptr && !shaping.lazy) {
        try {
            for (const llama_token token : shaping.prefill) {
                llama_sampler_accept(grammar.sampler, token);
            }
        } catch (const std::exception& e) {
            return std::string("the grammar for this answer does not take the template's opening: ") + e.what();
        }
    }

    const std::set<llama_token> shown(shaping.shown.begin(), shaping.shown.end());
    bool        thinking = shaping.thinking;
    bool        thought  = false;   ///< a thinking block has closed; there is no second
    std::string recent;             ///< the end of the answer, where a block's markers are looked for

    std::vector<llama_token_data> candidates(static_cast<std::size_t>(std::max(0, n_vocab)));
    const auto  output_start = Clock::now();
    const int   limit        = params.max_tokens > 0 ? params.max_tokens : context_size;
    std::string ended;
    for (int produced = 0; produced < limit; ++produced) {
        if (cancel && cancel()) {
            stats.canceled = true;
            break;
        }
        const float* logits = llama_get_logits_ith(ctx_, -1);
        if (logits == nullptr) {
            ended = "the model gave nothing to sample from";
            break;
        }
        constexpr float kBarred = -std::numeric_limits<float>::infinity();
        const auto      fill    = [&] {
            for (int i = 0; i < n_vocab; ++i) {
                candidates[static_cast<std::size_t>(i)] = llama_token_data{i, logits[i], 0.0F};
            }
            return llama_token_data_array{candidates.data(), candidates.size(), -1, false};
        };
        llama_token_data_array choice = fill();
        llama_sampler_apply(chain.sampler, &choice);
        if (choice.selected < 0 || static_cast<std::size_t>(choice.selected) >= choice.size) {
            ended = "the model's sampling chose nothing";
            break;
        }
        llama_token token = choice.data[choice.selected].id;

        // Held to the grammar -- but not inside a thinking block, for a
        // grammar that waits for a call. The chain's pick is asked of the
        // grammar alone, and stands when it is allowed, as most are: the
        // grammar read over every candidate of a vocabulary of 200,000 took
        // most of the time a token takes, and JSON came at a third of the
        // speed. One it does not allow and the grammar goes first after all,
        // over every candidate before anything truncates them, so the only
        // tokens it allows are never the ones thrown away -- the order
        // build_sampler_chain puts it in -- and the chain picks again.
        const bool held = grammar.sampler != nullptr && !(shaping.lazy && thinking);
        if (held) {
            llama_token_data       alone{token, 1.0F, 0.0F};
            llama_token_data_array asked{&alone, 1, -1, false};
            llama_sampler_apply(grammar.sampler, &asked);
            if (alone.logit == kBarred) {
                choice = fill();
                llama_sampler_apply(grammar.sampler, &choice);
                if (std::none_of(choice.data, choice.data + choice.size,
                                 [](const llama_token_data& one) { return one.logit != kBarred; })) {
                    ended = "the answer's grammar left the model nothing it could say";
                    break;
                }
                llama_sampler_apply(chain.sampler, &choice);
                if (choice.selected < 0 || static_cast<std::size_t>(choice.selected) >= choice.size) {
                    ended = "the model's sampling chose nothing";
                    break;
                }
                token = choice.data[choice.selected].id;
            }
        }
        if (llama_vocab_is_eog(vocab, token)) {
            break;
        }
        llama_sampler_accept(chain.sampler, token);
        if (held) {
            try {
                llama_sampler_accept(grammar.sampler, token);
            } catch (const std::exception& e) {
                ended = std::string("the answer left its grammar: ") + e.what();
                break;
            }
        }
        const std::string piece = piece_of(vocab, token, shown.count(token) != 0);

        // A thinking block opening, and closing: once it has, whatever
        // closed it is read by the grammar, as llama-server's sampler replays
        // it, for a call may begin with it.
        if (grammar.sampler != nullptr && shaping.lazy && !shaping.think_open.empty()) {
            recent += piece;
            if (recent.size() > 512) {
                recent.erase(0, recent.size() - 256);
            }
            if (!thinking && !thought && arrived(recent, piece.size(), shaping.think_open)) {
                thinking = true;
            } else if (thinking) {
                for (const std::string& close : shaping.think_close) {
                    if (!arrived(recent, piece.size(), close)) {
                        continue;
                    }
                    thinking = false;
                    thought  = true;
                    try {
                        for (const llama_token closing : common_tokenize(vocab, close, false, true)) {
                            llama_sampler_accept(grammar.sampler, closing);
                        }
                    } catch (const std::exception&) {
                        // A grammar that cannot read it holds what follows the same.
                    }
                    break;
                }
            }
        }

        ++stats.output_tokens;
        if (produced + 1 >= limit) {
            stats.hit_limit = true;
        }
        if (shaping.on_piece && !shaping.on_piece(piece, token)) {
            break;   // a stop: the token is not read into the cache
        }
        llama_token next  = token;
        llama_batch batch = llama_batch_get_one(&next, 1);
        if (llama_decode(ctx_, batch) != 0) {
            stats.canceled = cancel && cancel();
            forget();
            break;
        }
        cached_.push_back(next);
    }
    stats.output_ms = ms_since(output_start);
    return ended;
}

Embedded LoadedModel::embed(const std::vector<std::string>& texts, const CancelCallback& cancel) {
    const std::lock_guard<std::recursive_mutex> lock(use_);
    Embedded           out;
    const llama_vocab* vocab = llama_model_get_vocab(model_);

    // Each text with the tokens the model's tokenizer puts around one -- an
    // embedding model's [CLS] and [SEP], or the end of text a model that
    // pools its last token reads from.
    std::vector<std::vector<llama_token>> tokens;
    std::size_t                           longest = 0;
    for (const std::string& text : texts) {
        std::vector<llama_token> read = common_tokenize(vocab, text, true, true);
        if (read.empty()) {
            out.error = "An input is empty";
            return out;
        }
        longest = std::max(longest, read.size());
        out.tokens += static_cast<int>(read.size());
        tokens.push_back(std::move(read));
    }
    const int trained = llama_model_n_ctx_train(model_);
    const int most    = trained > 0 ? trained : 8192;
    if (static_cast<int>(longest) > most) {
        out.too_long = static_cast<int>(longest);
        out.error    = "This model reads " + std::to_string(most) + " tokens at once, and an input is "
                     + std::to_string(longest);
        return out;
    }

    // A context of its own, as long as the longest text asks, made again
    // only for a longer one. A whole text in one batch: a model that reads
    // both ways reads all of it at once or not at all.
    if (embedder_ == nullptr || embedder_ctx_ < static_cast<int>(longest)) {
        if (embedder_ != nullptr) {
            llama_free(embedder_);
            embedder_ = nullptr;
        }
        int n_ctx = 512;
        while (n_ctx < static_cast<int>(longest)) {
            n_ctx *= 2;
        }
        n_ctx = std::max(static_cast<int>(longest), std::min(n_ctx, most));
        llama_context_params made = llama_context_default_params();
        made.n_ctx           = static_cast<std::uint32_t>(n_ctx);
        made.n_batch         = made.n_ctx;
        made.n_ubatch        = made.n_ctx;
        made.n_seq_max       = static_cast<std::uint32_t>(
            std::min<std::size_t>(kEmbedAtOnce, llama_max_parallel_sequences()));
        made.kv_unified      = true;
        made.embeddings      = true;
        made.pooling_type    = LLAMA_POOLING_TYPE_UNSPECIFIED;
        made.n_threads       = llama_n_threads(ctx_);
        made.n_threads_batch = llama_n_threads_batch(ctx_);
        embedder_ = llama_init_from_model(model_, made);
        if (embedder_ != nullptr && llama_pooling_type(embedder_) == LLAMA_POOLING_TYPE_NONE) {
            // A model that names no pooling -- a chat model, not an
            // embedding model -- has its hidden states averaged over the text.
            llama_free(embedder_);
            made.pooling_type = LLAMA_POOLING_TYPE_MEAN;
            embedder_         = llama_init_from_model(model_, made);
        }
        if (embedder_ == nullptr) {
            out.error = "There is no room for a context to make embeddings in";
            return out;
        }
        embedder_ctx_ = n_ctx;
    }
    if (llama_pooling_type(embedder_) == LLAMA_POOLING_TYPE_RANK) {
        out.error = "This model ranks texts rather than embedding them";
        return out;
    }

    const int n_embd = llama_model_n_embd_out(model_) > 0 ? llama_model_n_embd_out(model_) : llama_model_n_embd(model_);
    const std::size_t at_once = std::max<std::size_t>(1, llama_n_seq_max(embedder_));
    llama_batch batch = llama_batch_init(embedder_ctx_, 0, 1);
    struct BatchGuard {
        llama_batch& batch;
        ~BatchGuard() { llama_batch_free(batch); }
    } batch_guard{batch};
    const detail::Aborting aborting(embedder_, cancel);

    out.vectors.resize(texts.size());
    std::size_t next = 0;
    while (next < tokens.size()) {
        if (cancel && cancel()) {
            out.vectors.clear();
            out.error = "stopped";
            return out;
        }
        // As many texts as fit in one batch, each a sequence of its own.
        batch.n_tokens          = 0;
        const std::size_t first = next;
        while (next < tokens.size() && next - first < at_once
               && batch.n_tokens + static_cast<int>(tokens[next].size()) <= embedder_ctx_) {
            const auto sequence = static_cast<llama_seq_id>(next - first);
            for (std::size_t t = 0; t < tokens[next].size(); ++t) {
                const int at        = batch.n_tokens++;
                batch.token[at]     = tokens[next][t];
                batch.pos[at]       = static_cast<llama_pos>(t);
                batch.n_seq_id[at]  = 1;
                batch.seq_id[at][0] = sequence;
                batch.logits[at]    = 1;
            }
            ++next;
        }
        if (llama_memory_t memory = llama_get_memory(embedder_); memory != nullptr) {
            llama_memory_clear(memory, true);
        }
        if (llama_decode(embedder_, batch) != 0) {
            out.vectors.clear();
            out.error = cancel && cancel() ? "stopped" : "The model could not read the input";
            return out;
        }
        for (std::size_t i = first; i < next; ++i) {
            const float* values = llama_get_embeddings_seq(embedder_, static_cast<llama_seq_id>(i - first));
            if (values == nullptr) {
                out.vectors.clear();
                out.error = "The model gave no embedding";
                return out;
            }
            // Unit length, as OpenAI's are: a dot product is then the cosine
            // every client takes it for.
            double sum = 0.0;
            for (int d = 0; d < n_embd; ++d) {
                sum += static_cast<double>(values[d]) * static_cast<double>(values[d]);
            }
            const double scale = sum > 0.0 ? 1.0 / std::sqrt(sum) : 0.0;
            std::vector<float>& vector = out.vectors[i];
            vector.resize(static_cast<std::size_t>(n_embd));
            for (int d = 0; d < n_embd; ++d) {
                vector[static_cast<std::size_t>(d)] = static_cast<float>(static_cast<double>(values[d]) * scale);
            }
        }
    }
    return out;
}

}  // namespace crucible
