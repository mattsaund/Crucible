// SPDX-License-Identifier: MIT
//
// MCP: tools from other programs, offered to every expert.
//
// The Model Context Protocol is how a program offers tools to a model it has
// never met: a server, started as a process, answers JSON-RPC on its standard
// input and output -- "what tools have you", "call this one with these
// arguments" -- and there are servers for GitHub, databases, browsers, design
// files, issue trackers, most things somebody building software reaches for.
// Claude Code speaks it, and so does this: a server added in Settings, Tools
// has its tools offered to every expert, local or not, through the one text
// verb TOOL, and the call goes to the server and the answer comes back as the
// tool's result.
//
// Started when first wanted rather than with Crucible: a server is a program
// someone else wrote, often fetched by npx on its first run, and a window
// that waited for all of them before it could open would be the worse
// trade. A server that will not start says why, in Settings and to the
// expert that asked.
//
// Like RUN, a server is a program running as the person, and is bounded by
// nothing Crucible can enforce: it is added by hand, by somebody who chose it.
#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "crucible/tools/attachments.hpp"

namespace crucible::util {
class Subprocess;
}

namespace crucible::tools::mcp {

/// One server, as Settings keeps it.
struct ServerConfig {
    std::string              name;      ///< what tools are called by: "github"
    std::string              command;   ///< the program: "npx"
    std::vector<std::string> args;      ///< "-y", "@modelcontextprotocol/server-github"
    std::vector<std::string> env;       ///< "NAME=VALUE", for keys a server wants
    bool                     enabled = true;
};

/// One tool a server offers.
struct Tool {
    std::string    server;
    std::string    name;
    std::string    description;
    nlohmann::json schema;   ///< its inputSchema, as the server gave it
};

/// What a call came to.
struct Result {
    bool                      ok = false;
    std::string               text;     ///< the text parts of the answer, joined
    std::vector<attach::Image> images;  ///< the picture parts, for a model that sees them
    std::string               error;
};

/// How a server is doing, for Settings.
struct Status {
    std::string name;
    bool        running = false;
    std::size_t tools   = 0;
    std::string error;
};

/// One server's process and the conversation with it.
class Client {
public:
    Client() = default;
    ~Client();
    Client(const Client&)            = delete;
    Client& operator=(const Client&) = delete;

    /// Start it and ask what it offers. False, with `error`, when it would not
    /// start or would not say.
    bool start(const ServerConfig& config, std::string& error);
    void stop();
    bool running() const;

    std::vector<Tool> tools() const;

    /// Call `tool` with `arguments`, waiting at most `timeout_seconds`.
    Result call(const std::string& tool, const nlohmann::json& arguments, int timeout_seconds);

private:
    /// Send a request and wait for its answer. Null, with `error`, when there
    /// was none in time or the server went away.
    nlohmann::json request(const std::string& method, const nlohmann::json& params, int timeout_seconds,
                           std::string& error);
    void read_all();

    ServerConfig                       config_;
    std::unique_ptr<util::Subprocess>  child_;
    std::thread                        reader_;
    std::mutex                         calling_;   ///< one request at a time per server
    mutable std::mutex                 mutex_;
    std::condition_variable            arrived_;
    std::deque<nlohmann::json>         answers_;
    bool                               ended_ = true;
    long                               next_id_ = 0;
    std::vector<Tool>                  tools_;
};

/// Every server Settings names.
class Hub {
public:
    Hub() = default;
    ~Hub();
    Hub(const Hub&)            = delete;
    Hub& operator=(const Hub&) = delete;

    /// Take the server list. A server whose settings changed is stopped, to
    /// start again the new way when next wanted; one removed is stopped.
    void configure(const std::vector<ServerConfig>& servers);

    /// The tools of every server that is running. Never starts one: a server
    /// fetched by npx on its first run takes long enough to start that asking
    /// for the list must not wait for it. See warm().
    std::vector<Tool> tools() const;

    /// Start every enabled server that is not running. Slow -- each is a
    /// program starting -- so it is for a thread of its own.
    void warm();

    /// Call `tool` of `server`.
    Result call(const std::string& server, const std::string& tool, const nlohmann::json& arguments,
                int timeout_seconds);

    /// How each server is doing, without starting any.
    std::vector<Status> status() const;

    /// Whether any server is enabled -- whether TOOL is worth teaching.
    bool any() const;

    void stop_all();

private:
    /// Shared, so a call in progress keeps its client alive when a settings
    /// change replaces it.
    struct Entry {
        ServerConfig            config;
        std::shared_ptr<Client> client;
        std::string             error;
    };

    /// The running client for `name`, starting it when it is not. Null, with
    /// `error`, when there is none or it would not start.
    std::shared_ptr<Client> client_for(const std::string& name, std::string& error);

    mutable std::mutex           mutex_;
    std::map<std::string, Entry> servers_;
};

namespace detail {

/// The tools out of a tools/list answer, for `server`.
std::vector<Tool> tools_from(const std::string& server, const nlohmann::json& result);

/// A tools/call answer, as text and pictures.
Result result_from(const nlohmann::json& result);

/// One line describing a tool for a model: "github/create_issue: Create an
/// issue -- title*, body". The starred arguments are required.
std::string describe(const Tool& tool);

}  // namespace detail

}  // namespace crucible::tools::mcp
