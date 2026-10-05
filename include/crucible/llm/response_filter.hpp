// SPDX-License-Identifier: MIT
//
// Separating what a model thinks from what it means you to read.
//
// A reasoning model does not emit an answer. It emits its working and then an
// answer, with markers between them, and the markers are not always suppressed
// by the tokenizer: gpt-oss writes `<|channel|>analysis<|message|>` as ordinary
// visible text, because llama.cpp classifies those tokens as user-defined
// rather than control and renders them like any other word. Without something
// here, a question about boiling water comes back as
//
//   <|channel|>analysis<|message|>We need to answer: "What is the boiling
//   point of water at sea level?" The typical answer: 100C (212F). But we
//   should consider...<|end|><|start|>assistant<|channel|>final<|message|>At
//   standard atmospheric pressure, water boils at 100 C.
//
// when the answer is the last sentence of it.
//
// Two conventions are in the wild and both are handled: OpenAI's harmony
// channels (gpt-oss) and the `<think>` tags used by DeepSeek-R1, Qwen3 and
// most of what followed them. A model that uses neither is unaffected -- every
// byte it produces is answer, which is what the filter does with text it does
// not recognize.
//
// Harmony has one more thing in it worth catching: a tool call. gpt-oss was
// trained to call tools in its own format -- a message addressed `to=` a
// function, with JSON for arguments -- and it reaches for that format even
// when told to write `READ: file` on a line. Left alone, a call like that is
// filed as reasoning and the round reads as an expert that said nothing. So a
// message addressed to a tool is turned into the line Crucible's own protocol
// would have used, when there is one that means the same thing, and goes to
// the answer where the tool parser looks.
#pragma once

#include <string>
#include <string_view>

namespace crucible {

/// Splits a model's output into reasoning and answer as it arrives.
///
/// Streaming, so it has to cope with a marker arriving in pieces: `<|chan` at
/// the end of one chunk and `nel|>` at the start of the next is the ordinary
/// case, not the unlikely one. Text that might still turn out to be the start
/// of a marker is held back until the next chunk settles it, and `flush`
/// releases whatever was still pending when generation stopped.
class ResponseFilter {
public:
    /// One chunk's worth of output, sorted.
    struct Piece {
        std::string reasoning;  ///< the working: shown while it happens, then dropped
        std::string answer;     ///< what goes in the transcript and the history
    };

    /// Sort `chunk`. Either field may come back empty.
    Piece feed(std::string_view chunk);

    /// Whatever is still held back. Call once when generation ends: a model cut
    /// off mid-marker would otherwise lose its last few characters.
    Piece flush();

    /// True once the answer channel has been reached. Until then a reasoning
    /// model is still working, which is worth saying on screen.
    bool answering() const { return sink_ == Sink::Answer; }

private:
    /// Where text is currently going.
    enum class Sink {
        Answer,     ///< the default, and where a model with no markers stays
        Reasoning,  ///< inside `<think>` or a non-final harmony channel
        Discard,    ///< between harmony messages: role names and headers
        Call,       ///< a harmony message addressed to a tool; see finish_call
    };

    /// What the parser is in the middle of reading.
    enum class State {
        Text,         ///< ordinary content, going to `sink_`
        ChannelName,  ///< after `<|channel|>`, collecting the name
    };

    /// Consume as much of `buffer_` as can be decided, appending to `piece`.
    void drain(Piece& piece, bool final_chunk);

    /// A message has ended, or the output has. Settle whatever was open: a
    /// tool call becomes a protocol line, and a "channel name" that never
    /// reached a message turns out to have been the message itself.
    void close_message(Piece& piece);

    /// The tool call collected in `call_`, as an answer line or as reasoning.
    void finish_call(Piece& piece);

    /// Add a whole line to the answer, on a line of its own.
    void answer_line(Piece& piece, std::string_view line);

    std::string buffer_;
    std::string channel_;
    std::string header_;      ///< the role header before <|channel|>, which may name a recipient
    std::string recipient_;   ///< who the message in `call_` is addressed to
    std::string call_;        ///< the body of a message addressed to a tool
    char        last_answer_ = '\n';  ///< the last character sent to the answer
    Sink        sink_  = Sink::Answer;
    State       state_ = State::Text;
};

/// The protocol line a harmony tool call means, or an empty string.
///
/// `recipient` is what the message was addressed to -- "functions.read",
/// "container.exec", "LIST" -- and `body` is what it carried, usually JSON.
/// Exposed for the tests; see the comment at the top of this file.
std::string harmony_call_line(std::string_view recipient, std::string_view body);

}  // namespace crucible
