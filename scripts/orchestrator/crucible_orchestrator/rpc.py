# SPDX-License-Identifier: MIT
"""The pipe between Crucible's core and this process.

One JSON object per line, in both directions, on this process's standard input
and output. Standard error is a log file the core opened, so a warning printed
by anything here can never land in the middle of a message.

Three shapes:

    {"id": 7, "method": "route", "params": {...}}     a request
    {"id": 7, "result": {...}}                         its answer
    {"id": 7, "error": "what went wrong"}              or its failure

A message with a "method" is a request, whichever side sent it; one without is
an answer to the receiver's own request of that id. Each side numbers its own.

Calls nest. The core asks for a route and waits; to answer, this side asks the
core to score labels with the delegator and waits in turn; the core answers on
the thread that was waiting for the route, because that is the thread that owns
the models. So `call` handles any request that arrives while it waits, and so
does the core -- neither side ever has two of its own calls outstanding.
"""

import json
import sys
import traceback


class CoreError(Exception):
    """The core answered a call with an error."""


class Disconnected(Exception):
    """The other end of the pipe has gone."""


class Channel:
    def __init__(self, reader=None, writer=None):
        self._reader = reader if reader is not None else sys.stdin.buffer
        self._writer = writer if writer is not None else sys.stdout.buffer
        self._next_id = 0
        self.handlers = {}

    # --- the wire -------------------------------------------------------------

    def send(self, message):
        line = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        self._writer.write(line.encode("utf-8") + b"\n")
        self._writer.flush()

    def receive(self):
        """The next message, or None at the end of the input."""
        while True:
            raw = self._reader.readline()
            if not raw:
                return None
            raw = raw.strip()
            if not raw:
                continue
            try:
                message = json.loads(raw.decode("utf-8"))
            except ValueError:
                print("crucible_orchestrator: ignored a line that is not JSON: %r" % raw[:200],
                      file=sys.stderr)
                continue
            if isinstance(message, dict):
                return message

    def notify(self, method, params=None):
        self.send({"method": method, "params": params or {}})

    # --- asking the core --------------------------------------------------------

    def call(self, method, params=None):
        """Ask the core and wait for the answer, serving its requests meanwhile."""
        self._next_id += 1
        wanted = self._next_id
        self.send({"id": wanted, "method": method, "params": params or {}})
        while True:
            message = self.receive()
            if message is None:
                raise Disconnected("the core closed the pipe while %s was waiting" % method)
            if "method" in message:
                self.dispatch(message)
                continue
            if message.get("id") != wanted:
                print("crucible_orchestrator: an answer to %r arrived while waiting for %r"
                      % (message.get("id"), wanted), file=sys.stderr)
                continue
            if "error" in message and message["error"] is not None:
                raise CoreError(str(message["error"]))
            return message.get("result")

    # --- being asked ------------------------------------------------------------

    def dispatch(self, message):
        method = message.get("method", "")
        params = message.get("params") or {}
        has_id = "id" in message
        handler = self.handlers.get(method)
        if handler is None:
            if has_id:
                self.send({"id": message["id"], "error": "no such method: %s" % method})
            return
        try:
            result = handler(params)
        except Disconnected:
            raise
        except Exception as error:  # every failure goes back as an answer
            traceback.print_exc(file=sys.stderr)
            if has_id:
                self.send({"id": message["id"], "error": "%s failed: %s" % (method, error)})
            return
        if has_id:
            self.send({"id": message["id"], "result": result})

    def serve(self):
        """Answer the core's requests until it closes the pipe."""
        while True:
            message = self.receive()
            if message is None:
                return
            if "method" in message:
                self.dispatch(message)
