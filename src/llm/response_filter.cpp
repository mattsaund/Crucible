// SPDX-License-Identifier: MIT
//
// See response_filter.hpp for what this is for.
#include "crucible/llm/response_filter.hpp"

#include <algorithm>
#include <cassert>
#include <array>
#include <cctype>
#include <string_view>

#include <nlohmann/json.hpp>

#include "crucible/util/format.hpp"

namespace crucible {
namespace {

/// Every marker either convention uses, longest first so that scanning finds
/// the longest match at a position rather than a prefix of it.
///
/// `<|constrain|>` is in the list only to be swallowed: it introduces the type
/// of a tool call's arguments, and printing it would be exactly the noise this
/// file exists to remove.
constexpr std::array<std::string_view, 10> kMarkers{{
    "<|constrain|>",
    "<|channel|>",
    "<|message|>",
    "<|return|>",
    "</think>",
    "<|start|>",
    "<|think>",
    "<think>",
    "<|end|>",
    "<|call|>",
}};

/// The longest marker, which is how far back a partial one can reach.
constexpr std::size_t longest_marker() {
    std::size_t longest = 0;
    for (const std::string_view marker : kMarkers) {
        longest = std::max(longest, marker.size());
    }
    return longest;
}

/// The marker beginning at `text`, or an empty view if none does.
std::string_view marker_at(std::string_view text) {
    for (const std::string_view marker : kMarkers) {
        if (text.substr(0, marker.size()) == marker) {
            return marker;
        }
    }
    return {};
}

/// Could `tail` be the beginning of a marker that has not finished arriving?
bool could_begin_marker(std::string_view tail) {
    if (tail.empty()) {
        return false;
    }
    return std::any_of(kMarkers.begin(), kMarkers.end(), [tail](std::string_view marker) {
        return marker.size() > tail.size() && marker.substr(0, tail.size()) == tail;
    });
}

/// Where the earliest possible marker starts, or npos.
///
/// A marker only ever starts with '<' in both conventions, so finding the next
/// one is a search for that character rather than a scan of the whole table at
/// every position.
std::size_t next_candidate(std::string_view text, std::size_t from) {
    return text.find('<', from);
}

/// Who a harmony header addresses, or empty: the word after "to=".
std::string recipient_of(std::string_view header) {
    const std::size_t at = header.find("to=");
    if (at == std::string_view::npos) {
        return {};
    }
    std::string_view rest = header.substr(at + 3);
    const std::size_t end = rest.find_first_of(" \t\r\n<");
    return std::string(rest.substr(0, end));
}

/// Is this what a harmony channel name looks like? "analysis", "commentary",
/// "final", optionally followed by a recipient and a type. Anything else that
/// arrives where a channel name goes was the model writing its message in the
/// wrong place -- which gpt-oss does, putting "LIST: ." straight after the
/// marker with no <|message|> at all.
bool looks_like_channel(std::string_view name) {
    const std::string trimmed = format::trim(std::string(name));
    for (const std::string_view known : {"analysis", "commentary", "final"}) {
        if (trimmed.rfind(known, 0) == 0
            && (trimmed.size() == known.size()
                || std::isspace(static_cast<unsigned char>(trimmed[known.size()])) != 0)) {
            return true;
        }
    }
    return false;
}

/// The protocol's own verbs, as a line would begin with them.
constexpr std::array<std::string_view, 9> kVerbs{{
    "LIST", "READ", "WRITE", "RUN", "SEARCH", "ASK", "NOTE", "DONE", "HANDOFF",
}};

bool starts_with_verb(std::string_view text) {
    for (const std::string_view verb : kVerbs) {
        if (text.size() > verb.size() && text.substr(0, verb.size()) == verb
            && text[verb.size()] == ':') {
            return true;
        }
    }
    return false;
}

}  // namespace

std::string harmony_call_line(std::string_view recipient, std::string_view raw_body) {
    using json = nlohmann::json;
    const std::string body = format::trim(std::string(raw_body));

    // Already the protocol, addressed to something for form's sake.
    if (starts_with_verb(body)) {
        return body;
    }

    // The function's own name, without the namespace it was filed under:
    // "functions.read" and "container.exec" are "read" and "exec".
    std::string name = format::to_lower(recipient);
    if (const std::size_t dot = name.rfind('.'); dot != std::string::npos) {
        name.erase(0, dot + 1);
    }

    const json args = json::parse(body, nullptr, /*allow_exceptions=*/false);
    const auto field = [&args](std::initializer_list<const char*> keys) -> std::string {
        if (!args.is_object()) {
            return {};
        }
        for (const char* key : keys) {
            const auto found = args.find(key);
            if (found != args.end() && found->is_string()) {
                return found->get<std::string>();
            }
        }
        return {};
    };
    // What the body says when it is not JSON at all: the argument as written.
    const std::string plain = args.is_discarded() ? body : std::string();

    const auto is = [&name](std::initializer_list<const char*> names) {
        return std::any_of(names.begin(), names.end(),
                           [&name](const char* one) { return name == one; });
    };

    if (is({"exec", "bash", "shell", "sh", "run", "run_command", "execute", "terminal",
            "command"})) {
        std::string command = field({"command", "cmd", "script", "code"});
        if (command.empty() && args.is_object() && args.contains("cmd")
            && args["cmd"].is_array()) {
            // ["bash", "-lc", "ls -R"] is a shell being asked to run the last
            // element; anything else is the words of the command itself.
            const json& words = args["cmd"];
            const bool via_shell = words.size() >= 3 && words[0].is_string()
                                && words[1].is_string() && words[2].is_string()
                                && (words[1].get<std::string>() == "-lc"
                                    || words[1].get<std::string>() == "-c");
            if (via_shell) {
                command = words[2].get<std::string>();
            } else {
                for (const json& word : words) {
                    if (word.is_string()) {
                        command += (command.empty() ? "" : " ") + word.get<std::string>();
                    }
                }
            }
        }
        if (command.empty()) {
            command = plain;
        }
        return command.empty() ? std::string() : "RUN: " + command;
    }
    if (is({"read", "open", "cat", "read_file", "open_file", "view", "view_file"})) {
        const std::string path = !plain.empty() ? plain
                               : field({"path", "file", "filename", "file_path", "target"});
        return path.empty() ? std::string() : "READ: " + path;
    }
    if (is({"list", "ls", "list_dir", "list_files", "listdir", "dir"})) {
        std::string path = !plain.empty() ? plain : field({"path", "dir", "directory"});
        return "LIST: " + (path.empty() ? std::string(".") : path);
    }
    if (is({"write", "write_file", "create_file", "save", "save_file", "edit", "edit_file"})) {
        const std::string path    = field({"path", "file", "filename", "file_path"});
        const std::string content = field({"content", "contents", "text", "body", "data"});
        if (path.empty()) {
            return {};
        }
        // Fenced the way the protocol asks, unless the file itself holds a
        // fence -- then the other delimiter the parser accepts.
        const bool fenced = content.find("```") == std::string::npos;
        return "WRITE: " + path + "\n" + (fenced ? "```" : "<<<") + "\n" + content + "\n"
             + (fenced ? "```" : ">>>");
    }
    if (is({"search", "web_search", "find"})) {
        const std::string query = !plain.empty() ? plain : field({"query", "q", "search"});
        return query.empty() ? std::string() : "SEARCH: " + query;
    }
    // The verbs that carry a sentence rather than a path or a command.
    struct Spoken {
        std::array<const char*, 3> names;
        const char*                verb;
    };
    static constexpr std::array<Spoken, 4> kSpoken{{
        {{"note", "", ""},                       "NOTE"},
        {{"ask", "ask_user", "question"},        "ASK"},
        {{"done", "finish", "complete"},         "DONE"},
        {{"handoff", "hand_off", ""},            "HANDOFF"},
    }};
    for (const Spoken& spoken : kSpoken) {
        const bool named = std::any_of(spoken.names.begin(), spoken.names.end(),
                                       [&name](const char* one) { return *one != '\0' && name == one; });
        const char* verb = spoken.verb;
        if (named) {
            const std::string text = !plain.empty() ? plain
                                   : field({"text", "message", "question", "summary",
                                            "argument", "note", "work", "reason"});
            return std::string(verb) + ": " + text;
        }
    }
    return {};
}

void ResponseFilter::answer_line(Piece& piece, std::string_view line) {
    if (line.empty()) {
        return;
    }
    if (last_answer_ != '\n') {
        piece.answer += '\n';
    }
    piece.answer += line;
    piece.answer += '\n';
    last_answer_ = '\n';
}

void ResponseFilter::finish_call(Piece& piece) {
    const std::string line = harmony_call_line(recipient_, call_);
    if (!line.empty()) {
        answer_line(piece, line);
    } else {
        // Something no protocol verb means -- a Python sandbox, a browser it
        // does not have. It was the model working, and that is where it goes.
        piece.reasoning += call_;
    }
    call_.clear();
    recipient_.clear();
}

void ResponseFilter::close_message(Piece& piece) {
    if (sink_ == Sink::Call) {
        finish_call(piece);
    }
    if (state_ == State::ChannelName && !looks_like_channel(channel_)) {
        answer_line(piece, format::trim(channel_));
    }
    channel_.clear();
}

void ResponseFilter::drain(Piece& piece, bool final_chunk) {
    const auto emit = [this, &piece](std::string_view text) {
        if (text.empty()) {
            return;
        }
        switch (sink_) {
            case Sink::Answer:
                piece.answer += text;
                last_answer_ = text.back();
                break;
            case Sink::Reasoning: piece.reasoning += text; break;
            case Sink::Call:      call_ += text; break;
            case Sink::Discard:   header_ += text; break;
        }
    };

    std::size_t at = 0;
    while (at < buffer_.size()) {
        const std::string_view rest(buffer_.data() + at, buffer_.size() - at);
        const std::size_t candidate = next_candidate(rest, 0);

        // Everything up to the next '<' cannot be part of a marker.
        if (candidate == std::string_view::npos) {
            if (state_ == State::ChannelName) {
                channel_ += rest;
            } else {
                emit(rest);
            }
            at = buffer_.size();
            break;
        }
        if (candidate > 0) {
            if (state_ == State::ChannelName) {
                channel_ += rest.substr(0, candidate);
            } else {
                emit(rest.substr(0, candidate));
            }
            at += candidate;
            continue;
        }

        // At a '<'. Either a marker starts here, or one might once more text
        // arrives, or it is just a less-than sign in the answer.
        const std::string_view here(buffer_.data() + at, buffer_.size() - at);
        const std::string_view marker = marker_at(here);
        if (marker.empty()) {
            if (!final_chunk && could_begin_marker(here)) {
                break;  // hold it back; the next chunk decides
            }
            // A real '<'. Emit it and carry on past it, so the search for the
            // next candidate does not find this one again.
            if (state_ == State::ChannelName) {
                channel_ += '<';
            } else {
                emit(here.substr(0, 1));
            }
            at += 1;
            continue;
        }

        at += marker.size();

        if (marker == "<|channel|>") {
            channel_.clear();
            state_ = State::ChannelName;
        } else if (marker == "<|message|>") {
            // Addressed to a tool -- in the channel name or the role header
            // before it, both of which harmony allows -- is a call to collect.
            // Otherwise "final" is the answer and "analysis" and "commentary"
            // are the model working. A message with no channel at all is an
            // answer: that is what a model using only part of the convention
            // means by it.
            recipient_ = recipient_of(channel_);
            if (recipient_.empty()) {
                recipient_ = recipient_of(header_);
            }
            const bool final_channel = channel_.empty() || channel_ == "final";
            sink_  = !recipient_.empty() ? Sink::Call
                   : final_channel       ? Sink::Answer
                                         : Sink::Reasoning;
            call_.clear();
            header_.clear();
            state_ = State::Text;
        } else if (marker == "<|start|>" || marker == "<|end|>" || marker == "<|return|>" ||
                   marker == "<|call|>") {
            // The end of a message. Whatever was open is settled now -- a
            // call, or a channel name that was really the message -- and what
            // follows is a role name and a header, not anything anybody wants
            // to read.
            close_message(piece);
            header_.clear();
            sink_  = Sink::Discard;
            state_ = State::Text;
        } else if (marker == "<think>" || marker == "<|think>") {
            sink_  = Sink::Reasoning;
            state_ = State::Text;
        } else if (marker == "</think>") {
            sink_  = Sink::Answer;
            state_ = State::Text;
        }
        // <|constrain|> falls through: swallowed, nothing else changes.
    }

    buffer_.erase(0, at);
}

ResponseFilter::Piece ResponseFilter::feed(std::string_view chunk) {
    Piece piece;
    buffer_ += chunk;
    drain(piece, /*final_chunk=*/false);
    // What is held back is only ever a partial marker, which is shorter than
    // the longest marker by definition -- `could_begin_marker` says so. So the
    // buffer cannot grow with the reply, and a paragraph full of less-than
    // signs is emitted as it arrives rather than accumulating to the end.
    assert(buffer_.size() < longest_marker());
    return piece;
}

ResponseFilter::Piece ResponseFilter::flush() {
    Piece piece;
    drain(piece, /*final_chunk=*/true);
    buffer_.clear();
    // A call ends at <|call|>, and <|call|> is where generation stops -- so on
    // the model that makes these the marker usually never arrives as text.
    close_message(piece);
    return piece;
}

}  // namespace crucible
