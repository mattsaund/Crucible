# SPDX-License-Identifier: MIT
"""A provider that answers from a script, for the chat turn's tests.

It speaks just enough of the chat-completions API for Crucible's client: a
POST to .../chat/completions is answered as a stream of `data:` lines, the way
a hosted model streams, and a GET to .../search is answered the way SearXNG
answers. Each request takes the next reply in the script and is written to the
log, so a test can read back exactly what the expert was sent, round by round.

    scripted_provider.py <script.json> <log.jsonl>

The script is {"replies": [...], "results": [...]}. A reply is any of

    {"content": "...", "reasoning": "...", "finish": "stop", "pieces": 3,
     "pause": 0.2, "status": 200, "error": "..."}

where `pieces` is how many parts the content arrives in, `pause` is how long
to wait before each, `status` other than 200 refuses the request with `error`
as the reason, and `error` with a 200 breaks off the stream after the content.
A request past the end of the script is answered "(out of script)".

It prints the port it took on a line of its own, and serves until its input
closes: the test ends it by closing that.
"""

import http.server
import json
import sys
import threading
import time
import urllib.parse


def main():
    with open(sys.argv[1], encoding="utf-8") as source:
        script = json.load(source)
    log_path = sys.argv[2]
    replies = list(script.get("replies") or [])
    results = script.get("results") or []
    lock = threading.Lock()

    def record(entry):
        with lock, open(log_path, "a", encoding="utf-8") as log:
            log.write(json.dumps(entry) + "\n")

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass   # the log is the requests, not the server's chatter

        def do_GET(self):
            asked = urllib.parse.urlparse(self.path)
            if not asked.path.endswith("/search"):
                # The model list a provider is asked for its context: none
                # listed, so the model's context is the one Crucible assumes.
                body = json.dumps({"data": []}).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return
            query = urllib.parse.parse_qs(asked.query).get("q", [""])[0]
            record({"search": query})
            body = json.dumps({"results": results}).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_POST(self):
            length = int(self.headers.get("Content-Length") or 0)
            body = json.loads(self.rfile.read(length).decode("utf-8") or "{}")
            with lock:
                reply = replies.pop(0) if replies else {"content": "(out of script)"}
            record({"path": self.path, "body": body})

            status = int(reply.get("status", 200))
            if status != 200:
                said = json.dumps({"error": {"message": reply.get("error", "refused")}}).encode("utf-8")
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(said)))
                self.end_headers()
                self.wfile.write(said)
                return

            self.send_response(200)
            self.send_header("Content-Type", "text/event-stream")
            self.end_headers()

            def event(payload):
                self.wfile.write(("data: " + json.dumps(payload) + "\n\n").encode("utf-8"))
                self.wfile.flush()

            reasoning = reply.get("reasoning", "")
            content = reply.get("content", "")
            if reasoning:
                event({"choices": [{"delta": {"reasoning_content": reasoning}}]})
            pieces = max(1, int(reply.get("pieces", 1)))
            size = -(-len(content) // pieces) if content else 0
            for start in range(0, len(content), size or 1):
                if reply.get("pause"):
                    time.sleep(float(reply["pause"]))
                event({"choices": [{"delta": {"content": content[start:start + size]}}]})
            if reply.get("error"):
                event({"error": {"message": reply["error"]}})
                return
            # Usage, so a turn counts what was said: about four characters a
            # token, never none for a reply that said or thought anything, or
            # what the script says when it says.
            said = len(content) + len(reasoning)
            tokens = reply.get("tokens", max(1, said // 4) if said else 0)
            event({"choices": [{"delta": {}, "finish_reason": reply.get("finish", "stop")}],
                   "usage": {"prompt_tokens": 10, "completion_tokens": tokens}})
            self.wfile.write(b"data: [DONE]\n\n")
            self.wfile.flush()

    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    print(server.server_address[1], flush=True)
    sys.stdin.read()   # until the test closes it
    server.shutdown()


if __name__ == "__main__":
    main()
