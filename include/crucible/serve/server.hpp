// SPDX-License-Identifier: MIT
//
// The API server: a model on this machine, offered to other programs at an
// address, in the shape OpenAI's API has and every client speaks.
//
// What LM Studio's server and llama-server do, for a model Crucible holds: an
// editor's assistant, a script, another machine on the network, each pointed
// at http://<this machine>:<port>/v1 as if it were OpenAI. Five routes and
// nothing else -- the models list, a chat completion, a text completion,
// embeddings and a browser's preflight -- so what reaches it is a
// conversation or a text and what leaves it is the answer. No request names a
// file or reaches the window's own API (api/surface.hpp), which stays inside
// the process. A tool a request offers is one the client runs: the model's
// call goes back to the client, which sends the result in its next request.
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

    /// A chat completion's conversation: each message's text, and the
    /// pictures that came with it, for a model asked the way the chat asks
    /// one.
    std::vector<ChatMessage> messages;

    /// The same conversation in OpenAI's shape, for a model on this machine
    /// (see LoadedModel::open_chat): the calls an assistant made and the
    /// results sent back, and each picture a {"type": "media_marker"} part
    /// where it was, its bytes in `messages`' pictures in the same order.
    nlohmann::json chat;

    /// The tools offered, OpenAI's list, or null; how the model may use them
    /// ("auto", "none", "required"); whether it may call more than one at a
    /// time, unset to leave it to the model.
    nlohmann::json      tools;
    std::string         tool_choice = "auto";
    std::optional<bool> parallel_tool_calls;

    /// What the answer must be, as JSON Schema text: {"type": "object"} for
    /// OpenAI's JSON mode, the schema itself for a structured output. Empty
    /// for anything.
    std::string schema;

    /// A text completion's prompt, read as it is, with no chat template
    /// around it. `raw` says which of the two this is.
    std::string prompt;
    bool        raw = false;

    /// For /v1/embeddings: the texts to embed. `embed` says this is one.
    std::vector<std::string> texts;
    bool                     embed = false;

    // What the request set of the sampling. What it left out is the model's
    // own: Settings, Generation.
    std::optional<float>         temperature;
    std::optional<float>         top_p;
    std::optional<int>           top_k;
    std::optional<float>         min_p;
    std::optional<float>         repeat_penalty;
    std::optional<int>           max_tokens;
    std::optional<std::uint32_t> seed;

    /// How hard a reasoning model should think: low, medium or high, or none
    /// for not at all where the model can be told so. Empty leaves it to the
    /// model.
    std::string effort;
};

/// A tool call as it streams: which call of the answer it is, its id and
/// name with its first piece, and more of its arguments after.
struct CallPiece {
    int         index = 0;
    std::string id;
    std::string name;
    std::string arguments;
};

/// Where an answer goes as it comes.
struct Sink {
    std::function<void(std::string_view)> answer;      ///< the content
    std::function<void(std::string_view)> reasoning;   ///< a thinking model's working
    std::function<void(const CallPiece&)> call;        ///< a tool call begun or grown

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

    /// An embeddings request's vectors, one for each text, in order.
    std::vector<std::vector<float>> vectors;
};

/// What answers.
class Backend {
public:
    virtual ~Backend() = default;

    /// The id of the model served, which is its file's name. Empty when
    /// there is none to serve.
    virtual std::string model() const = 0;

    /// The id /v1/embeddings answers with when a request names no model: a
    /// model chosen for embeddings, or the one served. A request may name
    /// either.
    virtual std::string embedder() const { return model(); }

    /// Answer `ask`, telling `sink` as it goes. Blocks until it is done or
    /// `sink.cancel` says to stop. Called on the server's own thread, one
    /// request at a time.
    virtual Answer answer(const Ask& ask, const Sink& sink) = 0;
};

/// Where it listens: ServerConfig, less the model, which is the backend's.
///
/// This machine is 127.0.0.1 and ::1; the network is every address of this
/// machine, IPv4 and IPv6 on one socket where the system has IPv6, and IPv4
/// alone where it does not.
struct Settings {
    bool        enabled = false;
    int         port    = 0;      ///< 0 takes any free port, which is for the tests
    bool        network = false;  ///< every address of this machine, not the loopback alone
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
    std::string embedder;           ///< the id embeddings come from, when it is another

    /// Listening on IPv6 as well as IPv4: ::1 beside 127.0.0.1, or both on
    /// the network.
    bool        ipv6 = false;

    /// This machine's address on its network, while it listens there, and
    /// its IPv6 address there when it has one other computers can reach.
    std::string lan;
    std::string lan6;

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
    /// megabyte of text, and a photograph sent with one a few megabytes of
    /// base64, as OpenAI takes pictures of up to twenty; anything past this
    /// is not a conversation, and is refused before it is read.
    static constexpr std::size_t kLargestBody = std::size_t{20} << 20;

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
