# SPDX-License-Identifier: MIT
"""The build loop against a core that plays the architect and the agents from a script."""

import json
import os
import sys
import threading
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
                 can_make=False, make_ok=True, confirm_plan=False, git=True, repo=False, agents=1,
                 listing=".:\n  (empty)\n"):
        self.replies = list(replies)
        self.listing = listing
        self.agents = agents
        self.lock = threading.RLock()
        self.handles = 0
        self.held = {}        # seat handle -> expert
        self.released = []
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
                "rounds_per_task": 12, "confirm_plan": self.confirm_plan, "agents": self.agents}

    def spawn(self, target, *args):
        thread = threading.Thread(target=target, args=args, daemon=True)
        thread.start()
        return thread

    def flags(self):
        return self.call("engine.flags")

    def mood(self, mood, text="", linked=None):
        self.moods.append((mood, text))

    def call(self, method, params=None):
        with self.lock:
            return self.answer(method, params or {})

    def answer(self, method, params):
        if method == "engine.flags":
            stop = ((self.stop_after is not None and self.rounds >= self.stop_after)
                    or not self.replies)
            return {"stop": stop, "cancel": False, "running": True, "auto_edits": self.auto_edits}
        if method == "delegator.ready":
            return {"available": False}
        if method == "seat.take":
            self.seats.append(params["expert"])
            self.handles += 1
            handle = "s%d" % self.handles
            self.held[handle] = params["expert"]
            return {"ok": True, "seat": handle}
        if method == "seat.release":
            self.released.append(self.held.pop(params["seat"], None))
            return {}
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
                return {"ok": True, "output": self.listing, "summary": "listed .", "detail": "", "changed": []}
            if params["kind"] == "run":
                ok = self.check_ok if params["argument"] == PLAN["check"] else True
                return {"ok": ok, "output": "$ " + params["argument"] + "\n" + ("passed" if ok else "1 failed"),
                        "summary": "$ " + params["argument"], "detail": "", "changed": []}
            changed = [params["argument"]] if params["kind"] in ("write", "edit") else []
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
            self.committed_paths = getattr(self, "committed_paths", [])
            self.committed_paths.append(params.get("paths"))
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

    def test_asked_for_in_one_task_it_is_one_task(self):
        plan, _ = build.read_plan(plan_text())
        self.assertTrue(build.ONE_TASK.search("In one task, rewrite app.js"))
        self.assertTrue(build.ONE_TASK.search("do it as a single task please"))
        self.assertFalse(build.ONE_TASK.search("someone tasked with it"))
        merged = build.one_task(plan, "In one task, write the todo app")
        self.assertEqual(len(merged["tasks"]), 1)
        task = merged["tasks"][0]
        self.assertIn("- Write todo.py", task["detail"])
        self.assertIn("- Write the README", task["detail"])
        self.assertEqual(task["after"], [])
        self.assertEqual(merged["check"], plan["check"])
        files = [f for t in plan["tasks"] for f in t.get("files", [])]
        self.assertEqual(sorted(set(files)), sorted(task["files"]))

    def test_a_check_that_describes_rather_than_runs_is_no_check(self):
        said = json.dumps({"summary": "s", "run": "open index.html",
                           "check": "open index.html in a web browser and verify that the charts are drawn",
                           "ship": "", "tasks": [{"title": "a", "detail": "b", "after": []}]})
        plan, _ = build.read_plan(said)
        self.assertEqual(plan["check"], "")
        self.assertEqual(plan["run"], "open index.html")
        # Commands that only happen to share a word are commands.
        self.assertEqual(build.command_or_nothing("npm run verify"), "npm run verify")
        self.assertEqual(build.command_or_nothing("python -m pytest -q tests/test_should_work.py"),
                         "python -m pytest -q tests/test_should_work.py")

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
        # Programming is remote and did tasks 1 and 2; Writing is local, so
        # its work teaches nobody -- but who each task went to does.
        work = [r for r in core.records if r["expert"] not in ("architect", "delegator")]
        self.assertEqual([r["expert"] for r in work], ["programming", "programming"])
        self.assertEqual(work[0]["files"], ["todo.py"])
        routed = [r for r in core.records if r["expert"] == "delegator"]
        self.assertEqual([r["completion"] for r in routed], ["Programming", "Programming", "Writing"])
        self.assertTrue(routed[0]["prompt"].startswith("Python back end: Write todo.py"))
        # The plan a remote architect wrote, with the prompt it answered.
        planned = [r for r in core.records if r["expert"] == "architect"]
        self.assertEqual(len(planned), 1)
        self.assertIn("Write a plan as JSON", planned[0]["prompt"])
        self.assertIn('"tasks"', planned[0]["completion"])

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


class SideBySide(unittest.TestCase):
    """Agents working at once: tasks that do not wait on each other run
    together, and what must not overlap does not."""

    PLAN = {
        "summary": "Three independent pieces.", "run": "", "check": "", "ship": "",
        "tasks": [
            {"title": "Make a.py", "detail": "a", "files": ["a.py"], "needs": "python", "after": []},
            {"title": "Make b.py", "detail": "b", "files": ["b.py"], "needs": "python", "after": []},
            {"title": "Make c.md", "detail": "c", "files": ["c.md"], "needs": "documentation", "after": [0, 1]},
        ],
    }

    class Core(ScriptedCore):
        """Answers each agent from its own task, and records how many rounds
        were in flight at once."""

        def __init__(self, plan, seats, barrier=None, **kwargs):
            super().__init__([], **kwargs)
            self.plan = plan
            self.seat_info = seats
            self.barrier = barrier
            self.in_flight = 0
            self.most = 0
            self.turns = {}

        def call(self, method, params=None):
            params = params or {}
            if method == "seat.chat":
                return self.chat(params)
            with self.lock:
                if method == "engine.flags":
                    return {"stop": False, "cancel": False, "running": True, "auto_edits": True}
                return self.answer(method, params)

        def chat(self, params):
            system = params["messages"][0]["content"]
            if "Write a plan as JSON" in params["messages"][-1]["content"]:
                return {"answer": plan_text(self.plan), "reasoning": "", "ms": 1}
            if "The build is over" in params["messages"][-1]["content"]:
                return {"answer": "All three were made.", "reasoning": "", "ms": 1}
            number = int(system.split("Your task, ", 1)[1].split(" of", 1)[0])
            task = self.plan["tasks"][number - 1]
            with self.lock:
                self.in_flight += 1
                self.most = max(self.most, self.in_flight)
                turn = self.turns.get(task["title"], 0)
                self.turns[task["title"]] = turn + 1
            try:
                if turn == 0 and self.barrier is not None and task["after"] == []:
                    self.barrier.wait()   # both first tasks are mid-round at once, or this times out
                if turn == 0:
                    return {"answer": "WRITE: " + task["files"][0] + "\n```\nx\n```", "reasoning": "", "ms": 1}
                return {"answer": "DONE: made " + task["files"][0], "reasoning": "", "ms": 1}
            finally:
                with self.lock:
                    self.in_flight -= 1

    def run_build(self, core, seats):
        out = params(core, seats=seats)
        build.Build(core, out).run()
        return core.published[-1]

    def test_independent_tasks_run_at_the_same_time(self):
        seats = {"programming": {"model": True, "remote": True, "local": ""},
                 "writing": {"model": True, "remote": True, "local": ""}}
        core = self.Core(self.PLAN, seats, barrier=threading.Barrier(2, timeout=5), agents=3)
        journal = self.run_build(core, seats)
        self.assertEqual([t["state"] for t in journal["tasks"]], ["done", "done", "done"])
        self.assertEqual(core.most, 2)
        # The third came after both, and its own files are all it committed.
        self.assertEqual(core.committed_paths, [["a.py"], ["b.py"], ["c.md"]] if
                         core.commits[0].startswith("Make a") else [["b.py"], ["a.py"], ["c.md"]])
        # Every seat taken was handed back.
        self.assertEqual(core.held, {})

    def test_one_model_on_this_machine_takes_one_agent_at_a_time(self):
        seats = {"programming": {"model": True, "remote": False, "local": "/m/coder.gguf"},
                 "writing": {"model": True, "remote": False, "local": "/m/coder.gguf"}}
        core = self.Core(self.PLAN, seats, agents=3)
        journal = self.run_build(core, seats)
        self.assertEqual([t["state"] for t in journal["tasks"]], ["done", "done", "done"])
        self.assertEqual(core.most, 1)

    def test_tasks_that_share_a_file_wait_for_each_other(self):
        plan = json.loads(json.dumps(self.PLAN))
        plan["tasks"][1]["files"] = ["a.py"]
        seats = {"programming": {"model": True, "remote": True, "local": ""},
                 "writing": {"model": True, "remote": True, "local": ""}}
        core = self.Core(plan, seats, agents=3)
        journal = self.run_build(core, seats)
        self.assertEqual([t["state"] for t in journal["tasks"]], ["done", "done", "done"])
        self.assertEqual(core.most, 1)

    def test_one_agent_is_the_old_build_exactly(self):
        seats = {"programming": {"model": True, "remote": True, "local": ""},
                 "writing": {"model": True, "remote": True, "local": ""}}
        core = self.Core(self.PLAN, seats, agents=1)
        self.run_build(core, seats)
        self.assertEqual(core.most, 1)
        # With one agent the whole tree is committed, as before.
        self.assertEqual(core.committed_paths, [None, None, None])


class SmallChanges(unittest.TestCase):
    """"Make the button blue" on a project that exists: one task, no plan to
    agree, and the agent's own account as the outcome."""

    ONE = {"summary": "Make the button blue.", "run": "", "check": "", "ship": "",
           "tasks": [{"title": "Make the button blue", "detail": "In style.css, .add { color: blue }",
                      "files": ["style.css"], "needs": "CSS", "after": []}]}

    def test_a_one_task_change_is_not_put_to_the_person_and_needs_no_write_up(self):
        core = ScriptedCore([plan_text(self.ONE),
                             "EDIT: style.css\n<<<<<<< SEARCH\ncolor: red;\n=======\ncolor: blue;\n>>>>>>> REPLACE",
                             "DONE: the button is blue now", "(unused)"],
                            confirm_plan=True, listing=".:\n  index.html\n  style.css\n  app.js\n")
        build.Build(core, params(core, directive="make the button blue")).run()
        journal = core.published[-1]
        self.assertEqual(journal["state"], "done")
        self.assertEqual(journal["outcome"], "the button is blue now")
        # No question asked, and the three rounds were the plan, the edit and DONE.
        self.assertFalse(any(step["kind"] == "ask" for step in journal["steps"]))
        self.assertEqual(core.rounds, 3)

    def test_the_architect_is_told_how_to_make_a_program_for_a_computer(self):
        prompt = build.plan_prompt("a budgeting app with charts", "/p", "It is empty.")
        self.assertIn("window.crucible.save(data)", prompt)
        self.assertIn("ONE task", prompt)
        system = build.task_system_prompt("", "d", {"summary": ""}, {"title": "t", "detail": "d", "files": []},
                                          0, 1, "/p", "", "")
        self.assertIn("window.crucible.load()", system)
        self.assertIn("EDIT", system)


class Mixing(unittest.TestCase):
    """Frontier models for the large tasks, local ones for the small, when the
    roster has both."""

    def build_with(self, seats, size, chosen):
        core = ScriptedCore([])
        out = params(core, seats=seats)
        made = build.Build(core, out)
        decision = build.routing.Decision(chosen, 0.9, build.routing.KEYWORD, "keywords")
        task = {"title": "t", "detail": "d", "size": size, "needs": ""}
        return made.mixed(decision, task, "work")

    def test_a_large_task_goes_to_a_providers_model_and_a_small_one_stays_here(self):
        seats = {"programming": {"model": True, "remote": True}, "writing": {"model": True, "remote": False,
                                                                             "local": "/m/coder.gguf"}}
        self.assertEqual(self.build_with(seats, "large", "writing").expert, "programming")
        self.assertEqual(self.build_with(seats, "small", "programming").expert, "writing")
        # What already fits is left as it is.
        self.assertEqual(self.build_with(seats, "large", "programming").expert, "programming")
        self.assertEqual(self.build_with(seats, "", "programming").expert, "programming")

    def test_with_only_one_kind_there_is_nothing_to_mix(self):
        seats = {"programming": {"model": True, "remote": False}, "writing": {"model": True, "remote": False}}
        self.assertEqual(self.build_with(seats, "large", "writing").expert, "writing")

    def test_the_plan_says_how_large_each_task_is(self):
        plan, why = build.read_plan(json.dumps({"tasks": [
            {"title": "core", "detail": "d", "size": "LARGE"},
            {"title": "docs", "detail": "d", "size": "tiny"}]}))
        self.assertEqual([t["size"] for t in plan["tasks"]], ["large", ""])


class Notes(unittest.TestCase):
    def test_an_agent_that_only_writes_notes_is_told_to_act_and_then_stopped(self):
        notes = ["NOTE: still thinking"] * (build.IDLE_LIMIT + 2)
        core = ScriptedCore([plan_text({"summary": "", "run": "", "check": "", "ship": "", "tasks": [
            {"title": "Make a.py", "detail": "a", "files": ["a.py"], "needs": "python", "after": []}]})] + notes + ["x"])
        build.Build(core, params(core)).run()
        journal = core.published[-1]
        self.assertEqual(journal["tasks"][0]["outcome"], "the agent kept answering without acting")
        # Each note was answered with the push to act.
        self.assertIn("A NOTE does nothing", core.prompts[2][-1]["content"])

    def test_a_write_after_a_note_in_one_reply_is_done(self):
        core = ScriptedCore([plan_text({"summary": "", "run": "", "check": "", "ship": "", "tasks": [
            {"title": "Make a.py", "detail": "a", "files": ["a.py"], "needs": "python", "after": []}]}),
            "NOTE: starting\nWRITE: a.py\n```\nprint(1)\n```", "DONE: made a.py", "x"])
        build.Build(core, params(core)).run()
        self.assertIn(("write", "a.py"), core.ran)
        self.assertEqual(core.published[-1]["tasks"][0]["state"], "done")
