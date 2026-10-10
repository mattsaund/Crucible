// SPDX-License-Identifier: MIT
//
// MCP: a server's tools, listed and called -- against a server written here
// in a few lines of Python, which speaks the protocol's stdio transport the
// way the real ones do: one JSON-RPC message a line.
#include "test_helpers.hpp"

#include <fstream>

#include "crucible/tools/mcp.hpp"
#include "crucible/tools/workshop.hpp"
#include "crucible/util/subprocess.hpp"

namespace {

using json = nlohmann::json;

std::string a_python() {
    if (const char* chosen = std::getenv("CRUCIBLE_PYTHON"); chosen != nullptr && *chosen != '\0') {
        return chosen;
    }
    for (const char* name : {"python3", "python"}) {
        if (util::on_path(name)) {
            return name;
        }
    }
    return {};
}

/// A server with one tool, `add`, that adds two numbers -- and says a log
/// line and a notification first, which a client must step over.
const char* const kServer = R"PY(
import json, sys
def say(message):
    sys.stdout.write(json.dumps(message) + "\n")
    sys.stdout.flush()
for line in sys.stdin:
    message = json.loads(line)
    method = message.get("method")
    if method == "initialize":
        say({"jsonrpc": "2.0", "method": "notifications/message", "params": {"level": "info"}})
        say({"jsonrpc": "2.0", "id": message["id"], "result": {"protocolVersion": "2025-06-18",
             "capabilities": {"tools": {}}, "serverInfo": {"name": "sums", "version": "1"}}})
    elif method == "tools/list":
        say({"jsonrpc": "2.0", "id": message["id"], "result": {"tools": [{"name": "add",
             "description": "Add two numbers.\nAnything after the first line is not shown.",
             "inputSchema": {"type": "object", "properties": {"a": {"type": "number"}, "b": {"type": "number"},
                             "note": {"type": "string"}}, "required": ["a", "b"]}}]}})
    elif method == "tools/call":
        args = message["params"]["arguments"]
        if "a" not in args:
            say({"jsonrpc": "2.0", "id": message["id"], "result": {"isError": True,
                 "content": [{"type": "text", "text": "a is required"}]}})
        else:
            say({"jsonrpc": "2.0", "id": message["id"], "result": {"content": [{"type": "text",
                 "text": str(args["a"] + args["b"])}]}})
)PY";

tools::mcp::ServerConfig sums_server(const std::filesystem::path& dir, const std::string& python) {
    std::ofstream(dir / "server.py") << kServer;
    tools::mcp::ServerConfig config;
    config.name    = "sums";
    config.command = python;
    config.args    = {"-u", (dir / "server.py").string()};
    return config;
}

}  // namespace

TEST(a_tool_is_described_in_a_line_with_what_it_needs) {
    tools::mcp::Tool tool;
    tool.server      = "github";
    tool.name        = "create_issue";
    tool.description = "Create an issue.\nLong detail.";
    tool.schema      = json::parse(R"({"properties": {"title": {}, "body": {}}, "required": ["title"]})");
    CHECK_EQ(tools::mcp::detail::describe(tool), std::string("github/create_issue: Create an issue. -- body, title*"));
}

TEST(an_answer_becomes_text_and_pictures_and_an_error_says_so) {
    const tools::mcp::Result ok = tools::mcp::detail::result_from(json::parse(
        R"({"content": [{"type": "text", "text": "one"}, {"type": "image", "mimeType": "image/png", "data": "AAA"},
                        {"type": "text", "text": "two"}]})"));
    CHECK(ok.ok);
    CHECK_EQ(ok.text, std::string("one\ntwo"));
    CHECK_EQ(ok.images.size(), std::size_t{1});
    const tools::mcp::Result bad = tools::mcp::detail::result_from(json::parse(
        R"({"isError": true, "content": [{"type": "text", "text": "no such repo"}]})"));
    CHECK(!bad.ok);
    CHECK_EQ(bad.error, std::string("no such repo"));
}

TEST(a_server_is_started_listed_and_called) {
    const std::string python = a_python();
    if (python.empty()) {
        std::printf("      (no Python on PATH; skipped)\n");
        return;
    }
    TempDir dir;
    tools::mcp::Hub hub;
    hub.configure({sums_server(dir.path(), python)});
    CHECK(hub.any());
    // Nothing is started by asking for the list...
    CHECK(hub.tools().empty());
    // ...and everything by warming it.
    hub.warm();
    const std::vector<tools::mcp::Tool> offered = hub.tools();
    CHECK_EQ(offered.size(), std::size_t{1});
    if (offered.empty()) {
        return;
    }
    CHECK_EQ(offered[0].name, std::string("add"));

    const tools::mcp::Result sum = hub.call("sums", "add", json{{"a", 2}, {"b", 3}}, 10);
    CHECK(sum.ok);
    CHECK_EQ(sum.text, std::string("5"));
    const tools::mcp::Result missing = hub.call("sums", "add", json{{"b", 3}}, 10);
    CHECK(!missing.ok);
    CHECK_EQ(missing.error, std::string("a is required"));
    CHECK(!hub.call("nobody", "add", json::object(), 5).ok);

    // Through the workshop's verb, as an expert writes it.
    tools::WorkshopSettings settings;
    settings.enabled = true;
    settings.root    = dir.path();
    settings.mcp     = &hub;
    const std::optional<tools::ToolCall> call =
        tools::parse_tool_call("TOOL: sums/add\n```json\n{\"a\": 40, \"b\": 2}\n```", "");
    CHECK(call.has_value() && call->kind == tools::ToolKind::Mcp);
    const tools::ToolResult result = tools::run_tool(*call, settings, {}, {});
    CHECK(result.ok);
    CHECK_EQ(result.output, std::string("42"));
    CHECK(tools::workshop_instructions(settings, tools::ToolAudience::Cook).find("sums/add: Add two numbers. -- a*, b*, note")
          != std::string::npos);

    // A server taken out of the list is stopped.
    hub.configure({});
    CHECK(!hub.any());
    CHECK(hub.status().empty());
}
