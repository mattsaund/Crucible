# SPDX-License-Identifier: MIT
"""The cook loop against a core that plays the expert from a script."""

import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import cook  # noqa: E402

ROSTER = [
    {"id": "programming", "name": "Programming", "tag": "PROG", "blurb": "code",
     "keywords": ["code", "test", "docstring"], "examples": []},
    {"id": "writing", "name": "Writing", "tag": "WRIT", "blurb": "docs",
     "keywords": ["documentation", "readme"], "examples": []},
]

VERBS = ("LIST", "READ", "WRITE", "EDIT", "FIND", "RUN", "SEARCH", "ASK", "NOTE", "DONE", "HANDOFF")


def parse(answer):
    """A small stand-in for the core's parser: `VERB: argument`, a fence after
    WRITE, and an action after a NOTE winning over the NOTE."""
    matches = list(re.finditer(r"^(%s): ?(.*)$" % "|".join(VERBS), answer, re.M))
    acting = [m for m in matches if m.group(1) != "NOTE"]
    match = acting[0] if acting else (matches[0] if matches else None)
    if not match:
        attempted = re.search(r"^(%s)\b" % "|".join(VERBS), answer, re.M)
        return {"call": None, "attempted": attempted.group(1).lower() if attempted else "none"}
    content = ""
    if match.group(1) == "WRITE":
        fence = re.search(r"```\n(.*?)```", answer, re.S)
        content = fence.group(1) if fence else ""
    if match.group(1) == "EDIT":
        content = answer[match.end():]
    return {"call": {"kind": match.group(1).lower(), "argument": match.group(2).strip(),
                     "content": content}, "attempted": "none"}


class ScriptedCore:
    """The core, with the expert's replies taken from a list, one per round."""

    def __init__(self, replies, stop_after=None, auto_edits=True, approve=True, answer="yes",
                 chat_error=""):
        self.replies = list(replies)
        self.rounds = 0
        self.stop_after = stop_after
        self.auto_edits = auto_edits
        self.approve = approve
        self.answer = answer
        self.chat_error = chat_error
        self.published = []
        self.seats = []
        self.prompts = []
        self.ran = []
        self.asked_edit = []

    def call(self, method, params=None):
        params = params or {}
        if method == "engine.flags":
            # Out of script is the person pressing Stop: a cook never ends
            # itself on DONE, so a test that let it would never end.
            stop = ((self.stop_after is not None and self.rounds >= self.stop_after)
                    or not self.replies)
            return {"stop": stop, "cancel": False, "running": True, "auto_edits": self.auto_edits}
        if method == "delegator.ready":
            return {"available": False}
        if method == "seat.take":
            self.seats.append(params["expert"])
            return {"ok": True}
        if method == "seat.chat":
            self.rounds += 1
            self.prompts.append(params["messages"])
            if self.chat_error:
                return {"error": self.chat_error, "ms": 5}
            reply = self.replies.pop(0) if self.replies else "DONE: nothing more to do"
            return {"answer": reply, "reasoning": "", "ms": 5}
        if method == "tools.parse":
            return parse(params["answer"])
        if method == "tools.run":
            self.ran.append((params["kind"], params["argument"]))
            changed = [params["argument"]] if params["kind"] == "write" else []
            return {"ok": True, "output": "ran " + params["kind"], "summary": params["kind"] + " "
                    + params["argument"], "detail": "", "changed": changed}
        if method == "edit.ask":
            self.asked_edit.append(params["argument"])
            return {"approved": self.approve}
        if method == "cook.await_answer":
            return {"answer": self.answer}
        if method == "cook.publish":
            self.published.append(params["cook"])
            return {}
        if method in ("mood", "seat.attachments"):
            return {"text": ""}
        raise AssertionError("unexpected call " + method)


def params(**extra):
    out = {"id": "c1", "goal": "add a docstring to each test", "root": "/p", "roster": ROSTER,
           "seats": {"programming": {"model": True}, "writing": {"model": True}},
           "routing": {}, "system_prompt": "You are helpful.", "tools": "\n\nTOOLS",
           "budget_seconds": 0}
    out.update(extra)
    return out


def kinds(journal):
    return [step["kind"] for step in journal["steps"]]


class Loop(unittest.TestCase):
    def test_it_works_past_done_until_stopped_then_finishes(self):
        core = ScriptedCore([
            "READ: test_x.py",
            "WRITE: test_x.py\n```\ndef test_a():\n    \"\"\"A.\"\"\"\n```",
            "DONE: added the docstrings",
            "HANDOFF: tidy the tests",
            "READ: test_x.py",
            # the finishing pass
            "DONE: added docstrings; nothing left",
        ], stop_after=5)
        cook.run(cook.Core(core), params())
        journal = core.published[-1]
        # DONE closed a piece of work, and the loop asked for the next one.
        self.assertEqual(kinds(journal)[:4], ["read", "write", "done", "handoff"])
        self.assertIn(("write", "test_x.py"), core.ran)
        self.assertEqual(journal["state"], "stopped")
        self.assertEqual(journal["outcome"], "added docstrings; nothing left")
        self.assertGreater(journal["ended_unix"], 0)
        # The first thing it was shown is what is in the project.
        self.assertEqual(core.ran[0], ("list", "."))
        self.assertIn("Here is what is in the project", core.prompts[0][-1]["content"])

    def test_the_account_of_what_was_done_survives_a_trim(self):
        core = ScriptedCore(["WRITE: a.py\n```\nx = 1\n```", "DONE: wrote a"], stop_after=3)
        cook.run(cook.Core(core), params())
        third = core.prompts[2]
        account = [m["content"] for m in third if m["content"].startswith("What you have done")]
        self.assertTrue(account and "Files you have changed: a.py" in account[0])

    def test_a_model_that_never_acts_is_stopped(self):
        core = ScriptedCore(["I think the tests are fine."] * 20)
        cook.run(cook.Core(core), params())
        journal = core.published[-1]
        self.assertEqual(kinds(journal).count("think"), cook.IDLE_LIMIT)
        self.assertEqual(journal["state"], "failed")
        self.assertIn("without taking an action", journal["outcome"])
        # No finishing pass: it has just shown it will not act.
        self.assertEqual(core.rounds, cook.IDLE_LIMIT)

    def test_a_nearly_right_command_is_told_what_was_wrong(self):
        core = ScriptedCore(["WRITE test.py fixed it", "DONE: ok"], stop_after=2)
        cook.run(cook.Core(core), params())
        self.assertIn("WRITE needs a colon", core.prompts[1][-1]["content"])

    def test_going_in_circles_is_noticed_and_ended(self):
        core = ScriptedCore(["READ: same.py"] * 400)
        cook.run(cook.Core(core), params())
        journal = core.published[-1]
        self.assertEqual(journal["state"], "failed")
        self.assertIn("same action", journal["outcome"])
        said = [m[-1]["content"] for m in core.prompts]
        self.assertTrue(any("going in circles" in s for s in said))
        self.assertTrue(any(s.startswith("Stop. The last several actions") for s in said))

    def test_a_declined_edit_is_not_written(self):
        core = ScriptedCore(["WRITE: a.py\n```\nx = 1\n```", "DONE: gave up"], stop_after=2,
                            auto_edits=False, approve=False)
        cook.run(cook.Core(core), params())
        self.assertEqual(core.asked_edit, ["a.py"])
        self.assertNotIn(("write", "a.py"), core.ran)
        self.assertIn("you declined the edit to a.py",
                      [s["summary"] for s in core.published[-1]["steps"]])

    def test_a_question_waits_for_the_answer(self):
        core = ScriptedCore(["ASK: which file?", "DONE: ok"], stop_after=2, answer="test_x.py")
        cook.run(cook.Core(core), params())
        self.assertIn({"role": "user", "content": "The user answered: test_x.py"}, core.prompts[1])

    def test_a_handoff_goes_back_through_the_delegator(self):
        core = ScriptedCore(["HANDOFF: write the README documentation", "DONE: ok"], stop_after=2)
        cook.run(cook.Core(core), params())
        self.assertEqual(core.seats, ["programming", "writing"])
        self.assertIn("Programming handed over to Writing",
                      [s["summary"] for s in core.published[-1]["steps"]])

    def test_an_expert_that_cannot_be_reached_ends_it(self):
        core = ScriptedCore(["READ: a.py"], chat_error="the provider refused the key")
        cook.run(cook.Core(core), params())
        journal = core.published[-1]
        self.assertEqual(journal["state"], "failed")
        self.assertEqual(journal["outcome"], "stopped: the provider refused the key")

    def test_no_expert_with_a_model_fails_at_once(self):
        core = ScriptedCore([])
        cook.run(cook.Core(core), params(seats={}))
        journal = core.published[-1]
        self.assertEqual(journal["state"], "failed")
        self.assertEqual(core.rounds, 0)


class Clip(unittest.TestCase):
    def test_cut_at_a_word(self):
        text = "We need to add a one-line docstring to each test in test_orbit.py, single line"
        out = cook.clip(text, 60)
        self.assertTrue(out.endswith("..."))
        self.assertTrue(text.startswith(out[:-3]))
        self.assertFalse(out[:-3].endswith("singl"))

    def test_short_text_is_untouched(self):
        self.assertEqual(cook.clip("  short  ", 60), "short")


if __name__ == "__main__":
    unittest.main()
