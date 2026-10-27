// SPDX-License-Identifier: MIT
//
// The API server. See server.hpp for what it is and why it is shaped so.
//
// Three things live here. The HTTP: the routes, the key, CORS, how large a
// body may be. OpenAI's shapes: a request read into an Ask, an answer written
// back as a completion or as server-sent events, and every refusal as OpenAI's
// error object, which is what a client knows how to read. And the queue:
// requests waiting their turn, and the one thread that hands each to the
// backend.
#include "crucible/serve/server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <exception>
#include <mutex>
#include <random>
#include <thread>
#include <utility>

#include <httplib.h>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include "crucible/util/sha256.hpp"

namespace crucible::serve {
namespace {

using json  = nlohmann::json;
using Clock = std::chrono::steady_clock;

/// How many answered requests the list in Settings keeps.
constexpr std::size_t kRecent = 20;

/// How many requests may wait their turn. One past this is turned away at
/// once rather than holding a connection open for what could be an hour.
constexpr std::size_t kMostWaiting = 32;

/// How often a request's thread looks up from waiting, to see whether its
/// client is still there and the server still listening.
constexpr auto kLook = std::chrono::milliseconds(100);

constexpr const char* kChatPath = "/v1/chat/completions";
constexpr const char* kTextPath = "/v1/completions";

std::string dump(const json& document) {
    // A model's text can end in half a character when it was stopped; the
    // replacement character goes out rather than an exception.
    return document.dump(-1, ' ', false, json::error_handler_t::replace);
}

/// OpenAI's error object: a message, a type, and the parameter and the code
/// when there are any. Every client knows how to read one of these, and most
/// show the message as it is.
json error_json(std::string_view message, int status, std::string_view code, std::string_view param) {
    const char* type = status == 429 ? "rate_limit_error"
                     : status >= 500 ? "server_error"
                                     : "invalid_request_error";
    return json{{"error", json{{"message", std::string(message)},
                               {"type", type},
                               {"param", param.empty() ? json(nullptr) : json(std::string(param))},
                               {"code", code.empty() ? json(nullptr) : json(std::string(code))}}}};
}

void refuse(httplib::Response& res, int status, std::string_view message,
            std::string_view code = {}, std::string_view param = {}) {
    res.status = status;
    res.set_content(dump(error_json(message, status, code, param)), "application/json");
}

/// A completion's id: "chatcmpl-" and twenty-four letters, as OpenAI's are.
std::string new_id(std::string_view prefix) {
    static constexpr char kLetters[] = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    thread_local std::mt19937_64 dice{std::random_device{}()};
    std::string id(prefix);
    for (int i = 0; i < 24; ++i) {
        id += kLetters[dice() % (sizeof(kLetters) - 1)];
    }
    return id;
}

std::string digest_of(std::string_view key) {
    util::Sha256 hash;
    hash.update(key);
    return hash.hex();
}

/// Whether two digests are one, in a time that does not depend on how much
/// of them agrees. Digests rather than the keys themselves, so neither the
/// length of the key nor how much of a guess was right shows in how long a
/// refusal took.
bool same_digest(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) {
        return false;   // both are always 64 digits; this is not the key's length
    }
    unsigned char differs = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        differs = static_cast<unsigned char>(differs | (static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i])));
    }
    return differs == 0;
}

/// The key a request carries: "Authorization: Bearer <key>", the scheme in
/// any case.
std::string bearer(const httplib::Request& req) {
    const std::string value = req.get_header_value("Authorization");
    constexpr std::string_view scheme = "bearer ";
    if (value.size() < scheme.size()) {
        return {};
    }
    for (std::size_t i = 0; i < scheme.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(value[i])) != scheme[i]) {
            return {};
        }
    }
    std::string key = value.substr(scheme.size());
    while (!key.empty() && (key.back() == ' ' || key.back() == '\t')) {
        key.pop_back();
    }
    const std::size_t start = key.find_first_not_of(" \t");
    return start == std::string::npos ? std::string() : key.substr(start);
}

/// Whether `named` is how a request names the model served as `id`: the
/// file's name, or the same without ".gguf", which is how a person types it.
bool names(const std::string& id, const std::string& named) {
    if (named == id) {
        return true;
    }
    constexpr std::string_view gguf = ".gguf";
    return id.size() > gguf.size() && id.compare(id.size() - gguf.size(), gguf.size(), gguf) == 0
        && named == id.substr(0, id.size() - gguf.size());
}

std::int64_t now_seconds() {
    return static_cast<std::int64_t>(std::time(nullptr));
}

/// This machine's address on its network: the one a packet to the outside
/// would leave from. A datagram socket "connected" to an address nobody owns
/// asks the routing table without sending anything. Where there is no route
/// out, the first interface that is up and is not the loopback. Empty when
/// there is no network at all.
std::string lan_address() {
#if defined(_WIN32)
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) {
        return {};
    }
    struct Cleanup {
        ~Cleanup() { WSACleanup(); }
    } cleanup;
#endif
    std::string found;
    const auto sock = ::socket(AF_INET, SOCK_DGRAM, 0);
#if defined(_WIN32)
    const bool opened = sock != INVALID_SOCKET;
#else
    const bool opened = sock >= 0;
#endif
    if (opened) {
        sockaddr_in outside{};
        outside.sin_family = AF_INET;
        outside.sin_port   = htons(9);
        // 192.0.2.1 is TEST-NET-1, kept for documentation: nobody has it,
        // and the route to it is the default one.
        inet_pton(AF_INET, "192.0.2.1", &outside.sin_addr);
        if (::connect(sock, reinterpret_cast<const sockaddr*>(&outside), sizeof(outside)) == 0) {
            sockaddr_in local{};
            socklen_t   length = sizeof(local);
            if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0
                && local.sin_addr.s_addr != htonl(INADDR_ANY)) {
                char text[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text)) != nullptr) {
                    found = text;
                }
            }
        }
#if defined(_WIN32)
        closesocket(sock);
#else
        ::close(sock);
#endif
    }
#if !defined(_WIN32)
    if (found.empty()) {
        ifaddrs* all = nullptr;
        if (getifaddrs(&all) == 0) {
            for (const ifaddrs* one = all; one != nullptr && found.empty(); one = one->ifa_next) {
                if (one->ifa_addr == nullptr || one->ifa_addr->sa_family != AF_INET
                    || (one->ifa_flags & IFF_UP) == 0 || (one->ifa_flags & IFF_LOOPBACK) != 0) {
                    continue;
                }
                char text[INET_ADDRSTRLEN] = {};
                const auto* address = reinterpret_cast<const sockaddr_in*>(one->ifa_addr);
                if (inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text)) != nullptr) {
                    found = text;
                }
            }
            freeifaddrs(all);
        }
    }
#endif
    return found.rfind("127.", 0) == 0 ? std::string() : found;
}

// --- a request, read ----------------------------------------------------------

/// What both kinds of completion are asked the same way: the sampling, the
/// stream, the stop sequences. Returns a refusal, naming the parameter in
/// `param`, or empty.
struct Shape {
    bool                     stream = false;
    bool                     usage  = false;   ///< stream_options.include_usage
    std::vector<std::string> stops;
};

std::string read_number(const json& body, const char* key, std::optional<float>& into, float least,
                        float most, std::string& param) {
    const auto found = body.find(key);
    if (found == body.end() || found->is_null()) {
        return {};
    }
    if (!found->is_number()) {
        param = key;
        return std::string(key) + " must be a number";
    }
    const double value = found->get<double>();
    if (value < static_cast<double>(least) || value > static_cast<double>(most)) {
        param = key;
        return std::string(key) + " is out of range";
    }
    into = static_cast<float>(value);
    return {};
}

std::string read_shared(const json& body, Ask& ask, Shape& shape, std::string& param) {
    if (const auto stream = body.find("stream"); stream != body.end() && !stream->is_null()) {
        if (!stream->is_boolean()) {
            param = "stream";
            return "stream must be true or false";
        }
        shape.stream = stream->get<bool>();
    }
    if (const auto options = body.find("stream_options"); options != body.end() && options->is_object()) {
        const auto usage = options->find("include_usage");
        shape.usage = usage != options->end() && usage->is_boolean() && usage->get<bool>();
    }
    if (const auto n = body.find("n"); n != body.end() && !n->is_null()
        && !(n->is_number_integer() && n->get<long long>() == 1)) {
        param = "n";
        return "only n=1 is supported";
    }

    std::string why;
    if (!(why = read_number(body, "temperature", ask.temperature, 0.0F, 2.0F, param)).empty()
        || !(why = read_number(body, "top_p", ask.top_p, 0.0F, 1.0F, param)).empty()
        || !(why = read_number(body, "min_p", ask.min_p, 0.0F, 1.0F, param)).empty()
        || !(why = read_number(body, "repeat_penalty", ask.repeat_penalty, 0.0F, 10.0F, param)).empty()) {
        return why;
    }
    if (const auto top_k = body.find("top_k"); top_k != body.end() && !top_k->is_null()) {
        if (!top_k->is_number_integer() || top_k->get<long long>() < 0) {
            param = "top_k";
            return "top_k must be a whole number, 0 or more";
        }
        ask.top_k = static_cast<int>(std::min<long long>(top_k->get<long long>(), 100000));
    }
    // max_completion_tokens is the newer name, and wins where both are sent.
    for (const char* key : {"max_tokens", "max_completion_tokens"}) {
        const auto found = body.find(key);
        if (found == body.end() || found->is_null()) {
            continue;
        }
        if (!found->is_number_integer()) {
            param = key;
            return std::string(key) + " must be a whole number";
        }
        // Zero or less is no limit, as llama-server reads it.
        const long long most = found->get<long long>();
        ask.max_tokens = most <= 0 ? -1 : static_cast<int>(std::min<long long>(most, 1 << 24));
    }
    if (const auto seed = body.find("seed"); seed != body.end() && !seed->is_null()) {
        if (!seed->is_number_integer()) {
            param = "seed";
            return "seed must be a whole number";
        }
        ask.seed = static_cast<std::uint32_t>(static_cast<unsigned long long>(seed->get<long long>()) & 0xFFFFFFFFULL);
    }
    if (const auto stop = body.find("stop"); stop != body.end() && !stop->is_null()) {
        if (stop->is_string()) {
            shape.stops.push_back(stop->get<std::string>());
        } else if (stop->is_array() && stop->size() <= 16) {
            for (const json& one : *stop) {
                if (!one.is_string()) {
                    param = "stop";
                    return "stop must be a string or a list of strings";
                }
                shape.stops.push_back(one.get<std::string>());
            }
        } else {
            param = "stop";
            return "stop must be a string or a list of up to 16";
        }
        shape.stops.erase(std::remove(shape.stops.begin(), shape.stops.end(), std::string()), shape.stops.end());
    }
    if (const auto effort = body.find("reasoning_effort"); effort != body.end() && effort->is_string()) {
        const std::string level = effort->get<std::string>();
        if (level == "minimal" || level == "low") {
            ask.effort = "low";
        } else if (level == "medium" || level == "high") {
            ask.effort = level;
        }
    }
    // A JSON mode would need a grammar the model is held to, which this
    // server does not write. Said rather than ignored: a client that asked
    // for JSON and is handed prose has been told nothing.
    if (const auto format = body.find("response_format"); format != body.end() && format->is_object()
        && format->value("type", "text") != "text") {
        param = "response_format";
        return "only text responses are supported";
    }
    return {};
}

/// A message's text: a string, or the text parts of a list of them.
std::string read_content(const json& content, std::string& text) {
    text.clear();
    if (content.is_null()) {
        return {};
    }
    if (content.is_string()) {
        text = content.get<std::string>();
        return {};
    }
    if (!content.is_array()) {
        return "a message's content must be a string or a list of parts";
    }
    for (const json& part : content) {
        if (!part.is_object()) {
            return "a content part must be an object";
        }
        const std::string type = part.value("type", std::string());
        const auto        said = part.find("text");
        if (type == "text" && said != part.end() && said->is_string()) {
            text += said->get<std::string>();
            continue;
        }
        return "content of type \"" + type + "\" is not supported";
    }
    return {};
}

std::string read_chat(const json& body, Ask& ask, std::string& param) {
    const auto messages = body.find("messages");
    if (messages == body.end() || !messages->is_array() || messages->empty()) {
        param = "messages";
        return "messages must be a list with at least one message";
    }
    for (const json& one : *messages) {
        if (!one.is_object() || !one.contains("role") || !one["role"].is_string()) {
            param = "messages";
            return "each message needs a role";
        }
        std::string role = one["role"].get<std::string>();
        // OpenAI's newer name for the system message.
        if (role == "developer") {
            role = "system";
        }
        if (role != "system" && role != "user" && role != "assistant") {
            param = "messages";
            return "role \"" + role + "\" is not supported";
        }
        ChatMessage message;
        message.role = role;
        if (const std::string why = read_content(one.value("content", json(nullptr)), message.content); !why.empty()) {
            param = "messages";
            return why;
        }
        ask.messages.push_back(std::move(message));
    }
    return {};
}

std::string read_text(const json& body, Ask& ask, std::string& param) {
    const auto prompt = body.find("prompt");
    if (prompt != body.end() && prompt->is_string()) {
        ask.prompt = prompt->get<std::string>();
    } else if (prompt != body.end() && prompt->is_array() && prompt->size() == 1 && (*prompt)[0].is_string()) {
        ask.prompt = (*prompt)[0].get<std::string>();
    } else {
        param = "prompt";
        return "prompt must be a string";
    }
    ask.raw = true;
    return {};
}

// --- an answer, written ---------------------------------------------------------

/// Watches the answer for a stop sequence as it streams: hands on what
/// cannot be the start of one, holds back what still might be, and says when
/// one has been met -- which ends the answer before it.
class Stops {
public:
    explicit Stops(std::vector<std::string> words) : words_(std::move(words)) {}

    /// What of `chunk`, with whatever was held back, is safe to pass on.
    std::string feed(std::string_view chunk) {
        if (met_) {
            return {};
        }
        held_ += chunk;
        if (words_.empty()) {
            return std::exchange(held_, {});
        }
        std::size_t first = std::string::npos;
        for (const std::string& word : words_) {
            first = std::min(first, held_.find(word));
        }
        if (first != std::string::npos) {
            met_ = true;
            std::string out = held_.substr(0, first);
            held_.clear();
            return out;
        }
        // Held back: the longest end of what came that one of the words
        // starts with. A word starts on a character's first byte, so what is
        // passed on always ends on a whole character.
        std::size_t keep = 0;
        for (const std::string& word : words_) {
            for (std::size_t length = std::min(word.size() - 1, held_.size()); length > keep; --length) {
                if (held_.compare(held_.size() - length, length, word, 0, length) == 0) {
                    keep = length;
                    break;
                }
            }
        }
        std::string out = held_.substr(0, held_.size() - keep);
        held_.erase(0, held_.size() - keep);
        return out;
    }

    /// What was held back, once nothing more is coming.
    std::string flush() { return met_ ? std::string() : std::exchange(held_, {}); }

    bool met() const { return met_; }

private:
    std::vector<std::string> words_;
    std::string              held_;
    bool                     met_ = false;
};

json usage_json(const Answer& answer) {
    return json{{"prompt_tokens", answer.prompt_tokens},
                {"completion_tokens", answer.completion_tokens},
                {"total_tokens", answer.prompt_tokens + answer.completion_tokens}};
}

std::string fingerprint() { return std::string("crucible-") + CRUCIBLE_VERSION; }

// --- the queue ----------------------------------------------------------------------

/// One request on its way through: what it asks, and what has come back.
struct Job {
    Ask   ask;
    Shape shape;

    std::mutex              mutex;
    std::condition_variable moved;   ///< any of what is below changed

    /// Text not yet taken by the request's thread, in the order it came.
    struct Piece {
        bool        reasoning = false;
        std::string text;
    };
    std::deque<Piece> pieces;

    bool        started = false;   ///< the model is the request's: no refusal comes after this
    bool        done    = false;
    Answer      answer;
    std::string waiting;

    /// Set when nobody wants the rest: the client went, the server is
    /// stopping, a stop sequence was met. Read between tokens.
    std::atomic<bool> dropped{false};

    void add(bool reasoning, std::string_view text) {
        if (text.empty()) {
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(mutex);
            started = true;
            if (!pieces.empty() && pieces.back().reasoning == reasoning) {
                pieces.back().text += text;
            } else {
                pieces.push_back({reasoning, std::string(text)});
            }
        }
        moved.notify_all();
    }
};

}  // namespace

struct Server::Impl : std::enable_shared_from_this<Server::Impl> {
    Impl(Backend& backend_in, std::function<void()> changed_in)
        : backend(backend_in), changed(std::move(changed_in)) {}

    Backend&              backend;
    std::function<void()> changed;

    /// The fence between the requests' threads and the backend and `changed`,
    /// both of which belong to whoever made this and may be gone once stop()
    /// has returned: a request's thread asks across it only under the lock,
    /// and only while `detached` is false. See close().
    mutable std::mutex reaching;
    bool               detached = false;

    /// apply() and stop() one at a time.
    std::mutex control;

    /// The port the listener close() last let go of: one asked for again
    /// right away may still be held by the connections it had.
    int closed_port = 0;

    /// Everything below.
    mutable std::mutex mutex;
    Settings           settings;
    std::string        key_digest;    ///< the key's, empty when the server is open
    std::string        error;
    int                port = 0;      ///< the one listened on
    std::int64_t       since = 0;     ///< when listening began, for the models list
    std::unique_ptr<httplib::Server>   http;
    std::thread                        listener;
    std::shared_ptr<std::atomic<bool>> closing;   ///< the listener's own: set when it is going

    std::deque<std::shared_ptr<Job>> queue;
    std::shared_ptr<Job>             current;
    std::condition_variable          arrived;  ///< a job was queued, or it is time to go
    std::thread                      worker;
    std::atomic<bool>                quitting{false};

    std::deque<Record> recent;   ///< newest first

    mutable std::string       lan;
    mutable Clock::time_point lan_at{};

    void tell() const {
        const std::lock_guard<std::mutex> lock(reaching);
        if (!detached && changed) {
            changed();
        }
    }

    /// The id served, as the requests' threads ask it: empty once stopped.
    std::string model_now() const {
        const std::lock_guard<std::mutex> lock(reaching);
        return detached ? std::string() : backend.model();
    }

    void file(Record record) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            recent.push_front(std::move(record));
            while (recent.size() > kRecent) {
                recent.pop_back();
            }
        }
        tell();
    }

    // --- the thread that answers ---------------------------------------------

    void work() {
        for (;;) {
            std::shared_ptr<Job> job;
            {
                std::unique_lock<std::mutex> lock(mutex);
                arrived.wait(lock, [this] { return !queue.empty() || quitting.load(); });
                if (quitting.load()) {
                    return;
                }
                job = std::move(queue.front());
                queue.pop_front();
                if (job->dropped.load()) {
                    continue;   // its client gave up while it waited
                }
                current = job;
            }
            tell();

            Sink sink;
            sink.answer    = [&job](std::string_view text) { job->add(false, text); };
            sink.reasoning = [&job](std::string_view text) { job->add(true, text); };
            sink.cancel    = [this, &job] { return job->dropped.load() || quitting.load(); };
            sink.waiting   = [this, &job](const std::string& what) {
                {
                    const std::lock_guard<std::mutex> lock(job->mutex);
                    job->waiting = what;
                    if (what.empty()) {
                        job->started = true;
                    }
                }
                job->moved.notify_all();
                tell();
            };

            Answer answer;
            try {
                answer = backend.answer(job->ask, sink);
            } catch (const std::exception& e) {
                answer.error  = e.what();
                answer.status = 500;
            } catch (...) {
                answer.error  = "the model failed";
                answer.status = 500;
            }
            {
                const std::lock_guard<std::mutex> lock(job->mutex);
                job->answer = std::move(answer);
                job->done   = true;
            }
            job->moved.notify_all();
            {
                const std::lock_guard<std::mutex> lock(mutex);
                current.reset();
            }
            tell();
        }
    }

    void start_worker() {
        const std::lock_guard<std::mutex> lock(mutex);
        if (!worker.joinable()) {
            quitting = false;
            worker   = std::thread(&Impl::work, this);
        }
    }

    // --- a request's thread --------------------------------------------------

    /// Wait until `ready` holds for `job`, looking up every so often to see
    /// whether the client and the listener are still there. False, and the
    /// job dropped, when either went.
    template <typename Ready>
    bool wait_for(Job& job, std::unique_lock<std::mutex>& lock, const Ready& ready,
                  const httplib::Request& req, const std::atomic<bool>& gone) {
        while (!ready()) {
            if (gone.load() || quitting.load() || req.is_connection_closed()) {
                job.dropped = true;
                return false;
            }
            job.moved.wait_for(lock, kLook);
        }
        return true;
    }

    void models(const httplib::Request& req, httplib::Response& res) {
        const std::string id = model_now();
        std::int64_t created = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            created = since;
        }
        const auto model_json = [&] {
            return json{{"id", id}, {"object", "model"}, {"created", created}, {"owned_by", "crucible"}};
        };
        if (req.matches.size() > 1) {
            const std::string named = req.matches[1];
            if (id.empty() || !names(id, named)) {
                refuse(res, 404, "The model `" + named + "` does not exist", "model_not_found", "model");
                return;
            }
            res.set_content(dump(model_json()), "application/json");
            return;
        }
        json data = json::array();
        if (!id.empty()) {
            data.push_back(model_json());
        }
        res.set_content(dump(json{{"object", "list"}, {"data", std::move(data)}}), "application/json");
    }

    void complete(const httplib::Request& req, httplib::Response& res, bool chat,
                  const std::shared_ptr<std::atomic<bool>>& gone) {
        const auto began = Clock::now();
        Record record;
        record.at   = now_seconds();
        record.path = req.path;
        record.from = req.remote_addr;
        const auto refused = [&](int status, std::string_view message, std::string_view code = {},
                                 std::string_view param = {}) {
            refuse(res, status, message, code, param);
            record.status = status;
            record.ms     = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::now() - began).count());
            file(record);
        };

        const json body = json::parse(req.body, nullptr, /*allow_exceptions=*/false);
        if (body.is_discarded() || !body.is_object()) {
            refused(400, "The body is not a JSON object");
            return;
        }
        auto job = std::make_shared<Job>();
        std::string param;
        std::string why = chat ? read_chat(body, job->ask, param) : read_text(body, job->ask, param);
        if (why.empty()) {
            why = read_shared(body, job->ask, job->shape, param);
        }
        if (!why.empty()) {
            refused(400, why, {}, param);
            return;
        }

        const std::string served = model_now();
        record.model             = served;
        record.stream            = job->shape.stream;
        const auto named = body.find("model");
        if (named != body.end() && !named->is_null() && !named->is_string()) {
            refused(400, "model must be a string", {}, "model");
            return;
        }
        if (served.empty()) {
            refused(503, "No model is being served", "model_not_available");
            return;
        }
        // A request that names no model is given the one there is, as local
        // servers do. One that names another is told it is not here.
        if (named != body.end() && named->is_string() && !named->get<std::string>().empty()
            && !names(served, named->get<std::string>())) {
            refused(404, "The model `" + named->get<std::string>() + "` does not exist. This server has `"
                             + served + "`.",
                    "model_not_found", "model");
            return;
        }
        job->ask.model = served;

        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (queue.size() >= kMostWaiting) {
                // Outside the lock below: refused() files the record, which
                // takes it.
                why = "Too many requests are waiting";
            } else {
                queue.push_back(job);
            }
        }
        if (!why.empty()) {
            refused(429, why, "rate_limit_exceeded");
            return;
        }
        arrived.notify_all();
        tell();

        // The model is the request's, or there is a refusal: either way the
        // status line can be decided now. Until then nothing is sent, so a
        // refusal still goes out as one.
        // A wait that ended with no answer: the client went, and nobody is
        // there to tell, or the server is stopping or moving, which the
        // client is told.
        const auto given_up = [&] {
            if (req.is_connection_closed()) {
                record.status = 499;   // nginx's word for a client that went before the answer
                record.ms     = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                      Clock::now() - began).count());
                file(record);
            } else {
                refused(503, "The server stopped", "server_stopped");
            }
        };
        {
            std::unique_lock<std::mutex> lock(job->mutex);
            if (!wait_for(*job, lock, [&] { return job->started || job->done; }, req, *gone)) {
                lock.unlock();
                given_up();
                return;
            }
            if (job->done && !job->started) {
                const Answer failed = job->answer;
                lock.unlock();
                refused(failed.error.empty() ? 500 : failed.status,
                        failed.error.empty() ? "The model gave no answer" : failed.error, failed.code);
                return;
            }
        }

        const std::string id      = new_id(chat ? "chatcmpl-" : "cmpl-");
        const std::int64_t created = now_seconds();
        if (job->shape.stream) {
            stream(res, job, chat, id, created, served, record, began);
            return;
        }

        // --- all at once ---------------------------------------------------
        Stops       stops(job->shape.stops);
        std::string content;
        std::string reasoning;
        for (;;) {
            std::deque<Job::Piece> pieces;
            bool done = false;
            {
                std::unique_lock<std::mutex> lock(job->mutex);
                if (!wait_for(*job, lock, [&] { return !job->pieces.empty() || job->done; }, req, *gone)) {
                    lock.unlock();
                    given_up();
                    return;
                }
                pieces.swap(job->pieces);
                done = job->done;
            }
            for (const Job::Piece& piece : pieces) {
                if (piece.reasoning) {
                    reasoning += piece.text;
                } else if (!stops.met()) {
                    content += stops.feed(piece.text);
                    if (stops.met()) {
                        job->dropped = true;   // the rest is not wanted: stop the model
                    }
                }
            }
            if (done) {
                break;
            }
        }
        content += stops.flush();
        const Answer answer = [&] {
            const std::lock_guard<std::mutex> lock(job->mutex);
            return job->answer;
        }();
        if (!answer.error.empty() && !stops.met()) {
            refused(answer.status, answer.error, answer.code);
            return;
        }
        const char* finish = !stops.met() && answer.hit_limit ? "length" : "stop";
        json choice;
        if (chat) {
            json message{{"role", "assistant"}, {"content", content}};
            if (!reasoning.empty()) {
                message["reasoning_content"] = reasoning;
            }
            choice = json{{"index", 0}, {"message", std::move(message)}, {"logprobs", nullptr},
                          {"finish_reason", finish}};
        } else {
            choice = json{{"index", 0}, {"text", content}, {"logprobs", nullptr}, {"finish_reason", finish}};
        }
        json reply{{"id", id},
                   {"object", chat ? "chat.completion" : "text_completion"},
                   {"created", created},
                   {"model", served},
                   {"system_fingerprint", fingerprint()},
                   {"choices", json::array({std::move(choice)})},
                   {"usage", usage_json(answer)}};
        res.set_content(dump(reply), "application/json");
        record.prompt_tokens     = answer.prompt_tokens;
        record.completion_tokens = answer.completion_tokens;
        record.ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          Clock::now() - began).count());
        file(record);
    }

    /// The answer as server-sent events, a piece at a time: a first event
    /// that says who is speaking, then the reasoning and the content as they
    /// come, then why it ended, the usage when asked for, and [DONE].
    void stream(httplib::Response& res, const std::shared_ptr<Job>& job, bool chat, const std::string& id,
                std::int64_t created, const std::string& model, const Record& record, Clock::time_point began) {
        struct Writer {
            std::shared_ptr<Job> job;
            bool                 chat = true;
            std::string          id;
            std::int64_t         created = 0;
            std::string          model;
            Stops                stops;
            Record               record;
            Clock::time_point    began;
            bool                 opened = false;
            bool                 filed  = false;

            json chunk(json said, const json& finish) const {
                json choice = chat ? json{{"index", 0}, {"delta", std::move(said)}}
                                   : json{{"index", 0}, {"text", std::move(said)}};
                choice["logprobs"]      = nullptr;
                choice["finish_reason"] = finish;
                return json{{"id", id},
                            {"object", chat ? "chat.completion.chunk" : "text_completion"},
                            {"created", created},
                            {"model", model},
                            {"system_fingerprint", fingerprint()},
                            {"choices", json::array({std::move(choice)})}};
            }
            static std::string event(const json& data) { return "data: " + dump(data) + "\n\n"; }
            std::string text(const std::string& said) const {
                return event(chunk(chat ? json{{"content", said}} : json(said), nullptr));
            }
        };
        auto writer      = std::make_shared<Writer>(Writer{job, chat, id, created, model,
                                                           Stops(job->shape.stops), record, began});
        Impl* const self = this;

        res.set_header("Cache-Control", "no-cache");
        res.set_header("X-Accel-Buffering", "no");
        res.set_chunked_content_provider(
            "text/event-stream",
            [writer, self](std::size_t, httplib::DataSink& sink) {
                Writer&     w = *writer;
                std::string out;
                if (!w.opened) {
                    w.opened = true;
                    if (w.chat) {
                        out += Writer::event(w.chunk(json{{"role", "assistant"}, {"content", ""}}, nullptr));
                    }
                }
                std::deque<Job::Piece> pieces;
                bool                   done = false;
                Answer                 answer;
                {
                    std::unique_lock<std::mutex> lock(w.job->mutex);
                    w.job->moved.wait_for(lock, kLook, [&] { return !w.job->pieces.empty() || w.job->done; });
                    pieces.swap(w.job->pieces);
                    done = w.job->done;
                    if (done) {
                        answer = w.job->answer;
                    }
                }
                for (const Job::Piece& piece : pieces) {
                    if (piece.reasoning) {
                        if (w.chat) {
                            out += Writer::event(w.chunk(json{{"reasoning_content", piece.text}}, nullptr));
                        }
                        continue;
                    }
                    if (w.stops.met()) {
                        continue;
                    }
                    if (const std::string safe = w.stops.feed(piece.text); !safe.empty()) {
                        out += w.text(safe);
                    }
                    if (w.stops.met()) {
                        w.job->dropped = true;   // the rest is not wanted: stop the model
                    }
                }
                if (!done) {
                    return out.empty() || sink.write(out.data(), out.size());
                }

                if (const std::string tail = w.stops.flush(); !tail.empty()) {
                    out += w.text(tail);
                }
                if (!answer.error.empty() && !w.stops.met()) {
                    // A failure partway, said in the stream: the status line
                    // went out with the first piece.
                    out += Writer::event(error_json(answer.error, answer.status, answer.code, {}));
                } else {
                    const char* finish = !w.stops.met() && answer.hit_limit ? "length" : "stop";
                    out += Writer::event(w.chunk(w.chat ? json::object() : json(""), finish));
                    if (w.job->shape.usage) {
                        json usage    = w.chunk(json::object(), nullptr);
                        usage["choices"] = json::array();
                        usage["usage"]   = usage_json(answer);
                        out += Writer::event(usage);
                    }
                }
                out += "data: [DONE]\n\n";
                w.record.prompt_tokens     = answer.prompt_tokens;
                w.record.completion_tokens = answer.completion_tokens;
                if (!sink.write(out.data(), out.size())) {
                    return false;
                }
                sink.done();
                w.record.ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    Clock::now() - w.began).count());
                w.filed = true;
                self->file(w.record);
                return true;
            },
            [writer, self](bool ok) {
                if (ok) {
                    return;
                }
                // The client went, or the listener did, partway through.
                writer->job->dropped = true;
                if (!writer->filed) {
                    writer->filed         = true;
                    writer->record.status = 499;
                    writer->record.ms     = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                                  Clock::now() - writer->began).count());
                    self->file(writer->record);
                }
            });
    }

    // --- listening -------------------------------------------------------------

    void routes(httplib::Server& routed, const std::shared_ptr<std::atomic<bool>>& gone) {
        // Every response a browser's page may read: a web client on another
        // origin is a client like any other, and the key is what guards the
        // server, not where the page came from.
        routed.set_default_headers({{"Access-Control-Allow-Origin", "*"}});
        routed.set_tcp_nodelay(true);   // a token is sent as it comes, not when a packet fills
        routed.set_payload_max_length(Server::kLargestBody);
        // Only this server on its port. httplib's own choice is SO_REUSEPORT
        // where there is one, which would let a second server -- LM Studio,
        // llama-server, another Crucible -- share the port with this one,
        // each taking every other connection, rather than say it is taken.
        routed.set_socket_options([](socket_t sock) {
#if defined(_WIN32)
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
            httplib::set_socket_opt(sock, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
        });

        // Too large, said before the body is sent: a client that waits for a
        // 100 Continue is told now rather than after four megabytes.
        routed.set_expect_100_continue_handler([](const httplib::Request& req, httplib::Response& res) {
            if (req.get_header_value_u64("Content-Length") > Server::kLargestBody) {
                refuse(res, 413, "The request is larger than this server takes", "request_too_large");
                return 413;
            }
            return 100;
        });

        // Before anything is routed: the browser's preflight, the key, and
        // the size of what is coming. A request refused here never has its
        // body read.
        routed.set_pre_routing_handler([this](const httplib::Request& req, httplib::Response& res) {
            if (req.method == "OPTIONS") {
                res.status = 204;
                res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
                const std::string asked = req.get_header_value("Access-Control-Request-Headers");
                res.set_header("Access-Control-Allow-Headers", asked.empty() ? "Authorization, Content-Type" : asked);
                res.set_header("Access-Control-Max-Age", "600");
                return httplib::Server::HandlerResponse::Handled;
            }
            std::string digest;
            {
                const std::lock_guard<std::mutex> lock(mutex);
                digest = key_digest;
            }
            if (!digest.empty()) {
                const std::string key = bearer(req);
                if (!same_digest(digest_of(key), digest)) {
                    refuse(res, 401, key.empty() ? "This server needs an API key: Authorization: Bearer <key>"
                                                 : "Incorrect API key provided",
                           "invalid_api_key");
                    if (req.path == kChatPath || req.path == kTextPath) {
                        Record record;
                        record.at     = now_seconds();
                        record.path   = req.path;
                        record.from   = req.remote_addr;
                        record.status = 401;
                        file(record);
                    }
                    return httplib::Server::HandlerResponse::Handled;
                }
            }
            if (req.get_header_value_u64("Content-Length") > Server::kLargestBody) {
                refuse(res, 413, "The request is larger than this server takes", "request_too_large");
                return httplib::Server::HandlerResponse::Handled;
            }
            return httplib::Server::HandlerResponse::Unhandled;
        });

        routed.Get("/v1/models", [this](const httplib::Request& req, httplib::Response& res) { models(req, res); });
        routed.Get(R"(/v1/models/([^/]+))",
                 [this](const httplib::Request& req, httplib::Response& res) { models(req, res); });
        routed.Post(kChatPath, [this, gone](const httplib::Request& req, httplib::Response& res) {
            complete(req, res, true, gone);
        });
        routed.Post(kTextPath, [this, gone](const httplib::Request& req, httplib::Response& res) {
            complete(req, res, false, gone);
        });

        // Whatever else went wrong, in OpenAI's shape too: a path that is not
        // one of the four, a body cut off or too large.
        routed.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
            if (!res.body.empty()) {
                return httplib::Server::HandlerResponse::Unhandled;   // one of ours, already said
            }
            if (res.status == 404) {
                refuse(res, 404, "Unknown request URL: " + req.method + " " + req.path, "unknown_url");
            } else if (res.status == 413) {
                refuse(res, 413, "The request is larger than this server takes", "request_too_large");
            } else {
                refuse(res, res.status, httplib::status_message(res.status));
            }
            return httplib::Server::HandlerResponse::Handled;
        });
        routed.set_exception_handler([](const httplib::Request&, httplib::Response& res, std::exception_ptr thrown) {
            std::string what = "the server failed";
            try {
                if (thrown) {
                    std::rethrow_exception(thrown);
                }
            } catch (const std::exception& e) {
                what = e.what();
            } catch (...) {
            }
            refuse(res, 500, what);
        });
    }

    /// Listen as `wanted` says. Returns why it could not.
    std::string open(const Settings& wanted) {
        auto made = std::make_unique<httplib::Server>();
        auto gone = std::make_shared<std::atomic<bool>>(false);
        routes(*made, gone);

        const std::string host = wanted.network ? "0.0.0.0" : "127.0.0.1";
        int bound = wanted.port;
        if (bound == 0) {
            bound = made->bind_to_any_port(host);
            if (bound <= 0) {
                return "No free port";
            }
        } else {
            // The listener just let go of this port may have had connections
            // that are still ending, and Windows keeps a port that had one to
            // itself until they have: a moment, not a refusal.
            bool taken = !made->bind_to_port(host, bound);
            for (int tries = 0; taken && bound == closed_port && tries < 10; ++tries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                taken = !made->bind_to_port(host, bound);
            }
            if (taken) {
                return "Port " + std::to_string(bound) + (bound < 1024 ? " needs administrator rights" : " is in use");
            }
        }
        httplib::Server* listening = made.get();
        std::thread thread([listening] { listening->listen_after_bind(); });
        listening->wait_until_ready();

        const std::lock_guard<std::mutex> lock(mutex);
        this->http     = std::move(made);
        this->listener = std::move(thread);
        this->closing  = std::move(gone);
        this->port     = bound;
        this->since    = now_seconds();
        return {};
    }

    /// Stop listening, ending what is in flight on the way.
    ///
    /// Without waiting for it to end. The listener's thread is joined -- and
    /// with it every request's -- on a thread of its own, which keeps this
    /// alive until then: a request's thread reading a request still arriving
    /// does not look up until a byte comes, and a client that sends one a
    /// minute would otherwise hold whoever is closing for as long as it
    /// likes. That is the window, when a setting changes, and Crucible, when
    /// it closes. A request's thread that outlives stop() finds the fence up.
    void close() {
        std::unique_ptr<httplib::Server>   http_was;
        std::thread                        listener_was;
        std::shared_ptr<std::atomic<bool>> closing_was;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            http_was     = std::move(http);
            listener_was = std::move(listener);
            closing_was  = std::move(closing);
            closed_port  = http_was ? port : closed_port;
            port         = 0;
        }
        if (closing_was) {
            closing_was->store(true);   // a request's thread waiting on its answer gives up
        }
        if (!http_was) {
            return;
        }
        http_was->stop();   // the port is free again from here
        std::thread([http_was = std::move(http_was), listener_was = std::move(listener_was),
                     keep = shared_from_this()]() mutable {
            if (listener_was.joinable()) {
                listener_was.join();
            }
            http_was.reset();
        }).detach();
    }
};

Server::Server(Backend& backend, std::function<void()> changed)
    : impl_(std::make_shared<Impl>(backend, std::move(changed))) {}

Server::~Server() { stop(); }

std::string Server::apply(const Settings& wanted) {
    Impl& s = *impl_;
    const std::lock_guard<std::mutex> one_at_a_time(s.control);
    bool same_place = false;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        same_place = s.http && wanted.enabled && wanted.port == s.settings.port && wanted.network == s.settings.network;
        s.settings   = wanted;
        s.key_digest = wanted.key.empty() ? std::string() : digest_of(wanted.key);
        s.settings.key.clear();   // the digest is all that is kept
        if (!wanted.enabled) {
            s.error.clear();
        }
    }
    if (same_place) {
        // Only the key changed, if anything: every request reads it afresh.
        s.tell();
        return {};
    }
    s.close();
    std::string error;
    if (wanted.enabled) {
        {
            const std::lock_guard<std::mutex> lock(s.reaching);
            s.detached = false;
        }
        s.start_worker();
        error = s.open(wanted);
    }
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        s.error = error;
    }
    s.tell();
    return error;
}

void Server::stop() {
    Impl& s = *impl_;
    const std::lock_guard<std::mutex> one_at_a_time(s.control);
    s.close();
    std::thread worker;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        s.quitting = true;
        for (const std::shared_ptr<Job>& job : s.queue) {
            job->dropped = true;
        }
        s.queue.clear();
        if (s.current) {
            s.current->dropped = true;
        }
        worker = std::move(s.worker);
        s.settings.enabled = false;
    }
    s.arrived.notify_all();
    if (worker.joinable()) {
        worker.join();
    }
    s.quitting = false;
    // Whatever a connection still open does from here, it reaches neither the
    // backend nor `changed`, which may be gone once this returns.
    const std::lock_guard<std::mutex> lock(s.reaching);
    s.detached = true;
}

Status Server::status() const {
    const Impl& s = *impl_;
    Status out;
    std::shared_ptr<Job> current;
    {
        const std::lock_guard<std::mutex> lock(s.mutex);
        out.enabled   = s.settings.enabled;
        out.listening = s.http != nullptr;
        out.port      = s.http ? s.port : s.settings.port;
        out.network   = s.settings.network;
        out.key       = !s.key_digest.empty();
        out.error     = s.error;
        out.queued    = static_cast<int>(std::count_if(s.queue.begin(), s.queue.end(),
                                                       [](const auto& job) { return !job->dropped.load(); }));
        out.recent.assign(s.recent.begin(), s.recent.end());
        current = s.current;
        if (out.listening && out.network) {
            // Asked again now and then, not on every snapshot: a laptop
            // moves between networks, and the address with it.
            const auto now = Clock::now();
            if (s.lan.empty() || now - s.lan_at > std::chrono::seconds(10)) {
                s.lan    = lan_address();
                s.lan_at = now;
            }
            out.lan = s.lan;
        }
    }
    if (current) {
        const std::lock_guard<std::mutex> lock(current->mutex);
        out.answering = true;
        out.waiting   = current->waiting;
    }
    out.model = s.backend.model();
    return out;
}

json status_json(const Status& status) {
    json addresses = json::object();
    if (status.listening) {
        const std::string port = std::to_string(status.port);
        addresses["local"] = "http://localhost:" + port + "/v1";
        if (status.network && !status.lan.empty()) {
            addresses["network"] = "http://" + status.lan + ":" + port + "/v1";
        }
    }
    json recent = json::array();
    for (const Record& one : status.recent) {
        recent.push_back(json{{"at", one.at}, {"model", one.model}, {"path", one.path}, {"from", one.from},
                              {"status", one.status}, {"prompt", one.prompt_tokens},
                              {"completion", one.completion_tokens}, {"ms", one.ms}, {"stream", one.stream}});
    }
    return json{{"enabled", status.enabled},     {"listening", status.listening}, {"port", status.port},
                {"network", status.network},     {"key", status.key},             {"error", status.error},
                {"model", status.model},         {"answering", status.answering}, {"waiting", status.waiting},
                {"queued", status.queued},       {"addresses", std::move(addresses)},
                {"recent", std::move(recent)}};
}

}  // namespace crucible::serve
