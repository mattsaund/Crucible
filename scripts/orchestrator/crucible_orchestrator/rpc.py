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

Calls nest, and many are outstanding at once. The core asks for a route and,
while that is being answered, for a build; the build's agents each ask the core
for their own rounds. So every request the core makes is answered on a thread
of its own, and a call from any thread waits for the answer with its own id.
Somebody has to read the pipe while they wait: serve() does, for the life of
the process, and before it runs -- in the tests, which drive a channel by hand
-- the first caller to find nobody reading reads for everyone.

Whose a request is travels with it. The core says "context": "build" or
"chat" on what it asks, the thread answering takes that on, and every call it
makes says the same -- which is how the core knows whether the build's Stop or
the chat's applies to an agent's round. A thread a handler starts keeps its
context when started through spawn().
"""

import json
import sys
import threading
import traceback


class CoreError(Exception):
    """The core answered a call with an error."""


class Disconnected(Exception):
    """The other end of the pipe has gone."""


class _Waiter:
    __slots__ = ("event", "message", "closed")

    def __init__(self):
        self.event = threading.Event()
        self.message = None
        self.closed = False


class Channel:
    def __init__(self, reader=None, writer=None):
        self._reader = reader if reader is not None else sys.stdin.buffer
        self._writer = writer if writer is not None else sys.stdout.buffer
        self._lock = threading.Lock()        # ids, waiters, closed, threads
        self._send_lock = threading.Lock()   # one line on the pipe at a time
        self._read_lock = threading.Lock()   # whoever is reading the pipe
        self._next_id = 0
        self._waiting = {}
        self._closed = False
        self._threads = []
        self._local = threading.local()
        self.handlers = {}

    # --- whose work this is ---------------------------------------------------

    @property
    def context(self):
        return getattr(self._local, "context", "")

    def set_context(self, context):
        self._local.context = context or ""

    def spawn(self, target, *args):
        """Start `target` on a thread that keeps this thread's context."""
        context = self.context

        def run():
            self.set_context(context)
            target(*args)

        thread = threading.Thread(target=run, daemon=True)
        with self._lock:
            self._threads.append(thread)
        thread.start()
        return thread

    def settle(self):
        """Wait for every thread this channel started to finish."""
        while True:
            with self._lock:
                running = [t for t in self._threads if t.is_alive()]
                self._threads = running
            if not running:
                return
            for thread in running:
                thread.join()

    # --- the wire -------------------------------------------------------------

    def send(self, message):
        line = json.dumps(message, ensure_ascii=False, separators=(",", ":"))
        with self._send_lock:
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

    # --- reading ----------------------------------------------------------------

    def _pump(self):
        """Read one message and act on it. False at the end of the input."""
        message = self.receive()
        if message is None:
            self._close()
            return False
        if "method" in message:
            self.spawn_request(message)
            return True
        waiter = None
        with self._lock:
            waiter = self._waiting.pop(message.get("id"), None)
        if waiter is None:
            print("crucible_orchestrator: an answer to %r arrived that nobody was waiting for"
                  % message.get("id"), file=sys.stderr)
            return True
        waiter.message = message
        waiter.event.set()
        return True

    def _close(self):
        with self._lock:
            self._closed = True
            waiting, self._waiting = self._waiting, {}
        for waiter in waiting.values():
            waiter.closed = True
            waiter.event.set()

    # --- asking the core --------------------------------------------------------

    def call(self, method, params=None):
        """Ask the core and wait for the answer. From any thread."""
        params = dict(params or {})
        if self.context and "context" not in params:
            params["context"] = self.context
        waiter = _Waiter()
        with self._lock:
            if self._closed:
                raise Disconnected("the core closed the pipe before %s was asked" % method)
            self._next_id += 1
            wanted = self._next_id
            self._waiting[wanted] = waiter
        self.send({"id": wanted, "method": method, "params": params})
        while not waiter.event.is_set():
            # Read for everyone when nobody else is; otherwise wait to be
            # handed the answer by whoever is.
            if self._read_lock.acquire(blocking=False):
                try:
                    while not waiter.event.is_set() and self._pump():
                        pass
                finally:
                    self._read_lock.release()
            else:
                waiter.event.wait(0.05)
        if waiter.closed:
            raise Disconnected("the core closed the pipe while %s was waiting" % method)
        message = waiter.message
        if "error" in message and message["error"] is not None:
            raise CoreError(str(message["error"]))
        return message.get("result")

    # --- being asked ------------------------------------------------------------

    def spawn_request(self, message):
        """Answer a request on a thread of its own, in the context it says."""
        context = (message.get("params") or {}).get("context", "")

        def run():
            self.set_context(context)
            self.dispatch(message)

        thread = threading.Thread(target=run, daemon=True)
        with self._lock:
            self._threads.append(thread)
        thread.start()

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
            return
        except Exception as error:  # every failure goes back as an answer
            traceback.print_exc(file=sys.stderr)
            if has_id:
                self.send({"id": message["id"], "error": "%s failed: %s" % (method, error)})
            return
        if has_id:
            self.send({"id": message["id"], "result": result})

    def serve(self):
        """Answer the core's requests until it closes the pipe."""
        with self._read_lock:
            while self._pump():
                pass
        self.settle()
