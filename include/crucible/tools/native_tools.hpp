// SPDX-License-Identifier: MIT
//
// The workshop's tools as a model on this machine was trained to call them.
//
// Every family of model has its own way of writing a tool call, learned in
// training: Qwen writes <tool_call><function=...>, flint <tool_call>name
// <arg_key>, gpt-oss a commentary channel to functions.name, LFM a Python list.
// Told instead to write `READ: file` on a line of its own, they did for a
// while and then fell back on what they know, and a turn ended on a bare
// <tool_call> with nothing done. So a model whose chat template can write tools
// is offered them as functions, through that template, and what it calls is
// read back by the parser that goes with it (LoadedModel::open_chat).
//
// Here is what they are called and how each becomes the ToolCall the workshop
// runs, so there is one set of tools however a model asks for them. A model
// whose template has no tools, and a provider's, keep the `VERB:` lines.
#pragma once

#include <optional>
#include <string>
#include <string_view>

#include <nlohmann/json.hpp>

#include "crucible/tools/workshop.hpp"

namespace crucible::tools {

/// The name of the tool that passes the next step to another expert.
inline constexpr std::string_view kHandOffTool = "hand_off";

/// The tools `settings` allows, in OpenAI's shape, each one's parameters in
/// the order a model should write them: llama.cpp's parsers hold it to that
/// order, so a file's path comes before its contents. `search` offers the web
/// search as well, which is a switch of its own.
nlohmann::ordered_json native_tools(const WorkshopSettings& settings, bool search);

/// What the expert is told beside them: how to work, and nothing about how to
/// write a call, which its template has said already.
std::string native_instructions(const WorkshopSettings& settings);

/// The call `name` with `arguments` (JSON, as the model wrote it) is, as the
/// workshop runs one. Nothing, with `why` for the model, when it cannot be.
std::optional<ToolCall> call_from_native(std::string_view name, std::string_view arguments, std::string& why);

/// A call a reply ends on, in one of the formats models are trained on, and
/// what was said before it.
struct CallInText {
    ToolCall    call;
    std::string prose;
};

/// The call at the end of `answer`, written out as text. A model sometimes
/// spells its tool-call tag in ordinary tokens rather than the special one,
/// and llama.cpp's parser rightly takes that for words, so the call would end
/// the turn as what the expert said. Only a whole call, of a tool that exists,
/// with nothing after it: a reply that shows one in the middle of explaining
/// something is still a reply.
std::optional<CallInText> call_in_text(std::string_view answer);

}  // namespace crucible::tools
