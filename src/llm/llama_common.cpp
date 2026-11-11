// SPDX-License-Identifier: MIT
//
// The few functions of llama.cpp's `common` that its chat pieces call from
// files Crucible does not compile. See cmake/CrucibleDependencies.cmake.
//
// The chat code -- the Jinja engine, the parsers, JSON Schema to GBNF -- is
// compiled from llama.cpp's tree for the API server. It calls a handful of
// string helpers and the tokenizer's wrappers from common.cpp, the logger from
// log.cpp and two helpers from sampling.cpp; and those files bring the rest of
// `common` with them -- argument parsing, the model fitter, speculative
// decoding, a logger with a thread of its own writing to the terminal. What is
// here does what those functions do, written against the same declarations, so
// a change to one of them upstream is a compile error here rather than a
// difference nobody notices. The logger is the one that differs on purpose:
// its warnings go where llama.cpp's own do, Crucible's log, and nothing below
// a warning goes anywhere.
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "common.h"
#include "log.h"
#include "sampling.h"

// --- common.cpp -----------------------------------------------------------------

std::string string_join(const std::vector<std::string>& values, const std::string& separator) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i > 0) {
            out += separator;
        }
        out += values[i];
    }
    return out;
}

std::vector<std::string> string_split(const std::string& str, const std::string& delimiter) {
    std::vector<std::string> parts;
    std::size_t start = 0;
    std::size_t end   = str.find(delimiter);
    while (end != std::string::npos) {
        parts.push_back(str.substr(start, end - start));
        start = end + delimiter.length();
        end   = str.find(delimiter, start);
    }
    parts.push_back(str.substr(start));
    return parts;
}

std::string string_repeat(const std::string& str, size_t n) {
    std::string out;
    out.reserve(str.size() * n);
    for (size_t i = 0; i < n; ++i) {
        out += str;
    }
    return out;
}

void string_replace_all(std::string& s, const std::string& search, const std::string& replace) {
    if (search.empty()) {
        return;
    }
    std::string built;
    built.reserve(s.length());
    std::size_t last = 0;
    for (std::size_t at = s.find(search); at != std::string::npos; at = s.find(search, last)) {
        built.append(s, last, at - last);
        built.append(replace);
        last = at + search.length();
    }
    built.append(s, last, std::string::npos);
    s = std::move(built);
}

std::vector<llama_token> common_tokenize(const llama_vocab* vocab, const std::string& text, bool add_special,
                                         bool parse_special) {
    // A negative count is llama.cpp saying how much room it needs.
    std::vector<llama_token> tokens(text.length() + (add_special ? 2 : 0));
    int count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.length()), tokens.data(),
                               static_cast<int32_t>(tokens.size()), add_special, parse_special);
    if (count == std::numeric_limits<int32_t>::min()) {
        throw std::runtime_error("Tokenization failed: input text too large");
    }
    if (count < 0) {
        tokens.resize(static_cast<std::size_t>(-count));
        count = llama_tokenize(vocab, text.data(), static_cast<int32_t>(text.length()), tokens.data(),
                               static_cast<int32_t>(tokens.size()), add_special, parse_special);
    }
    tokens.resize(count > 0 ? static_cast<std::size_t>(count) : 0);
    return tokens;
}

std::string common_token_to_piece(const llama_vocab* vocab, llama_token token, bool special) {
    std::string piece(16, '\0');
    int written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, special);
    if (written < 0) {
        piece.resize(static_cast<std::size_t>(-written));
        written = llama_token_to_piece(vocab, token, piece.data(), static_cast<int32_t>(piece.size()), 0, special);
    }
    piece.resize(written > 0 ? static_cast<std::size_t>(written) : 0);
    return piece;
}

// --- log.cpp --------------------------------------------------------------------

int common_log_get_verbosity_thold(void) {
    return LOG_LEVEL_WARN;
}

struct common_log* common_log_main() {
    return nullptr;   // nothing of its own: see common_log_add
}

void common_log_add(struct common_log* /*log*/, enum ggml_log_level level, const char* fmt, ...) {
    ggml_log_callback callback  = nullptr;
    void*             user_data = nullptr;
    llama_log_get(&callback, &user_data);
    if (callback == nullptr) {
        return;
    }
    char    line[1024];
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    callback(level, line, user_data);
}

// --- sampling.cpp ---------------------------------------------------------------

void common_sampling_add_preserved_tokens(common_params_sampling& sampling, const llama_vocab* vocab,
                                          const std::vector<std::string>& tokens) {
    for (const std::string& text : tokens) {
        const std::vector<llama_token> ids = common_tokenize(vocab, text, false, true);
        if (ids.size() == 1) {
            sampling.preserved_tokens.insert(ids[0]);
        }
    }
}

void common_sampling_add_grammar_triggers(common_params_sampling& sampling, const llama_vocab* vocab,
                                          std::vector<common_grammar_trigger> triggers) {
    for (common_grammar_trigger& trigger : triggers) {
        if (trigger.type != COMMON_GRAMMAR_TRIGGER_TYPE_WORD) {
            sampling.grammar_triggers.push_back(std::move(trigger));
            continue;
        }
        // A word that is one token is watched for as that token, which it
        // can only be when the token is one the parser is shown.
        const std::vector<llama_token> ids = common_tokenize(vocab, trigger.value, false, true);
        if (ids.size() != 1) {
            sampling.grammar_triggers.push_back({COMMON_GRAMMAR_TRIGGER_TYPE_WORD, trigger.value});
            continue;
        }
        if (sampling.preserved_tokens.count(ids[0]) == 0) {
            throw std::runtime_error("Grammar trigger word should be marked as preserved token: " + trigger.value);
        }
        common_grammar_trigger token_trigger;
        token_trigger.type  = COMMON_GRAMMAR_TRIGGER_TYPE_TOKEN;
        token_trigger.value = trigger.value;
        token_trigger.token = ids[0];
        sampling.grammar_triggers.push_back(std::move(token_trigger));
    }
    if (sampling.grammar_lazy && sampling.grammar_triggers.empty()) {
        throw std::runtime_error("Error: no triggers set for lazy grammar!");
    }
}
