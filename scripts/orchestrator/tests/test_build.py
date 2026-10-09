# SPDX-License-Identifier: MIT
"""The build loop against a core that plays the architect and the agents from a script."""

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))
sys.path.insert(0, os.path.dirname(__file__))   # for test_cook's roster and parser

from crucible_orchestrator import build  # noqa: E402
from test_cook import parse  # noqa: E402

ROSTER = [
    {"id": "programming", "name": "Programming", "tag": "PROG", "blurb": "code",
     "keywords": ["code", "python", "test", "tests", "pytest", "debugging"], "examples": []},
    {"id": "writing", "name": "Writing", "tag": "WRIT", "blurb": "docs",
     "keywords": ["documentation", "readme", "writing"], "examples": []},
]

PLAN = {
    "summary": "A tiny Python CLI with a test.",
    "run": "python todo.py",
    "check": "python -m pytest -q",
    "ship": "",
    "tasks": [
        {"title": "Write todo.py", "detail": "A CLI that adds and lists items.", "files": ["todo.py"],
         "needs": "Python back end", "after": []},
        {"title": "Write the tests", "detail": "pytest for add and list.", "files": ["test_todo.py"],
         "needs": "testing", "after": [0]},
        {"title": "Write the README", "detail": "How to run it.", "files": ["README.md"],
         "needs": "technical writing", "after": [0, 1]},
    ],
}


class ScriptedCore:
    """The core, with every model reply taken from a list, one per round.

    Routing is by keywords (there is no delegator), so "Python back end" lands
    on Programming and "technical writing" on Writing, which is the roster
    test_cook.py uses.
    """

    def __init__(self, replies, stop_after=None, auto_edits=True, answers=None, check_ok=True,
                 can_make=False, make_ok=True, confirm_plan=False, git=True, repo=False):
        self.replies = list(replies)
        self.rounds = 0
        self.stop_after = stop_after
        self.auto_edits = auto_edits
        self.answers = list(answers or [])
        self.check_ok = check_ok
        self.can_make = can_make
        self.make_ok = make_ok
        self.confirm_plan = confirm_plan
        self.git = git
        self.repo = repo
        self.published = []
        self.seats = []
        self.prompts = []
        self.ran = []
        self.made = []
        self.commits = []
        self.records = []
        self.moods = []

    def settings(self):
        return {"architect": "", "can_make": self.can_make, "auto_commit": True,
                "rounds_per_task": 12, "confirm_plan": self.confirm_plan}

    def flags(self):
        return self.call("engine.flags")

    def mood(self, mood, text="", linked=None):
        self.moods.append((mood, text))

    def call(self, method, params=None):
        params = params or {}
        if method == "engine.flags":
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
            reply = self.replies.pop(0) if self.replies else "DONE: nothing more"
            return {"answer": reply, "reasoning": "", "ms": 3}
        if method == "tools.parse":
            return parse(params["answer"])
        if method == "tools.run":
            self.ran.append((params["kind"], params["argument"]))
            if params["kind"] == "list":
                return {"ok": True, "output": ".:\n  (empty)\n", "summary": "listed .", "detail": "", "changed": []}
            if params["kind"] == "run":
                ok = self.check_ok if params["argument"] == PLAN["check"] else True
                return {"ok": ok, "output": "$ " + params["argument"] + "\n" + ("passed" if ok else "1 failed"),
                        "summary": "$ " + params["argument"], "detail": "", "changed": []}
            changed = [params["argument"]] if params["kind"] == "write" else []
            return {"ok": True, "output": "ran " + params["kind"], "summary": params["kind"] + " "
                    + params["argument"], "detail": "", "changed": changed}
        if method == "edit.ask":
            return {"approved": True}
        if method == "cook.await_answer":
            return {"answer": self.answers.pop(0) if self.answers else "go"}
        if method == "cook.publish":
            self.published.append(json.loads(json.dumps(params["cook"])))
            return {}
        if method == "roster.make":
            if not self.make_ok:
                return {"ok": False, "error": "no model for new agents"}
            made = params["name"].lower().replace(" ", "-")
            self.made.append(params["name"])
            return {"ok": True, "id": made, "name": params["name"], "tag": "NEW", "remote": True,
                    "keywords": [w for w in params["name"].lower().split()]}
        if method == "git.ready":
            return {"git": self.git, "repo": self.repo}
        if method == "git.init":
            self.repo = True
            return {"ok": True}
        if method == "git.commit":
            self.commits.append(params["message"])
            return {"ok": True, "summary": "committed abc1234 " + params["message"].split("\n")[0]}
        if method == "teach.record":
            self.records.append(params)
            return {}
        if method == "machine.facts":
            return {"os": "macOS", "shell": "/bin/sh", "programs": ["python3", "git"]}
        if method in ("mood", "seat.attachments"):
            return {"text": ""}
        raise AssertionError("unexpected call " + method)


def params(core, **extra):
    out = {"id": "b1", "directive": "a todo CLI in Python with tests", "root": "/p",
           "roster": json.loads(json.dumps(ROSTER)),
           "seats": {"programming": {"model": True, "remote": True}, "writing": {"model": True}},
           "routing": {}, "system_prompt": "You are helpful.", "tools": "\n\nTOOLS",
           "settings": core.settings()}
    out.update(extra)
    return out


def plan_text(plan=PLAN):
    return "Here is the plan:\n```json\n" + json.dumps(plan) + "\n```"


class ReadingThePlan(unittest.TestCase):
    def test_json_is_found_inside_prose_and_fences(self):
        plan, why = build.read_plan(plan_text())
        self.assertEqual(why, "")
        self.assertEqual([t["title"] for t in plan["tasks"]], ["Write todo.py", "Write the tests", "Write the README"])
        self.assertEqual(plan["check"], "python -m pytest -q")

    def test_a_trailing_comma_is_forgiven(self):
        text = '{"summary": "s", "tasks": [{"title": "a", "detail": "b", "after": [],},]}'
        plan, why = build.read_plan(text)
        self.assertIsNotNone(plan, why)

    def test_no_tasks_is_no_plan(self):
        self.assertEqual(build.read_plan("I would start by...")[0], None)
        self.assertEqual(build.read_plan('{"summary": "x", "tasks": []}')[0], None)

    def test_bad_dependencies_are_dropped(self):
        plan, _ = build.read_plan(json.dumps({"tasks": [
            {"title": "a", "detail": "a", "after": [1, 7, -1, 0]},
            {"title": "b", "detail": "b", "after": [0]},
        ]}))
        # a waits on b and b on a: the back edge goes, so the order is an order.
        self.assertEqual(plan["tasks"][1]["after"], [0])
        self.assertEqual(plan["tasks"][0]["after"], [])
        self.assertEqual(build.order_tasks(plan["tasks"]), [0, 1])

    def test_too_many_tasks_are_cut(self):
        many = {"tasks": [{"title": "t%d" % i, "detail": "d"} for i in range(30)]}
        plan, _ = build.read_plan(json.dumps(many))
        self.assertEqual(len(plan["tasks"]), build.MAX_TASKS)
        self.assertIn("30 tasks", plan["summary"])

    def test_order_honors_after(self):
        order = build.order_tasks([{"after": [2]}, {"after": []}, {"after": [1]}])
        self.assertEqual(order, [1, 2, 0])


class TheLoop(unittest.TestCase):
    def test_a_build_plans_runs_each_task_checks_and_reviews(self):
        core = ScriptedCore([
            plan_text(),                                     # the architect
            "WRITE: todo.py\n```\nprint('hi')\n```", "DONE: wrote todo.py",     # task 1
            "WRITE: test_todo.py\n```\ndef test(): pass\n```", "DONE: tests in",  # task 2
            "WRITE: README.md\n```\n# todo\n```", "DONE: readme",                 # task 3
            "It builds a todo CLI. Run it with python todo.py.",                # the review
        ])
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["kind"], "build")
        self.assertEqual(journal["state"], "done")
        self.assertEqual([t["state"] for t in journal["tasks"]], ["done", "done", "done"])
        self.assertEqual([t["expert"] for t in journal["tasks"]], ["programming", "programming", "writing"])
        self.assertEqual(journal["plan"]["run"], "python todo.py")
        # The check ran, after the tasks.
        self.assertIn(("run", PLAN["check"]), core.ran)
        self.assertEqual(journal["outcome"], "It builds a todo CLI. Run it with python todo.py.")
        # Every step says which task it belonged to; the plan's says none.
        kinds = [(s["kind"], s["task"]) for s in journal["steps"]]
        self.assertIn(("plan", -1), kinds)
        self.assertIn(("write", 0), kinds)
        self.assertIn(("done", 2), kinds)
        self.assertIn(("review", -1), kinds)
        # A task's agent is told what the others did.
        third = core.prompts[5][0]["content"]
        self.assertIn("task 1, Write todo.py (done): wrote todo.py", third)
        self.assertIn("Your task, 3 of 3", third)

    def test_each_finished_task_is_committed_starting_the_repository_first(self):
        core = ScriptedCore([plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "all done"], repo=False)
        build.run(core, params(core))
        self.assertEqual(core.commits[0].split("\n")[0], "Write todo.py")
        self.assertEqual(len(core.commits), 3)
        self.assertIn("started a git repository", [s["summary"] for s in core.published[-1]["steps"]])

    def test_without_git_nothing_is_committed_and_it_is_said_once(self):
        core = ScriptedCore([plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "all done"], git=False)
        build.run(core, params(core))
        self.assertEqual(core.commits, [])
        said = [s["summary"] for s in core.published[-1]["steps"] if "git is not installed" in s["summary"]]
        self.assertEqual(len(said), 1)

    def test_a_remote_agents_work_is_recorded_for_teaching(self):
        core = ScriptedCore([plan_text(), "WRITE: todo.py\n```\nx\n```", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"])
        build.run(core, params(core))
        # Programming is remote and did tasks 1 and 2; Writing is local.
        self.assertEqual([r["expert"] for r in core.records], ["programming", "programming"])
        self.assertEqual(core.records[0]["files"], ["todo.py"])

    def test_a_failed_check_adds_a_fix_task_and_checks_again(self):
        core = ScriptedCore([plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c",
                             "READ: test_todo.py", "DONE: fixed the import", "reviewed"],
                            check_ok=False)
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][3]["title"], "Make the check pass")
        self.assertEqual(journal["tasks"][3]["state"], "done")
        self.assertIn("1 failed", journal["tasks"][3]["detail"])
        self.assertEqual([r for r in core.ran if r == ("run", PLAN["check"])].__len__(), 2)

    def test_a_task_that_runs_out_of_rounds_is_incomplete_and_the_build_goes_on(self):
        # Twelve different reads: busy, never the same thing twice, never done.
        core = ScriptedCore([plan_text()] + ["READ: part%d.py" % i for i in range(12)]
                            + ["DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"])
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][0]["state"], "incomplete")
        self.assertIn("rounds", journal["tasks"][0]["outcome"])
        self.assertEqual(journal["tasks"][1]["state"], "done")
        self.assertEqual(journal["state"], "done")

    def test_a_task_whose_dependency_failed_is_skipped(self):
        core = ScriptedCore([plan_text()] + ["I would rather not."] * build.IDLE_LIMIT + ["DONE: b", "DONE: b", "r"])
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][0]["state"], "incomplete")
        # Task 2 waits on 1 only; incomplete still counts as done enough to go on.
        self.assertEqual(journal["tasks"][1]["state"], "done")

    def test_stop_mid_task_makes_a_finishing_pass_and_skips_the_rest(self):
        core = ScriptedCore([plan_text(), "READ: todo.py", "READ: todo.py",
                             "DONE: left it running"], stop_after=3)
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["state"], "stopped")
        self.assertEqual(journal["tasks"][0]["state"], "stopped")
        self.assertEqual([t["state"] for t in journal["tasks"][1:]], ["skipped", "skipped"])
        self.assertEqual(journal["outcome"], "left it running")

    def test_a_question_in_a_task_is_answered(self):
        core = ScriptedCore([plan_text(), "ASK: sqlite or json?", "DONE: json", "DONE: json", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"],
                            answers=["json"])
        build.run(core, params(core))
        self.assertIn({"role": "user", "content": "The user answered: json"}, core.prompts[2])

    def test_the_plan_is_put_to_the_person_first_when_asked(self):
        core = ScriptedCore([plan_text(), plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"],
                            confirm_plan=True, answers=["use sqlite instead", "go"])
        build.run(core, params(core))
        # Asked twice: once with the first plan, once with the revised one.
        asked = [s["summary"] for s in core.published[-1]["steps"] if s["kind"] == "ask"]
        self.assertEqual(len(asked), 2)
        self.assertIn("use sqlite instead", core.prompts[1][-1]["content"])
        self.assertEqual(core.published[-1]["state"], "done")

    def test_an_unusable_plan_is_asked_for_again_then_becomes_one_task(self):
        core = ScriptedCore(["Sure! First we should...", "still prose", "DONE: did it all", "r"])
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(len(journal["tasks"]), 1)
        self.assertEqual(journal["tasks"][0]["detail"], "a todo CLI in Python with tests")
        self.assertIn("Reply with only the JSON", core.prompts[1][-1]["content"])
        self.assertEqual(journal["state"], "done")

    def test_a_seat_is_made_when_nobody_fits_and_a_model_is_configured(self):
        plan = json.loads(json.dumps(PLAN))
        plan["tasks"][1] = {"title": "Design the schema", "detail": "tables for items and tags",
                            "files": ["schema.sql"], "needs": "SQL schema", "after": [0]}
        core = ScriptedCore([plan_text(plan), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"], can_make=True)
        build.run(core, params(core))
        self.assertEqual(core.made, ["SQL Schema"])
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][1]["expert"], "sql-schema")
        self.assertTrue(any(s["summary"].startswith("added SQL Schema to the experts")
                            for s in journal["steps"]))

    def test_no_seat_is_made_when_the_delegator_found_one(self):
        core = ScriptedCore([plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"], can_make=True)
        build.run(core, params(core))
        self.assertEqual(core.made, [])

    def test_a_handoff_inside_a_task_changes_who_holds_it(self):
        core = ScriptedCore([plan_text(), "HANDOFF: write the documentation for this", "DONE: wrote it",
                             "DONE: wrote it", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"])
        build.run(core, params(core))
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][0]["expert"], "writing")
        self.assertIn("Programming handed task 1 to Writing", [s["summary"] for s in journal["steps"]])

    def test_done_without_writing_is_refused_once_when_files_were_expected(self):
        core = ScriptedCore([plan_text(), "DONE: wrote todo.py", "WRITE: todo.py\n```\nx\n```", "DONE: now it is",
                             "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"])
        build.run(core, params(core))
        journal = core.published[-1]
        # The first DONE was sent back; the write that followed was accepted.
        self.assertIn("Nothing has been written yet", core.prompts[2][-1]["content"])
        self.assertIn(("write", "todo.py"), core.ran)
        self.assertEqual(journal["tasks"][0]["state"], "done")
        # A second DONE with still nothing written is taken at its word.
        self.assertEqual(journal["tasks"][1]["state"], "done")
        self.assertEqual(sum(1 for s in journal["steps"] if "said DONE without writing" in s["summary"]), 3)

    def test_the_prompts_say_what_the_machine_has(self):
        core = ScriptedCore([plan_text(), "DONE: a", "DONE: a", "DONE: b", "DONE: b", "DONE: c", "DONE: c", "r"])
        build.run(core, params(core))
        self.assertIn("There is no `python`: use `python3`", core.prompts[0][-1]["content"])
        self.assertIn("On PATH: python3, git", core.prompts[1][0]["content"])

    def test_no_expert_with_a_model_fails_at_once(self):
        core = ScriptedCore([])
        build.run(core, params(core, seats={}))
        self.assertEqual(core.published[-1]["state"], "failed")
        self.assertEqual(core.rounds, 0)


if __name__ == "__main__":
    unittest.main()
