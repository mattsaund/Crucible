// SPDX-License-Identifier: MIT
//
// See mcp.hpp.
#include "crucible/tools/mcp.hpp"

#include <chrono>
#include <filesystem>

#include "crucible/config/paths.hpp"
#include "crucible/util/subprocess.hpp"
#include "crucible/util/text.hpp"

namespace crucible::tools::mcp {
namespace {

using json = nlohmann::json;

/// The protocol version this client was written against. A server that
/// speaks a later one answers with its own and stays compatible.
constexpr const char* kProtocol = "2025-06-18";

}  // namespace

// ---------------------------------------------------------------------------
// Reading what a server says
// ---------------------------------------------------------------------------

namespace detail {

std::vector<Tool> tools_from(const std::string& server, const json& result) {
    std::vector<Tool> out;
    if (!result.is_object() || !result.contains("tools") || !result["tools"].is_array()) {
        return out;
    }
    for (const json& one : result["tools"]) {
        if (!one.is_object() || !one.contains("name")) {
            continue;
        }
        Tool tool;
        tool.server      = server;
        tool.name        = one.value("name", "");
        tool.description = one.value("description", "");
        tool.schema      = one.value("inputSchema", json::object());
        out.push_back(std::move(tool));
    }
    return out;
}

Result result_from(const json& result) {
    Result out;
    if (!result.is_object()) {
        out.error = "the server's answer was not an object";
        return out;
    }
    for (const json& part : result.value("content", json::array())) {
        const std::string type = part.value("type", "");
        if (type == "text") {
            if (!out.text.empty()) {
                out.text += "\n";
            }
            out.text += part.value("text", "");
        } else if (type == "image") {
            attach::Image image;
            image.mime = part.value("mimeType", "image/png");
            image.data = part.value("data", "");
            out.images.push_back(std::move(image));
        } else if (type == "resource") {
            const json resource = part.value("resource", json::object());
            if (!out.text.empty()) {
                out.text += "\n";
            }
            out.text += resource.value("text", resource.value("uri", ""));
        }
    }
    // Some servers put their whole answer in structuredContent and nothing
    // in content.
    if (out.text.empty() && result.contains("structuredContent")) {
        out.text = result["structuredContent"].dump(2, ' ', false, json::error_handler_t::replace);
    }
    out.ok = !result.value("isError", false);
    if (!out.ok) {
        out.error = out.text.empty() ? std::string("the tool reported an error") : out.text;
    }
    return out;
}

std::string describe(const Tool& tool) {
    std::string line = tool.server + "/" + tool.name;
    std::string about = tool.description;
    if (const std::size_t end = about.find('\n'); end != std::string::npos) {
        about = about.substr(0, end);
    }
    if (about.size() > 140) {
        about = about.substr(0, 137) + "...";
    }
    if (!about.empty()) {
        line += ": " + about;
    }
    // The arguments, required ones starred, so a model knows what to send
    // without the whole schema in its window.
    const json properties = tool.schema.value("properties", json::object());
    const json required   = tool.schema.value("required", json::array());
    std::string args;
    for (auto it = properties.begin(); it != properties.end(); ++it) {
        if (!args.empty()) {
            args += ", ";
        }
        args += it.key();
        for (const json& name : required) {
            if (name.is_string() && name.get<std::string>() == it.key()) {
                args += "*";
            }
        }
    }
    if (!args.empty()) {
        line += " -- " + args;
    }
    return line;
}

}  // namespace detail

// ---------------------------------------------------------------------------
// One server
// ---------------------------------------------------------------------------

Client::~Client() { stop(); }

bool Client::running() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return child_ && !ended_;
}

std::vector<Tool> Client::tools() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return tools_;
}

bool Client::start(const ServerConfig& config, std::string& error) {
    stop();
    config_ = config;
    std::vector<std::string> argv{config.command};
    argv.insert(argv.end(), config.args.begin(), config.args.end());
#if defined(_WIN32)
    // npx, npm, uvx and the like are .cmd scripts on Windows, and a script is
    // not a program Windows will start -- the command interpreter runs it. A
    // server named "npx", which is how most are written, is handed to cmd.
    if (std::filesystem::path(config.command).extension().empty() && !util::on_path(config.command + ".exe")
        && (util::on_path(config.command + ".cmd") || util::on_path(config.command + ".bat"))) {
        argv.insert(argv.begin(), {"cmd", "/d", "/c"});
    }
#endif
    auto child = std::make_unique<util::Subprocess>();
    util::Subprocess::Streams streams;
    streams.input  = true;
    // What a server prints that is not the protocol goes to a log of its own,
    // never into the middle of a message.
    streams.errors = paths::data_dir() / ("mcp-" + config.name + ".log");
    if (!child->start(argv, {}, config.env, error, streams)) {
        error = config.name + " would not start: " + error;
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child_ = std::move(child);
        ended_ = false;
        answers_.clear();
    }
    reader_ = std::thread([this] { read_all(); });

    const json hello = request("initialize",
                               json{{"protocolVersion", kProtocol},
                                    {"capabilities", json::object()},
                                    {"clientInfo", {{"name", "Crucible"}, {"version", "0.9"}}}},
                               60, error);
    if (hello.is_null()) {
        error = config.name + " did not answer: " + error;
        stop();
        return false;
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child_->write_line(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}.dump());
    }
    const json listed = request("tools/list", json::object(), 30, error);
    if (listed.is_null()) {
        error = config.name + " would not list its tools: " + error;
        stop();
        return false;
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    tools_ = detail::tools_from(config.name, listed);
    return true;
}

void Client::stop() {
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (child_) {
            child_->close_input();
            child_->interrupt();
        }
    }
    if (reader_.joinable()) {
        reader_.join();
    }
    const std::lock_guard<std::mutex> lock(mutex_);
    if (child_) {
        child_->wait();
        child_.reset();
    }
    ended_ = true;
    tools_.clear();
}

void Client::read_all() {
    util::Subprocess* child = nullptr;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        child = child_.get();
    }
    std::string line;
    while (child != nullptr && child->read_line(line)) {
        json message = json::parse(line, nullptr, false);
        if (!message.is_object() || !message.contains("id") || message.contains("method")) {
            continue;   // a notification, a log line, or a request we do not serve
        }
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            answers_.push_back(std::move(message));
        }
        arrived_.notify_all();
    }
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        ended_ = true;
    }
    arrived_.notify_all();
}

json Client::request(const std::string& method, const json& params, int timeout_seconds, std::string& error) {
    const std::lock_guard<std::mutex> one_at_a_time(calling_);
    long id = 0;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        if (!child_ || ended_) {
            error = "the server is not running";
            return nullptr;
        }
        id = ++next_id_;
        if (!child_->write_line(json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}}
                                    .dump(-1, ' ', false, json::error_handler_t::replace))) {
            error = "the server stopped listening";
            return nullptr;
        }
    }
    std::unique_lock<std::mutex> lock(mutex_);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    for (;;) {
        for (auto it = answers_.begin(); it != answers_.end(); ++it) {
            if (it->value("id", json(nullptr)) == json(id)) {
                const json answer = std::move(*it);
                answers_.erase(it);
                if (answer.contains("error") && !answer["error"].is_null()) {
                    error = answer["error"].value("message", answer["error"].dump());
                    return nullptr;
                }
                return answer.contains("result") ? answer["result"] : json::object();
            }
        }
        if (ended_) {
            error = "the server went away";
            return nullptr;
        }
        if (arrived_.wait_until(lock, deadline) == std::cv_status::timeout) {
            error = "no answer in " + std::to_string(timeout_seconds) + " seconds";
            return nullptr;
        }
    }
}

Result Client::call(const std::string& tool, const json& arguments, int timeout_seconds) {
    std::string error;
    const json answer = request("tools/call", json{{"name", tool}, {"arguments", arguments}}, timeout_seconds, error);
    if (answer.is_null()) {
        Result out;
        out.error = error;
        return out;
    }
    return detail::result_from(answer);
}

// ---------------------------------------------------------------------------
// Every server
// ---------------------------------------------------------------------------

Hub::~Hub() { stop_all(); }

void Hub::configure(const std::vector<ServerConfig>& servers) {
    std::map<std::string, Entry> keep;
    std::vector<std::shared_ptr<Client>> retired;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const ServerConfig& config : servers) {
            if (config.name.empty() || config.command.empty()) {
                continue;
            }
            Entry entry;
            entry.config = config;
            const auto found = servers_.find(config.name);
            const bool same = found != servers_.end() && found->second.config.command == config.command
                           && found->second.config.args == config.args && found->second.config.env == config.env
                           && found->second.config.enabled == config.enabled;
            if (same) {
                entry.client = std::move(found->second.client);
                entry.error  = found->second.error;
            }
            keep[config.name] = std::move(entry);
        }
        for (auto& [name, entry] : servers_) {
            if (entry.client) {
                retired.push_back(std::move(entry.client));
            }
        }
        servers_ = std::move(keep);
    }
    // Let go of outside the lock: a server can take a moment to stop, and
    // one still answering a call stops when that call lets go of it too.
    retired.clear();
}

std::shared_ptr<Client> Hub::client_for(const std::string& name, std::string& error) {
    ServerConfig config;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        const auto found = servers_.find(name);
        if (found == servers_.end() || !found->second.config.enabled) {
            error = "there is no MCP server called " + name + " -- Settings, Tools lists them";
            return nullptr;
        }
        if (found->second.client && found->second.client->running()) {
            return found->second.client;
        }
        config = found->second.config;
    }
    // Started without the lock: other servers' calls go on meanwhile.
    auto client = std::make_shared<Client>();
    const bool ok = client->start(config, error);
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto found = servers_.find(name);
    if (found == servers_.end()) {
        error = name + " was removed while it started";
        return nullptr;
    }
    found->second.error  = ok ? std::string() : error;
    found->second.client = ok ? client : nullptr;
    return ok ? client : nullptr;
}

std::vector<Tool> Hub::tools() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Tool> out;
    for (const auto& [name, entry] : servers_) {
        if (entry.config.enabled && entry.client && entry.client->running()) {
            const std::vector<Tool> some = entry.client->tools();
            out.insert(out.end(), some.begin(), some.end());
        }
    }
    return out;
}

void Hub::warm() {
    std::vector<std::string> names;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (const auto& [name, entry] : servers_) {
            if (entry.config.enabled && !(entry.client && entry.client->running())) {
                names.push_back(name);
            }
        }
    }
    for (const std::string& name : names) {
        std::string error;
        client_for(name, error);
    }
}

Result Hub::call(const std::string& server, const std::string& tool, const json& arguments, int timeout_seconds) {
    std::string error;
    const std::shared_ptr<Client> client = client_for(server, error);
    if (!client) {
        Result out;
        out.error = error;
        return out;
    }
    return client->call(tool, arguments, timeout_seconds);
}

std::vector<Status> Hub::status() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<Status> out;
    for (const auto& [name, entry] : servers_) {
        Status one;
        one.name    = name;
        one.running = entry.client && entry.client->running();
        one.tools   = one.running ? entry.client->tools().size() : 0;
        one.error   = entry.error;
        out.push_back(std::move(one));
    }
    return out;
}

bool Hub::any() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const auto& [name, entry] : servers_) {
        if (entry.config.enabled) {
            return true;
        }
    }
    return false;
}

void Hub::stop_all() {
    std::vector<std::shared_ptr<Client>> all;
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto& [name, entry] : servers_) {
            if (entry.client) {
                all.push_back(std::move(entry.client));
            }
        }
    }
    all.clear();
}

}  // namespace crucible::tools::mcp
