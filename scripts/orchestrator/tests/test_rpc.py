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

    def test_text_survives_the_trip_whatever_it_is(self):
        writer = io.BytesIO()
        channel = rpc.Channel(io.BytesIO(), writer)
        channel.notify("hello", {"text": "naïve — 日本語\n\"quoted\""})
        self.assertEqual(sent(writer)[0]["params"]["text"], "naïve — 日本語\n\"quoted\"")


if __name__ == "__main__":
    unittest.main()
