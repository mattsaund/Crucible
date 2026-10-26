# SPDX-License-Identifier: MIT
"""A chat turn against a core that plays the expert from a script.

The core here keeps a transcript the way the real one does -- a round's words
streamed onto the reply, a paragraph before them when the turn had said
something already, actions above it -- so what a turn leaves on screen can be
read back and checked, along with what the expert was sent each round.
tests/test_chat.cpp walks the same turns through the real core.
"""

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

VERBS = ("LIST", "READ", "WRITE", "EDIT", "RUN", "NOTE", "DONE", "SEARCH", "SCREENSHOT")


def parse(answer):
    """A small stand-in for the core's parser: `VERB: argument`, a fence
    after WRITE, and a SEARCH line as a search."""
    match = re.search(r"^(%s): ?(.*)$" % "|".join(VERBS), answer, re.M)
    search = re.search(r"^SEARCH: ?(.*)$", answer, re.M)
    if not match:
        return {"call": None, "attempted": "none", "search": ""}
    content = ""
    if match.group(1) == "WRITE":
        fence = re.search(r"```\n(.*?)```", answer, re.S)
        content = fence.group(1) if fence else ""
    return {"call": {"kind": match.group(1).lower(), "argument": match.group(2).strip(),
                     "content": content, "shell": "", "unrun": ""},
            "attempted": "none", "search": search.group(1).strip() if search else ""}


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
        self.paragraphs = []   # whether each round began with a paragraph
        self.ran = []
        self.asked_edit = []
        self.searched = []
        self.routed = None
        self.opened = None
        self.fitted = None
        self.replies_during_asks = []

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
            return {"has_model": self.has_model}
        if method == "turn.load":
            return dict(self.seat)
        if method == "turn.open":
            self.opened = params
            return {"used": self.used, "size": self.size}
        if method == "turn.fit":
            self.fitted = params
            return {"dropped": self.dropped}
        if method == "turn.round":
            self.sent.append([dict(m) for m in params["messages"]])
            self.paragraphs.append(params["paragraph"])
            out = self.rounds.pop(0) if self.rounds else said("(out of script)")
            if out["answer"] and not out.get("discard"):
                if params["paragraph"]:
                    self.reply += "\n\n"
                self.reply += out["answer"]
            out = dict(out)
            out["reply"] = self.reply
            return out
        if method == "tools.parse":
            return parse(params["answer"] or params["reasoning"])
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

    def test_without_tools_or_search_a_turn_is_one_round_whatever_it_says(self):
        core = run(ScriptedCore([said("READ: notes.txt")], seat={"workshop": False, "tools": ""}))
        self.assertEqual(len(core.sent), 1)
        self.assertEqual(core.ran, [])
        # A call left where the answer goes is never shown as the answer.
        self.assertEqual(core.ended["reply"], "(ran out of turns before finishing. Ask again to carry on)")
        # Remembered all the same, as it was said: the history is what the
        # model said, not what was said about it.
        self.assertTrue(core.ended["remember"])

    def test_a_reasoning_model_that_never_answered_is_told_apart_from_one_that_said_nothing(self):
        thought = run(ScriptedCore([said("", hit_limit=True, output_tokens=40)]))
        self.assertEqual(thought.ended["reply"], '(thought until "Longest reply" ran out. Raise it in '
                                                 'Settings, Generation, or lower the effort)')
        self.assertFalse(thought.ended["remember"])
        quiet = run(ScriptedCore([said("", output_tokens=3)]))
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

    def test_the_same_call_twice_is_not_run_again(self):
        core = run(ScriptedCore([said("READ: a.txt"), said("READ: a.txt"), said("Done.")]))
        self.assertEqual(core.ran, [("read", "a.txt")])
        self.assertEqual(core.sent[2][-1], {"role": "user", "content": chat.ALREADY_DONE})
        self.assertEqual(core.reply, "Done.")

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
        core = run(Watching([said("WRITE: a.txt\n```\nx\n```"), said("Written.")]))
        self.assertEqual(core.ran, [("write", "a.txt")])
        self.assertEqual(shown, [{"summary": "write a.txt", "body": "the diff", "language": "a.txt",
                                  "picture": ""}])
        self.assertEqual(core.reply, "Written.")

    def test_with_auto_on_a_write_is_not_asked_about_and_a_write_may_be_made_twice(self):
        core = run(ScriptedCore([said("WRITE: a.txt\n```\nx\n```"), said("WRITE: a.txt\n```\ny\n```"),
                                 said("Done.")], auto_edits=True))
        self.assertEqual(core.asked_edit, [])
        self.assertEqual(core.ran, [("write", "a.txt"), ("write", "a.txt")])

    def test_the_last_round_is_told_it_is_the_last_and_one_still_calling_says_so(self):
        rounds = [said("READ: f%d.txt" % i) for i in range(chat.TOOL_ROUNDS)]
        core = run(ScriptedCore(rounds + [said("Never asked.")]))
        self.assertEqual(len(core.sent), chat.TOOL_ROUNDS)
        self.assertEqual(len(core.ran), chat.TOOL_ROUNDS - 1)
        self.assertTrue(core.sent[-1][-1]["content"].endswith(chat.LAST_ACTION))
        self.assertFalse(core.sent[-2][-1]["content"].endswith(chat.LAST_ACTION))
        self.assertEqual(core.ended["reply"], "(ran out of turns before finishing. Ask again to carry on)")
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

    def test_the_last_search_says_so_and_a_turn_still_searching_says_why_it_stopped(self):
        core = run(ScriptedCore([said("SEARCH: one"), said("SEARCH: two"), said("SEARCH: three")],
                                seat={"workshop": False, "tools": ""}, results=0), web_search=True)
        self.assertEqual(core.actions, ['searched "one": nothing found for "one"',
                                        'searched "two": nothing found for "two"'])
        self.assertFalse(core.sent[1][-1]["content"].endswith(chat.LAST_SEARCH))
        self.assertTrue(core.sent[2][-1]["content"].endswith(chat.LAST_SEARCH))
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
        self.assertEqual(refused.actions, ["read a.txt"])

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

    def test_the_rounds_a_turn_may_take(self):
        self.assertEqual(chat.rounds_for({}, False), 1)
        self.assertEqual(chat.rounds_for({"web_search": True, "search_rounds": 2}, False), 3)
        self.assertEqual(chat.rounds_for({"web_search": True, "search_rounds": 0}, False), 1)
        self.assertEqual(chat.rounds_for({"web_search": True, "search_rounds": 20}, True), 21)
        self.assertEqual(chat.rounds_for({}, True), chat.TOOL_ROUNDS)

    def test_a_pinned_prompt_is_routed_as_it_was_typed(self):
        self.assertEqual(chat.routed_prompt("Q", "", ["a.md", "b.py"]), "Q\n\nAttached: a.md b.py")
        self.assertEqual(chat.routed_prompt("Q", "physics", ["a.md"]), "Q")
        self.assertEqual(chat.routed_prompt("Q", "", []), "Q")


if __name__ == "__main__":
    unittest.main()
