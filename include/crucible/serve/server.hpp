// SPDX-License-Identifier: MIT
//
// The API server: a model on this machine, offered to other programs at an
// address, in the shape OpenAI's API has and every client speaks.
//
// What LM Studio's server and llama-server do, for a model Crucible holds: an
// editor's assistant, a script, another machine on the network, each pointed
// at http://<this machine>:<port>/v1 as if it were OpenAI. Four routes and
// nothing else -- the models list, a chat completion, a text completion and a
// browser's preflight -- so what reaches it is a conversation and what leaves
// it is the answer. No request names a file, runs a tool or reaches the
// window's own API (api/surface.hpp), which stays inside the process.
//
// The HTTP is cpp-httplib's, compiled from the copy llama.cpp carries. What
// answers is a Backend: the engine, which holds the model the way a seat
// holds one (see engine_serve.cpp), or a stand-in in the tests. Requests are
// answered one at a time, in the order they came, on a thread of the
// server's own, so the requests' threads only ever wait on the server and a
// stop never waits on a model. While one waits -- for the chat to finish with
// the model, for memory, for a load -- the status says what for.
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/llm/chat_model.hpp"

namespace crucible::serve {

/// One request, as the backend is asked it.
struct Ask {
    /// The id the request was checked against. The backend refuses it when
    /// that is no longer the model it serves, which a change in Settings
    /// between the asking and the answering can make so.
    std::string model;

    /// A chat completion's conversation.
    std::vector<ChatMessage> messages;

    /// A text completion's prompt, read as it is, with no chat template
    /// around it. `raw` says which of the two this is.
    std::string prompt;
    bool        raw = false;

    // What the request set of the sampling. What it left out is the model's
    // own: Settings, Generation.
    std::optional<float>         temperature;
    std::optional<float>         top_p;
    std::optional<int>           top_k;
    std::optional<float>         min_p;
    std::optional<float>         repeat_penalty;
    std::optional<int>           max_tokens;
    std::optional<std::uint32_t> seed;

    /// How hard a reasoning model should think: low, medium or high. Empty
    /// leaves it to the model.
    std::string effort;
};

/// Where an answer goes as it comes.
struct Sink {
    std::function<void(std::string_view)> answer;      ///< the content
    std::function<void(std::string_view)> reasoning;   ///< a thinking model's working

    /// True once the answer is not wanted: the client went, the server is
    /// stopping, or a stop sequence was met.
    std::function<bool()> cancel;

    /// What the request waits for now -- "the chat", "memory", "loading" --
    /// or empty once the model is the request's and it is being answered.
    std::function<void(const std::string&)> waiting;
};

/// How a request went.
struct Answer {
    int  prompt_tokens     = 0;
    int  completion_tokens = 0;

    /// Ended at max_tokens or at the end of the context, rather than where
    /// the model chose to.
    bool hit_limit = false;

    /// Why there is no answer, for the client. Empty when there is one.
    std::string error;

    /// The HTTP status `error` goes with: 400 for a request this model
    /// cannot take, 404 for a model it does not serve, 503 when there is
    /// none to serve, 500 for anything else.
    int status = 500;

    /// OpenAI's code for it, when it has one: "context_length_exceeded".
    std::string code;
};

/// What answers.
class Backend {
public:
    virtual ~Backend() = default;

    /// The id of the model served, which is its file's name. Empty when
    /// there is none to serve.
    virtual std::string model() const = 0;

    /// Answer `ask`, telling `sink` as it goes. Blocks until it is done or
    /// `sink.cancel` says to stop. Called on the server's own thread, one
    /// request at a time.
    virtual Answer answer(const Ask& ask, const Sink& sink) = 0;
};

/// Where it listens: ServerConfig, less the model, which is the backend's.
struct Settings {
    bool        enabled = false;
    int         port    = 0;      ///< 0 takes any free port, which is for the tests
    bool        network = false;  ///< every address of this machine, not 127.0.0.1 alone
    std::string key;              ///< empty leaves it open
};

/// One request answered, for the list in Settings.
struct Record {
    std::int64_t at = 0;          ///< when it came, in seconds since 1970
    std::string  model;
    std::string  path;            ///< "/v1/chat/completions"
    std::string  from;            ///< the address it came from
    int          status = 200;
    int          prompt_tokens     = 0;
    int          completion_tokens = 0;
    long         ms = 0;          ///< from its arriving to its last byte
    bool         stream = false;
};

/// What the server is doing.
struct Status {
    bool        enabled   = false;  ///< asked to listen
    bool        listening = false;
    int         port      = 0;      ///< the one it listens on, or was asked for
    bool        network   = false;
    bool        key       = false;  ///< a key is asked for
    std::string error;              ///< why it is not listening, when it was asked to
    std::string model;              ///< the id served, empty when there is none

    /// This machine's address on its network, while it listens there.
    std::string lan;

    bool        answering = false;  ///< a request has the model
    std::string waiting;            ///< what the request being answered waits for
    int         queued    = 0;      ///< requests behind it

    std::vector<Record> recent;     ///< the last few, the newest first
};

/// The status as the page is sent it, under "server" in the snapshot: with
/// the addresses to use written out.
nlohmann::json status_json(const Status& status);

class Server {
public:
    /// The largest request body taken. A long conversation is well under a
    /// megabyte of text; anything near this is not one, and is refused before
    /// it is read.
    static constexpr std::size_t kLargestBody = std::size_t{4} << 20;

    /// `changed` is called, from any thread, whenever what status() says has
    /// moved: so the window can draw it.
    explicit Server(Backend& backend, std::function<void()> changed = {});
    ~Server();
    Server(const Server&)            = delete;
    Server& operator=(const Server&) = delete;

    /// Listen as `settings` say: start, move to another port or address, take
    /// a key or drop one, or stop. Quick whatever is being answered: a request
    /// on an address that is going is ended. Returns why it could not listen,
    /// or empty.
    std::string apply(const Settings& settings);

    /// Stop listening, end every request, and end the thread that answers
    /// them. For the way out; apply() starts it all again. Once it returns,
    /// nothing reaches the backend or `changed` until then, whatever a
    /// connection still open does.
    void stop();

    Status status() const;

private:
    // Shared, because a connection that will not end -- a client sending its
    // request a byte a minute -- keeps what it is reading with alive after
    // this is gone. See Impl::close.
    struct Impl;
    std::shared_ptr<Impl> impl_;
};

}  // namespace crucible::serve
