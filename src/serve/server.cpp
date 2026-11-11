// SPDX-License-Identifier: MIT
//
// The API server. See server.hpp for what it is and why it is shaped so.
//
// Three things live here. The HTTP: the routes, the key, CORS, how large a
// body may be, and the addresses it listens on. OpenAI's shapes: a request
// read into an Ask, an answer written back as a completion or as server-sent
// events, tool calls and all, and every refusal as OpenAI's error object,
// which is what a client knows how to read. And the queue: requests waiting
// their turn, and the one thread that hands each to the backend.
#include "crucible/serve/server.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <ctime>
#include <deque>
#include <exception>
#include <map>
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

#include "crucible/util/format.hpp"
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

constexpr const char* kChatPath  = "/v1/chat/completions";
constexpr const char* kTextPath  = "/v1/completions";
constexpr const char* kEmbedPath = "/v1/embeddings";

/// How deep a request's JSON may nest. A conversation is a few levels deep
/// and a tool's schema a dozen at most. What reads them after the parse --
/// the copy, the chat template, the schema made a grammar -- recurses, and a
/// body that is megabytes of brackets would run off the end of the stack.
constexpr int kDeepest = 64;

/// How many texts one embeddings request may carry, as OpenAI's may.
constexpr std::size_t kMostTexts = 2048;

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

/// `address` as text, IPv4 or IPv6, or empty for any other kind.
std::string address_text(const sockaddr* address) {
    char text[INET6_ADDRSTRLEN] = {};
    if (address->sa_family == AF_INET) {
        const auto* four = reinterpret_cast<const sockaddr_in*>(address);
        return inet_ntop(AF_INET, &four->sin_addr, text, sizeof(text)) != nullptr ? std::string(text) : std::string();
    }
    if (address->sa_family == AF_INET6) {
        const auto* six = reinterpret_cast<const sockaddr_in6*>(address);
        return inet_ntop(AF_INET6, &six->sin6_addr, text, sizeof(text)) != nullptr ? std::string(text) : std::string();
    }
    return {};
}

/// Whether another computer could reach this machine at `address`: not the
/// loopback, not the unspecified address, and for IPv6 not a link-local one,
/// which is reached only with this machine's interface named after it.
bool reachable(const std::string& address) {
    const auto starts = [&address](std::string_view prefix) {
        return address.size() >= prefix.size()
            && std::equal(prefix.begin(), prefix.end(), address.begin(), [](char a, char b) {
                   return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
               });
    };
    return !address.empty() && !starts("127.") && address != "0.0.0.0" && address != "::" && address != "::1"
        && !starts("fe8") && !starts("fe9") && !starts("fea") && !starts("feb") && !starts("::ffff:");
}

/// This machine's address on its network, IPv4 or IPv6 as `family` says: the
/// one a packet to the outside would leave from. A datagram socket
/// "connected" to an address nobody owns asks the routing table without
/// sending anything. Where there is no route out, the first interface that
/// is up and has an address another computer could reach. Empty when there
/// is no network at all, or none of that kind.
std::string lan_address(int family) {
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
    const auto sock = ::socket(family, SOCK_DGRAM, 0);
#if defined(_WIN32)
    const bool opened = sock != INVALID_SOCKET;
#else
    const bool opened = sock >= 0;
#endif
    if (opened) {
        // 192.0.2.1 and 2001:db8::1 are kept for documentation: nobody has
        // them, and the route to each is the default one.
        sockaddr_storage outside{};
        socklen_t        size = 0;
        if (family == AF_INET6) {
            auto* six        = reinterpret_cast<sockaddr_in6*>(&outside);
            six->sin6_family = AF_INET6;
            six->sin6_port   = htons(9);
            inet_pton(AF_INET6, "2001:db8::1", &six->sin6_addr);
            size = sizeof(sockaddr_in6);
        } else {
            auto* four       = reinterpret_cast<sockaddr_in*>(&outside);
            four->sin_family = AF_INET;
            four->sin_port   = htons(9);
            inet_pton(AF_INET, "192.0.2.1", &four->sin_addr);
            size = sizeof(sockaddr_in);
        }
        if (::connect(sock, reinterpret_cast<const sockaddr*>(&outside), size) == 0) {
            sockaddr_storage local{};
            socklen_t        length = sizeof(local);
            if (::getsockname(sock, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
                found = address_text(reinterpret_cast<const sockaddr*>(&local));
            }
        }
#if defined(_WIN32)
        closesocket(sock);
#else
        ::close(sock);
#endif
    }
#if !defined(_WIN32)
    if (!reachable(found)) {
        found.clear();
        ifaddrs* all = nullptr;
        if (getifaddrs(&all) == 0) {
            for (const ifaddrs* one = all; one != nullptr && found.empty(); one = one->ifa_next) {
                if (one->ifa_addr == nullptr || one->ifa_addr->sa_family != family
                    || (one->ifa_flags & IFF_UP) == 0 || (one->ifa_flags & IFF_LOOPBACK) != 0) {
                    continue;
                }
                if (const std::string text = address_text(one->ifa_addr); reachable(text)) {
                    found = text;
                }
            }
            freeifaddrs(all);
        }
    }
#endif
    return reachable(found) ? found : std::string();
}

/// Where a request came from, as a person reads it: an IPv4 client of the
/// network's dual-stack socket arrives as an IPv6 address with the IPv4 one
/// inside it, "::ffff:192.168.1.5".
std::string from_address(const std::string& remote) {
    constexpr std::string_view mapped = "::ffff:";
    if (remote.size() > mapped.size() && remote.compare(0, mapped.size(), mapped) == 0
        && remote.find('.') != std::string::npos) {
        return remote.substr(mapped.size());
    }
    return remote;
}

/// Whether `value` nests deeper than kDeepest, found without recursing.
bool too_deep(const json& value) {
    std::vector<std::pair<const json*, int>> pending{{&value, 1}};
    while (!pending.empty()) {
        const auto [at, depth] = pending.back();
        pending.pop_back();
        if (depth > kDeepest) {
            return true;
        }
        if (at->is_structured()) {
            for (const json& child : *at) {
                pending.emplace_back(&child, depth + 1);
            }
        }
    }
    return false;
}

/// `object[key]` when it is a string, and empty otherwise: a field of the
/// wrong type is the same as one left out, never an exception.
std::string text_of(const json& object, const char* key) {
    if (!object.is_object()) {
        return {};
    }
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string();
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
        } else if (level == "medium" || level == "high" || level == "none") {
            ask.effort = level;
        }
    }
    return {};
}

/// What a chat completion's answer must be: JSON mode, an object of any
/// shape, or a structured output, one shape given by a schema. Each becomes
/// a grammar the model is held to (see LoadedModel::open_chat).
std::string read_format(const json& body, Ask& ask, std::string& param) {
    const auto format = body.find("response_format");
    if (format == body.end() || format->is_null()) {
        return {};
    }
    param = "response_format";
    if (!format->is_object()) {
        return "response_format must be an object";
    }
    const std::string type = text_of(*format, "type");
    if (type == "text" || type.empty()) {
        return {};
    }
    if (type == "json_object") {
        // llama-server takes a schema here too, which some clients send.
        const auto schema = format->find("schema");
        ask.schema = schema != format->end() && schema->is_object() && !schema->empty() ? dump(*schema)
                                                                                         : R"({"type":"object"})";
        return {};
    }
    if (type == "json_schema") {
        const auto wrapper = format->find("json_schema");
        if (wrapper == format->end() || !wrapper->is_object()) {
            return "response_format.json_schema must be an object with a schema in it";
        }
        const auto schema = wrapper->find("schema");
        if (schema != wrapper->end() && !schema->is_null() && !schema->is_object()) {
            return "response_format.json_schema.schema must be an object";
        }
        ask.schema = schema == wrapper->end() || schema->is_null() || schema->empty() ? R"({"type":"object"})"
                                                                                      : dump(*schema);
        return {};
    }
    return "response_format type \"" + type + "\" is not supported: text, json_object or json_schema";
}

/// The tools a chat completion offers, and how the model may use them, as
/// OpenAI's API has them. A tool here is a function the client runs: the
/// model's call to it goes back in the answer.
std::string read_tools(const json& body, Ask& ask, std::string& param) {
    if (body.contains("functions") || body.contains("function_call")) {
        param = body.contains("functions") ? "functions" : "function_call";
        return "functions and function_call are the old names: send tools and tool_choice";
    }
    const auto tools = body.find("tools");
    if (tools != body.end() && !tools->is_null()) {
        param = "tools";
        if (!tools->is_array()) {
            return "tools must be a list";
        }
        for (const json& tool : *tools) {
            if (!tool.is_object() || text_of(tool, "type") != "function") {
                return "each tool must be {\"type\": \"function\", \"function\": {...}}";
            }
            const auto function = tool.find("function");
            if (function == tool.end() || !function->is_object() || text_of(*function, "name").empty()) {
                return "each tool's function needs a name";
            }
            if (const auto parameters = function->find("parameters");
                parameters != function->end() && !parameters->is_null() && !parameters->is_object()) {
                return "a function's parameters must be a JSON Schema object";
            }
            if (const auto described = function->find("description");
                described != function->end() && !described->is_null() && !described->is_string()) {
                return "a function's description must be a string";
            }
        }
        if (!tools->empty()) {
            ask.tools = *tools;
        }
    }
    if (const auto parallel = body.find("parallel_tool_calls"); parallel != body.end() && !parallel->is_null()) {
        if (!parallel->is_boolean()) {
            param = "parallel_tool_calls";
            return "parallel_tool_calls must be true or false";
        }
        ask.parallel_tool_calls = parallel->get<bool>();
    }
    const auto choice = body.find("tool_choice");
    if (choice == body.end() || choice->is_null()) {
        return {};
    }
    param = "tool_choice";
    if (choice->is_string()) {
        const std::string said = choice->get<std::string>();
        if (said != "auto" && said != "none" && said != "required") {
            return "tool_choice must be auto, none, required or a function";
        }
        if (said == "required" && ask.tools.is_null()) {
            return "tool_choice is required, and no tools are offered";
        }
        ask.tool_choice = said;
        return {};
    }
    // One function, named: that one alone is offered, and must be called.
    const std::string named = choice->is_object() ? text_of(choice->value("function", json::object()), "name")
                                                  : std::string();
    if (named.empty()) {
        return "tool_choice must be auto, none, required or {\"type\": \"function\", \"function\": {\"name\": ...}}";
    }
    json only = json::array();
    for (const json& tool : ask.tools.is_array() ? ask.tools : json::array()) {
        if (const auto function = tool.find("function"); function != tool.end() && text_of(*function, "name") == named) {
            only.push_back(tool);
        }
    }
    if (only.empty()) {
        return "tool_choice names " + named + ", which is not one of the tools";
    }
    ask.tools       = std::move(only);
    ask.tool_choice = "required";
    return {};
}

/// A picture sent as a data: URL, read into `image`. Returns why it cannot
/// be one, or empty. Only data: URLs: a URL on the web would have this
/// machine fetch whatever a client named, on its network as well as the
/// internet's, for anybody who can reach the server.
std::string read_picture(const json& part, ChatImage& image) {
    const auto inner = part.find("image_url");
    std::string url;
    if (inner != part.end() && inner->is_string()) {
        url = inner->get<std::string>();   // the short form some clients send
    } else if (inner != part.end() && inner->is_object()) {
        url = text_of(*inner, "url");
    }
    if (url.empty()) {
        return "an image_url part needs a url";
    }
    constexpr std::string_view scheme = "data:";
    if (url.compare(0, scheme.size(), scheme) != 0) {
        return "pictures are taken as data: URLs only (data:image/png;base64,...): this server fetches nothing";
    }
    const std::size_t comma = url.find(',');
    const std::string head  = comma == std::string::npos ? std::string() : url.substr(scheme.size(), comma - scheme.size());
    constexpr std::string_view base64 = ";base64";
    if (comma == std::string::npos || head.size() < base64.size()
        || head.compare(head.size() - base64.size(), base64.size(), base64) != 0) {
        return "a picture's data: URL must be base64";
    }
    std::string mime = head.substr(0, head.size() - base64.size());
    std::transform(mime.begin(), mime.end(), mime.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (mime.rfind("image/", 0) != 0) {
        return "a picture's data: URL must be an image, not " + (mime.empty() ? std::string("untyped data") : mime);
    }
    std::string bytes;
    if (!format::from_base64(std::string_view(url).substr(comma + 1), bytes) || bytes.empty()) {
        return "a picture's data: URL is not base64";
    }
    image.mime = mime == "image/jpg" ? "image/jpeg" : mime;
    image.data = url.substr(comma + 1);
    return {};
}

/// A message's content: its text, the pictures that came with it, and the
/// content in OpenAI's shape for Ask::chat, each picture's part a
/// {"type": "media_marker"} where it was. `pictures` says whether the
/// message's role may carry any.
std::string read_content(const json& content, ChatMessage& message, json& shaped, bool pictures) {
    message.content.clear();
    shaped = content.is_string() ? content : json(nullptr);
    if (content.is_null() || content.is_string()) {
        message.content = content.is_string() ? content.get<std::string>() : std::string();
        return {};
    }
    if (!content.is_array()) {
        return "a message's content must be a string or a list of parts";
    }
    shaped = json::array();
    for (const json& part : content) {
        if (!part.is_object()) {
            return "a content part must be an object";
        }
        const std::string type = text_of(part, "type");
        if (type == "text") {
            const auto said = part.find("text");
            if (said == part.end() || !said->is_string()) {
                return "a text part needs its text";
            }
            message.content += said->get<std::string>();
            shaped.push_back(json{{"type", "text"}, {"text", *said}});
            continue;
        }
        if (type == "image_url" && pictures) {
            ChatImage image;
            if (const std::string why = read_picture(part, image); !why.empty()) {
                return why;
            }
            message.images.push_back(std::move(image));
            shaped.push_back(json{{"type", "media_marker"}});
            continue;
        }
        if (type == "image_url") {
            return "pictures are taken in a user's message only";
        }
        if (type == "input_audio" || type == "file") {
            return "content of type \"" + type + "\" is not supported: text and pictures are";
        }
        return "content of type \"" + type + "\" is not supported";
    }
    return {};
}

/// The calls an assistant's message made, as Ask::chat keeps them: OpenAI's
/// shape, the arguments a string of JSON as OpenAI sends them.
std::string read_calls(const json& calls, json& shaped) {
    if (!calls.is_array()) {
        return "tool_calls must be a list";
    }
    shaped = json::array();
    for (const json& call : calls) {
        const auto function = call.is_object() ? call.find("function") : call.end();
        if (!call.is_object() || function == call.end() || !function->is_object()
            || text_of(*function, "name").empty()) {
            return "each tool call needs a function with a name";
        }
        const auto arguments = function->find("arguments");
        json said{{"type", "function"},
                  {"function", {{"name", text_of(*function, "name")},
                                {"arguments", arguments == function->end() || arguments->is_null() ? std::string("{}")
                                              : arguments->is_string() ? arguments->get<std::string>()
                                                                       : dump(*arguments)}}}};
        if (const std::string id = text_of(call, "id"); !id.empty()) {
            said["id"] = id;
        }
        shaped.push_back(std::move(said));
    }
    return {};
}

std::string read_chat(const json& body, Ask& ask, std::string& param) {
    const auto messages = body.find("messages");
    if (messages == body.end() || !messages->is_array() || messages->empty()) {
        param = "messages";
        return "messages must be a list with at least one message";
    }
    param = "messages";
    ask.chat = json::array();
    for (const json& one : *messages) {
        const std::string role = text_of(one, "role");
        if (role.empty()) {
            return "each message needs a role";
        }
        if (role != "system" && role != "developer" && role != "user" && role != "assistant" && role != "tool") {
            return "role \"" + role + "\" is not supported";
        }
        ChatMessage message;
        // OpenAI's newer name for the system message, which is what it is to
        // a model asked the way the chat asks one. The template has it as it
        // came: gpt-oss's has a developer message of its own.
        message.role = role == "developer" ? "system" : role;
        json shaped{{"role", role}};
        json content;
        if (const std::string why = read_content(one.value("content", json(nullptr)), message, content, role == "user");
            !why.empty()) {
            return why;
        }
        shaped["content"] = content;
        if (role == "assistant") {
            if (const auto calls = one.find("tool_calls"); calls != one.end() && !calls->is_null()) {
                json said;
                if (const std::string why = read_calls(*calls, said); !why.empty()) {
                    return why;
                }
                if (!said.empty()) {
                    shaped["tool_calls"] = std::move(said);
                }
            }
            if (const std::string reasoning = text_of(one, "reasoning_content"); !reasoning.empty()) {
                shaped["reasoning_content"] = reasoning;
            }
            if (content.is_null() && !shaped.contains("tool_calls")) {
                return "an assistant message needs content or tool_calls";
            }
        } else if (content.is_null()) {
            return "a " + role + " message needs content";
        }
        if (role == "tool") {
            const std::string id = text_of(one, "tool_call_id");
            if (id.empty()) {
                return "a tool message needs the tool_call_id of the call it answers";
            }
            shaped["tool_call_id"] = id;
            if (const std::string name = text_of(one, "name"); !name.empty()) {
                shaped["name"] = name;
            }
        }
        ask.messages.push_back(std::move(message));
        ask.chat.push_back(std::move(shaped));
    }
    param.clear();
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

    /// What has come and the request's thread has not taken yet, in the
    /// order it came: text, or a piece of a tool call.
    struct Piece {
        bool        reasoning = false;
        std::string text;
        bool        is_call = false;
        CallPiece   call;
    };
    std::deque<Piece> pieces;

    bool        started = false;   ///< the model is the request's: no refusal comes after this
    bool        done    = false;
    Answer      answer;
    std::string waiting;

    /// When a call is required, the reply is the call alone, as OpenAI
    /// sends it: words the model says around it are held, and dropped once
    /// a call comes. A call that never comes (cut short, say) gives them back.
    std::string held;
    bool        called = false;

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
            if (!reasoning && ask.tool_choice == "required") {
                if (!called) {
                    held += text;
                }
            } else if (!pieces.empty() && !pieces.back().is_call && pieces.back().reasoning == reasoning) {
                pieces.back().text += text;
            } else {
                Piece piece;
                piece.reasoning = reasoning;
                piece.text      = std::string(text);
                pieces.push_back(std::move(piece));
            }
        }
        moved.notify_all();
    }

    void add_call(const CallPiece& call) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            started = true;
            called  = true;
            held.clear();
            Piece piece;
            piece.is_call = true;
            piece.call    = call;
            pieces.push_back(std::move(piece));
        }
        moved.notify_all();
    }

    /// The model is through: its answer, after any words still held.
    void finish(Answer&& given) {
        {
            const std::lock_guard<std::mutex> lock(mutex);
            if (!held.empty()) {
                Piece piece;
                piece.text = std::move(held);
                pieces.push_back(std::move(piece));
                held.clear();
            }
            answer = std::move(given);
            done   = true;
        }
        moved.notify_all();
    }
};

/// A tool call put together from its pieces: OpenAI's shape of it, in a
/// complete answer.
struct Calls {
    std::vector<json> calls;

    void add(const CallPiece& piece) {
        const auto at = static_cast<std::size_t>(std::max(0, piece.index));
        while (calls.size() <= at) {
            calls.push_back(json{{"id", ""}, {"type", "function"}, {"function", {{"name", ""}, {"arguments", ""}}}});
        }
        json& call = calls[at];
        if (!piece.id.empty()) {
            call["id"] = piece.id;
        }
        if (!piece.name.empty()) {
            call["function"]["name"] = piece.name;
        }
        call["function"]["arguments"] = call["function"]["arguments"].get<std::string>() + piece.arguments;
    }

    bool empty() const { return calls.empty(); }
};

/// A tool call's piece as a stream's delta has it: the first of a call with
/// its id, its type and its name, the rest with more of its arguments.
json call_delta(const CallPiece& piece) {
    json function{{"arguments", piece.arguments}};
    json said{{"index", piece.index}};
    if (!piece.name.empty()) {
        function["name"] = piece.name;
        said["id"]       = piece.id;
        said["type"]     = "function";
    }
    said["function"] = std::move(function);
    return json{{"tool_calls", json::array({std::move(said)})}};
}

/// Why an answer ended, in OpenAI's word for it: a stop sequence met, the
/// limit reached, a call made, or the model done.
const char* finish_reason(bool stopped, bool limit, bool called) {
    if (stopped) {
        return "stop";
    }
    if (limit) {
        return "length";
    }
    return called ? "tool_calls" : "stop";
}

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

    /// A listener for each address listened on, and its thread: one on
    /// this machine for 127.0.0.1 and one for ::1, and one on the network
    /// for both kinds at once where there is IPv6 (see open).
    std::vector<std::unique_ptr<httplib::Server>> http;
    std::vector<std::thread>                      listeners;
    bool                                          ipv6 = false;   ///< one of them listens on IPv6
    std::shared_ptr<std::atomic<bool>>            closing;        ///< the listeners' own: set when they are going

    std::deque<std::shared_ptr<Job>> queue;
    std::shared_ptr<Job>             current;
    std::condition_variable          arrived;  ///< a job was queued, or it is time to go
    std::thread                      worker;
    std::atomic<bool>                quitting{false};

    std::deque<Record> recent;   ///< newest first

    mutable std::string       lan;
    mutable std::string       lan6;
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

    /// The id embeddings come from, as the requests' threads ask it.
    std::string embedder_now() const {
        const std::lock_guard<std::mutex> lock(reaching);
        return detached ? std::string() : backend.embedder();
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
            sink.call      = [&job](const CallPiece& piece) { job->add_call(piece); };
            // The server stopping under an answer leaves it cut short, which
            // the backend cannot tell from a client that went or a stop
            // sequence met. Noted here, so it goes out as the failure it is
            // rather than as a whole answer. Atomic: a backend may ask from
            // a thread of its compute's.
            std::atomic<bool> stopped_under{false};
            sink.cancel = [this, &job, &stopped_under] {
                if (quitting.load()) {
                    stopped_under = true;
                    return true;
                }
                return job->dropped.load();
            };
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
            if (stopped_under.load() && answer.error.empty()) {
                answer.error  = "The server stopped";
                answer.status = 503;
                answer.code   = "server_stopped";
            }
            job->finish(std::move(answer));
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
        // The model served, and the one embeddings come from when that is
        // another: a client that lists the models picks each from here.
        std::vector<std::string> ids;
        if (const std::string id = model_now(); !id.empty()) {
            ids.push_back(id);
        }
        if (const std::string id = embedder_now(); !id.empty() && std::find(ids.begin(), ids.end(), id) == ids.end()) {
            ids.push_back(id);
        }
        std::int64_t created = 0;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            created = since;
        }
        const auto model_json = [&](const std::string& id) {
            return json{{"id", id}, {"object", "model"}, {"created", created}, {"owned_by", "crucible"}};
        };
        if (req.matches.size() > 1) {
            const std::string named = req.matches[1];
            for (const std::string& id : ids) {
                if (names(id, named)) {
                    res.set_content(dump(model_json(id)), "application/json");
                    return;
                }
            }
            refuse(res, 404, "The model `" + named + "` does not exist", "model_not_found", "model");
            return;
        }
        json data = json::array();
        for (const std::string& id : ids) {
            data.push_back(model_json(id));
        }
        res.set_content(dump(json{{"object", "list"}, {"data", std::move(data)}}), "application/json");
    }

    /// A wait that ended with no answer: the client went, and nobody is
    /// there to tell, or the server is stopping or moving, which the client
    /// is told.
    void given_up(const httplib::Request& req, httplib::Response& res, Record& record, Clock::time_point began) {
        record.status = req.is_connection_closed() ? 499 : 503;   // 499: nginx's word for a client that went
        record.ms     = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                              Clock::now() - began).count());
        if (record.status == 503) {
            refuse(res, 503, "The server stopped", "server_stopped");
        }
        file(record);
    }

    /// Queue `job` and wait until the model is the request's, or it has an
    /// answer without ever having been. False, with the response written and
    /// the record filed, when it went no further: refused, the client gone,
    /// the server stopping.
    bool take_turn(const std::shared_ptr<Job>& job, const httplib::Request& req, httplib::Response& res,
                   const std::atomic<bool>& gone, Record& record, Clock::time_point began) {
        const auto refused = [&](int status, std::string_view message, std::string_view code = {}) {
            refuse(res, status, message, code);
            record.status = status;
            record.ms     = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  Clock::now() - began).count());
            file(record);
        };
        bool full = false;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            full = queue.size() >= kMostWaiting;
            if (!full) {
                queue.push_back(job);
            }
        }
        if (full) {
            // Outside the lock above: refused() files the record, which takes it.
            refused(429, "Too many requests are waiting", "rate_limit_exceeded");
            return false;
        }
        arrived.notify_all();
        tell();

        std::unique_lock<std::mutex> lock(job->mutex);
        if (!wait_for(*job, lock, [&] { return job->started || job->done; }, req, gone)) {
            lock.unlock();
            given_up(req, res, record, began);
            return false;
        }
        if (job->done && !job->started) {
            const Answer failed = job->answer;
            lock.unlock();
            refused(failed.error.empty() ? 500 : failed.status,
                    failed.error.empty() ? "The model gave no answer" : failed.error, failed.code);
            return false;
        }
        return true;
    }

    /// /v1/embeddings: each text a vector, from the model chosen for them or
    /// the one served, as floats or as base64 of their bytes.
    void embeddings(const httplib::Request& req, httplib::Response& res,
                    const std::shared_ptr<std::atomic<bool>>& gone) {
        const auto began = Clock::now();
        Record record;
        record.at   = now_seconds();
        record.path = req.path;
        record.from = from_address(req.remote_addr);
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
        if (too_deep(body)) {
            refused(400, "The body nests deeper than " + std::to_string(kDeepest) + " levels");
            return;
        }
        auto job       = std::make_shared<Job>();
        job->ask.embed = true;
        const auto input = body.find("input");
        if (input != body.end() && input->is_string()) {
            job->ask.texts.push_back(input->get<std::string>());
        } else if (input != body.end() && input->is_array() && !input->empty() && input->size() <= kMostTexts) {
            for (const json& one : *input) {
                if (!one.is_string()) {
                    refused(400, "input must be a string or a list of strings: text, not tokens", {}, "input");
                    return;
                }
                job->ask.texts.push_back(one.get<std::string>());
            }
        } else {
            refused(400, "input must be a string or a list of up to " + std::to_string(kMostTexts) + " strings", {},
                    "input");
            return;
        }
        if (std::any_of(job->ask.texts.begin(), job->ask.texts.end(),
                        [](const std::string& text) { return text.empty(); })) {
            refused(400, "input must not be empty", {}, "input");
            return;
        }
        const std::string encoding = text_of(body, "encoding_format");
        if (!encoding.empty() && encoding != "float" && encoding != "base64") {
            refused(400, "encoding_format must be float or base64", {}, "encoding_format");
            return;
        }
        long long dimensions = 0;
        if (const auto asked = body.find("dimensions"); asked != body.end() && !asked->is_null()) {
            if (!asked->is_number_integer() || asked->get<long long>() < 1) {
                refused(400, "dimensions must be a whole number, 1 or more", {}, "dimensions");
                return;
            }
            dimensions = asked->get<long long>();
        }

        // A request that names no model is given the one chosen for
        // embeddings, and one that names the model served is given that.
        const std::string served   = model_now();
        const std::string embedder = embedder_now();
        const auto        named    = body.find("model");
        if (named != body.end() && !named->is_null() && !named->is_string()) {
            refused(400, "model must be a string", {}, "model");
            return;
        }
        std::string chosen = embedder.empty() ? served : embedder;
        if (named != body.end() && named->is_string() && !named->get<std::string>().empty()) {
            const std::string asked = named->get<std::string>();
            chosen = !embedder.empty() && names(embedder, asked) ? embedder
                   : !served.empty() && names(served, asked)     ? served
                                                                 : std::string();
            if (chosen.empty()) {
                refused(404, "The model `" + asked + "` does not exist. This server makes embeddings with `"
                                 + (embedder.empty() ? served : embedder) + "`.",
                        "model_not_found", "model");
                return;
            }
        }
        if (chosen.empty()) {
            refused(503, "No model is being served", "model_not_available");
            return;
        }
        job->ask.model = chosen;
        record.model   = chosen;
        if (!take_turn(job, req, res, *gone, record, began)) {
            return;
        }
        // The vectors come whole, at the end.
        {
            std::unique_lock<std::mutex> lock(job->mutex);
            if (!wait_for(*job, lock, [&] { return job->done; }, req, *gone)) {
                lock.unlock();
                given_up(req, res, record, began);
                return;
            }
        }
        const Answer answer = [&] {
            const std::lock_guard<std::mutex> lock(job->mutex);
            return job->answer;
        }();
        if (!answer.error.empty() || answer.vectors.size() != job->ask.texts.size()) {
            refused(answer.error.empty() ? 500 : answer.status,
                    answer.error.empty() ? "The model gave no embeddings" : answer.error, answer.code);
            return;
        }
        json data = json::array();
        for (std::size_t i = 0; i < answer.vectors.size(); ++i) {
            std::vector<float> vector = answer.vectors[i];
            if (dimensions > 0) {
                if (static_cast<std::size_t>(dimensions) > vector.size()) {
                    refused(400, "This model's embeddings have " + std::to_string(vector.size()) + " dimensions",
                            {}, "dimensions");
                    return;
                }
                // Cut short and scaled back to unit length, as OpenAI's
                // shortened embeddings are.
                vector.resize(static_cast<std::size_t>(dimensions));
                double sum = 0.0;
                for (const float value : vector) {
                    sum += static_cast<double>(value) * static_cast<double>(value);
                }
                if (sum > 0.0) {
                    const double scale = 1.0 / std::sqrt(sum);
                    for (float& value : vector) {
                        value = static_cast<float>(static_cast<double>(value) * scale);
                    }
                }
            }
            json one{{"object", "embedding"}, {"index", i}};
            if (encoding == "base64") {
                // The floats' own bytes, little-endian as every machine
                // Crucible runs on keeps them: what OpenAI's clients decode.
                one["embedding"] = format::base64(std::string_view(reinterpret_cast<const char*>(vector.data()),
                                                                   vector.size() * sizeof(float)));
            } else {
                one["embedding"] = vector;
            }
            data.push_back(std::move(one));
        }
        res.set_content(dump(json{{"object", "list"},
                                  {"data", std::move(data)},
                                  {"model", chosen},
                                  {"usage", {{"prompt_tokens", answer.prompt_tokens},
                                             {"total_tokens", answer.prompt_tokens}}}}),
                        "application/json");
        record.prompt_tokens = answer.prompt_tokens;
        record.ms = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          Clock::now() - began).count());
        file(record);
    }

    void complete(const httplib::Request& req, httplib::Response& res, bool chat,
                  const std::shared_ptr<std::atomic<bool>>& gone) {
        const auto began = Clock::now();
        Record record;
        record.at   = now_seconds();
        record.path = req.path;
        record.from = from_address(req.remote_addr);
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
        if (too_deep(body)) {
            refused(400, "The body nests deeper than " + std::to_string(kDeepest) + " levels");
            return;
        }
        auto job = std::make_shared<Job>();
        std::string param;
        std::string why = chat ? read_chat(body, job->ask, param) : read_text(body, job->ask, param);
        if (why.empty()) {
            why = read_shared(body, job->ask, job->shape, param);
        }
        if (why.empty() && chat) {
            why = read_tools(body, job->ask, param);
        }
        if (why.empty() && chat) {
            why = read_format(body, job->ask, param);
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
        // servers do. One that names another is told it is not here, and one
        // that names the model embeddings come from is told what that is.
        if (named != body.end() && named->is_string() && !named->get<std::string>().empty()
            && !names(served, named->get<std::string>())) {
            const std::string asked    = named->get<std::string>();
            const std::string embedder = embedder_now();
            if (!embedder.empty() && names(embedder, asked)) {
                refused(400, "`" + embedder + "` makes this server's embeddings. It answers with `" + served + "`.",
                        "model_not_found", "model");
            } else {
                refused(404, "The model `" + asked + "` does not exist. This server has `" + served + "`.",
                        "model_not_found", "model");
            }
            return;
        }
        job->ask.model = served;

        // The model is the request's, or there is a refusal: either way the
        // status line can be decided now. Until then nothing is sent, so a
        // refusal still goes out as one.
        if (!take_turn(job, req, res, *gone, record, began)) {
            return;
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
        Calls       calls;
        for (;;) {
            std::deque<Job::Piece> pieces;
            bool done = false;
            {
                std::unique_lock<std::mutex> lock(job->mutex);
                if (!wait_for(*job, lock, [&] { return !job->pieces.empty() || job->done; }, req, *gone)) {
                    lock.unlock();
                    given_up(req, res, record, began);
                    return;
                }
                pieces.swap(job->pieces);
                done = job->done;
            }
            for (const Job::Piece& piece : pieces) {
                if (piece.is_call) {
                    if (!stops.met()) {
                        calls.add(piece.call);
                    }
                } else if (piece.reasoning) {
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
        const char* finish = finish_reason(stops.met(), answer.hit_limit, !calls.empty());
        json choice;
        if (chat) {
            // No words beside a call is null, as OpenAI sends it.
            json message{{"role", "assistant"},
                         {"content", content.empty() && !calls.empty() ? json(nullptr) : json(content)}};
            if (!reasoning.empty()) {
                message["reasoning_content"] = reasoning;
            }
            if (!calls.empty()) {
                message["tool_calls"] = calls.calls;
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
    /// that says who is speaking, then the reasoning, the content and the
    /// tool calls as they come, then why it ended, the usage when asked for,
    /// and [DONE].
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
            bool                 called = false;   ///< a tool call has gone out

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
                    if (piece.is_call) {
                        if (!w.chat || w.stops.met()) {
                            continue;
                        }
                        // What was held back to watch for a stop sequence
                        // goes first: it came first.
                        if (const std::string held = w.stops.flush(); !held.empty()) {
                            out += w.text(held);
                        }
                        w.called = true;
                        out += Writer::event(w.chunk(call_delta(piece.call), nullptr));
                        continue;
                    }
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
                    const char* finish = finish_reason(w.stops.met(), answer.hit_limit, w.called);
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
                    if (req.path == kChatPath || req.path == kTextPath || req.path == kEmbedPath) {
                        Record record;
                        record.at     = now_seconds();
                        record.path   = req.path;
                        record.from   = from_address(req.remote_addr);
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
        routed.Post(kEmbedPath, [this, gone](const httplib::Request& req, httplib::Response& res) {
            embeddings(req, res, gone);
        });

        // Whatever else went wrong, in OpenAI's shape too: a path that is not
        // one of the five, a body cut off or too large.
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
    ///
    /// On the network one socket takes IPv4 and IPv6 both: bound to :: with
    /// IPV6_V6ONLY off, which cpp-httplib sets so on every IPv6 socket it
    /// makes. It has to be said: Windows has it on unless told, which would
    /// leave every IPv4 client out. A machine without IPv6 refuses ::, and
    /// is listened on at 0.0.0.0 instead. This machine is two sockets, there
    /// being no one address that is both 127.0.0.1 and ::1: the second on
    /// the port the first was given, where the machine has IPv6.
    std::string open(const Settings& wanted) {
        auto gone = std::make_shared<std::atomic<bool>>(false);
        const auto made = [&] {
            auto one = std::make_unique<httplib::Server>();
            routes(*one, gone);
            one->set_ipv6_v6only(false);   // said, not left to the library's default
            return one;
        };
        // The first address, which decides the port. The listener just let
        // go of this port may have had connections that are still ending,
        // and Windows keeps a port that had one to itself until they have:
        // a moment, not a refusal.
        const auto bind_first = [&](httplib::Server& server, const std::string& host, int& bound) {
            if (bound == 0) {
                bound = server.bind_to_any_port(host);
                return bound > 0;
            }
            bool taken = !server.bind_to_port(host, bound);
            for (int tries = 0; taken && bound == closed_port && tries < 10; ++tries) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                taken = !server.bind_to_port(host, bound);
            }
            return !taken;
        };
        const auto failed = [&wanted] {
            const int asked = wanted.port;
            return asked == 0 ? std::string("No free port")
                              : "Port " + std::to_string(asked) + (asked < 1024 ? " needs administrator rights"
                                                                                : " is in use");
        };

        std::vector<std::unique_ptr<httplib::Server>> servers;
        bool on_ipv6 = false;
        int  bound   = wanted.port;
        if (wanted.network) {
            auto both = made();
            if (bind_first(*both, "::", bound)) {
                on_ipv6 = true;
                servers.push_back(std::move(both));
            } else {
                auto four = made();
                bound     = wanted.port;
                if (!bind_first(*four, "0.0.0.0", bound)) {
                    return failed();
                }
                servers.push_back(std::move(four));
            }
        } else {
            auto four = made();
            if (!bind_first(*four, "127.0.0.1", bound)) {
                return failed();
            }
            servers.push_back(std::move(four));
            if (auto six = made(); six->bind_to_port("::1", bound)) {
                on_ipv6 = true;
                servers.push_back(std::move(six));
            }
        }
        std::vector<std::thread> threads;
        for (const std::unique_ptr<httplib::Server>& server : servers) {
            httplib::Server* listening = server.get();
            threads.emplace_back([listening] { listening->listen_after_bind(); });
            listening->wait_until_ready();
        }

        const std::lock_guard<std::mutex> lock(mutex);
        this->http      = std::move(servers);
        this->listeners = std::move(threads);
        this->ipv6      = on_ipv6;
        this->closing   = std::move(gone);
        this->port      = bound;
        this->since     = now_seconds();
        return {};
    }

    /// Stop listening, ending what is in flight on the way.
    ///
    /// Without waiting for it to end. The listeners' threads are joined --
    /// and with them every request's -- on a thread of its own, which keeps
    /// this alive until then: a request's thread reading a request still
    /// arriving does not look up until a byte comes, and a client that sends
    /// one a minute would otherwise hold whoever is closing for as long as it
    /// likes. That is the window, when a setting changes, and Crucible, when
    /// it closes. A request's thread that outlives stop() finds the fence up.
    void close() {
        std::vector<std::unique_ptr<httplib::Server>> http_was;
        std::vector<std::thread>                      listeners_was;
        std::shared_ptr<std::atomic<bool>>            closing_was;
        {
            const std::lock_guard<std::mutex> lock(mutex);
            http_was      = std::move(http);
            listeners_was = std::move(listeners);
            closing_was   = std::move(closing);
            http.clear();
            listeners.clear();
            closed_port = http_was.empty() ? closed_port : port;
            port        = 0;
            ipv6        = false;
        }
        if (closing_was) {
            closing_was->store(true);   // a request's thread waiting on its answer gives up
        }
        if (http_was.empty()) {
            return;
        }
        for (const std::unique_ptr<httplib::Server>& server : http_was) {
            server->stop();   // the port is free again from here
        }
        std::thread([http_was = std::move(http_was), listeners_was = std::move(listeners_was),
                     keep = shared_from_this()]() mutable {
            for (std::thread& listener : listeners_was) {
                if (listener.joinable()) {
                    listener.join();
                }
            }
            http_was.clear();
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
        same_place = !s.http.empty() && wanted.enabled && wanted.port == s.settings.port && wanted.network == s.settings.network;
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
        out.listening = !s.http.empty();
        out.port      = !s.http.empty() ? s.port : s.settings.port;
        out.network   = s.settings.network;
        out.ipv6      = s.ipv6;
        out.key       = !s.key_digest.empty();
        out.error     = s.error;
        out.queued    = static_cast<int>(std::count_if(s.queue.begin(), s.queue.end(),
                                                       [](const auto& job) { return !job->dropped.load(); }));
        out.recent.assign(s.recent.begin(), s.recent.end());
        current = s.current;
        if (out.listening) {
            // Asked again now and then, not on every snapshot: a laptop
            // moves between networks, and the address with it. Asked with
            // the network off too, so the page can say where another
            // computer would reach it before it is let in.
            const auto now = Clock::now();
            if (s.lan.empty() || now - s.lan_at > std::chrono::seconds(10)) {
                s.lan    = lan_address(AF_INET);
                s.lan6   = lan_address(AF_INET6);
                s.lan_at = now;
            }
            out.lan  = s.lan;
            out.lan6 = s.lan6;
        }
    }
    if (current) {
        const std::lock_guard<std::mutex> lock(current->mutex);
        out.answering = true;
        out.waiting   = current->waiting;
    }
    out.model = s.backend.model();
    if (std::string embedder = s.backend.embedder(); embedder != out.model) {
        out.embedder = std::move(embedder);
    }
    return out;
}

json status_json(const Status& status) {
    // Where another program reaches it: this machine by name, which is both
    // kinds of address where both are listened on, and ::1 itself then; the
    // network by this machine's address there, and its IPv6 one beside it.
    json addresses = json::object();
    if (status.listening) {
        const std::string port = std::to_string(status.port);
        addresses["local"] = "http://localhost:" + port + "/v1";
        if (status.ipv6) {
            addresses["local6"] = "http://[::1]:" + port + "/v1";
        }
        if (status.network && !status.lan.empty()) {
            addresses["network"] = "http://" + status.lan + ":" + port + "/v1";
        }
        if (status.network && status.ipv6 && !status.lan6.empty()) {
            addresses["network6"] = "http://[" + status.lan6 + "]:" + port + "/v1";
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
                {"embedder", status.embedder},   {"ipv6", status.listening && status.ipv6},
                {"lan", status.listening ? status.lan : std::string()},
                {"lan6", status.listening && status.ipv6 ? status.lan6 : std::string()},
                {"recent", std::move(recent)}};
}

}  // namespace crucible::serve
