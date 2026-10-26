# SPDX-License-Identifier: MIT
"""The cook loop against a core that plays the expert from a script."""

import json
import os
import re
import sys
import unittest
from unittest import mock

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
    """The core, with the expert's replies taken from a list, one per round: an
    answer, or an (answer, reasoning) pair. A tool's result is "ran <verb>",
    or what `results` says for the argument it was given."""

    def __init__(self, replies, stop_after=None, auto_edits=True, approve=True, answer="yes",
                 chat_error="", results=None):
        self.replies = list(replies)
        self.results = results or {}
        self.rounds = 0
        self.stop_after = stop_after
        self.auto_edits = auto_edits
        self.approve = approve
        self.answer = answer
        self.chat_error = chat_error
        self.published = []
        self.held_steps = []
        self.publish_bodies = []   # how many steps each publish carried
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
        if method == "delegator.done":
            return {}
        if method == "seat.take":
            self.seats.append(params["expert"])
            return {"ok": True}
        if method == "seat.chat":
            self.rounds += 1
            self.prompts.append(params["messages"])
            if self.chat_error:
                return {"error": self.chat_error, "ms": 5}
            reply = self.replies.pop(0) if self.replies else "DONE: nothing more to do"
            answer, reasoning = reply if isinstance(reply, tuple) else (reply, "")
            return {"answer": answer, "reasoning": reasoning, "ms": 5}
        if method == "tools.parse":
            return parse(params["answer"])
        if method == "tools.run":
            self.ran.append((params["kind"], params["argument"]))
            if params["argument"] in self.results:
                return dict({"summary": params["argument"], "detail": "", "changed": []},
                            **self.results[params["argument"]])
            changed = [params["argument"]] if params["kind"] == "write" else []
            return {"ok": True, "output": "ran " + params["kind"], "summary": params["kind"] + " "
                    + params["argument"], "detail": "", "changed": changed}
        if method == "edit.ask":
            self.asked_edit.append(params["argument"])
            return {"approved": self.approve}
        if method == "cook.await_answer":
            return {"answer": self.answer}
        if method == "cook.publish":
            # The journal put back together the way the core does: what it
            # holds up to steps_from, and the new steps after it.
            body = params["cook"]
            steps = self.held_steps[:params.get("steps_from", 0)] + list(body.get("steps") or [])
            self.held_steps = steps
            whole = dict(body)
            whole["steps"] = steps
            self.published.append(json.loads(json.dumps(whole)))
            self.publish_bodies.append(len(body.get("steps") or []))
            return {"steps": len(steps)}
        if method == "machine.facts":
            return {"os": "Linux", "shell": "/bin/sh", "programs": ["python3", "git"]}
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


class CookTest(unittest.TestCase):
    def setUp(self):
        # A rest is minutes of real time; here it is a list of the seconds.
        self.rested = []
        patcher = mock.patch.object(cook, "pause", self.rested.append)
        patcher.start()
        self.addCleanup(patcher.stop)


class Loop(CookTest):
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

    def test_the_finishing_pass_asks_before_it_writes_when_auto_is_off(self):
        # Stop, then the finishing pass: with Auto off its WRITE is put to
        # the person like any other, and a no leaves the file alone.
        core = ScriptedCore(["READ: a.py", "WRITE: a.py\n```\nx = 1\n```", "DONE: tidied"],
                            stop_after=1, auto_edits=False, approve=False)
        cook.run(cook.Core(core), params())
        self.assertIn("a.py", core.asked_edit)
        self.assertNotIn(("write", "a.py"), core.ran)

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

    def test_a_thought_is_cut_at_a_word_and_kept_whole_beside_its_line(self):
        thought = ("We need to add a docstring to each test.\nThe files are:\ntest_orbit.py\n"
                   "test_period.py\n" + "test_escape_velocity_at_the_surface_of_the_earth.py\n" * 4)
        core = ScriptedCore([("", thought), "DONE: ok"], stop_after=2)
        cook.run(cook.Core(core), params())
        step = [s for s in core.published[-1]["steps"] if s["kind"] == "think"][0]
        self.assertTrue(step["summary"].startswith("(only thought) We need to add a docstring"))
        self.assertNotIn("\n", step["summary"])
        # Where the line stops, a word stopped: no name is cut in two.
        kept = step["summary"][len("(only thought) "):-len("...")]
        whole = " ".join(thought.split())
        self.assertTrue(whole.startswith(kept))
        self.assertFalse(whole[len(kept)].isalnum())
        # And the whole of it opens from the step.
        self.assertEqual(step["detail"], thought.strip())


def notes(journal):
    return [step["summary"] for step in journal["steps"]]


def last_said(prompt):
    """What the loop said to the model last, in one round's messages."""
    return prompt[-1]["content"]


class TheSameCallAgain(CookTest):
    """A call whose answer is still in front of the model, with nothing changed
    since: not run the first time, and the end of the piece of work the next."""

    def test_a_read_again_is_not_run_and_then_ends_the_piece(self):
        core = ScriptedCore(["READ: a.py", "READ: a.py", "READ: a.py", "HANDOFF: tidy a.py"],
                            stop_after=4)
        cook.run(cook.Core(core), params())
        self.assertEqual(core.ran.count(("read", "a.py")), 1)
        self.assertIn("You already did exactly that", last_said(core.prompts[2]))
        said = notes(core.published[-1])
        self.assertIn("not run, the same as before with nothing changed: READ: a.py", said)
        self.assertTrue(any(s.startswith("ended the piece: the same call again") for s in said))
        # The piece is over, and the next one is asked for after a rest: it
        # changed nothing.
        self.assertIn("HANDOFF: <the next piece of work>", last_said(core.prompts[3]))
        self.assertEqual(len(self.rested), cook.REST_FIRST)

    def test_the_same_lines_read_with_sed_are_the_same_read(self):
        core = ScriptedCore(["READ: style.css 1-80", "RUN: sed -n '1,80p' style.css", "DONE: ok"],
                            stop_after=3)
        cook.run(cook.Core(core), params())
        self.assertNotIn(("run", "sed -n '1,80p' style.css"), core.ran)
        self.assertIn("You already did exactly that", last_said(core.prompts[2]))

    def test_a_command_that_prints_a_file_is_pointed_at_read(self):
        core = ScriptedCore(["RUN: sed -n '1,80p' style.css", "RUN: cat app.js", "DONE: ok"],
                            stop_after=3)
        cook.run(cook.Core(core), params())
        handed = [m["content"] for m in core.prompts[2] if m["content"].startswith("ran run")]
        self.assertEqual(handed, ["ran run\n(READ: style.css 1-80 reads the same lines, numbered.)",
                                  "ran run\n(READ: app.js reads it numbered, and READ: app.js 1-80 just "
                                  "those lines.)"])

    def test_apply_patch_is_answered_with_what_to_use_instead(self):
        # gpt-oss's own edit tool, run as a command: not here, and said so.
        for line in ("apply_patch <<'PATCH'", "cd src && apply_patch <<'PATCH'"):
            with self.subTest(line):
                hint = cook.read_hint({"kind": "run", "argument": line},
                                      {"ok": False, "output": "apply_patch: command not found"})
                self.assertIn("There is no apply_patch here", hint)
                self.assertIn("EDIT: <path>", hint)
        self.assertEqual(cook.read_hint({"kind": "run", "argument": "make"}, {"ok": False, "output": ""}), "")
        self.assertEqual(cook.read_hint({"kind": "write", "argument": "apply_patch.py"}, {"ok": True}), "")

    def test_nothing_is_the_same_once_a_file_has_changed(self):
        core = ScriptedCore(["READ: a.py", "WRITE: a.py\n```\nx = 1\n```", "READ: a.py", "DONE: ok"],
                            stop_after=4)
        cook.run(cook.Core(core), params())
        self.assertEqual(core.ran.count(("read", "a.py")), 2)

    def test_a_command_that_may_change_things_makes_what_follows_new(self):
        # Run twice with nothing between, it says what it said; after an
        # install, it may say something else.
        core = ScriptedCore(["RUN: pytest", "RUN: pytest", "RUN: pip install pytest", "RUN: pytest",
                             "DONE: ok"], stop_after=5)
        cook.run(cook.Core(core), params())
        self.assertEqual(core.ran.count(("run", "pytest")), 2)
        self.assertIn("You already did exactly that", last_said(core.prompts[2]))

    def test_an_answer_out_of_sight_may_be_asked_for_again(self):
        # Five other reads later the first one's result has left the window.
        replies = ["READ: a.py"] + ["READ: f%d.py" % i for i in range(5)] + ["READ: a.py", "DONE: ok"]
        core = ScriptedCore(replies, stop_after=len(replies))
        cook.run(cook.Core(core), params())
        self.assertEqual(core.ran.count(("read", "a.py")), 2)

    def test_a_program_that_is_not_here_is_named_in_every_prompt_after(self):
        missing = {"ok": False, "output": "$ pytest\nsh: 1: pytest: not found\n\n(exit status 127)",
                   "summary": "$ pytest  (exit 127)"}
        core = ScriptedCore(["RUN: pytest", "READ: a.py", "DONE: ok"], stop_after=3,
                            results={"pytest": missing})
        cook.run(cook.Core(core), params())
        self.assertNotIn("Not installed here", core.prompts[0][0]["content"])
        self.assertIn("Not installed here, so do not run: pytest.", core.prompts[1][0]["content"])
        # What is on PATH is said from the start, as it is to a build's agents.
        self.assertIn("On PATH: python3, git.", core.prompts[0][0]["content"])


class AfterTheGoal(CookTest):
    """Once the goal as written is met, each piece of work starts from an aim,
    and pieces that change nothing are followed by a rest."""

    def test_the_next_piece_is_an_aim_chosen_within_the_goal(self):
        core = ScriptedCore(["WRITE: test_x.py\n```\nx = 1\n```", "DONE: added the docstrings",
                             "HANDOFF: give the docstring in test_x.py an example", "READ: test_x.py"],
                            stop_after=4)
        cook.run(cook.Core(core), params())
        asked = last_said(core.prompts[2])
        self.assertTrue(asked.startswith("That piece is done. The goal is met."))
        self.assertIn("do not rename, move or rewrite anything it does not ask about", asked)
        # The aim is in front of it for the whole piece, and the goal's own
        # prompt keeps it to what was asked.
        self.assertNotIn("Its aim:", core.prompts[1][0]["content"])
        self.assertIn("Its aim: the next most valuable improvement", core.prompts[2][0]["content"])
        self.assertIn("Its aim: give the docstring in test_x.py an example", core.prompts[3][0]["content"])
        self.assertIn("leave alone what it does not mention", core.prompts[0][0]["content"])

    def test_pieces_that_change_nothing_rest_longer_each_time_until_one_does(self):
        core = ScriptedCore(["WRITE: a.py\n```\nx = 1\n```", "DONE: wrote it",
                             "DONE: nothing worth changing", "DONE: still nothing",
                             "WRITE: a.py\n```\nx = 2\n```", "DONE: better", "DONE: nothing"],
                            stop_after=7)
        cook.run(cook.Core(core), params())
        rests = [s for s in notes(core.published[-1]) if s.startswith("that piece changed nothing")]
        self.assertEqual(rests, ["that piece changed nothing: the next starts in 1 minute",
                                 "that piece changed nothing: the next starts in 2 minutes"])
        self.assertEqual(len(self.rested), cook.REST_FIRST * 3)
        self.assertEqual(core.ran.count(("write", "a.py")), 2)

    def test_a_rest_is_never_longer_than_the_most(self):
        resting = cook.Cook(cook.Core(ScriptedCore([])), params())
        for _ in range(12):
            resting.piece_over()
        self.assertEqual(resting.resting, cook.REST_MOST)
        resting.changed = True
        resting.piece_over()
        self.assertEqual(resting.resting, 0)

    def test_stop_ends_a_rest_at_once(self):
        core = ScriptedCore(["DONE: nothing to do", "DONE: left it as it was"])
        pressed = []

        def press_stop_on_the_fifth_second(seconds):
            pressed.append(seconds)
            if len(pressed) == 5:
                core.stop_after = 0

        with mock.patch.object(cook, "pause", press_stop_on_the_fifth_second):
            cook.run(cook.Core(core), params())
        self.assertEqual(len(pressed), 5)
        self.assertEqual(core.published[-1]["state"], "stopped")


class Clip(unittest.TestCase):
    def test_cut_at_a_word(self):
        text = "We need to add a one-line docstring to each test in test_orbit.py, single line"
        out = cook.clip(text, 60)
        self.assertTrue(out.endswith("..."))
        self.assertTrue(text.startswith(out[:-3]))
        self.assertFalse(out[:-3].endswith("singl"))

    def test_short_text_is_untouched(self):
        self.assertEqual(cook.clip("  short  ", 60), "short")

    def test_a_line_break_is_somewhere_to_cut(self):
        # A thought that lists files a line at a time has no space to cut at.
        listing = "files:\n" + "\n".join("assets/icons/icon_%02d.svg" % i for i in range(10))
        kept = cook.clip(listing, 60)[:-3]
        self.assertTrue(listing.startswith(kept))
        self.assertEqual(listing[len(kept)], "\n")

    def test_a_run_with_no_break_is_cut_after_a_mark_rather_than_inside_a_name(self):
        text = 'Reading it: {"path":"/home/me/project/tests/test_orbit.py","start":1,"end":400}'
        kept = cook.clip(text, 60)[:-3]
        self.assertTrue(text.startswith(kept))
        self.assertFalse(text[len(kept) - 1].isalnum() and text[len(kept)].isalnum())

    def test_a_step_is_one_line(self):
        self.assertEqual(cook.line("a\n\n  b\tc", 60), "a b c")


if __name__ == "__main__":
    unittest.main()


class PublishingTest(unittest.TestCase):
    def test_only_new_steps_are_sent_and_a_lost_count_sends_again(self):
        class Core:
            def __init__(self, holds=None):
                self.calls = []
                self.holds = holds

            def call(self, method, params):
                self.calls.append(params)
                held = params["steps_from"] + len(params["cook"]["steps"])
                return {"steps": held if self.holds is None else self.holds}

        journal = {"goal": "g", "steps": [{"summary": "a"}, {"summary": "b"}]}
        core = Core()
        sent = cook.publish_journal(core, journal, 0)
        self.assertEqual(sent, 2)
        journal["steps"].append({"summary": "c"})
        sent = cook.publish_journal(core, journal, sent)
        self.assertEqual(core.calls[-1]["steps_from"], 2)
        self.assertEqual([s["summary"] for s in core.calls[-1]["cook"]["steps"]], ["c"])
        self.assertEqual(sent, 3)
        # The journal itself is left whole.
        self.assertEqual(len(journal["steps"]), 3)
        # A core that says it holds fewer is sent the rest next time.
        lost = Core(holds=1)
        sent = cook.publish_journal(lost, journal, 3)
        self.assertEqual(sent, 1)
        cook.publish_journal(Core(), journal, sent)
