# SPDX-License-Identifier: MIT
"""A provider that answers from a script, for the chat turn's tests.

It speaks just enough of both APIs for Crucible's client: a POST to
.../chat/completions is answered as a stream of `data:` lines, the way a hosted
model streams, a POST to .../messages the way Claude's Messages API streams,
and a GET to .../search the way SearXNG answers. Each request takes the next
reply in the script and is written to the log, so a test can read back exactly
what the expert was sent, round by round, and with which key.

    scripted_provider.py <script.json> <log.jsonl>

The script is {"replies": [...], "results": [...], "models": {...}}. A reply
is any of

    {"content": "...", "reasoning": "...", "finish": "stop", "pieces": 3,
     "pause": 0.2, "status": 200, "error": "...", "refusal": "...",
     "whole": true, "details": {...}, "declined": "...", "fallback": "...",
     "sticky": "..."}

where `pieces` is how many parts the content arrives in, `pause` is how long
to wait before each, `status` other than 200 refuses the request with `error`
as the reason, and `error` with a 200 breaks off the stream after the content.
`refusal` is a model's own words declining, in the field OpenAI's models put
them in, and `whole` answers in one document rather than a stream, the way a
server that does not stream would.

For the Messages API, `finish` is the stop reason ("end_turn" unless given)
and `details` its stop_details. `fallback` names a model that takes over from
the one asked, which declines after writing `declined`, or before writing
anything when there is none. `sticky` names one that answers in its place
without the one asked running at all, which is how a conversation that fell
back before is answered.

A request past the end of the script is answered "(out of script)". A GET of a
model's own entry, .../models/<id>, is answered with the script's models[id]
when it has one, and any other GET with a listing that lists nothing.

It prints the port it took on a line of its own, and serves until its input
closes: the test ends it by closing that.
"""

import http.server
import json
import sys
import threading
import time
import urllib.parse

# The headers a request is logged with: whatever carries the key, and the
# betas Claude's API is told about.
LOGGED_HEADERS = ("authorization", "x-api-key", "x-goog-api-key", "anthropic-beta")


def main():
    with open(sys.argv[1], encoding="utf-8") as source:
        script = json.load(source)
    log_path = sys.argv[2]
    replies = list(script.get("replies") or [])
    results = script.get("results") or []
    models = script.get("models") or {}
    lock = threading.Lock()

    def record(entry):
        with lock, open(log_path, "a", encoding="utf-8") as log:
            log.write(json.dumps(entry) + "\n")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass   # the log is the requests, not the server's chatter

        def logged_headers(self):
            return {name: self.headers[name] for name in LOGGED_HEADERS if self.headers.get(name)}

        def send_json(self, status, document):
            body = json.dumps(document).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            asked = urllib.parse.urlparse(self.path)
            if not asked.path.endswith("/search"):
                # A model's own entry, or the list a provider is asked for its
                # context. Nothing listed unless the script says, so the
                # model's context is the one Crucible assumes.
                record({"get": asked.path, "headers": self.logged_headers()})
                entry = models.get(asked.path.rstrip("/").rsplit("/", 1)[-1])
                self.send_json(200, entry if entry is not None else {"data": []})
                return
            query = urllib.parse.parse_qs(asked.query).get("q", [""])[0]
            record({"search": query})
            self.send_json(200, {"results": results})

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            with lock:
                reply = replies.pop(0) if replies else {"content": "(out of script)"}
            record({"path": self.path, "body": body, "headers": self.logged_headers()})

            status = int(reply.get("status", 200))
            if status != 200:
                self.send_json(status, {"error": {"message": reply.get("error", "refused")}})
                return

            claude = self.path.rstrip("/").endswith("/messages")
            if reply.get("whole"):
                self.send_json(200, whole_message(reply, body) if claude else whole_completion(reply))
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()
            if claude:
                self.stream_message(reply, body)
            else:
                self.stream_completion(reply)

        def event(self, payload, name=None):
            framed = ("event: " + name + "\n" if name else "") + "data: " + json.dumps(payload) + "\n\n"
            self.wfile.write(framed.encode("utf-8"))
            self.wfile.flush()

        def stream_completion(self, reply):
            reasoning = reply.get("reasoning", "")
            content = reply.get("content", "")
            refusal = reply.get("refusal", "")
            if reasoning:
                self.event({"choices": [{"delta": {"reasoning_content": reasoning}}]})
            for piece in pieces(content, reply):
                self.event({"choices": [{"delta": {"content": piece}}]})
            for piece in pieces(refusal, reply):
                self.event({"choices": [{"delta": {"content": None, "refusal": piece}}]})
            if reply.get("error"):
                self.event({"error": {"message": reply["error"]}})
                return
            self.event({"choices": [{"delta": {}, "finish_reason": reply.get("finish", "stop")}],
                        "usage": {"prompt_tokens": 10, "completion_tokens": tokens(reply)}})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

        def stream_message(self, reply, body):
            asked = body.get("model", "")
            took_over = reply.get("fallback") or reply.get("sticky") or ""
            declined = reply.get("declined", "")
            # The model that message_start names: the one asked, unless it
            # declined before writing anything or never ran.
            first = took_over if took_over and not declined else asked
            self.event({"type": "message_start",
                        "message": {"id": "msg_scripted", "type": "message", "role": "assistant",
                                    "model": first, "content": [], "stop_reason": None,
                                    "usage": {"input_tokens": 10, "output_tokens": 1}}},
                       "message_start")
            index = [0]

            def block(start, deltas):
                self.event({"type": "content_block_start", "index": index[0], "content_block": start},
                           "content_block_start")
                for delta in deltas:
                    self.event({"type": "content_block_delta", "index": index[0], "delta": delta},
                               "content_block_delta")
                self.event({"type": "content_block_stop", "index": index[0]}, "content_block_stop")
                index[0] += 1

            if declined:
                block({"type": "text", "text": ""}, [{"type": "text_delta", "text": declined}])
            if reply.get("fallback"):
                block({"type": "fallback", "from": {"model": asked}, "to": {"model": took_over}}, [])
            if reply.get("reasoning"):
                block({"type": "thinking", "thinking": ""},
                      [{"type": "thinking_delta", "thinking": reply["reasoning"]},
                       {"type": "signature_delta", "signature": "c2NyaXB0ZWQ="}])
            content = reply.get("content", "")
            if content:
                block({"type": "text", "text": ""},
                      [{"type": "text_delta", "text": piece} for piece in pieces(content, reply)])
            if reply.get("error"):
                self.event({"type": "error", "error": {"type": "overloaded_error", "message": reply["error"]}},
                           "error")
                return
            self.event({"type": "message_delta",
                        "delta": {"stop_reason": reply.get("finish", "end_turn"),
                                  "stop_details": reply.get("details")},
                        "usage": usage_of(reply, asked)}, "message_delta")
            self.event({"type": "message_stop"}, "message_stop")

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(server.server_address[1], flush=True)
    sys.stdin.read()   # until the test closes it
    server.shutdown()


def pieces(text, reply):
    """`text` in as many parts as the reply asks for, after its pause each."""
    if not text:
        return
    count = max(1, int(reply.get("pieces", 1)))
    size = -(-len(text) // count)
    for start in range(0, len(text), size):
        if reply.get("pause"):
            time.sleep(float(reply["pause"]))
        yield text[start:start + size]


def tokens(reply):
    """About four characters a token, never none for a reply that said or
    thought anything, or what the script says when it says."""
    said = sum(len(reply.get(key, "")) for key in ("content", "reasoning", "refusal"))
    return reply.get("tokens", max(1, said // 4) if said else 0)


def usage_of(reply, asked):
    """What a Messages reply says it used: one model's count, or each model's
    when another answered in place of the one asked."""
    usage = {"input_tokens": 10, "output_tokens": tokens(reply), "cache_read_input_tokens": 0,
             "cache_creation_input_tokens": 0}
    took_over = reply.get("fallback") or reply.get("sticky")
    if took_over:
        attempts = [] if reply.get("sticky") else [
            {"type": "message", "model": asked, "input_tokens": 12,
             "output_tokens": len(reply.get("declined", "")) // 4,
             "cache_read_input_tokens": 0, "cache_creation_input_tokens": 0}]
        attempts.append({"type": "fallback_message", "model": took_over, "input_tokens": 10,
                         "output_tokens": tokens(reply), "cache_read_input_tokens": 0,
                         "cache_creation_input_tokens": 0})
        usage["iterations"] = attempts
    return usage


def whole_completion(reply):
    """A chat completion, all at once."""
    message = {"role": "assistant", "content": reply.get("content") or None}
    if reply.get("refusal"):
        message["refusal"] = reply["refusal"]
    if reply.get("reasoning"):
        message["reasoning_content"] = reply["reasoning"]
    return {"id": "chatcmpl-scripted", "object": "chat.completion",
            "choices": [{"index": 0, "message": message, "finish_reason": reply.get("finish", "stop")}],
            "usage": {"prompt_tokens": 10, "completion_tokens": tokens(reply)}}


def whole_message(reply, body):
    """A Messages reply, all at once. One that fell back leaves out what the
    declining model wrote, and the hand-over comes first."""
    asked = body.get("model", "")
    took_over = reply.get("fallback") or reply.get("sticky") or ""
    content = []
    if reply.get("fallback"):
        content.append({"type": "fallback", "from": {"model": asked}, "to": {"model": took_over}})
    if reply.get("reasoning"):
        content.append({"type": "thinking", "thinking": reply["reasoning"], "signature": "c2NyaXB0ZWQ="})
    if reply.get("content"):
        content.append({"type": "text", "text": reply["content"]})
    return {"id": "msg_scripted", "type": "message", "role": "assistant", "model": took_over or asked,
            "content": content, "stop_reason": reply.get("finish", "end_turn"),
            "stop_details": reply.get("details"), "usage": usage_of(reply, asked)}


if __name__ == "__main__":
    main()
