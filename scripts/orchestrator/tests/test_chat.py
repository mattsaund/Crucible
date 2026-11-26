# SPDX-License-Identifier: MIT
"""A chat turn against a core that plays the expert from a script.

The core here keeps a transcript the way the real one does -- a round's words
streamed onto the reply, a paragraph before them when the turn had said
something already, actions above it -- so what a turn leaves on screen can be
read back and checked, along with what the expert was sent each round.
tests/test_chat.cpp walks the same turns through the real core.
"""

import json
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from crucible_orchestrator import chat, rpc  # noqa: E402

ROSTER = [
    {"id": "programming", "name": "Programming", "tag": "PROG", "blurb": "code",
     "keywords": ["python"], "examples": []},
    {"id": "writing", "name": "Writing", "tag": "WRIT", "blurb": "docs",
     "keywords": ["readme"], "examples": []},
]

VERBS = ("LIST", "READ", "WRITE", "EDIT", "RUN", "NOTE", "DONE", "SEARCH", "SCREENSHOT", "HANDOFF")


def parse(answer):
    """A small stand-in for the core's parser: `VERB: argument`, a fence
    after WRITE, and a SEARCH line as a search."""
    match = re.search(r"^(%s): ?(.*)$" % "|".join(VERBS), answer, re.M)
    search = re.search(r"^SEARCH: ?(.*)$", answer, re.M)
    if not match:
        near = re.search(r"^(%s) \S" % "|".join(VERBS), answer, re.M)
        inside = re.search(r"^.*\S (WRITE|EDIT): ?\S.*\n(```|<{5,9} ?SEARCH)", answer, re.M)
        if not near and inside:
            return {"call": None, "attempted": inside.group(1).lower(), "inline": True, "search": ""}
        return {"call": None, "attempted": near.group(1).lower() if near else "none", "search": ""}
    content = ""
    if match.group(1) == "WRITE":
        fence = re.search(r"```\n(.*?)```", answer, re.S)
        content = fence.group(1) if fence else ""
    return {"call": {"kind": match.group(1).lower(), "argument": match.group(2).strip(),
                     "content": content, "shell": "", "unrun": ""},
            "attempted": "none", "search": search.group(1).strip() if search else ""}


def native(name, arguments):
    """A small stand-in for tools::call_from_native."""
    given = json.loads(arguments or "{}")
    if name == "web_search":
        return {"call": None, "attempted": "none", "search": given["query"]}
    kinds = {"read_file": ("read", "path"), "write_file": ("write", "path"), "list_files": ("list", "path"),
             "run_command": ("run", "command"), "hand_off": ("handoff", "next_step")}
    if name not in kinds:
        return {"call": None, "error": "there is no tool called " + name, "attempted": "none", "search": ""}
    kind, field = kinds[name]
    if field not in given:
        return {"call": None, "error": name + " needs " + field, "attempted": "none", "search": ""}
    return {"call": {"kind": kind, "argument": given[field], "content": given.get("content", ""),
                     "shell": "", "unrun": ""}, "attempted": "none", "search": ""}


def calling(name, answer="", **arguments):
    """A round that called a tool in the model's own format."""
    return said(answer, output_tokens=5,
                calls=[{"id": "call_" + name, "name": name, "arguments": json.dumps(arguments)}])


def said(answer="", **extra):
    """One round's reply, as the script gives it."""
    out = {"answer": answer, "reasoning": "", "error": "", "discard": False, "notes": [],
           "canceled": False, "hit_limit": False, "output_tokens": max(1, len(answer) // 4)}
    out.update(extra)
    return out


class ScriptedCore:
    """The core, with the expert's rounds taken from a list."""

    def __init__(self, rounds, seat=None, has_model=True, cancel=False, auto_edits=False,
                 approve=True, used=100, size=1000, dropped=0, results=1, pictures="",
                 sees=True):
        self.rounds = list(rounds)
        self.seat = {"ok": True, "remote": False, "sees_images": True, "takes_effort": False,
                     "workshop": True, "tools": "\n\nTOOLS"}
        self.seat.update(seat or {})
        self.has_model = has_model
        self.cancel = cancel
        self.auto_edits = auto_edits
        self.approve = approve
        self.used = used
        self.size = size
        self.dropped = dropped
        self.results = results
        self.pictures = pictures
        self.sees = sees
        # What the turn has done, as the core would keep it.
        self.reply = ""
        self.actions = []
        self.ended = None
        self.calls = []        # every method, in order
        self.moods = []
        self.sent = []         # the turn's own messages, each round
        self.systems = []      # each round's own instructions, or None for the turn's
        self.paragraphs = []   # whether each round began with a paragraph
        self.ran = []
        self.asked_edit = []
        self.searched = []
        self.routed = None
        self.routes = []       # every expert seated, in order
        self.loads = 0
        self.opens = 0
        self.quiet = []        # the checkpoint rounds' messages, never shown
        self.notes = []        # the account each round was given beside the request
        self.offered = []      # whether each round was offered its tools
        self.opened = None
        self.fitted = None
        self.replies_during_asks = []
        self.refuse = {}       # what an expert's load says, by id, for one that fails
        self.round_params = [] # everything each round was asked with

    def call(self, method, params=None):
        params = params or {}
        self.calls.append(method)
        if method.startswith("turn."):
            assert params.get("turn") == 3, method + " did not name the turn"
        if method == "delegator.ready":
            return {"available": False}
        if method == "delegator.done":
            return {}
        if method == "engine.flags":
            return {"stop": False, "cancel": self.cancel, "running": True, "auto_edits": self.auto_edits}
        if method == "mood":
            self.moods.append((params["mood"], params["text"]))
            return {}
        if method == "turn.route":
            self.routed = params["decision"]
            self.routes.append(params["decision"]["expert"])
            return {"has_model": self.has_model}
        if method == "turn.load":
            self.loads += 1
            refused = self.refuse.get(self.routed["expert"]) if self.routed else None
            if refused:
                return {"ok": False, "error": refused}
            return dict(self.seat)
        if method == "turn.open":
            self.opened = params
            self.opens += 1
            return {"used": self.used, "size": self.size}
        if method == "turn.fit":
            self.fitted = params
            return {"dropped": self.dropped}
        if method == "turn.round":
            self.round_params.append(dict(params))
            if params.get("quiet"):
                # The checkpoint's round: what it says never reaches the reply.
                self.quiet.append([dict(m) for m in params["messages"]])
                out = self.rounds.pop(0) if self.rounds else said("(out of script)")
                return dict(out, reply=self.reply)
            self.sent.append([dict(m) for m in params["messages"]])
            self.systems.append(params.get("system"))
            self.notes.append(params.get("note", ""))
            self.offered.append(params.get("tools", True))
            self.paragraphs.append(params["paragraph"])
            out = self.rounds.pop(0) if self.rounds else said("(out of script)")
            if out.get("looped"):
                # Streamed and then stopped: the turn takes it off again.
                self.reply += ("\n\n" if params["paragraph"] else "") + out["answer"]
                return dict(out, reply=self.reply)
            if out["answer"] and not out.get("discard"):
                if params["paragraph"]:
                    self.reply += "\n\n"
                self.reply += out["answer"]
            out = dict(out)
            out["reply"] = self.reply
            return out
        if method == "tools.parse":
            return parse(params["answer"] or params["reasoning"])
        if method == "tools.native":
            return native(params["name"], params["arguments"])
        if method == "turn.ask_edit":
            self.asked_edit.append(params["call"]["argument"])
            self.reply = params["reply"]
            self.replies_during_asks.append(self.reply)
            return {"approved": self.approve}
        if method == "turn.tool":
            call = params["call"]
            self.ran.append((call["kind"], call["argument"]))
            return {"ok": True, "output": "ran " + call["kind"] + " " + call["argument"],
                    "summary": call["kind"] + " " + call["argument"], "detail": "the diff",
                    "changed": [], "picture": "", "pictures": self.pictures, "sees": self.sees}
        if method == "turn.search":
            self.searched.append(params["query"])
            if not self.results:
                return {"count": 0, "error": 'nothing found for "%s"' % params["query"],
                        "provider": "searxng", "text": "nothing for " + params["query"]}
            return {"count": self.results, "error": "", "provider": "searxng",
                    "text": "results for " + params["query"]}
        if method in ("turn.show", "turn.end"):
            self.actions.extend(a["summary"] for a in params.get("actions", []))
            if "reply" in params:
                self.reply = params["reply"]
            if method == "turn.end":
                self.ended = params
            return {}
        raise AssertionError("unexpected call " + method)


def saying_what_changed(core):
    """`core`, with a write or an edit saying which file it changed, as the real one does."""
    original = core.call

    def call(method, params=None):
        out = original(method, params)
        if method == "turn.tool" and params["call"]["kind"] in ("write", "edit"):
            out = dict(out, changed=[params["call"]["argument"]])
        return out
    core.call = call
    return core


def params(**extra):
    out = {"turn": 3, "prompt": "What is in notes.txt?", "pinned": "", "attachments": [],
           "roster": ROSTER,
           "seats": {"programming": {"model": True}, "writing": {"model": True}},
           "routing": {}, "system_prompt": "You are helpful.", "effort": "", "web_search": False,
           "search_rounds": 2, "search_tools": "\n\nSEARCH TOOLS", "overflow": "rolling"}
    out.update(extra)
    return out


def run(core, **extra):
    chat.turn(core, params(**extra))
    return core


class ASeatThatWillNotLoad(unittest.TestCase):
    TOO_BIG = "flint.gguf would put 12.3 GB on an RTX 3060, which has 11.5 GB free"

    def core(self, *rounds):
        core = ScriptedCore(list(rounds) or [said("Done.")])
        return core

    def test_the_next_expert_takes_the_prompt_and_the_transcript_says_why(self):
        core = self.core(said("Answered by the other one."))
        core.refuse = {"programming": self.TOO_BIG}
        run(core, prompt="write some python")
        self.assertEqual(core.routes, ["programming", "writing"])
        self.assertEqual(core.loads, 2)
        self.assertEqual(core.ended["state"], "finished")
        self.assertEqual(core.reply, "Answered by the other one.")
        self.assertIn("Programming could not load, so Writing takes this", core.actions)

    def test_with_nobody_left_the_turn_fails_with_the_first_experts_reason(self):
        core = self.core()
        core.refuse = {"programming": self.TOO_BIG, "writing": "writing.gguf is not there"}
        run(core, prompt="write some python")
        self.assertEqual(core.ended["state"], "failed")
        self.assertEqual(core.ended["reason"], self.TOO_BIG)

    def test_a_prompt_sent_to_an_expert_by_name_stays_with_it(self):
        core = self.core()
        core.refuse = {"programming": self.TOO_BIG}
        run(core, pinned="programming")
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.ended["state"], "failed")
        self.assertEqual(core.ended["reason"], self.TOO_BIG)

    def test_a_load_the_person_stopped_is_not_passed_to_anybody(self):
        core = self.core()
        core.refuse = {"programming": "stopped"}
        run(core, prompt="write some python")
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.ended["state"], "canceled")


class TheCheckBeforeItEnds(unittest.TestCase):
    """A prompt that wants something made or changed is asked once more if it is finished."""

    def test_what_counts_as_asking_for_work(self):
        for prompt in ("Make the page heading blue and center it.",
                       "Add a monthly summary page, then write a short note in README.md.",
                       "The Net total is wrong. Find out why and fix it.",
                       "Can you add a delete button?",
                       "please rename app.js"):
            self.assertTrue(chat.asks_for_work(prompt), prompt)
        for prompt in ("What does byMonth in app.js return?",
                       "How do I add a button in HTML?",
                       "Explain how to fix a merge conflict.",
                       "why did the build fail",
                       "Thanks!", "", None):
            self.assertFalse(chat.asks_for_work(prompt), prompt)

    def test_a_question_is_answered_with_no_check(self):
        core = run(ScriptedCore([said("It says hello.")]))
        self.assertEqual(core.quiet, [])
        self.assertEqual(core.reply, "It says hello.")

    def test_work_that_is_done_says_so_and_the_answer_stands(self):
        checked = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("Made a.txt."),
                                    said("All requirements have been met. DONE")], auto_edits=True))
        self.assertEqual(checked.reply, "Made a.txt.")
        core = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("Made a.txt."), said("DONE")],
                                auto_edits=True), prompt="Make a.txt")
        self.assertEqual(len(core.quiet), 1)
        self.assertIn(chat.REVIEW.format("Make a.txt"), core.quiet[0][-1]["content"])
        self.assertEqual(core.reply, "Made a.txt.")
        self.assertEqual(core.ended["state"], "finished")
        self.assertTrue(core.ended["remember"])

    def test_the_word_the_check_asks_for_stays_out_of_the_answer(self):
        core = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("Made a.txt."), said("READ: a.txt"),
                                 said("Made a.txt, and it is right. All requirements have been met. DONE")],
                                auto_edits=True), prompt="Make a.txt")
        self.assertEqual(core.reply, "Made a.txt, and it is right. All requirements have been met.")
        for kept in ("Its status is now DONE", "Set the flag to DONE."):
            self.assertEqual(chat.TRAILING_DONE.sub("", kept), kept)

    def test_a_change_only_described_is_made_when_it_is_checked(self):
        core = run(saying_what_changed(ScriptedCore([said("Change it like this:\n```\nh1 { color: blue }\n```"),
                                                     said("WRITE: style.css\n```\nh1 { color: blue }\n```"),
                                                     said("It is blue now."), said("DONE")], auto_edits=True)),
                   prompt="Make the heading blue")
        self.assertEqual(core.ran, [("write", "style.css")])
        self.assertEqual(len(core.quiet), 2)
        self.assertIn("It is blue now.", core.reply)

    def test_an_early_all_done_goes_when_the_check_finds_more_to_do(self):
        core = run(saying_what_changed(ScriptedCore(
            [said("WRITE: a.txt\n```\nx\n```"), said("All done, it is finished."),
             said("WRITE: b.txt\n```\ny\n```"), said("Made a.txt and b.txt."), said("DONE")], auto_edits=True)),
            prompt="Make a.txt and b.txt")
        self.assertEqual(core.ran, [("write", "a.txt"), ("write", "b.txt")])
        self.assertEqual(core.reply, "Made a.txt and b.txt.")

    def test_a_write_names_the_file_it_changed_and_one_that_failed_names_none(self):
        core = ScriptedCore([said("WRITE: /home/user/a.txt\n```\nx\n```"),
                             said("EDIT: b.txt\n<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE"),
                             said("Wrote it."), said("DONE")], auto_edits=True)
        original = core.call
        shown = []

        def call(method, params=None):
            out = original(method, params)
            if method == "turn.tool":
                out = dict(out, changed=["a.txt"] if params["call"]["kind"] == "write" else [],
                           ok=params["call"]["kind"] == "write")
            if method == "turn.show":
                shown.extend(a for a in params.get("actions", []) if "language" in a)
            return out
        core.call = call
        run(core, prompt="Write a.txt")
        self.assertEqual([a["language"] for a in shown], ["a.txt", ""])

    def test_after_an_edit_the_person_declined_it_is_not_pressed_to_do_it(self):
        core = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("Left it alone.")], approve=False),
                   prompt="Make a.txt")
        self.assertEqual(core.quiet, [])
        self.assertEqual(core.ran, [])
        self.assertEqual(core.reply, "Left it alone.")

    def test_a_call_left_as_the_answer_says_which_way_it_ended(self):
        repeated = {"attempted": "none", "call": {"kind": "read"}, "search": ""}
        self.assertEqual(chat.unanswered("READ: a", repeated, False, spent=False),
                         "kept trying the same thing and was stopped")
        self.assertIn("used all the actions", chat.unanswered("READ: a", repeated, False, spent=True))
        self.assertEqual(chat.unanswered("<tool_call>", {"attempted": "call"}, False),
                         "wrote a tool call that could not be read")

    def test_it_is_checked_only_so_many_times(self):
        rounds = []
        for i in range(chat.REVIEWS + 3):
            rounds += [said("Finished %d." % i), said("WRITE: f%d.txt\n```\nx\n```" % i)]
        core = run(saying_what_changed(ScriptedCore(rounds, auto_edits=True)), prompt="Make the files")
        self.assertEqual(len(core.quiet), chat.REVIEWS)
        self.assertEqual(core.ended["state"], "finished")


class ItsOwnToolFormat(unittest.TestCase):
    """A model whose template can write tools calls them the way it was trained to."""

    SEAT = {"native": True, "tools": "\n\nWORK WITH YOUR TOOLS", "text_tools": "\n\nVERB LINES"}

    def core(self, *rounds, **extra):
        return ScriptedCore(list(rounds), seat=self.SEAT, auto_edits=True, **extra)

    def test_a_call_runs_and_what_came_back_is_its_result(self):
        core = run(self.core(calling("read_file", "Let me look.", path="notes.txt"), said("It says hello.")))
        self.assertEqual(core.ran, [("read", "notes.txt")])
        asked, result = core.sent[1]
        self.assertEqual(asked["role"], "assistant")
        self.assertEqual(asked["content"], "Let me look.")
        self.assertEqual(asked["tool_calls"][0]["function"]["name"], "read_file")
        self.assertEqual(json.loads(asked["tool_calls"][0]["function"]["arguments"]), {"path": "notes.txt"})
        self.assertEqual(result["role"], "tool")
        self.assertEqual(result["tool_call_id"], asked["tool_calls"][0]["id"])
        self.assertEqual(result["content"], "ran read notes.txt")
        # What it said stays on the reply and the call was never on it.
        self.assertEqual(core.reply, "Let me look.\n\nIt says hello.")
        self.assertEqual(core.opened["system"], "You are helpful." + chat.MARKDOWN + "\n\nWORK WITH YOUR TOOLS")

    def test_a_call_it_got_wrong_is_told_so_and_is_not_run(self):
        core = run(self.core(calling("read_file", notes="x"), calling("read_file", path="notes.txt"),
                             said("It says hello.")))
        self.assertEqual(core.ran, [("read", "notes.txt")])
        self.assertEqual(core.sent[1][1]["role"], "tool")
        self.assertEqual(core.sent[1][1]["content"], chat.BAD_CALL.format("read_file needs path"))

    def test_a_model_that_cannot_get_a_call_right_is_not_asked_for_ever(self):
        core = run(self.core(*[calling("made_up") for _ in range(10)]))
        self.assertEqual(core.ran, [])
        self.assertLessEqual(len(core.sent), chat.NUDGES + 1)
        self.assertEqual(core.ended["state"], "finished")

    def test_it_may_still_write_a_line(self):
        core = run(self.core(said("READ: notes.txt"), said("It says hello.")))
        self.assertEqual(core.ran, [("read", "notes.txt")])
        self.assertEqual(core.sent[1][1]["role"], "user")

    def test_a_template_that_will_not_take_the_conversation_falls_back_on_the_lines(self):
        core = run(self.core(said("", no_native=True), said("READ: notes.txt"), said("It says hello.")))
        self.assertEqual(core.ran, [("read", "notes.txt")])
        self.assertTrue(core.systems[1].endswith("VERB LINES"))

    def test_a_handoff_is_a_call_and_the_next_expert_carries_on(self):
        core = run(self.core(calling("hand_off", "The page is done.", next_step="write the README for it"),
                             calling("write_file", path="README.md", content="# Page"), said("Both written.")))
        self.assertEqual(core.routes, ["programming", "writing"])
        self.assertEqual(core.ran, [("write", "README.md")])
        self.assertEqual(core.sent[1][1]["role"], "tool")
        self.assertEqual(core.sent[1][1]["content"],
                         chat.TAKES_OVER.format("Programming", "write the README for it").strip())
        self.assertEqual(core.reply, "The page is done.\n\nBoth written.")

    def test_an_expert_with_no_tool_format_reads_the_calls_as_what_was_done(self):
        log = [{"role": "assistant", "content": "Looking.",
                "tool_calls": [{"id": "1", "type": "function",
                                "function": {"name": "read_file", "arguments": '{"path": "a.txt"}'}}]},
               {"role": "tool", "tool_call_id": "1", "content": "hello"}]
        flat = chat.plain(log)
        self.assertEqual([m["role"] for m in flat], ["assistant", "user"])
        # As something done, in words: written as the call, a small model
        # copied it into its answer instead of making one.
        self.assertEqual(flat[0]["content"], "Looking.\n\nI read a.txt.")
        self.assertNotIn("tool_calls", flat[0])
        self.assertEqual(flat[1], {"role": "user", "content": "hello"})
        for name, arguments, words in (
                ("write_file", '{"path": "a.txt", "content": "secret text"}', "I wrote a.txt."),
                ("run_command", '{"command": "make test"}', "I ran make test."),
                ("hand_off", '{"next_step": "write the docs"}', "I handed on the next step, write the docs."),
                ("click", '{"x": 3}', "I used click."),
                ("read_file", "not json", "I read.")):
            self.assertEqual(chat.as_done({"name": name, "arguments": arguments}), words)

    def test_a_prompt_longer_than_the_context_lets_the_older_exchanges_go_and_asks_again(self):
        rounds = [calling("read_file", path="f%d.txt" % i) for i in range(6)]
        rounds += [said("", too_long=1200), said("Read them.")]
        core = run(self.core(*rounds))
        self.assertEqual(core.reply, "Read them.")
        self.assertEqual(len(core.sent[-1]), chat.RECENT)
        self.assertIn("What has been done", core.notes[-1])

    def test_with_nothing_left_to_let_go_it_says_the_request_no_longer_fits(self):
        core = run(self.core(said("", too_long=1200)))
        self.assertEqual(core.ended["state"], "failed")
        self.assertIn("no longer fits", core.ended["reason"])


class OneRound(unittest.TestCase):
    def test_an_answer_is_one_round_and_is_remembered(self):
        core = run(ScriptedCore([said("It says hello.")]))
        self.assertEqual(core.reply, "It says hello.")
        self.assertEqual(core.ended["state"], "finished")
        self.assertTrue(core.ended["remember"])
        self.assertNotIn("reply", core.ended)
        self.assertEqual((core.ended["mood"], core.ended["status"]), ("idle", ""))
        self.assertEqual(core.sent, [[]])
        self.assertEqual(core.paragraphs, [False])
        self.assertIn(("thinking", "Programming is reading"), core.moods)

    def test_the_instructions_are_the_persons_prompt_then_how_replies_are_drawn_then_the_tools(self):
        core = run(ScriptedCore([said("Hi.")]))
        self.assertEqual(core.opened["system"], "You are helpful." + chat.MARKDOWN + "\n\nTOOLS")
        self.assertEqual(core.opened["history"], 24)
        self.assertEqual(core.opened["share"], 0.75)

    def test_the_search_tool_is_described_only_when_it_is_on(self):
        core = run(ScriptedCore([said("Hi.")]), web_search=True)
        self.assertEqual(core.opened["system"],
                         "You are helpful." + chat.MARKDOWN + "\n\nSEARCH TOOLS\n\nTOOLS")

    def test_the_effort_is_a_line_for_a_local_model_that_takes_one_and_nothing_otherwise(self):
        core = run(ScriptedCore([said("Hi.")], seat={"takes_effort": True}), effort="high")
        self.assertTrue(core.opened["system"].startswith("You are helpful.\n\nReasoning: high\n\n"))
        remote = run(ScriptedCore([said("Hi.")], seat={"takes_effort": True, "remote": True}),
                     effort="high")
        self.assertNotIn("Reasoning:", remote.opened["system"])
        plain = run(ScriptedCore([said("Hi.")]), effort="high")
        self.assertNotIn("Reasoning:", plain.opened["system"])

    def test_the_effort_is_the_level_the_core_says_the_seat_takes(self):
        # The core turns the seat's own effort, or the box's, into the nearest
        # level the model takes, and that is the line: gpt-oss has no "max".
        core = run(ScriptedCore([said("Hi.")], seat={"takes_effort": True, "effort": "high"}), effort="max")
        self.assertTrue(core.opened["system"].startswith("You are helpful.\n\nReasoning: high\n\n"))
        # The model's own default, said as no level at all.
        unset = run(ScriptedCore([said("Hi.")], seat={"takes_effort": True, "effort": ""}), effort="max")
        self.assertNotIn("Reasoning:", unset.opened["system"])

    def test_without_tools_or_search_a_turn_is_one_round_whatever_it_says(self):
        core = run(ScriptedCore([said("READ: notes.txt")], seat={"workshop": False, "tools": ""}))
        self.assertEqual(len(core.sent), 1)
        self.assertEqual(core.ran, [])
        # A call left where the answer goes is never shown as the answer, and
        # with no folder open it is said why nothing ran.
        self.assertEqual(core.ended["reply"],
                         "(reached for the project with no folder open. Open one to let it work there)")
        # Remembered all the same, as it was said: the history is what the
        # model said, not what was said about it.
        self.assertTrue(core.ended["remember"])

    def test_a_reasoning_model_that_never_answered_is_told_apart_from_one_that_said_nothing(self):
        thought = run(ScriptedCore([said("", hit_limit=True, output_tokens=40)]))
        self.assertEqual(thought.ended["reply"], '(thought until "Longest reply" ran out. Raise it in '
                                                 'Settings, Generation, or lower the effort)')
        self.assertFalse(thought.ended["remember"])
        # Asked to say something, a couple of times, before that is the end.
        quiet = run(ScriptedCore([said("", output_tokens=3) for _ in range(chat.NUDGES + 1)]))
        self.assertEqual(quiet.sent[1][-1]["content"], chat.SAID_NOTHING)
        self.assertEqual(len(quiet.sent), chat.NUDGES + 1)
        self.assertEqual(quiet.ended["reply"], "(stopped without answering)")
        # A round that produced nothing at all is not dressed up as anything.
        nothing = run(ScriptedCore([said("", output_tokens=0)]))
        self.assertNotIn("reply", nothing.ended)


class Routing(unittest.TestCase):
    def test_an_attachment_is_part_of_what_is_routed_and_said_while_it_is_read(self):
        core = run(ScriptedCore([said("Docs.")]), prompt="What does this say?",
                   attachments=["readme.md"])
        self.assertEqual(core.routed["expert"], "writing")
        self.assertEqual(core.routed["source"], "keywords")
        self.assertIn(("thinking", "Writing is reading the attachments"), core.moods)
        pinned = run(ScriptedCore([said("Code.")]), prompt="What does this say?",
                     attachments=["readme.md"], pinned="programming")
        self.assertEqual(pinned.routed["expert"], "programming")
        self.assertEqual(pinned.routed["source"], "pinned")

    def test_a_turn_stopped_while_it_was_routed_fails_quietly(self):
        core = run(ScriptedCore([], cancel=True))
        self.assertEqual(core.ended["state"], "failed")
        self.assertEqual(core.ended["reason"], "canceled before routing finished")
        self.assertEqual((core.ended["mood"], core.ended["status"]), ("idle", ""))
        self.assertNotIn("turn.load", core.calls)

    def test_no_model_to_answer_is_said_plainly(self):
        core = run(ScriptedCore([], has_model=False))
        self.assertEqual(core.ended["reason"], chat.NO_MODEL)
        self.assertEqual((core.ended["mood"], core.ended["status"]), ("error", "no experts configured"))

    def test_a_load_that_was_stopped_is_canceled_and_one_that_failed_says_why(self):
        stopped = run(ScriptedCore([], seat={"ok": False, "error": "stopped"}))
        self.assertEqual(stopped.ended["state"], "canceled")
        self.assertEqual(stopped.ended["mood"], "idle")
        failed = run(ScriptedCore([], seat={"ok": False, "error": "out of memory"}))
        self.assertEqual(failed.ended["state"], "failed")
        self.assertEqual(failed.ended["reason"], "out of memory")
        self.assertEqual((failed.ended["mood"], failed.ended["status"]), ("error", "out of memory"))
        self.assertNotIn("turn.open", failed.calls)

    def test_a_route_that_could_not_be_had_fails_the_turn_and_leaves_the_seats_alone(self):
        class Broken(ScriptedCore):
            def call(self, method, params=None):
                if method == "delegator.ready":
                    raise rpc.CoreError("the delegator would not load")
                return super().call(method, params)
        core = run(Broken([]))
        self.assertEqual(core.ended["reason"], "routing failed: the delegator would not load")
        self.assertEqual(core.ended["status"], core.ended["reason"])
        self.assertFalse(core.ended["unlink"])


class Fitting(unittest.TestCase):
    def test_a_conversation_over_its_share_is_refused_when_set_to_stop(self):
        core = run(ScriptedCore([said("Never.")], used=800, size=1000), overflow="stop")
        self.assertEqual(core.ended["reason"], 'this conversation no longer fits: 800 of 1000 tokens. '
                                               'Raise "Context" in Settings, Generation, or start a '
                                               'new chat.')
        self.assertEqual(core.ended["status"], "context full")
        self.assertEqual(core.sent, [])

    def test_a_conversation_within_its_share_is_fitted_and_what_went_is_said(self):
        core = run(ScriptedCore([said("Fine.")], used=750, size=1000, dropped=4), overflow="stop")
        self.assertEqual(core.fitted, {"turn": 3, "policy": "stop", "budget": 750})
        self.assertEqual(core.actions[0], "dropped 2 earlier exchanges to stay inside the context")
        one = run(ScriptedCore([said("Fine.")], dropped=2))
        self.assertEqual(one.actions[0], "dropped 1 earlier exchange to stay inside the context")
        none = run(ScriptedCore([said("Fine.")]))
        self.assertEqual(none.actions, [])


class Tools(unittest.TestCase):
    def test_a_call_is_run_its_line_comes_off_and_its_result_goes_back(self):
        core = run(ScriptedCore([said("Let me look.\nREAD: notes.txt"), said("It says hello.")]))
        self.assertEqual(core.ran, [("read", "notes.txt")])
        self.assertEqual(core.reply, "Let me look.\n\nIt says hello.")
        self.assertEqual(core.actions, ["read notes.txt"])
        self.assertEqual(core.paragraphs, [False, True])
        self.assertEqual(core.sent[1], [
            {"role": "assistant", "content": "Let me look.\nREAD: notes.txt"},
            {"role": "user", "content": "ran read notes.txt"},
        ])
        self.assertIn(("thinking", "read notes.txt"), core.moods)

    def test_the_same_call_with_nothing_changed_is_not_run_again_and_twice_ends_the_tools(self):
        core = run(ScriptedCore([said("READ: a.txt"), said("READ: a.txt"), said("Done."), said("Never.")]))
        self.assertEqual(core.ran, [("read", "a.txt")])
        self.assertEqual(len(core.sent), 3)
        # Told once, and it carries on with its tools.
        self.assertEqual(core.sent[2][-1], {"role": "user", "content": chat.steps.REPEAT_NUDGE.format("request")})
        self.assertIn("TOOLS", core.systems[2])
        self.assertEqual(core.reply, "Done.")
        self.assertNotIn("reply", core.ended)
        self.assertTrue(core.ended["remember"])

        # A second repeat in the same prompt ends the tools: the next round
        # answers with none offered.
        twice = run(ScriptedCore([said("READ: a.txt"), said("READ: a.txt"), said("READ: a.txt"),
                                  said("From what I have.")]))
        self.assertEqual(twice.ran, [("read", "a.txt")])
        self.assertEqual(twice.sent[3][-1], {"role": "user", "content": chat.ALREADY_DONE})
        self.assertEqual(twice.systems[3], "You are helpful." + chat.MARKDOWN)
        self.assertEqual(twice.reply, "From what I have.")

        # A file changed since is a different file to read.
        changed = ScriptedCore([said("READ: a.txt"), said("WRITE: a.txt\n```\nx\n```"),
                                said("READ: a.txt"), said("Fixed.")], auto_edits=True)
        original = changed.call

        def call(method, params=None):
            out = original(method, params)
            if method == "turn.tool" and params["call"]["kind"] == "write":
                out = dict(out, changed=["a.txt"])
            return out
        changed.call = call
        run(changed)
        self.assertEqual(changed.ran, [("read", "a.txt"), ("write", "a.txt"), ("read", "a.txt")])

    def test_a_write_is_asked_about_and_a_no_takes_back_what_was_said_about_it(self):
        core = run(ScriptedCore([said("Before.\nREAD: a.txt"), said("Writing.\nWRITE: a.txt\n```\nx\n```"),
                                 said("Left alone.")], approve=False))
        self.assertEqual(core.asked_edit, ["a.txt"])
        # While the person decides, the turn shows what it said and not the call.
        self.assertEqual(core.replies_during_asks, ["Before.\n\nWriting."])
        self.assertNotIn(("write", "a.txt"), core.ran)
        self.assertEqual(core.actions, ["read a.txt", "declined the edit to a.txt"])
        self.assertEqual(core.sent[2][-1], {"role": "user", "content": chat.DECLINED})
        self.assertEqual(core.reply, "Before.\n\nLeft alone.")

    def test_a_yes_writes_and_the_diff_is_kept_as_the_file_it_is(self):
        shown = []

        class Watching(ScriptedCore):
            def call(self, method, params=None):
                if method == "turn.show":
                    shown.extend(params.get("actions", []))
                return super().call(method, params)
        core = run(saying_what_changed(Watching([said("WRITE: a.txt\n```\nx\n```"), said("Written.")])))
        self.assertEqual(core.ran, [("write", "a.txt")])
        self.assertEqual(shown, [{"summary": "write a.txt", "body": "the diff", "language": "a.txt",
                                  "picture": ""}])
        self.assertEqual(core.reply, "Written.")

    def test_with_auto_on_a_write_is_not_asked_about_and_a_write_may_be_made_twice(self):
        core = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("WRITE: a.txt\n```\ny\n```"),
                                 said("Done.")], auto_edits=True))
        self.assertEqual(core.asked_edit, [])
        self.assertEqual(core.ran, [("write", "a.txt"), ("write", "a.txt")])

    def test_the_last_action_is_told_it_is_the_last_and_one_still_calling_is_asked_to_answer(self):
        rounds = [said("READ: f%d.txt" % i) for i in range(3)]
        core = run(ScriptedCore(rounds + [said("One more.\nREAD: last.txt"), said("Shakespeare."),
                                          said("Never asked.")]), actions=3)
        self.assertEqual(len(core.sent), 5)
        self.assertEqual(len(core.ran), 3)
        self.assertNotIn(("read", "last.txt"), core.ran)
        self.assertTrue(core.sent[3][-1]["content"].endswith(chat.LAST_ACTION))
        self.assertFalse(core.sent[2][-1]["content"].endswith(chat.LAST_ACTION))
        # The call it made instead of answering goes back unrun, and the round
        # after is offered no tools.
        self.assertEqual(core.sent[-1][-2:], [
            {"role": "assistant", "content": "One more.\nREAD: last.txt"},
            {"role": "user", "content": chat.NOT_RUN},
        ])
        self.assertTrue(all("TOOLS" in system for system in core.systems[:-1]))
        self.assertEqual(core.systems[-1], "You are helpful." + chat.MARKDOWN)
        # The call's line is off the reply, and what was said before it stays.
        self.assertEqual(core.reply, "One more.\n\nShakespeare.")
        self.assertNotIn("reply", core.ended)
        self.assertTrue(core.ended["remember"])

    def test_a_turn_that_calls_even_with_no_tools_offered_says_so(self):
        rounds = [said("READ: f%d.txt" % i) for i in range(4)]
        core = run(ScriptedCore(rounds + [said("Never asked.")]), actions=2)
        self.assertEqual(len(core.sent), 4)
        self.assertEqual(core.ended["reply"], '(used all the actions this request may take. Ask again '
                                              'to carry on, or raise "Actions per prompt" in Settings, '
                                              'Tools)')
        self.assertTrue(core.ended["remember"])

    def test_a_picture_goes_with_the_result_to_a_model_that_sees_and_is_named_to_one_that_does_not(self):
        sees = run(ScriptedCore([said("SCREENSHOT: "), said("A window.")], pictures="p1"))
        self.assertEqual(sees.sent[1][-1], {"role": "user", "content": "ran screenshot ", "pictures": "p1"})
        blind = run(ScriptedCore([said("SCREENSHOT: "), said("A window.")], pictures="p1", sees=False))
        self.assertEqual(blind.sent[1][-1], {"role": "user", "content": "ran screenshot " + chat.TEXT_ONLY})

    def test_a_call_is_not_looked_for_without_a_workshop_but_a_search_is(self):
        core = run(ScriptedCore([said("SEARCH: news"), said("Here.")],
                                seat={"workshop": False, "tools": ""}), web_search=True)
        self.assertEqual(core.searched, ["news"])
        self.assertEqual(len(core.sent), 2)


class Loop(unittest.TestCase):
    """A prompt worked until it is done, by whoever fits each step."""

    def test_a_prompt_is_worked_past_a_dozen_actions_until_it_answers(self):
        rounds = [said("READ: f%d.txt" % i) for i in range(20)] + [said("All twenty read.")]
        core = run(ScriptedCore(rounds))
        self.assertEqual(len(core.ran), 20)
        self.assertEqual(core.reply, "All twenty read.")
        self.assertNotIn("reply", core.ended)
        self.assertTrue(core.ended["remember"])

    def test_what_it_is_shown_only_grows_so_the_model_reads_only_what_is_new(self):
        rounds = [said("READ: f%d.txt" % i, prompt_tokens=50 + i) for i in range(10)] + [said("Read.")]
        core = run(ScriptedCore(rounds))
        # Every round continues the one before it, under the same instructions:
        # anything that changes near the start is read again, all of it.
        self.assertEqual(len(set(core.systems)), 1)
        self.assertEqual(set(core.notes), {""})
        for before, after in zip(core.sent, core.sent[1:]):
            self.assertEqual(after[:len(before)], before)

    def test_the_older_exchanges_go_together_once_the_context_is_nearly_full(self):
        # Of a context of 1000, a conversation may take 750 and is crowded at 600.
        rounds = [said("READ: f%d.txt" % i, prompt_tokens=100 * i) for i in range(8)] + [said("Read.")]
        core = run(ScriptedCore(rounds))
        self.assertEqual(len(core.sent[7]), 14)
        self.assertEqual(core.notes[7], "")
        # The round after one of 700 keeps the last few, and an account of the
        # rest beside the request, not in the instructions.
        self.assertEqual(len(core.sent[8]), chat.RECENT)
        self.assertEqual(core.sent[8][0]["role"], "assistant")
        self.assertIn("What has been done for this request so far", core.notes[8])
        self.assertIn("- read f0.txt", core.notes[8])
        self.assertEqual(core.systems[8], core.systems[0])

    def test_an_expert_hands_off_and_the_next_one_carries_on_with_the_conversation(self):
        core = run(ScriptedCore([said("The page is done.\nHANDOFF: write the README for it"),
                                 said("WRITE: README.md\n```\n# Page\n```"), said("Both written.")],
                                auto_edits=True))
        # Routed on the step: keywords here, which read README as Writing's.
        self.assertEqual(core.routes, ["programming", "writing"])
        self.assertEqual(core.loads, 2)
        self.assertEqual(core.opens, 2)
        self.assertIn("Writing takes over: write the README for it", core.actions)
        self.assertEqual(core.sent[1][-1]["content"],
                         chat.TAKES_OVER.format("Programming", "write the README for it").strip())
        self.assertEqual(core.ran, [("write", "README.md")])
        # The HANDOFF line is off the reply, and the words before it stay.
        self.assertEqual(core.reply, "The page is done.\n\nBoth written.")

    def test_a_handoff_nobody_fits_better_carries_on_with_the_same_expert(self):
        core = run(ScriptedCore([said("HANDOFF: tidy the python module"), said("Tidied.")]))
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.loads, 1)
        self.assertEqual(core.sent[1][-1]["content"],
                         chat.CARRY_ON.format("tidy the python module").strip())

    def test_a_prompt_sent_to_one_expert_stays_with_it(self):
        core = run(ScriptedCore([said("HANDOFF: write the README"), said("Done myself.")]),
                   pinned="programming")
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.sent[1][-1]["content"], chat.PINNED_ON.strip())
        self.assertEqual(core.reply, "Done myself.")

    def test_every_few_actions_the_delegator_may_give_the_next_step_to_another_expert(self):
        decisions = [chat.routing.Decision("programming", 0.9, chat.routing.MODEL),
                     chat.routing.Decision("writing", 0.8, chat.routing.MODEL)]
        asked = []

        def route(core, request):
            asked.append(request)
            return decisions.pop(0)
        rounds = ([said("READ: f%d.txt" % i) for i in range(chat.CHECKPOINT_EVERY)]
                  + [said("HANDOFF: document what the files do"), said("Documented.")])
        original = chat.routing.route
        chat.routing.route = route
        try:
            core = run(ScriptedCore(rounds))
        finally:
            chat.routing.route = original
        # The checkpoint asked, out of sight, and the step was routed as a task.
        self.assertEqual(len(core.quiet), 1)
        self.assertIn(chat.NEXT_STEP, core.quiet[0][-1]["content"])
        self.assertEqual(asked[1]["prompt"], "document what the files do")
        self.assertTrue(asked[1]["tasks"])
        self.assertEqual(core.routes, ["programming", "writing"])
        self.assertIn("Writing takes over: document what the files do", core.actions)
        # The new expert is told what it is taking over; nothing of the
        # checkpoint reached the reply.
        self.assertTrue(core.systems[-1].endswith(
            chat.TAKES_OVER.format("Programming", "document what the files do")))
        self.assertNotIn("HANDOFF", core.reply)
        self.assertEqual(core.reply, "Documented.")

    def test_the_checkpoint_keeps_the_expert_the_delegator_keeps(self):
        decisions = [chat.routing.Decision("programming", 0.9, chat.routing.MODEL)] * 2

        def route(core, request):
            return decisions.pop(0)
        rounds = ([said("READ: f%d.txt" % i) for i in range(chat.CHECKPOINT_EVERY)]
                  + [said("HANDOFF: keep reading"), said("Read them all.")])
        original = chat.routing.route
        chat.routing.route = route
        try:
            core = run(ScriptedCore(rounds))
        finally:
            chat.routing.route = original
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.loads, 1)
        self.assertEqual(core.reply, "Read them all.")

    def routed(self, decisions, rounds, **extra):
        """A turn with the delegator's decisions scripted, and writes that
        say which file they changed."""
        def route(core, request):
            return decisions.pop(0)
        core = ScriptedCore(rounds, auto_edits=True)
        original = core.call

        def call(method, params=None):
            out = original(method, params)
            if method == "turn.tool" and params["call"]["kind"] in ("write", "edit"):
                out = dict(out, changed=[params["call"]["argument"]])
            return out
        core.call = call
        before = chat.routing.route
        chat.routing.route = route
        try:
            run(core, **extra)
        finally:
            chat.routing.route = before
        return core

    def test_a_finished_piece_of_work_lets_the_delegator_give_the_next_one_to_another_expert(self):
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        core = self.routed([Decision("programming", 0.61, MODEL), Decision("writing", 0.9, MODEL)],
                           [said("WRITE: index.html\n```\n<p>leaves</p>\n```"),
                            said("HANDOFF: write a README explaining the haiku's imagery"),
                            said("WRITE: README.md\n```\n# Imagery\n```"), said("A page, and a README about it.")])
        # Asked right after the page was written, not six actions later.
        self.assertEqual(len(core.quiet), 1)
        self.assertEqual(core.routes, ["programming", "writing"])
        self.assertIn("Writing takes over: write a README explaining the haiku's imagery", core.actions)
        self.assertEqual(core.ran, [("write", "index.html"), ("write", "README.md")])
        self.assertEqual(core.reply, "A page, and a README about it.")

    def test_the_checkpoint_asks_for_its_one_line_without_the_thinking(self):
        # A reasoning model spent all 256 of its tokens on thinking about it,
        # and no step ever came back for the delegator to route.
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        core = self.routed([Decision("programming", 0.61, MODEL), Decision("writing", 0.9, MODEL)],
                           [said("WRITE: index.html\n```\n<p>leaves</p>\n```"),
                            said("HANDOFF: write a README"),
                            said("WRITE: README.md\n```\n# Imagery\n```"), said("Done.")])
        asked = [p for p in core.round_params if p.get("quiet")]
        self.assertEqual(len(asked), 1)
        self.assertTrue(asked[0]["quick"])
        self.assertFalse(any(p.get("quick") for p in core.round_params if not p.get("quiet")))

    def test_all_done_at_a_checkpoint_goes_to_nobody(self):
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        core = self.routed([Decision("programming", 0.9, MODEL), Decision("writing", 0.95, MODEL)],
                           [said("WRITE: a.py\n```\nx\n```"), said("HANDOFF: DONE."), said("Made it.")])
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.reply, "Made it.")

    def test_a_step_that_says_there_is_none_is_read_as_that(self):
        for step in ("DONE", "Done.", "All done", "Task completed: a and b", "Nothing left", "No further steps",
                     "The monthly summary page has been added and NOTE.md written", "Everything is finished"):
            self.assertTrue(chat.nothing_left(step), step)
        for step in ("write the README", "Create poem.txt with a short autumn poem", "Add tests for the parser",
                     "Finish the chart", "Complete the form validation", "document what the files do"):
            self.assertFalse(chat.nothing_left(step), step)

    def test_a_checkpoint_that_says_the_work_is_finished_keeps_the_expert(self):
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        core = self.routed([Decision("programming", 0.9, MODEL), Decision("writing", 0.95, MODEL)],
                           [said("WRITE: a.py\n```\nx\n```"), said("HANDOFF: The file has been written."),
                            said("Made it.")])
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.reply, "Made it.")

    def test_handing_on_that_it_is_all_done_finishes(self):
        core = run(ScriptedCore([said("READ: a.txt"), said("HANDOFF: DONE")]))
        self.assertEqual(core.routes, ["programming"])
        self.assertNotIn("DONE", core.reply)
        self.assertEqual(core.ended["state"], "finished")

    def test_one_stray_word_beside_a_call_stays_off_the_reply(self):
        core = run(ScriptedCore([calling("read_file", "comment?", path="a.txt"), said("It says hello.")],
                                seat={"native": True, "tools": "\n\nT", "text_tools": "\n\nV"}))
        self.assertEqual(core.ran, [("read", "a.txt")])
        self.assertEqual(core.reply, "It says hello.")

    def test_what_gpt_oss_writes_beside_a_call_stays_off_the_reply(self):
        seat = {"native": True, "tools": "\n\nT", "text_tools": "\n\nV", "takes_effort": True}
        core = run(ScriptedCore([calling("read_file", "We also need to look. Let's read it again.", path="a.txt"),
                                 said("It says hello.")], seat=seat))
        self.assertEqual(core.reply, "It says hello.")

    def test_a_check_that_changed_nothing_is_not_asked_a_second_time(self):
        core = saying_what_changed(ScriptedCore([said("WRITE: a.txt\n```\nb\n```"), said("Wrote it."),
                                                 said("READ: a.txt"), said("It is right."),
                                                 said("(out of script)")], auto_edits=True))
        run(core, prompt="Write a.txt.")
        self.assertEqual(len([p for p in core.round_params if p.get("quiet")]), 1)
        self.assertEqual(len(core.rounds), 1)
        self.assertIn("It is right.", core.reply)

    def test_a_model_that_thinks_too_long_is_asked_again_and_gets_on_with_it(self):
        core = ScriptedCore([said("READ: a.txt"), said("", reasoning="r" * 20000, overthought=True),
                             said("WRITE: a.txt\n```\nb\n```"), said("Changed it."), said("DONE")],
                            auto_edits=True)
        run(core, prompt="Change a.txt to say b.")
        self.assertEqual(core.ran, [("read", "a.txt"), ("write", "a.txt")])
        self.assertEqual(core.reply, "Changed it.")
        # Free to think before its first action, held to it after, and the
        # one round after the long one is told to get on with it.
        self.assertEqual(core.round_params[0]["think_most"], 0)
        self.assertEqual(core.round_params[1]["think_most"], chat.THINK_MOST)
        self.assertEqual([bool(p["rush"]) for p in core.round_params[:4]], [False, False, True, False])

    def test_thinking_twice_too_long_does_not_go_round(self):
        long = said("", reasoning="r" * 20000, overthought=True)
        core = ScriptedCore([said("READ: a.txt"), long, long, said("Nothing to change."), said("DONE")])
        run(core, prompt="Fix a.txt if it needs it.")
        self.assertEqual(core.reply, "Nothing to change.")

    def test_a_question_may_think_as_long_as_it_needs(self):
        core = run(ScriptedCore([said("READ: notes.txt"), said("It says hello.")]))
        self.assertEqual({p["think_most"] for p in core.round_params}, {0})

    def test_node_on_a_script_for_a_web_page_is_not_a_failure_to_fix(self):
        call = {"kind": "run", "argument": "node app.js"}
        broke = {"ok": False, "output": "app.js:44\nReferenceError: document is not defined\n"}
        self.assertTrue(chat.steps.needs_a_page(call, broke))
        self.assertIn("web page", chat.steps.read_hint(call, broke))
        self.assertFalse(chat.steps.needs_a_page(call, {"ok": False, "output": "SyntaxError: bad"}))
        self.assertFalse(chat.steps.needs_a_page({"kind": "run", "argument": "pytest"}, broke))
        self.assertFalse(chat.steps.needs_a_page(call, dict(broke, ok=True)))

    def test_a_checkpoint_the_delegator_is_unsure_of_keeps_the_expert(self):
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        core = self.routed([Decision("programming", 0.9, MODEL), Decision("writing", 0.6, MODEL)],
                           [said("WRITE: a.py\n```\nx\n```"), said("HANDOFF: write the docs"),
                            said("Done.")])
        self.assertEqual(core.routes, ["programming"])
        self.assertEqual(core.loads, 1)

    def test_an_edit_and_test_loop_on_one_file_is_not_asked_its_next_step_each_time(self):
        Decision, MODEL = chat.routing.Decision, chat.routing.MODEL
        rounds = [said("WRITE: a.py\n```\nx\n```"), said("HANDOFF: test it"),
                  said("RUN: pytest"), said("EDIT: a.py\n<<<<<<< SEARCH\nx\n=======\ny\n>>>>>>> REPLACE"),
                  said("RUN: pytest"), said("EDIT: a.py\n<<<<<<< SEARCH\ny\n=======\nz\n>>>>>>> REPLACE"),
                  said("It passes.")]
        core = self.routed([Decision("programming", 0.9, MODEL)] * 2, rounds)
        # Once, for the new file; the edits to it are the same piece of work.
        self.assertEqual(len(core.quiet), 1)
        self.assertEqual(core.reply, "It passes.")

    def test_without_a_delegator_there_is_no_checkpoint(self):
        rounds = [said("READ: f%d.txt" % i) for i in range(chat.CHECKPOINT_EVERY + 1)] + [said("Read.")]
        core = run(ScriptedCore(rounds))
        self.assertEqual(core.quiet, [])
        self.assertEqual(core.reply, "Read.")

    def test_a_run_of_actions_going_round_is_told_so_and_a_second_answers_with_no_tools(self):
        going = [said("RUN: make" if i % 2 == 0 else "RUN: make test") for i in range(16)]
        core = run(ScriptedCore(going + [said("It will not build, because of the missing header.")]))
        self.assertEqual(len(core.ran), 16)
        self.assertTrue(core.sent[8][-1]["content"].endswith(chat.CYCLING))
        self.assertTrue(core.sent[16][-1]["content"].endswith(chat.ALREADY_DONE))
        # The last round is offered no tools.
        self.assertEqual(core.systems[16], "You are helpful." + chat.MARKDOWN)
        self.assertFalse(core.offered[16])
        self.assertEqual(core.reply, "It will not build, because of the missing header.")

    def test_a_reply_that_nearly_called_a_tool_is_told_what_was_wrong(self):
        core = run(ScriptedCore([said("WRITE index.html"), said("WRITE: index.html\n```\nx\n```"),
                                 said("Made it.")], auto_edits=True))
        self.assertEqual(core.sent[1][-1]["content"], chat.steps.nudge_for({"attempted": "write"}))
        self.assertEqual(core.ran, [("write", "index.html")])
        self.assertEqual(core.reply, "Made it.")

    def test_a_call_after_a_sentence_on_its_line_is_told_to_start_a_line(self):
        core = run(ScriptedCore([said("Here is the page. WRITE: index.html\n```\nx\n```"),
                                 said("WRITE: index.html\n```\nx\n```"), said("Made it.")],
                                auto_edits=True))
        nudge = core.sent[1][-1]["content"]
        self.assertEqual(nudge, chat.steps.nudge_for({"attempted": "write", "inline": True}))
        self.assertIn("WRITE has to start a line of its own", nudge)
        self.assertEqual(core.ran, [("write", "index.html")])
        self.assertEqual(core.reply, "Made it.")

    def test_an_answer_straight_after_a_failed_action_is_asked_to_face_it(self):
        core = ScriptedCore([said("WRITE: /index.html\n```\nx\n```"), said("I made the page."),
                             said("WRITE: index.html\n```\nx\n```"), said("Made index.html.")],
                            auto_edits=True)
        original = core.call

        def call(method, params=None):
            out = original(method, params)
            if method == "turn.tool" and params["call"]["argument"].startswith("/"):
                out = dict(out, ok=False, summary="/index.html is outside the project")
            return out
        core.call = call
        run(core)
        self.assertEqual(core.sent[2][-1]["content"],
                         chat.FAILED_LAST.format("/index.html is outside the project"))
        self.assertEqual(core.ran, [("write", "/index.html"), ("write", "index.html")])
        self.assertEqual(core.reply, "Made index.html.")
        # Only once: an answer after the nudge is the answer.
        once = ScriptedCore([said("RUN: make test"), said("Two tests fail."), said("They fail on the date.")])
        original_once = once.call

        def failing(method, params=None):
            out = original_once(method, params)
            if method == "turn.tool":
                out = dict(out, ok=False, summary="make test: 2 failed")
            return out
        once.call = failing
        run(once)
        self.assertEqual(once.reply, "They fail on the date.")

    def test_a_write_the_length_limit_cut_off_is_not_written_and_is_asked_for_again(self):
        core = run(ScriptedCore([said("WRITE: README.md\n```\n# Haiku\nThe haiku is", hit_limit=True),
                                 said("WRITE: README.md\n```\n# Haiku\n```"), said("Written.")],
                                auto_edits=True))
        self.assertEqual(core.ran, [("write", "README.md")])
        self.assertEqual(core.sent[1][-1], {"role": "user", "content": chat.CUT_OFF})
        self.assertEqual(core.reply, "Written.")

    def test_a_reply_that_repeats_itself_is_stopped_and_asked_once_more(self):
        core = run(ScriptedCore([said("again again again", looped=True), said("Plainly, once.")]))
        self.assertTrue(core.systems[1].endswith(chat.REPEATED_ITSELF))
        self.assertEqual(core.reply, "Plainly, once.")
        twice = run(ScriptedCore([said("a a a", looped=True), said("b b b", looped=True)]))
        self.assertEqual(twice.ended["reply"], "(kept repeating itself and was stopped)")


class Searching(unittest.TestCase):
    def test_a_search_is_run_once_and_asked_again_is_refused(self):
        core = run(ScriptedCore([said("SEARCH: capital of France"), said("SEARCH: capital of France"),
                                 said("Paris.")]), web_search=True)
        self.assertEqual(core.searched, ["capital of France"])
        self.assertEqual(core.actions, ['searched "capital of France": 1 result from searxng'])
        self.assertEqual(core.sent[1][-1], {"role": "user", "content": "results for capital of France"})
        self.assertEqual(core.sent[2][-1]["content"],
                         'You have already searched for "capital of France" and been given the '
                         'results above.' + chat.LAST_SEARCH)
        self.assertEqual(core.reply, "Paris.")
        self.assertIn(("thinking", 'searching for "capital of France"'), core.moods)
        self.assertIn(("thinking", 'already searched for "capital of France"'), core.moods)
        # Asked for twice, it is told so and carries on with what it has: with
        # a folder open it still has its other tools.
        self.assertEqual(len(core.sent), 3)
        self.assertIn("TOOLS", core.systems[2])

    def test_the_last_search_says_so_and_a_turn_still_searching_is_asked_to_answer(self):
        core = run(ScriptedCore([said("SEARCH: one"), said("SEARCH: two"), said("SEARCH: three"),
                                 said("Nothing turned up.")],
                                seat={"workshop": False, "tools": ""}, results=0), web_search=True)
        self.assertEqual(core.actions, ['searched "one": nothing found for "one"',
                                        'searched "two": nothing found for "two"'])
        self.assertFalse(core.sent[1][-1]["content"].endswith(chat.LAST_SEARCH))
        self.assertTrue(core.sent[2][-1]["content"].endswith(chat.LAST_SEARCH))
        self.assertEqual(core.searched, ["one", "two"])
        self.assertEqual(core.sent[3][-1], {"role": "user", "content": "There are no searches left for this "
                                                                        "request." + chat.LAST_SEARCH})
        self.assertEqual(core.systems[3], "You are helpful." + chat.MARKDOWN)
        self.assertEqual(core.reply, "Nothing turned up.")
        self.assertNotIn("reply", core.ended)

    def test_a_turn_still_searching_with_no_tools_offered_says_why_it_stopped(self):
        core = run(ScriptedCore([said("SEARCH: one"), said("SEARCH: two"), said("SEARCH: three"),
                                 said("SEARCH: four")],
                                seat={"workshop": False, "tools": ""}, results=0), web_search=True)
        self.assertEqual(len(core.sent), 4)
        self.assertEqual(core.ended["reply"], '(kept searching instead of answering. Raise "Searches '
                                              'per prompt" in Settings, Tools)')

    def test_many_results_are_counted_in_the_plural(self):
        core = run(ScriptedCore([said("SEARCH: x"), said("Done.")], results=3), web_search=True)
        self.assertEqual(core.actions, ['searched "x": 3 results from searxng'])


class Ending(unittest.TestCase):
    def test_a_provider_that_failed_fails_the_turn_with_its_reason(self):
        core = run(ScriptedCore([said("", error="Scripted is having trouble (HTTP 500)")]))
        self.assertEqual(core.ended["state"], "failed")
        self.assertEqual(core.ended["reason"], "Scripted is having trouble (HTTP 500)")
        self.assertEqual((core.ended["mood"], core.ended["status"]),
                         ("error", "Scripted is having trouble (HTTP 500)"))
        self.assertFalse(core.ended["remember"])

    def test_an_answer_cut_short_keeps_what_came_and_a_refusal_takes_it_back(self):
        cut = run(ScriptedCore([said("Half an ans", error="overloaded", notes=["another model answered"])]))
        self.assertEqual(cut.actions, ["another model answered", "cut short: overloaded"])
        self.assertEqual(cut.reply, "Half an ans")
        refused = run(ScriptedCore([said("Before.\nREAD: a.txt"),
                                    said("Sure, here", error="declined", discard=True)]))
        self.assertEqual(refused.reply, "Before.")
        # The prose before it stands, so the reason is said above it.
        self.assertEqual(refused.actions, ["read a.txt", "declined"])
        self.assertEqual(refused.ended["reason"], "declined")
        # Declined in the first round, the reason is the reply itself.
        first = run(ScriptedCore([said("Sure, here", error="declined", discard=True)]))
        self.assertEqual(first.actions, [])
        self.assertEqual(first.ended["reason"], "declined")

    def test_a_stopped_turn_ends_canceled_without_being_remembered(self):
        core = run(ScriptedCore([said("one two", canceled=True), said("never")]))
        self.assertEqual(len(core.sent), 1)
        self.assertEqual(core.ended["state"], "finished")
        self.assertEqual(core.ended["status"], "canceled")
        self.assertFalse(core.ended["remember"])
        self.assertNotIn("reply", core.ended)

    def test_a_core_that_fails_partway_ends_the_turn_with_the_reason(self):
        class Failing(ScriptedCore):
            def call(self, method, params=None):
                if method == "turn.tool":
                    raise rpc.CoreError("the disk went away")
                return super().call(method, params)
        core = run(Failing([said("READ: a.txt")]))
        self.assertEqual(core.ended["state"], "failed")
        self.assertEqual(core.ended["reason"], "the disk went away")


class Words(unittest.TestCase):
    def test_the_prose_before_a_call_is_kept_and_a_mention_of_the_verb_is_not_a_call(self):
        self.assertEqual(chat.prose_before_tool_call("I will fix it.\n\nWRITE: a.py\n```\n```", "write"),
                         "I will fix it.")
        self.assertEqual(chat.prose_before_tool_call("Use WRITE: to save.\n  WRITE: a.py", "write"),
                         "Use WRITE: to save.")
        self.assertEqual(chat.prose_before_tool_call("READ: a.py", "read"), "")
        self.assertEqual(chat.prose_before_tool_call("Look.\nTOOL: fs/list", "tool"), "Look.")
        # Kept as the core always kept it: a call that names its shell is
        # not found by its bare verb, and a call with no line of its own
        # leaves nothing to keep.
        self.assertEqual(chat.prose_before_tool_call("Run it.\nRUN bash: ls", "run"), "")
        self.assertEqual(chat.prose_before_tool_call("Just prose.", "read"), "")
        # Claude Haiku 5.5 once put a lone <br> before its READ: nothing to keep.
        self.assertEqual(chat.prose_before_tool_call("<br>\nREAD: notes.txt", "read"), "")
        self.assertEqual(chat.prose_before_tool_call("<br/> <BR>\n\nREAD: notes.txt", "read"), "")
        self.assertEqual(chat.prose_before_tool_call("Line one<br>line two\nREAD: a.py", "read"),
                         "Line one<br>line two")

    def test_paragraphs_join_what_there_is(self):
        self.assertEqual(chat.paragraphs("", "b"), "b")
        self.assertEqual(chat.paragraphs("a", ""), "a")
        self.assertEqual(chat.paragraphs("a", "b"), "a\n\nb")

    def test_the_status_line_is_cut_at_a_character_not_through_one(self):
        self.assertEqual(chat.doing("read", "notes.txt"), "read notes.txt")
        long = chat.doing("run", "x" * 100)
        self.assertEqual(long, "run " + "x" * 60 + "…")
        self.assertEqual(len(long.encode("utf-8")), 64 + 3)
        wide = chat.doing("read", "é" * 40)
        self.assertTrue(wide.endswith("…"))
        self.assertLessEqual(len(wide[:-1].encode("utf-8")), 64)
        self.assertEqual(wide[:-1], "read " + "é" * 29)

    def test_the_actions_a_prompt_may_take(self):
        self.assertEqual(chat.actions_for({}), chat.ACTIONS)
        self.assertEqual(chat.actions_for({"actions": 25}), 25)
        self.assertEqual(chat.actions_for({"actions": 0}), chat.ACTIONS)
        self.assertEqual(chat.actions_for({"actions": "x"}), chat.ACTIONS)

    def test_the_turn_is_shown_its_last_few_messages_from_an_answer_of_its_own(self):
        log = [{"role": "assistant" if i % 2 == 0 else "user", "content": str(i)} for i in range(12)]
        start, shown = chat.window(log)
        self.assertEqual(start, 12 - chat.RECENT)
        self.assertEqual(shown[0]["role"], "assistant")
        self.assertEqual(chat.window(log[:5]), (0, log[:5]))
        self.assertEqual(chat.running_account([]), "")
        account = chat.running_account([{"summary": "read a.py"}, {"summary": "ran tests", "ok": False},
                                        {"summary": "wrote b.py", "changed": ["b.py"]}])
        self.assertIn("- read a.py\n- ran tests  (failed)\n- wrote b.py\n", account)
        self.assertTrue(account.endswith("Files changed: b.py\n"))

    def test_a_pinned_prompt_is_routed_as_it_was_typed(self):
        self.assertEqual(chat.routed_prompt("Q", "", ["a.md", "b.py"]), "Q\n\nAttached: a.md b.py")
        self.assertEqual(chat.routed_prompt("Q", "physics", ["a.md"]), "Q")
        self.assertEqual(chat.routed_prompt("Q", "", []), "Q")


if __name__ == "__main__":
    unittest.main()
