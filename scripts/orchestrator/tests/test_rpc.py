# SPDX-License-Identifier: MIT
"""The pipe: one JSON object per line, and calls that nest."""

import io
import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import rpc  # noqa: E402


def lines(*messages):
    return io.BytesIO(b"".join(json.dumps(m).encode() + b"\n" for m in messages))


def sent(writer):
    return [json.loads(line) for line in writer.getvalue().decode().splitlines()]


class Calls(unittest.TestCase):
    def test_a_call_waits_for_its_own_answer(self):
        writer = io.BytesIO()
        channel = rpc.Channel(lines({"id": 1, "result": {"text": "rendered"}}), writer)
        self.assertEqual(channel.call("delegator.format", {"messages": []}), {"text": "rendered"})
        self.assertEqual(sent(writer), [{"id": 1, "method": "delegator.format",
                                         "params": {"messages": []}}])

    def test_a_request_that_arrives_while_waiting_is_answered_first(self):
        writer = io.BytesIO()
        channel = rpc.Channel(lines({"id": 50, "method": "ping", "params": {"say": "hi"}},
                                    {"id": 1, "result": 7}), writer)
        channel.handlers["ping"] = lambda params: {"pong": params["say"]}
        self.assertEqual(channel.call("engine.flags"), 7)
        channel.settle()   # answered on a thread of its own
        self.assertEqual(sent(writer)[1], {"id": 50, "result": {"pong": "hi"}})

    def test_an_error_comes_back_as_an_exception(self):
        channel = rpc.Channel(lines({"id": 1, "error": "no model"}), io.BytesIO())
        with self.assertRaises(rpc.CoreError):
            channel.call("seat.take")

    def test_a_closed_pipe_is_said_as_such(self):
        channel = rpc.Channel(io.BytesIO(b""), io.BytesIO())
        with self.assertRaises(rpc.Disconnected):
            channel.call("engine.flags")

    def test_a_failing_handler_answers_with_the_failure(self):
        writer = io.BytesIO()
        channel = rpc.Channel(lines({"id": 3, "method": "route", "params": {}}), writer)

        def broken(params):
            raise ValueError("the roster is empty")
        channel.handlers["route"] = broken
        stderr, sys.stderr = sys.stderr, io.StringIO()
        try:
            channel.serve()
        finally:
            sys.stderr = stderr
        self.assertEqual(sent(writer), [{"id": 3, "error": "route failed: the roster is empty"}])

    def test_an_unknown_method_is_refused(self):
        writer = io.BytesIO()
        rpc.Channel(lines({"id": 4, "method": "nope", "params": {}}), writer).serve()
        self.assertEqual(sent(writer), [{"id": 4, "error": "no such method: nope"}])

    def test_a_request_s_context_goes_with_the_calls_it_makes(self):
        # The core asks for a build; the build asks the core for a flag. The
        # flag is asked in the build's context, so the core knows which Stop
        # it is about.
        writer = io.BytesIO()
        channel = rpc.Channel(lines({"id": 9, "method": "build.run", "params": {"context": "build"}},
                                    {"id": 1, "result": {"stop": False}}), writer)
        seen = {}

        def run(params):
            seen["flags"] = channel.call("engine.flags")
            seen["spawned"] = []
            thread = channel.spawn(lambda: seen["spawned"].append(channel.context))
            thread.join()
            return {}
        channel.handlers["build.run"] = run
        channel.serve()
        messages = sent(writer)
        self.assertEqual(messages[0], {"id": 1, "method": "engine.flags", "params": {"context": "build"}})
        self.assertEqual(seen["spawned"], ["build"])
        self.assertEqual(messages[-1], {"id": 9, "result": {}})

    def test_calls_from_several_threads_each_get_their_own_answer(self):
        # Two agents asking at once, answered out of order: each is handed
        # the answer with its own id, whoever reads the pipe.
        import threading
        reader_side, writer_side = os.pipe()
        reader = os.fdopen(reader_side, "rb")
        feed = os.fdopen(writer_side, "wb")
        writer = io.BytesIO()
        channel = rpc.Channel(reader, writer)
        results = {}

        def ask(name):
            results[name] = channel.call("seat.chat", {"who": name})
        first = threading.Thread(target=ask, args=("first",))
        second = threading.Thread(target=ask, args=("second",))
        first.start()
        second.start()
        while len(sent(writer)) < 2:
            pass
        ids = {m["params"]["who"]: m["id"] for m in sent(writer)}
        feed.write(json.dumps({"id": ids["second"], "result": "two"}).encode() + b"\n")
        feed.write(json.dumps({"id": ids["first"], "result": "one"}).encode() + b"\n")
        feed.flush()
        first.join(5)
        second.join(5)
        feed.close()
        reader.close()
        self.assertEqual(results, {"first": "one", "second": "two"})

    def test_text_survives_the_trip_whatever_it_is(self):
        writer = io.BytesIO()
        channel = rpc.Channel(io.BytesIO(), writer)
        channel.notify("hello", {"text": "naïve — 日本語\n\"quoted\""})
        self.assertEqual(sent(writer)[0]["params"]["text"], "naïve — 日本語\n\"quoted\"")


if __name__ == "__main__":
    unittest.main()
