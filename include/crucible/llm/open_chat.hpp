// SPDX-License-Identifier: MIT
//
// A conversation as another program sends one to the API server, in OpenAI's
// shape: tools offered, calls made and their results sent back, pictures, an
// answer held to a schema.
//
// A model on this machine answers one through its own chat template (see
// LoadedModel::open_chat), which is what makes tools work at all: the
// template writes them into the prompt the way the model was trained to read
// them -- gpt-oss as a TypeScript namespace, Qwen as JSON between tags, LFM
// as a list in the system message -- and every family writes its calls back
// in a shape of its own, which the parser that goes with the template reads.
// Both are llama.cpp's, from the copy of its `common` compiled in for this.
#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/config/config.hpp"
#include "crucible/llm/chat_model.hpp"

namespace crucible {

/// A call the model made to one of the tools it was offered.
struct ToolCall {
    std::string id;          ///< "call_" and 24 letters, unless the model named the call itself
    std::string name;
    std::string arguments;   ///< JSON, as the model wrote it
};

/// A conversation in OpenAI's shape.
struct OpenChat {
    /// The messages as the request had them -- roles, text, the calls an
    /// assistant made and the results sent back -- each picture's part
    /// {"type": "media_marker"} where the picture was, its bytes in `images`
    /// in the same order.
    nlohmann::json         messages = nlohmann::json::array();
    std::vector<ChatImage> images;

    /// The tools offered, as OpenAI's list has them, or null for none.
    nlohmann::json tools;

    /// "auto", "none" or "required". One function named by the request is
    /// that function alone, offered as required.
    std::string tool_choice = "auto";

    /// Whether the answer may make more than one call. Unset leaves it to
    /// what the template says the model can do.
    std::optional<bool> parallel_tool_calls;

    /// What the answer must be, as JSON Schema text: {"type": "object"} for
    /// OpenAI's JSON mode. Empty for anything.
    std::string schema;

    /// How hard a reasoning model thinks: "low", "medium" or "high", "none"
    /// for not at all where the template can be told so, or empty to leave it
    /// to the model.
    std::string effort;

    ModelParams params;
};

/// A piece of the answer, as it comes: more of the reasoning, more of the
/// content, or a tool call begun or grown. llama-server's deltas.
struct OpenChatDelta {
    std::string reasoning;
    std::string content;

    /// Which call of the answer `tool` is, counting from 0, or -1 when this
    /// is no call. The call's id and name come with its first piece; the
    /// arguments come a piece at a time.
    int      call = -1;
    ToolCall tool;
};

struct OpenChatSink {
    std::function<void(const OpenChatDelta&)> on_delta;
    CancelCallback                             cancel;

    /// The request is one the model takes, and its prompt is about to be
    /// read: anything said after this comes from the model, not from what
    /// was asked.
    std::function<void()> on_start;
};

struct OpenChatResult {
    GenerationStats       stats;
    std::vector<ToolCall> calls;   ///< every call the answer made, whole

    /// Why there is no answer. Empty when there is one.
    std::string error;

    /// The request is one this model cannot take as it is -- a picture for
    /// a model that reads none, tools its template has no way to write, a
    /// schema that is not one -- rather than a failure of the model's.
    bool refused = false;

    /// The prompt, as this model reads it, is this many tokens and does not
    /// fit its context. Zero when it fits.
    int too_long = 0;
};

/// What LoadedModel::embed made of a list of texts.
struct Embedded {
    std::vector<std::vector<float>> vectors;   ///< one per text, in order, each of unit length
    int                             tokens = 0;

    /// Why there are none. Empty when there are.
    std::string error;

    /// A text is longer than the model reads at once: this many tokens.
    int too_long = 0;
};

}  // namespace crucible
